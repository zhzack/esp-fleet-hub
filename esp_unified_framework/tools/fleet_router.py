#!/usr/bin/env python3
"""
ESP Unified Framework - Fleet Hub HTTP Router & Shared Domain Logic
Handles device challenge-response registration, heartbeats, shadow config sync,
direct LAN device proxying, and fleet-wide batch OTA.
Used both by standalone tools/web_compiler.py (:8088) and py_web FastAPI (:8000).
"""

import os
import json
import urllib.request
import urllib.error
import urllib.parse

import hub_db
import build_flavor
import mdns_advertiser


def _direct_device_http(dev, path, method="GET", payload=None, timeout=5):
    ip = dev.get("ip")
    port = int(dev.get("web_port") or 80)
    if not ip or ip == "0.0.0.0":
        return False, 0, "Device IP not known"

    url = f"http://{ip}:{port}{path}"
    headers = {}
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if dev.get("api_token"):
        headers["X-API-Key"] = dev["api_token"]

    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(req, timeout=timeout) as resp:
            raw = resp.read()
            ctype = resp.headers.get("Content-Type", "application/json")
            return True, resp.status, (raw, ctype)
    except urllib.error.HTTPError as he:
        return False, he.code, he.read().decode("utf-8", "replace")
    except Exception as e:
        return False, 0, str(e)


def list_firmware_items():
    dist_dir = build_flavor.DIST_DIR
    items = []
    if os.path.isdir(dist_dir):
        for fn in sorted(os.listdir(dist_dir)):
            if not fn.endswith(".bin"):
                continue
            fp = os.path.join(dist_dir, fn)
            meta = build_flavor.load_artifact_meta(fn) or {}
            items.append({
                "file": fn,
                "size": os.path.getsize(fp),
                "target": meta.get(
                    "target",
                    "esp8266" if "8266" in fn.lower()
                    else ("esp32c3" if "c3" in fn.lower() else ("esp32s3" if "s3" in fn.lower() else "esp32")),
                ),
                "board": meta.get("board", "auto"),
                "flash_size": meta.get("flash_size", "4MB"),
                "fw_version": meta.get("fw_version", "1.0.0"),
                "sha256": meta.get("sha256") or build_flavor.compute_sha256(fp),
                "features": meta.get("features", {}),
                "build_time": meta.get("build_time", int(os.path.getmtime(fp))),
            })
    return items


def register_device_payload(payload):
    """Validate challenge proof (if device has secret) and register/update device."""
    dev_id = str(payload.get("device_id", "")).strip()
    if not dev_id:
        return 400, {"ok": False, "error": "missing device_id"}

    nonce = payload.get("nonce", "")
    proof = payload.get("proof", "")
    existing = hub_db.get_device(dev_id)
    if existing and existing.get("has_secret"):
        if not nonce or not proof:
            return 401, {"ok": False, "error": "HMAC challenge proof required"}
        ok, msg = hub_db.verify_and_consume_hmac(dev_id, nonce, proof)
        if not ok:
            hub_db.log_audit(dev_id, "auth_failed", msg)
            return 401, {"ok": False, "error": msg}

    dev = hub_db.register_or_update_device(payload)
    return 200, {"ok": True, "device": dev}


def push_config_to_device(dev_id, patch):
    dev = hub_db.update_desired_config(dev_id, patch or {})
    if not dev:
        return 404, {"ok": False, "error": "Device not found"}

    pushed_live = False
    push_err = ""
    if dev.get("ip") and dev["ip"] != "0.0.0.0":
        ok, st, res = _direct_device_http(dev, "/api/system/config", method="POST", payload=patch, timeout=4)
        if ok:
            pushed_live = True
            if isinstance(patch, dict) and patch.get("api_token") is not None:
                hub_db.update_device_meta(dev_id, api_token=patch["api_token"])
        else:
            push_err = str(res)

    return 200, {
        "ok": True,
        "pushed_live": pushed_live,
        "push_error": push_err,
        "device": hub_db.get_device(dev_id),
    }


def send_command_to_device(dev_id, cmd_type, cmd_payload=None):
    cmd_payload = cmd_payload or {}
    dev = hub_db.get_device(dev_id)
    if not dev:
        return 404, {"ok": False, "error": "Device not found"}

    direct_ok = False
    direct_resp = None
    if dev.get("ip") and dev["ip"] != "0.0.0.0":
        ep_map = {
            "gpio_set": "/api/gpio/set",
            "camera_config": "/api/camera/config",
            "camera_flash": "/api/camera/flash",
            "display_text": "/api/display/text",
            "display_config": "/api/display/config",
            "hid_text": "/api/hid/text",
            "hid_key": "/api/hid/key",
            "reboot": "/api/system/reboot",
        }
        if cmd_type in ep_map:
            ok, st, res = _direct_device_http(dev, ep_map[cmd_type], method="POST", payload=cmd_payload, timeout=5)
            if ok:
                direct_ok = True
                try:
                    direct_resp = json.loads(res[0].decode("utf-8", "replace"))
                except Exception:
                    direct_resp = {"ok": True}

    cmd_id = None
    if not direct_ok:
        cmd_id = hub_db.queue_command(dev_id, cmd_type, cmd_payload)

    return 200, {
        "ok": True,
        "delivered_direct": direct_ok,
        "queued_cmd_id": cmd_id,
        "response": direct_resp,
    }


def trigger_batch_ota(fw_file, device_ids, server_port=8088, dl_path="/api/download"):
    fp = os.path.join(build_flavor.DIST_DIR, fw_file)
    if not os.path.isfile(fp):
        return 404, {"ok": False, "error": f"Firmware not found: {fw_file}"}

    meta = build_flavor.load_artifact_meta(fw_file) or {}
    fw_target = meta.get("target", "").lower().replace("-", "")
    sha256 = meta.get("sha256") or build_flavor.compute_sha256(fp)
    lan_ip = mdns_advertiser.get_lan_ip()
    dl_url = f"http://{lan_ip}:{server_port}{dl_path}?file={urllib.parse.quote(fw_file)}"

    results = []
    for did in (device_ids or []):
        dev = hub_db.get_device(did)
        if not dev:
            results.append({"device_id": did, "ok": False, "error": "not found"})
            continue
        dev_chip = dev.get("chip", "").lower().replace("-", "")
        if fw_target and dev_chip:
            if ("esp8266" in dev_chip and fw_target != "esp8266") or (
                "esp32c3" in dev_chip and fw_target != "esp32c3"
            ) or (
                "esp32s3" in dev_chip and fw_target != "esp32s3"
            ):
                results.append({
                    "device_id": did,
                    "ok": False,
                    "error": f"Target mismatch: device={dev.get('chip')} vs fw={fw_target}",
                })
                continue

        ok, st, res = _direct_device_http(
            dev,
            "/api/ota/pull",
            method="POST",
            payload={"url": dl_url, "sha256": sha256, "version": meta.get("fw_version", "")},
            timeout=5,
        )
        if ok:
            hub_db.log_audit(did, "ota_pull_started", fw_file)
            results.append({"device_id": did, "ok": True, "mode": "direct_pull"})
        else:
            cid = hub_db.queue_command(did, "ota_pull", {"url": dl_url, "sha256": sha256})
            results.append({"device_id": did, "ok": True, "mode": "queued_heartbeat", "cmd_id": cid})

    return 200, {"ok": True, "url": dl_url, "sha256": sha256, "results": results}


def handle_get(handler, parsed):
    """Handle GET /api/fleet/* routes. Returns True if handled."""
    path = parsed.path
    qs = urllib.parse.parse_qs(parsed.query)

    if path == "/api/fleet/challenge":
        dev_id = qs.get("device_id", [""])[0]
        nonce = hub_db.issue_nonce(dev_id)
        _send_json(handler, 200, {"ok": True, "nonce": nonce})
        return True

    if path == "/api/fleet/devices":
        devs = hub_db.list_devices()
        logs = hub_db.list_audit_logs(30)
        _send_json(handler, 200, {"ok": True, "devices": devs, "audit": logs})
        return True

    if path == "/api/fleet/firmwares":
        _send_json(handler, 200, {"ok": True, "firmwares": list_firmware_items()})
        return True

    if path == "/api/fleet/audit":
        limit = int(qs.get("limit", ["50"])[0] or 50)
        _send_json(handler, 200, {"ok": True, "logs": hub_db.list_audit_logs(limit)})
        return True

    if path.startswith("/api/fleet/proxy/"):
        dev_id = urllib.parse.unquote(path[len("/api/fleet/proxy/"):])
        target_path = qs.get("path", ["/api/system/status"])[0]
        if not target_path.startswith("/"):
            target_path = "/" + target_path
        dev = hub_db.get_device(dev_id)
        if not dev:
            _send_json(handler, 404, {"ok": False, "error": "Device not found"})
            return True
        ok, status, result = _direct_device_http(dev, target_path, method="GET", timeout=8)
        if not ok:
            _send_json(handler, 502, {"ok": False, "status": status, "error": str(result)})
            return True
        raw, ctype = result
        handler.send_response(status)
        handler.send_header("Content-Type", ctype)
        handler.send_header("Access-Control-Allow-Origin", "*")
        handler.end_headers()
        handler.wfile.write(raw)
        return True

    return False


def handle_post(handler, path, payload, server_port=8088):
    """Handle POST /api/fleet/* routes. Returns True if handled."""
    if path == "/api/fleet/register":
        code, body = register_device_payload(payload)
        _send_json(handler, code, body)
        return True

    if path == "/api/fleet/heartbeat":
        dev_id = str(payload.get("device_id", "")).strip()
        if not dev_id:
            _send_json(handler, 400, {"ok": False, "error": "missing device_id"})
            return True
        res = hub_db.process_heartbeat(dev_id, payload)
        _send_json(handler, 200, res)
        return True

    if path == "/api/fleet/cmd_ack":
        dev_id = str(payload.get("device_id", ""))
        cmd_id = payload.get("cmd_id")
        ok = bool(payload.get("ok", True))
        hub_db.log_audit(dev_id, "cmd_ack", f"cmd_id={cmd_id} ok={ok}")
        _send_json(handler, 200, {"ok": True})
        return True

    if path == "/api/fleet/provision":
        dev_id = str(payload.get("device_id", "")).strip()
        if not dev_id:
            _send_json(handler, 400, {"ok": False, "error": "device_id required"})
            return True
        dev = hub_db.provision_device(
            device_id=dev_id,
            mac=payload.get("mac", ""),
            chip=payload.get("chip", ""),
            flash_mb=int(payload.get("flash_mb", 4) or 4),
            board=payload.get("board", "auto"),
            device_secret=payload.get("device_secret", ""),
            hub_url=payload.get("hub_url", ""),
        )
        _send_json(handler, 200, {"ok": True, "device": dev})
        return True

    if path == "/api/fleet/device/claim":
        dev_id = str(payload.get("device_id", "")).strip()
        dev = hub_db.update_device_meta(
            dev_id,
            name=payload.get("name"),
            claimed=payload.get("claimed", True),
            api_token=payload.get("api_token"),
        )
        if not dev:
            _send_json(handler, 404, {"ok": False, "error": "Device not found"})
            return True
        hub_db.log_audit(dev_id, "claimed", f"name={dev.get('name')}")
        _send_json(handler, 200, {"ok": True, "device": dev})
        return True

    if path == "/api/fleet/device/delete":
        dev_id = str(payload.get("device_id", "")).strip()
        ok = hub_db.delete_device(dev_id)
        _send_json(handler, 200, {"ok": ok})
        return True

    if path == "/api/fleet/device/config":
        code, body = push_config_to_device(
            str(payload.get("device_id", "")).strip(),
            payload.get("config") or {},
        )
        _send_json(handler, code, body)
        return True

    if path == "/api/fleet/device/command":
        code, body = send_command_to_device(
            str(payload.get("device_id", "")).strip(),
            str(payload.get("cmd_type", "")).strip(),
            payload.get("payload") or {},
        )
        _send_json(handler, code, body)
        return True

    if path == "/api/fleet/ota/batch":
        code, body = trigger_batch_ota(
            str(payload.get("file", "")).strip(),
            payload.get("device_ids") or [],
            server_port=server_port,
        )
        _send_json(handler, code, body)
        return True

    return False


def _send_json(handler, code, obj):
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()
    handler.wfile.write(json.dumps(obj, ensure_ascii=False).encode("utf-8"))
