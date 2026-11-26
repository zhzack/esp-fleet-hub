"""Hybrid WS + HTTP device command execution and GPIO pulse helpers."""

from __future__ import annotations

import asyncio
import json
import time
from typing import Any

from sqlalchemy.orm import Session
from starlette.concurrency import run_in_threadpool

from app.core.logger.logger import logger
from app.database.session import SessionLocal
from app.services import dispatcher
from app.services.device_registry import device_registry
from app.services import unified_bridge as ub

_HTTP_GET_ROUTES: dict[str, str] = {
    "get_status": "/api/system/status",
    "gpio.pins": "/api/gpio/pins",
    "display.status": "/api/display/status",
}

_HTTP_POST_ROUTES: dict[str, str] = {
    "gpio.set": "/api/gpio/set",
    "display.config": "/api/display/config",
    "display.text": "/api/display/text",
    "display.image": "/api/display/image",
    "reboot": "/api/system/reboot",
}

_PULSE_TASKS: dict[tuple[str, int], asyncio.Task] = {}


def _is_esp8266(hub_dev: dict | None) -> bool:
    return bool(hub_dev and "8266" in str(hub_dev.get("chip", "")).lower())


def _normalize_http_gpio_result(cmd_type: str, data: dict, hub_dev: dict | None) -> dict:
    if not _is_esp8266(hub_dev):
        return data
    if cmd_type == "gpio.pins" and isinstance(data.get("pins"), list):
        for p in data["pins"]:
            if isinstance(p, dict) and p.get("duty") is not None:
                p["duty"] = max(0, min(100, round(int(p["duty"]) * 100 / 255)))
    elif cmd_type == "gpio.set" and data.get("duty") is not None:
        data["duty"] = max(0, min(100, round(int(data["duty"]) * 100 / 255)))
    return data


async def execute_device_command(
    db: Session,
    *,
    user_id: int | None,
    device_uuid: str,
    cmd_type: str,
    payload: dict[str, Any] | None = None,
    timeout_ms: int = 4000,
) -> dict[str, Any]:
    """Dispatch command via WebSocket if online, falling back to direct HTTP on LAN unified nodes."""
    req_payload = dict(payload or {})
    if device_registry.is_online(device_uuid):
        return await dispatcher.deliver_command(
            db,
            device_uuid=device_uuid,
            cmd_type=cmd_type,
            payload=req_payload,
            user_id=user_id,
            timeout_ms=timeout_ms,
        )

    ub.hub_db.ensure_init()
    hub_dev = ub.hub_db.get_device(device_uuid)
    if hub_dev and hub_dev.get("ip") and hub_dev["ip"] != "0.0.0.0":
        method = None
        path = None
        body: dict[str, Any] | None = None
        if cmd_type in _HTTP_GET_ROUTES:
            method = "GET"
            path = _HTTP_GET_ROUTES[cmd_type]
        elif cmd_type in _HTTP_POST_ROUTES:
            method = "POST"
            path = _HTTP_POST_ROUTES[cmd_type]
            body = dict(req_payload)
            if cmd_type == "gpio.set" and _is_esp8266(hub_dev) and "duty" in body and body["duty"] is not None:
                pct = max(0, min(100, int(body["duty"])))
                body["duty"] = round(pct * 255 / 100)

        if method and path:
            t0 = time.monotonic()
            ok, st, res = await run_in_threadpool(
                ub.fleet_router._direct_device_http,
                hub_dev,
                path,
                method,
                body,
                max(2, round(timeout_ms / 1000)),
            )
            ms = int((time.monotonic() - t0) * 1000)
            if ok:
                raw, _ctype = res
                try:
                    parsed = json.loads(raw.decode("utf-8", "replace"))
                except Exception:
                    parsed = {"ok": True}
                if isinstance(parsed, dict):
                    parsed = _normalize_http_gpio_result(cmd_type, parsed, hub_dev)
                return {
                    "msg_id": "",
                    "device_id": device_uuid,
                    "status": "acked",
                    "via": "http",
                    "exec_time_ms": ms,
                    "result": parsed,
                }
            return {
                "msg_id": "",
                "device_id": device_uuid,
                "status": "failed",
                "via": "http",
                "error_code": f"E{st or 502}",
                "error_message": str(res),
                "result": None,
            }

    return {
        "msg_id": "",
        "device_id": device_uuid,
        "status": "failed",
        "error_code": "E002",
        "error_message": "device offline",
        "result": None,
    }


async def _delayed_gpio_restore(
    device_uuid: str,
    pin: int,
    user_id: int | None,
    delay_sec: int,
    *,
    restore_duty: bool,
) -> None:
    key = (device_uuid, pin)
    try:
        await asyncio.sleep(delay_sec)
        restore_payload = {"pin": pin, "duty": 0} if restore_duty else {"pin": pin, "val": 0}
        with SessionLocal() as db:
            await execute_device_command(
                db,
                user_id=user_id,
                device_uuid=device_uuid,
                cmd_type="gpio.set",
                payload=restore_payload,
                timeout_ms=4000,
            )
    except asyncio.CancelledError:
        return
    except Exception as e:
        logger.warning(f"[gpio-pulse] restore failed for {device_uuid} pin={pin}: {e}")
    finally:
        if _PULSE_TASKS.get(key) is asyncio.current_task():
            _PULSE_TASKS.pop(key, None)


def cancel_gpio_pulse(device_uuid: str, pin: int) -> None:
    task = _PULSE_TASKS.pop((device_uuid, int(pin)), None)
    if task and not task.done():
        task.cancel()


def cancel_all_pulses() -> None:
    for key, task in list(_PULSE_TASKS.items()):
        _PULSE_TASKS.pop(key, None)
        if not task.done():
            task.cancel()


async def execute_gpio_control(
    db: Session,
    *,
    user_id: int | None,
    device_uuid: str,
    pin: int,
    val: int | None = None,
    duty: int | None = None,
    op: str | None = None,
    duration_sec: int = 0,
) -> dict[str, Any]:
    """Set digital/PWM GPIO state with optional toggle resolution and auto-off pulse timer."""
    pin_i = int(pin)
    cancel_gpio_pulse(device_uuid, pin_i)

    cmd_payload: dict[str, Any] = {"pin": pin_i}
    resolved_op = (op or "").strip().lower()
    if resolved_op == "toggle" or (not resolved_op and val is None and duty is None):
        pins_res = await execute_device_command(
            db, user_id=user_id, device_uuid=device_uuid, cmd_type="gpio.pins", payload={}, timeout_ms=3000
        )
        cur_val = 0
        pins_list = (pins_res.get("result") or {}).get("pins") or []
        for row in pins_list:
            if isinstance(row, dict) and int(row.get("pin", -1)) == pin_i:
                cur_val = 1 if (int(row.get("val", 0) or 0) or int(row.get("duty", 0) or 0) > 0) else 0
                break
        cmd_payload["val"] = 0 if cur_val else 1
    elif resolved_op == "on":
        cmd_payload["val"] = 1
    elif resolved_op == "off":
        cmd_payload["val"] = 0
    elif resolved_op == "pwm" or duty is not None:
        cmd_payload["duty"] = max(0, min(100, int(duty or 0)))
    else:
        cmd_payload["val"] = 1 if int(val or 0) else 0

    res = await execute_device_command(
        db,
        user_id=user_id,
        device_uuid=device_uuid,
        cmd_type="gpio.set",
        payload=cmd_payload,
        timeout_ms=4000,
    )

    dur = max(0, int(duration_sec or 0))
    is_active_output = int(cmd_payload.get("val", 0) or 0) > 0 or int(cmd_payload.get("duty", 0) or 0) > 0
    if dur > 0 and is_active_output and res.get("status") == "acked":
        restore_duty = "duty" in cmd_payload
        _PULSE_TASKS[(device_uuid, pin_i)] = asyncio.create_task(
            _delayed_gpio_restore(device_uuid, pin_i, user_id, dur, restore_duty=restore_duty)
        )
        res["pulse_restore_sec"] = dur
    return res
