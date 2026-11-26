"""Authenticated /api/v2/unified/* management & cloud compiler routes for Vue SPA."""

from __future__ import annotations

import base64
from pathlib import Path
from typing import Any, Dict, List, Optional
import urllib.parse

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from fastapi.responses import FileResponse, JSONResponse, Response
from pydantic import BaseModel
from sqlalchemy.orm import Session
from starlette.concurrency import run_in_threadpool

from app.core.config.config import settings
from app.core.security import get_current_admin, get_current_user
from app.database.crud import device as crud_device
from app.database.models.device import Device
from app.database.models.user import User
from app.database.schemas.firmware import FirmwareOut
from app.database.session import get_db
from app.services import dispatcher
from app.services.device_registry import device_registry
from app.services.firmware_utils import build_signed_download_url, check_chip_compatibility
from app.services.ota_tracker import ota_tracker
from app.services import unified_bridge as ub

router = APIRouter()

_WS_PROXY_GET: dict[str, tuple[str, int]] = {
    "/api/system/status": ("get_status", 4000),
    "/api/status": ("get_status", 4000),
    "/api/system/config": ("sys.config", 4000),
    "/api/gpio/pins": ("gpio.pins", 4000),
    "/api/display/status": ("display.status", 4000),
    "/api/storage/list": ("storage.list", 5000),
    "/api/ota/status": ("ota.status", 4000),
    "/api/camera/config": ("camera.config.get", 4000),
    "/capture": ("camera.capture", 8000),
    "/api/camera/capture": ("camera.capture", 8000),
}

_WS_PROXY_POST: dict[str, tuple[str, int]] = {
    "/api/system/config": ("config.set", 5000),
    "/api/system/reboot": ("reboot", 2500),
    "/api/gpio/set": ("gpio.set", 4000),
    "/api/display/text": ("display.text", 4000),
    "/api/display/config": ("display.config", 4000),
    "/api/display/image": ("display.image", 8000),
    "/api/monitor/push": ("monitor.push", 3000),
    "/api/camera/flash": ("camera.flash", 4000),
    "/api/flash": ("camera.flash", 4000),
    "/api/camera/config": ("camera.config", 4000),
    "/api/framesize": ("camera.config", 4000),
    "/api/ota/pull": ("ota.start", 6000),
}

_WS_CMD_MAP: dict[str, str] = {
    "gpio_set": "gpio.set",
    "camera_config": "camera.config",
    "camera_flash": "camera.flash",
    "display_text": "display.text",
    "display_config": "display.config",
    "display_image": "display.image",
    "reboot": "reboot",
    "ota_pull": "ota.start",
}


class ClaimIn(BaseModel):
    name: Optional[str] = None
    claimed: bool = True
    api_token: Optional[str] = None


class ConfigPushIn(BaseModel):
    config: Dict[str, Any]


class CommandIn(BaseModel):
    cmd_type: str
    payload: Dict[str, Any] = {}


class BatchOtaIn(BaseModel):
    file: str
    device_ids: List[str]


class PublishIn(BaseModel):
    version: Optional[str] = None
    description: Optional[str] = None


def _request_port(request: Request) -> int:
    if request.url.port:
        return int(request.url.port)
    return 8000


def _build_download_url(request: Request, fw_file: str) -> str:
    return build_signed_download_url(
        request,
        "/api/fleet/download",
        f"fleet:{fw_file}",
        {"file": fw_file},
    )


def _build_ws_proxy_response(res: dict) -> Response:
    if res.get("status") != "acked":
        raise HTTPException(
            502,
            f"WS tunnel failed ({res.get('error_code') or res.get('status')}): {res.get('error_message') or 'device error'}",
        )
    inner = dict(res.get("result") or {})
    if "image_b64" in inner:
        raw = base64.b64decode(inner["image_b64"])
        return Response(content=raw, status_code=200, media_type=inner.get("content_type", "image/jpeg"))
    inner.setdefault("ok", True)
    inner["via"] = "websocket"
    return JSONResponse(status_code=200, content=inner)


async def _proxy_via_ws(
    db: Session,
    user: User,
    device_id: str,
    method: str,
    target: str,
    payload: dict | None = None,
) -> Response | None:
    if not device_registry.is_online(device_id):
        return None
    if method == "GET" and target in _WS_PROXY_GET:
        cmd_type, timeout_ms = _WS_PROXY_GET[target]
        res = await dispatcher.deliver_command(
            db, device_uuid=device_id, cmd_type=cmd_type, payload={}, user_id=user.id, timeout_ms=timeout_ms
        )
        return _build_ws_proxy_response(res)
    if method == "POST":
        if target == "/api/hid/text":
            res = await dispatcher.deliver_to_device(
                db,
                device_uuid=device_id,
                payload={"type": "text", "content": (payload or {}).get("text", ""), "need_ack": True},
                user_id=user.id,
                timeout_ms=4000,
            )
            return _build_ws_proxy_response(res)
        if target in _WS_PROXY_POST:
            cmd_type, timeout_ms = _WS_PROXY_POST[target]
            res = await dispatcher.deliver_command(
                db, device_uuid=device_id, cmd_type=cmd_type, payload=payload or {}, user_id=user.id, timeout_ms=timeout_ms
            )
            return _build_ws_proxy_response(res)
    return None


def _require_device_access(
    db: Session,
    user: User,
    device_id: str,
    *,
    allow_unclaimed: bool = False,
) -> Dict[str, Any]:
    """Enforce admin or owning-user authorization on a unified device."""
    ub.hub_db.ensure_init()
    hub_dev = ub.hub_db.get_device(device_id)
    if not hub_dev:
        raise HTTPException(404, "Device not found")
    if user.role == "admin":
        return hub_dev

    app_dev = crud_device.get_by_uuid(db, device_id)
    if app_dev:
        if app_dev.owner_user_id == user.id:
            return hub_dev
        if app_dev.owner_user_id is None and allow_unclaimed and not hub_dev.get("claimed"):
            return hub_dev
        raise HTTPException(403, "无权操作其他用户的设备")

    if not hub_dev.get("claimed") and allow_unclaimed:
        return hub_dev
    raise HTTPException(403, "请先认领设备或使用管理员账号操作")


@router.get("/devices")
def list_unified_devices(
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    ub.hub_db.ensure_init()
    all_devs = ub.hub_db.list_devices()
    prog_map = ota_tracker.get_all()
    for d in all_devs:
        if d["device_id"] in prog_map:
            d["ota_progress"] = prog_map[d["device_id"]]
        d["ws_online"] = device_registry.is_online(d["device_id"])

    logs = ub.hub_db.list_audit_logs(40)
    if user.role == "admin":
        return {"ok": True, "devices": all_devs, "audit": logs}

    owners = {
        row[0]: row[1]
        for row in db.query(Device.device_uuid, Device.owner_user_id).all()
    }
    visible = [
        d for d in all_devs
        if owners.get(d["device_id"]) == user.id
        or (owners.get(d["device_id"]) is None and not d.get("claimed"))
    ]
    visible_ids = {d["device_id"] for d in visible}
    return {
        "ok": True,
        "devices": visible,
        "audit": [a for a in logs if a.get("device_id") in visible_ids],
    }


@router.post("/devices/{device_id}/claim")
def claim_unified_device(
    device_id: str,
    body: ClaimIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    _require_device_access(db, user, device_id, allow_unclaimed=True)
    dev = ub.hub_db.update_device_meta(
        device_id,
        name=body.name,
        claimed=body.claimed,
        api_token=body.api_token,
    )
    if not dev:
        raise HTTPException(404, "Device not found")

    app_dev = crud_device.get_by_uuid(db, device_id)
    target_owner = user.id if body.claimed else None
    if app_dev is None:
        crud_device.create(
            db,
            device_uuid=device_id,
            owner_user_id=target_owner,
            name=dev.get("name") or device_id,
            capabilities=[],
        )
    else:
        app_dev.name = dev.get("name") or app_dev.name
        if body.claimed and app_dev.owner_user_id is None:
            app_dev.owner_user_id = user.id
        elif not body.claimed and (user.role == "admin" or app_dev.owner_user_id == user.id):
            app_dev.owner_user_id = None
        db.commit()

    ub.hub_db.log_audit(device_id, "claimed", f"name={dev.get('name')} by={user.username}")
    return {"ok": True, "device": dev}


@router.post("/devices/{device_id}/config")
async def push_unified_config(
    device_id: str,
    body: ConfigPushIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    _require_device_access(db, user, device_id, allow_unclaimed=False)
    dev = ub.hub_db.update_desired_config(device_id, body.config or {})
    if not dev:
        raise HTTPException(404, "Device not found")
    if isinstance(body.config, dict) and body.config.get("api_token") is not None:
        ub.hub_db.update_device_meta(device_id, api_token=body.config["api_token"])

    # Prefer WebSocket tunnel when device is online on /ws/device (works across NAT/Docker)
    if device_registry.is_online(device_id):
        ws_res = await dispatcher.deliver_command(
            db,
            device_uuid=device_id,
            cmd_type="config.set",
            payload=body.config or {},
            user_id=user.id,
            timeout_ms=5000,
        )
        if ws_res.get("status") == "acked":
            return {
                "ok": True,
                "pushed_live": True,
                "via": "websocket",
                "push_error": "",
                "device": ub.hub_db.get_device(device_id),
            }

    ok, _st, res = await run_in_threadpool(
        ub.fleet_router._direct_device_http,
        dev,
        "/api/system/config",
        "POST",
        body.config,
        4,
    )
    if ok and isinstance(body.config, dict) and body.config.get("api_token") is not None:
        ub.hub_db.update_device_meta(device_id, api_token=body.config["api_token"])
    return {
        "ok": True,
        "pushed_live": ok,
        "via": "http" if ok else "queued",
        "push_error": "" if ok else str(res),
        "device": ub.hub_db.get_device(device_id),
    }


@router.post("/devices/{device_id}/command")
async def send_unified_command(
    device_id: str,
    body: CommandIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    _require_device_access(db, user, device_id, allow_unclaimed=False)
    if device_registry.is_online(device_id):
        if body.cmd_type in ("hid_text", "hid_key"):
            action_pl = (
                {"type": "text", "content": body.payload.get("text", ""), "need_ack": True}
                if body.cmd_type == "hid_text"
                else {
                    "type": "keyboard_tap",
                    "key": body.payload.get("key", ""),
                    "need_ack": True,
                }
            )
            ws_res = await dispatcher.deliver_to_device(
                db, device_uuid=device_id, payload=action_pl, user_id=user.id, timeout_ms=5000
            )
            if ws_res.get("status") == "acked":
                return {
                    "ok": True,
                    "delivered_direct": True,
                    "via": "websocket",
                    "queued_cmd_id": None,
                    "response": ws_res,
                }
        elif body.cmd_type in _WS_CMD_MAP:
            ws_res = await dispatcher.deliver_command(
                db,
                device_uuid=device_id,
                cmd_type=_WS_CMD_MAP[body.cmd_type],
                payload=body.payload,
                user_id=user.id,
                timeout_ms=5000,
            )
            if ws_res.get("status") == "acked":
                return {
                    "ok": True,
                    "delivered_direct": True,
                    "via": "websocket",
                    "queued_cmd_id": None,
                    "response": ws_res.get("result") or {"ok": True},
                }

    code, res = await run_in_threadpool(
        ub.fleet_router.send_command_to_device, device_id, body.cmd_type, body.payload
    )
    return JSONResponse(status_code=code, content=res)


@router.delete("/devices/{device_id}")
def delete_unified_device(
    device_id: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    _require_device_access(db, user, device_id, allow_unclaimed=False)
    ok = ub.hub_db.delete_device(device_id)
    app_dev = crud_device.get_by_uuid(db, device_id)
    if app_dev:
        db.delete(app_dev)
        db.commit()
    return {"ok": ok}


@router.get("/devices/{device_id}/proxy")
async def proxy_unified_get(
    device_id: str,
    path: str = Query("/api/system/status"),
    via: str = Query("auto"),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    dev = _require_device_access(db, user, device_id, allow_unclaimed=False)
    target = path if path.startswith("/") else "/" + path
    if via == "ws":
        ws_resp = await _proxy_via_ws(db, user, device_id, "GET", target)
        if ws_resp is not None:
            return ws_resp
        raise HTTPException(502, "WS tunnel unavailable for path or device offline")

    ok, status, result = await run_in_threadpool(
        ub.fleet_router._direct_device_http, dev, target, "GET", None, 4
    )
    if ok:
        raw, ctype = result
        return Response(content=raw, status_code=status, media_type=ctype)
    if via != "http":
        ws_resp = await _proxy_via_ws(db, user, device_id, "GET", target)
        if ws_resp is not None:
            return ws_resp
    raise HTTPException(502, f"Proxy failed ({status}): {result}")


@router.post("/devices/{device_id}/proxy")
async def proxy_unified_post(
    device_id: str,
    request: Request,
    path: str = Query(...),
    via: str = Query("auto"),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    dev = _require_device_access(db, user, device_id, allow_unclaimed=False)
    try:
        payload = await request.json()
    except Exception:
        payload = {}
    target = path if path.startswith("/") else "/" + path
    if via == "ws":
        ws_resp = await _proxy_via_ws(db, user, device_id, "POST", target, payload)
        if ws_resp is not None:
            return ws_resp
        raise HTTPException(502, "WS tunnel unavailable for path or device offline")

    ok, status, result = await run_in_threadpool(
        ub.fleet_router._direct_device_http, dev, target, "POST", payload, 4
    )
    if ok:
        raw, ctype = result
        return Response(content=raw, status_code=status, media_type=ctype)
    if via != "http":
        ws_resp = await _proxy_via_ws(db, user, device_id, "POST", target, payload)
        if ws_resp is not None:
            return ws_resp
    raise HTTPException(502, f"Proxy failed ({status}): {result}")


@router.get("/profiles")
def get_profiles(_user: User = Depends(get_current_user)):
    return {"ok": True, "profiles": ub.web_compiler.list_profiles_data()}


@router.get("/ports")
def get_ports(_admin: User = Depends(get_current_admin)):
    return {"ok": True, "ports": ub.web_compiler.list_serial_ports()}


@router.get("/firmwares")
def get_unified_firmwares(_user: User = Depends(get_current_user)):
    return {"ok": True, "firmwares": ub.fleet_router.list_firmware_items()}


@router.get("/firmwares/{file_name}/download")
def download_unified_firmware(
    file_name: str,
    _user: User = Depends(get_current_user),
):
    safe = Path(file_name).name
    fp = Path(ub.build_flavor.DIST_DIR) / safe
    if not fp.is_file():
        raise HTTPException(404, "Firmware not found")
    return FileResponse(path=fp, media_type="application/octet-stream", filename=safe)


@router.post("/firmwares/{file_name}/publish", response_model=FirmwareOut)
def publish_unified_firmware(
    file_name: str,
    body: PublishIn = PublishIn(),
    db: Session = Depends(get_db),
    _admin: User = Depends(get_current_admin),
):
    safe = Path(file_name).name
    try:
        return ub.publish_firmware_to_hid_repo(
            db, safe, version_override=body.version, description=body.description
        )
    except FileNotFoundError as e:
        raise HTTPException(404, str(e))


@router.post("/ota/batch")
async def batch_unified_ota(
    body: BatchOtaIn,
    request: Request,
    db: Session = Depends(get_db),
    admin: User = Depends(get_current_admin),
):
    ub.hub_db.ensure_init()
    safe_fw = Path(body.file).name
    fp = Path(ub.build_flavor.DIST_DIR) / safe_fw
    if not fp.is_file():
        return JSONResponse(status_code=404, content={"ok": False, "error": f"Firmware not found: {safe_fw}"})

    meta = ub.build_flavor.load_artifact_meta(safe_fw) or {}
    fw_target = meta.get("target", "").lower().replace("-", "")
    fw_ver = meta.get("fw_version", "")
    sha256 = meta.get("sha256") or ub.build_flavor.compute_sha256(str(fp))
    fw_size = fp.stat().st_size
    dl_url = _build_download_url(request, safe_fw)

    results = []
    for did in (body.device_ids or []):
        dev = ub.hub_db.get_device(did)
        if not dev:
            results.append({"device_id": did, "ok": False, "error": "not found"})
            continue
        ok_chip, chip_err = check_chip_compatibility(dev.get("chip", ""), fw_target)
        if not ok_chip:
            results.append({
                "device_id": did,
                "ok": False,
                "error": chip_err,
            })
            continue

        ota_tracker.update_progress(
            did,
            {
                "status": "starting",
                "progress": 0,
                "bytes_read": 0,
                "total_bytes": fw_size,
                "version": fw_ver,
            },
        )

        # Prefer WebSocket tunnel first (cross-NAT + streams ota_progress frames into ota_tracker)
        if device_registry.is_online(did):
            ws_res = await dispatcher.deliver_command(
                db,
                device_uuid=did,
                cmd_type="ota.start",
                payload={"url": dl_url, "sha256": sha256, "version": fw_ver},
                user_id=admin.id,
                timeout_ms=6000,
            )
            if ws_res.get("status") == "acked":
                ub.hub_db.log_audit(did, "ota_ws_started", safe_fw)
                results.append({"device_id": did, "ok": True, "mode": "ws_ota_start"})
                continue

        ok, _st, _res = await run_in_threadpool(
            ub.fleet_router._direct_device_http,
            dev,
            "/api/ota/pull",
            "POST",
            {"url": dl_url, "sha256": sha256, "version": fw_ver},
            5,
        )
        if ok:
            ub.hub_db.log_audit(did, "ota_pull_started", safe_fw)
            results.append({"device_id": did, "ok": True, "mode": "direct_pull"})
        else:
            cid = ub.hub_db.queue_command(did, "ota_pull", {"url": dl_url, "sha256": sha256})
            ota_tracker.update_progress(
                did, {"status": "queued", "progress": 0, "bytes_read": 0, "total_bytes": fw_size, "version": fw_ver}
            )
            results.append({"device_id": did, "ok": True, "mode": "queued_heartbeat", "cmd_id": cid})

    return {"ok": True, "url": dl_url, "sha256": sha256, "results": results}


@router.get("/ota/progress/{device_id}")
def get_unified_ota_progress(
    device_id: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    _require_device_access(db, user, device_id, allow_unclaimed=False)
    p = ota_tracker.get_progress(device_id)
    if not p:
        return {"device_id": device_id, "status": "idle", "progress": 0, "bytes_read": 0, "total_bytes": 0}
    return p


@router.post("/build")
async def start_unified_build(
    request: Request,
    _admin: User = Depends(get_current_admin),
):
    if ub.web_compiler.is_build_running():
        raise HTTPException(409, "已有云端编译任务正在执行，请等待完成")
    payload = await request.json()
    try:
        tid = ub.start_cloud_build(payload, hub_port=_request_port(request))
    except ValueError as e:
        raise HTTPException(400, str(e))
    return {"ok": True, "task_id": tid, "message": "Build task launched"}


@router.get("/task")
def get_compiler_task(
    task_id: Optional[str] = Query(None),
    _user: User = Depends(get_current_user),
):
    return ub.web_compiler.get_task_snapshot(task_id)


@router.post("/flash_serial")
async def start_unified_serial_flash(
    request: Request,
    _admin: User = Depends(get_current_admin),
):
    ub.hub_db.ensure_init()
    payload = await request.json()
    port = str(payload.get("port", "COM11"))
    if ub.web_compiler.is_port_flashing(port):
        raise HTTPException(409, f"串口 {port} 正在烧录中，请勿重复下发")
    tid = ub.start_serial_flash(payload, hub_port=_request_port(request))
    return {"ok": True, "task_id": tid, "message": "Serial flash started"}
