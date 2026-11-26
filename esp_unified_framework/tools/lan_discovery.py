#!/usr/bin/env python3
"""
LAN Discovery (mDNS `_esp-unified._tcp` + Concurrent HTTP Subnet Probe) & Key-Based Adopt Helper.
Works across any new LAN/subnet without hardcoded IPs or central hub assumptions.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
import ipaddress
import json
import socket
import struct
import time
from typing import Any, Iterable
import urllib.error
import urllib.request

import mdns_advertiser

MDNS_ADDR = mdns_advertiser.MDNS_ADDR
MDNS_PORT = mdns_advertiser.MDNS_PORT
UNIFIED_MDNS_SERVICE = "_esp-unified._tcp.local"

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _build_mdns_ptr_query(service_fqdn: str, *, unicast_response: bool = False) -> bytes:
    hdr = struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0)
    qname = mdns_advertiser._encode_name(service_fqdn)
    qclass = 0x8001 if unicast_response else 1
    q = struct.pack(">HH", mdns_advertiser.DNS_TYPE_PTR, qclass)
    return hdr + qname + q


def _parse_txt_rdata(rdata: bytes) -> dict[str, str]:
    out: dict[str, str] = {}
    i = 0
    while i < len(rdata):
        slen = rdata[i]
        i += 1
        if slen == 0 or i + slen > len(rdata):
            break
        item = rdata[i : i + slen].decode("utf-8", "ignore")
        i += slen
        if "=" in item:
            k, v = item.split("=", 1)
            out[k.strip()] = v.strip()
        elif item:
            out[item.strip()] = "1"
    return out


def _parse_dns_packet(pkt: bytes) -> dict[str, Any]:
    if len(pkt) < 12:
        return {}
    _tid, flags, qdcount, ancount, nscount, arcount = struct.unpack(">HHHHHH", pkt[:12])
    if (flags & 0x8000) == 0:
        return {}  # Ignore queries
    off = 12
    for _ in range(qdcount):
        _, off = mdns_advertiser._decode_name(pkt, off)
        off += 4
        if off > len(pkt):
            return {}

    ptrs: list[tuple[str, str]] = []
    srvs: dict[str, tuple[str, int]] = {}
    txts: dict[str, dict[str, str]] = {}
    addrs: dict[str, str] = {}

    for _ in range(ancount + nscount + arcount):
        if off >= len(pkt):
            break
        rname, off = mdns_advertiser._decode_name(pkt, off)
        if off + 10 > len(pkt):
            break
        rtype, _rclass, ttl, rdlen = struct.unpack(">HHIH", pkt[off : off + 10])
        off += 10
        if off + rdlen > len(pkt):
            break
        rdata_off = off
        rdata = pkt[off : off + rdlen]
        off += rdlen
        if ttl == 0:
            continue

        rn_low = rname.lower()
        if rtype == mdns_advertiser.DNS_TYPE_PTR:
            target, _ = mdns_advertiser._decode_name(pkt, rdata_off)
            ptrs.append((rn_low, target))
        elif rtype == mdns_advertiser.DNS_TYPE_SRV and rdlen >= 6:
            _prio, _wt, port = struct.unpack(">HHH", rdata[:6])
            target_host, _ = mdns_advertiser._decode_name(pkt, rdata_off + 6)
            srvs[rn_low] = (target_host.lower(), int(port))
        elif rtype == mdns_advertiser.DNS_TYPE_TXT:
            txts[rn_low] = _parse_txt_rdata(rdata)
        elif rtype == mdns_advertiser.DNS_TYPE_A and rdlen == 4:
            addrs[rn_low] = socket.inet_ntoa(rdata)

    return {"ptrs": ptrs, "srvs": srvs, "txts": txts, "addrs": addrs}


def discover_mdns_unified(timeout_s: float = 1.4, service: str = UNIFIED_MDNS_SERVICE) -> list[dict[str, Any]]:
    """Broadcast mDNS PTR query for `_esp-unified._tcp.local` and collect ESP responders."""
    svc_low = service.strip(".").lower()
    query_qm = _build_mdns_ptr_query(svc_low, unicast_response=False)
    query_qu = _build_mdns_ptr_query(svc_low, unicast_response=True)
    lan_ip = mdns_advertiser.get_lan_ip()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    multicast_joined = False
    try:
        try:
            sock.bind(("", MDNS_PORT))
            mreq = socket.inet_aton(MDNS_ADDR) + socket.inet_aton(lan_ip)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            multicast_joined = True
        except OSError:
            pass
        try:
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(lan_ip))
        except OSError:
            pass
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        sock.settimeout(0.25)

        srvs: dict[str, tuple[str, int]] = {}
        txts: dict[str, dict[str, str]] = {}
        addrs: dict[str, str] = {}
        instances: set[str] = set()

        sock.sendto(query_qm if multicast_joined else query_qu, (MDNS_ADDR, MDNS_PORT))
        deadline = time.monotonic() + max(0.3, min(5.0, timeout_s))
        second_sent = False
        while time.monotonic() < deadline:
            if not second_sent and deadline - time.monotonic() < timeout_s * 0.5:
                try:
                    sock.sendto(query_qu, (MDNS_ADDR, MDNS_PORT))
                    if multicast_joined:
                        sock.sendto(query_qm, (MDNS_ADDR, MDNS_PORT))
                except OSError:
                    pass
                second_sent = True
            try:
                pkt, sender = sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break

            parsed = _parse_dns_packet(pkt)
            if not parsed:
                continue
            for name_low, tgt in parsed["ptrs"]:
                if name_low == svc_low:
                    instances.add(tgt.lower())
            srvs.update(parsed["srvs"])
            txts.update(parsed["txts"])
            addrs.update(parsed["addrs"])
            # Fallback if TXT/SRV arrived referencing sender IP directly
            if parsed["txts"] and sender and sender[0]:
                for inst_k, txt_d in parsed["txts"].items():
                    if svc_low in inst_k and inst_k in srvs:
                        host_k = srvs[inst_k][0]
                        addrs.setdefault(host_k, sender[0])
    finally:
        sock.close()

    results: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    for inst in instances | set(srvs.keys()):
        if svc_low not in inst:
            continue
        srv = srvs.get(inst)
        txt = txts.get(inst, {})
        host = srv[0] if srv else ""
        port = srv[1] if srv else int(txt.get("port", 80) or 80)
        ip = addrs.get(host, "")
        if not ip:
            continue
        dev_id = txt.get("id") or inst.split(".", 1)[0]
        dedup = f"{dev_id}@{ip}:{port}"
        if dedup in seen_ids:
            continue
        seen_ids.add(dedup)
        results.append({
            "device_id": dev_id,
            "mdns_host": txt.get("mdns") or (host[:-6] if host.endswith(".local") else host),
            "ip": ip,
            "port": port,
            "chip": txt.get("slug", ""),
            "fw_version": txt.get("fw", ""),
            "auth_required": txt.get("auth") == "1",
            "auth_strict": txt.get("strict") == "1",
            "caps_hint": [c for c in (txt.get("caps", "")).split(",") if c],
            "via": ["mdns"],
        })
    return results


def probe_esp_device(ip: str, port: int = 80, timeout_s: float = 1.2) -> dict[str, Any] | None:
    """Query `GET http://<ip>:<port>/api/system/capabilities` to detect a unified ESP node."""
    url = f"http://{ip}:{int(port)}/api/system/capabilities"
    req = urllib.request.Request(url, method="GET")
    try:
        with OPENER.open(req, timeout=timeout_s) as resp:
            if resp.status != 200:
                return None
            raw = resp.read().decode("utf-8", "replace")
            data = json.loads(raw)
    except Exception:
        return None

    if not isinstance(data, dict) or not data.get("ok") or not data.get("device_id"):
        return None

    caps = data.get("capabilities") or {}
    if not isinstance(caps, dict):
        caps = {}
    return {
        "device_id": str(data.get("device_id")),
        "mdns_host": str(data.get("mdns_host", "")),
        "ip": str(data.get("ip") or ip),
        "port": int(data.get("web_port") or port),
        "chip": str(data.get("chip", "")),
        "board": str(data.get("board", "")),
        "board_slug": str(data.get("board_slug", "")),
        "fw_version": str(data.get("fw_version", "")),
        "mac": str(data.get("mac", "")),
        "flash_mb": int(data.get("flash_mb", 4) or 4),
        "auth_required": bool(data.get("auth_required", False)),
        "auth_strict": bool(data.get("auth_strict", False)),
        "hub_url": str(data.get("hub_url", "")),
        "capabilities": caps,
        "via": ["http"],
    }


def _expand_subnet_ips(subnet: str = "") -> list[str]:
    cidr = subnet.strip() if subnet else ""
    if not cidr:
        lan_ip = mdns_advertiser.get_lan_ip()
        if not lan_ip or lan_ip.startswith("127."):
            return []
        parts = lan_ip.split(".")
        if len(parts) != 4:
            return []
        cidr = f"{parts[0]}.{parts[1]}.{parts[2]}.0/24"
    net = ipaddress.ip_network(cidr, strict=False)
    hosts = [str(ip) for ip in net.hosts()]
    return hosts[:512]


def scan_lan_devices(
    *,
    subnet: str = "",
    ports: Iterable[int] = (80,),
    use_mdns: bool = True,
    use_http_scan: bool = True,
    timeout_s: float = 1.1,
) -> list[dict[str, Any]]:
    """Discover ESP Unified devices on the LAN via mDNS + concurrent HTTP probing."""
    by_id: dict[str, dict[str, Any]] = {}
    clean_ports = [int(p) for p in ports if 1 <= int(p) <= 65535] or [80]

    if use_mdns:
        try:
            for item in discover_mdns_unified(timeout_s=max(0.8, timeout_s)):
                did = item["device_id"]
                enriched = probe_esp_device(item["ip"], item["port"], timeout_s=timeout_s)
                if enriched:
                    enriched["via"] = sorted(set(item.get("via", []) + enriched.get("via", [])))
                    by_id[did] = enriched
                else:
                    by_id[did] = item
        except Exception:
            pass

    if use_http_scan:
        ips = _expand_subnet_ips(subnet)
        known_endpoints = {(v["ip"], int(v.get("port", 80))) for v in by_id.values()}
        targets = [(ip, p) for ip in ips for p in clean_ports if (ip, p) not in known_endpoints]
        if targets:
            with ThreadPoolExecutor(max_workers=min(48, len(targets))) as pool:
                futs = {pool.submit(probe_esp_device, ip, p, timeout_s): (ip, p) for ip, p in targets}
                for fut in as_completed(futs):
                    try:
                        res = fut.result()
                    except Exception:
                        res = None
                    if not res:
                        continue
                    did = res["device_id"]
                    if did in by_id:
                        via = sorted(set(by_id[did].get("via", []) + res.get("via", [])))
                        by_id[did].update(res)
                        by_id[did]["via"] = via
                    else:
                        by_id[did] = res

    return sorted(by_id.values(), key=lambda x: x.get("device_id", ""))


def _direct_json(
    ip: str,
    port: int,
    path: str,
    method: str = "GET",
    payload: dict | None = None,
    key: str = "",
    timeout_s: float = 4.0,
) -> tuple[int, dict[str, Any]]:
    url = f"http://{ip}:{int(port)}{path}"
    headers: dict[str, str] = {}
    data: bytes | None = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if key:
        headers["X-API-Key"] = key
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with OPENER.open(req, timeout=timeout_s) as resp:
            raw = resp.read().decode("utf-8", "replace")
            try:
                return resp.status, json.loads(raw)
            except Exception:
                return resp.status, {"ok": True}
    except urllib.error.HTTPError as he:
        raw = he.read().decode("utf-8", "replace") if he.fp else str(he)
        try:
            return he.code, json.loads(raw)
        except Exception:
            return he.code, {"ok": False, "error": raw[:200]}


def verify_device_key(ip: str, port: int = 80, key: str = "", timeout_s: float = 3.0) -> tuple[bool, int, dict[str, Any]]:
    """Verify key against `/api/auth/verify`, falling back cleanly on older firmware."""
    st, body = _direct_json(ip, port, "/api/auth/verify", method="POST", payload={"key": key}, key=key, timeout_s=timeout_s)
    if st == 200 and body.get("ok"):
        return True, st, body
    if st in (404, 405):
        # Legacy fallback: `/api/system/config` POST `{}` triggers web_server_check_auth
        st2, body2 = _direct_json(ip, port, "/api/system/config", method="POST", payload={}, key=key, timeout_s=timeout_s)
        if st2 == 200:
            return True, 200, {"ok": True, "authenticated": True, "legacy": True}
        return False, st2, body2
    return False, st, body


def adopt_device_via_http(
    ip: str,
    port: int = 80,
    *,
    api_token: str = "",
    config_patch: dict[str, Any] | None = None,
    timeout_s: float = 4.0,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Probe a LAN device, verify its access key, and optionally apply WS/Hub config."""
    caps = probe_esp_device(ip, port, timeout_s=timeout_s)
    if not caps:
        raise LookupError(f"无法连接设备 http://{ip}:{port}/api/system/capabilities")

    ok_auth, st, auth_body = verify_device_key(ip, port, key=api_token, timeout_s=timeout_s)
    if not ok_auth:
        msg = auth_body.get("message") or auth_body.get("error") or f"HTTP {st}"
        raise PermissionError(f"密钥校验失败: {msg}")

    cfg_result: dict[str, Any] = {}
    if config_patch:
        st_cfg, res_cfg = _direct_json(
            ip,
            port,
            "/api/system/config",
            method="POST",
            payload=config_patch,
            key=api_token,
            timeout_s=timeout_s,
        )
        if st_cfg == 401:
            raise PermissionError("推送配置时密钥被拒绝 (401)")
        if st_cfg >= 400:
            raise RuntimeError(f"下发配置失败 (HTTP {st_cfg}): {res_cfg}")
        cfg_result = res_cfg
        # Re-read capabilities/config snapshot after applying
        refreshed = probe_esp_device(ip, port, timeout_s=timeout_s)
        if refreshed:
            caps.update(refreshed)

    return caps, cfg_result
