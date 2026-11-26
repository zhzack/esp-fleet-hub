"""V2 Home Assistant / MQTT Auto-Discovery configuration, WOL & per-device exposure endpoints."""

from __future__ import annotations

import asyncio
import re
from typing import Any, Literal

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field
from sqlalchemy.orm import Session

from app.core.config.config import settings
from app.core.security import ensure_device_access, get_current_admin, get_current_user
from app.database.models.device import Device
from app.database.models.ha_integration import HaDeviceTarget
from app.database.models.user import User
from app.database.session import get_db
from app.services import unified_bridge as ub
from app.services.device_executor import execute_device_command
from app.services.device_registry import device_registry
from app.services.ha_mqtt_bridge import (
    build_device_discovery_specs,
    extract_device_caps_set,
    generate_xiaoai_automations_yaml,
    ha_bridge,
    load_broker_config,
    save_broker_config,
    send_wol,
    serialize_target,
    test_mqtt_connection,
    upsert_device_target,
)

router = APIRouter()

_MAC_HEX_RE = re.compile(r"^[0-9A-Fa-f]{12}$")


class HaBrokerConfigIn(BaseModel):
    enabled: bool | None = None
    broker_host: str | None = None
    broker_port: int | None = None
    mqtt_user: str | None = None
    mqtt_pass: str | None = None
    discovery_prefix: str | None = None
    topic_prefix: str | None = None
    sync_interval_sec: int | None = None


class HaTestIn(BaseModel):
    broker_host: str
    broker_port: int = 1883
    mqtt_user: str = ""
    mqtt_pass: str = ""


class ExposedPinIn(BaseModel):
    pin: int
    name: str = ""
    pwm: bool = False
    kind: Literal["switch", "light"] | None = None


class HaDeviceTargetIn(BaseModel):
    enabled: bool | None = None
    display_name: str | None = None
    area_name: str | None = None
    expose_hid: bool | None = None
    expose_gpio: bool | None = None
    expose_display: bool | None = None
    expose_camera: bool | None = None
    pc_enabled: bool | None = None
    pc_name: str | None = None
    pc_ip: str | None = None
    pc_mac: str | None = None
    pc_broadcast_ip: str | None = None
    pc_power_pin: int | None = None
    pc_power_pulse_ms: int | None = None
    pc_os: Literal["windows", "mac", "linux"] | None = None
    legacy_topic_prefix: str | None = None
    exposed_pins: list[ExposedPinIn] | None = None


class WolIn(BaseModel):
    mac: str | None = None
    broadcast_ip: str | None = None
    wake_key: bool = False


@router.get("/status")
def ha_status(
    db: Session = Depends(get_db),
    _user: User = Depends(get_current_user),
):
    return {
        "ok": True,
        "bridge": ha_bridge.get_status(),
        "config": load_broker_config(db, include_secret=False),
    }


@router.get("/config")
def ha_get_config(
    db: Session = Depends(get_db),
    _admin: User = Depends(get_current_admin),
):
    return {"ok": True, "config": load_broker_config(db, include_secret=False)}


@router.put("/config")
def ha_put_config(
    body: HaBrokerConfigIn,
    db: Session = Depends(get_db),
    _admin: User = Depends(get_current_admin),
):
    if body.broker_port is not None and not (1 <= body.broker_port <= 65535):
        raise HTTPException(400, "broker_port must be between 1 and 65535")
    if body.sync_interval_sec is not None and not (3 <= body.sync_interval_sec <= 300):
        raise HTTPException(400, "sync_interval_sec must be between 3 and 300")
    cfg = save_broker_config(db, body.model_dump(exclude_none=True))
    ha_bridge.request_reload()
    return {"ok": True, "config": cfg, "bridge": ha_bridge.get_status()}


@router.post("/test")
async def ha_test_broker(
    body: HaTestIn,
    db: Session = Depends(get_db),
    _admin: User = Depends(get_current_admin),
):
    if not (1 <= body.broker_port <= 65535):
        raise HTTPException(400, "broker_port must be between 1 and 65535")
    password = body.mqtt_pass
    if not password:
        saved = load_broker_config(db, include_secret=True)
        if saved.get("broker_host") == body.broker_host.strip():
            password = saved.get("mqtt_pass", "")
    ok, msg = await asyncio.to_thread(
        test_mqtt_connection,
        body.broker_host.strip(),
        body.broker_port,
        body.mqtt_user.strip(),
        password,
    )
    return {"ok": ok, "message": msg}


@router.post("/sync")
async def ha_force_sync(
    db: Session = Depends(get_db),
    _admin: User = Depends(get_current_admin),
):
    stats = await ha_bridge.sync_all_devices(db)
    return {"ok": True, **stats, "bridge": ha_bridge.get_status()}


@router.get("/devices")
def ha_list_devices(
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    ub.hub_db.ensure_init()
    cfg = load_broker_config(db)
    unified_map = {d["device_id"]: d for d in ub.hub_db.list_devices()}
    main_rows = {d.device_uuid: d for d in db.query(Device).all()}
    target_rows = {t.device_uuid: t for t in db.query(HaDeviceTarget).all()}
    all_ids = sorted(set(unified_map.keys()) | set(main_rows.keys()))

    items = []
    for did in all_ids:
        u_dev = unified_map.get(did) or {}
        m_dev = main_rows.get(did)
        if user.role != "admin":
            if m_dev is not None:
                if m_dev.owner_user_id not in (None, user.id):
                    continue
                if m_dev.owner_user_id is None and not settings.ALLOW_UNCLAIMED_DEVICE_CONTROL:
                    continue
            elif u_dev.get("claimed") or not settings.ALLOW_UNCLAIMED_DEVICE_CONTROL:
                continue

        tgt = serialize_target(target_rows.get(did), did)
        caps = extract_device_caps_set(
            getattr(m_dev, "capabilities", None) if m_dev else None,
            u_dev.get("capabilities"),
        )
        specs = build_device_discovery_specs(
            device_uuid=did,
            device_name=(u_dev.get("name") or (m_dev.name if m_dev else "") or did),
            chip=u_dev.get("chip", ""),
            fw_version=u_dev.get("fw_version", ""),
            caps=caps,
            target=tgt,
            topic_prefix=cfg["topic_prefix"],
            discovery_prefix=cfg["discovery_prefix"],
        )
        online = bool(
            device_registry.is_online(did)
            or u_dev.get("status") == "online"
            or (m_dev and m_dev.status == "online")
        )
        items.append({
            "device_uuid": did,
            "name": u_dev.get("name") or (m_dev.name if m_dev else "") or did,
            "chip": u_dev.get("chip", ""),
            "ip": u_dev.get("ip", ""),
            "online": online,
            "capabilities": sorted(caps),
            "entity_count": len(specs),
            "target": tgt,
        })
    return {"ok": True, "devices": items}


@router.put("/devices/{device_uuid}")
def ha_update_device_target(
    device_uuid: str,
    body: HaDeviceTargetIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    ensure_device_access(db, user, device_uuid)
    if body.pc_power_pin is not None and not (0 <= body.pc_power_pin <= 48):
        raise HTTPException(400, "pc_power_pin must be between 0 and 48")
    if body.pc_mac is not None and body.pc_mac.strip():
        clean = body.pc_mac.replace(":", "").replace("-", "").replace(".", "").strip()
        if not _MAC_HEX_RE.match(clean):
            raise HTTPException(400, "pc_mac must be a valid 12-hex MAC address")
    if body.exposed_pins is not None:
        for ep in body.exposed_pins:
            if not (0 <= ep.pin <= 48):
                raise HTTPException(400, f"exposed_pins pin {ep.pin} out of range (0..48)")

    payload = body.model_dump(exclude_unset=True)
    if "exposed_pins" in payload and payload["exposed_pins"] is not None:
        payload["exposed_pins"] = [ep.model_dump(exclude_none=True) for ep in body.exposed_pins]
    tgt = upsert_device_target(db, device_uuid, payload)
    ha_bridge.request_sync()
    return {"ok": True, "target": tgt}


@router.post("/devices/{device_uuid}/wol")
async def ha_trigger_wol(
    device_uuid: str,
    body: WolIn | None = None,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    ensure_device_access(db, user, device_uuid)
    row = db.query(HaDeviceTarget).filter(HaDeviceTarget.device_uuid == device_uuid).first()
    tgt = serialize_target(row, device_uuid)
    mac = (body.mac if body and body.mac else tgt.get("pc_mac", "")).strip()
    bcast = (body.broadcast_ip if body and body.broadcast_ip else tgt.get("pc_broadcast_ip", "255.255.255.255")).strip()
    if not mac or not send_wol(mac, bcast):
        raise HTTPException(400, "缺少或无效的 PC 网卡 MAC 地址，请先在设备 HA 映射中填写 pc_mac")

    hid_res = None
    if body and body.wake_key:
        hid_res = await execute_device_command(
            db,
            user_id=user.id,
            device_uuid=device_uuid,
            cmd_type="action",
            payload={"type": "keyboard_tap", "key": "KEY_SPACE", "need_ack": True},
            timeout_ms=3000,
        )
    return {"ok": True, "mac": mac, "broadcast_ip": bcast, "hid_wake": hid_res}


@router.get("/devices/{device_uuid}/automations")
def ha_device_automations_yaml(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    ensure_device_access(db, user, device_uuid)
    cfg = load_broker_config(db)
    row = db.query(HaDeviceTarget).filter(HaDeviceTarget.device_uuid == device_uuid).first()
    tgt = serialize_target(row, device_uuid)
    yml = generate_xiaoai_automations_yaml(device_uuid, tgt, topic_prefix=cfg["topic_prefix"])
    return {"ok": True, "device_uuid": device_uuid, "yaml": yml}
