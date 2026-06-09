"""Dispatcher: queue envelopes to devices, wait for ACKs, and write action_logs."""

import asyncio
import time
import uuid
from typing import Any

from sqlalchemy.orm import Session

from app.core.logger.logger import logger
from app.database.crud import action_log as crud_action_log
from app.database.crud import device as crud_device
from app.services.ack_tracker import ack_tracker
from app.services.device_registry import device_registry
from app.services.queue import queue_backend

_RETRY_DELAYS_MS = [100, 300, 900]


def _extract_ack_error(ack: dict[str, Any]) -> tuple[str, str]:
    err = ack.get("error")
    if isinstance(err, dict):
        code = err.get("code") or err.get("err_code") or "E000"
        message = err.get("message") or err.get("err_msg") or "failed"
        return str(code), str(message)
    code = ack.get("err_code") or ack.get("error_code") or "E000"
    message = ack.get("err_msg") or ack.get("error_message") or "failed"
    return str(code), str(message)


def _parse_ack_payload(ack: Any) -> tuple[bool, int, str | None, str | None]:
    if not isinstance(ack, dict):
        return False, 0, "E000", "invalid ack payload"

    exec_ms = int(ack.get("exec_time_ms", 0) or 0)
    status = ack.get("status")
    if isinstance(status, str):
        normalized = status.lower()
        if normalized in {"ok", "acked", "success"}:
            return True, exec_ms, None, None
        if normalized in {"failed", "error", "timeout"}:
            code, message = _extract_ack_error(ack)
            return False, exec_ms, code, message

    if "ok" in ack:
        if bool(ack.get("ok")):
            return True, exec_ms, None, None
        code, message = _extract_ack_error(ack)
        return False, exec_ms, code, message

    if any(
        key in ack
        for key in ("error", "err_code", "err_msg", "error_code", "error_message")
    ):
        code, message = _extract_ack_error(ack)
        return False, exec_ms, code, message

    return True, exec_ms, None, None


def build_envelope(
    *,
    msg_id: str,
    user_id: int | None,
    device_uuid: str,
    payload: dict,
    seq: int = 0,
    priority: int = 0,
    source: str = "web",
    msg_type: str = "action",
) -> dict:
    return {
        "header": {
            "msg_id": msg_id,
            "seq": seq,
            "timestamp": int(time.time()),
            "source": source,
            "user_id": str(user_id) if user_id else None,
            "device_id": device_uuid,
            "type": msg_type,
            "priority": priority,
        },
        "routing": {
            "target_devices": [device_uuid],
            "broadcast": False,
            "group": None,
        },
        "payload": payload,
    }


async def deliver_to_device(
    db: Session,
    *,
    device_uuid: str,
    payload: dict,
    user_id: int | None,
    timeout_ms: int = 2000,
    priority: int = 0,
) -> dict:
    msg_id = str(uuid.uuid4())
    action_type = payload.get("type", "unknown")
    dev_row = crud_device.get_by_uuid(db, device_uuid)
    device_db_id = dev_row.id if dev_row else None

    crud_action_log.create_pending(
        db,
        msg_id=msg_id,
        user_id=user_id,
        device_id=device_db_id,
        action_type=action_type,
        payload=payload,
    )

    if not device_registry.is_online(device_uuid):
        crud_action_log.mark_failed(db, msg_id, code="E002", message="device offline")
        return {
            "msg_id": msg_id,
            "device_id": device_uuid,
            "status": "failed",
            "error_code": "E002",
            "error_message": "device offline",
        }

    envelope = build_envelope(
        msg_id=msg_id,
        user_id=user_id,
        device_uuid=device_uuid,
        payload=payload,
        priority=priority,
    )

    last_error: tuple[str, str] | None = None
    for attempt, _ in enumerate(_RETRY_DELAYS_MS):
        ws = device_registry.get(device_uuid)
        if ws is None:
            last_error = ("E002", "device offline")
            break
        try:
            await queue_backend.push(device_uuid, envelope)
            crud_action_log.mark_sent(db, msg_id)
        except Exception as e:
            logger.exception(f"[dispatcher] push failed: {e}")
            last_error = ("E003", "queue push error")
            continue

        try:
            ack = await ack_tracker.expect(msg_id, timeout=timeout_ms / 1000)
            ack_ok, exec_ms, err_code, err_message = _parse_ack_payload(ack)
            if ack_ok:
                crud_action_log.mark_acked(db, msg_id, exec_ms)
                return {
                    "msg_id": msg_id,
                    "device_id": device_uuid,
                    "status": "acked",
                    "exec_time_ms": exec_ms,
                }
            last_error = (err_code or "E000", err_message or "failed")
            logger.warning(f"[dispatcher] device returned non-ok: {last_error}")
        except asyncio.TimeoutError:
            last_error = ("E006", f"ack timeout (attempt {attempt + 1})")
            logger.warning(f"[dispatcher] {last_error[1]} msg_id={msg_id}")
            await asyncio.sleep(_RETRY_DELAYS_MS[attempt] / 1000)

    code, message = last_error or ("E006", "retry exhausted")
    crud_action_log.mark_failed(db, msg_id, code=code, message=message)
    return {
        "msg_id": msg_id,
        "device_id": device_uuid,
        "status": "timeout" if code == "E006" else "failed",
        "error_code": code,
        "error_message": message,
    }


async def deliver_command(
    db: Session,
    *,
    device_uuid: str,
    cmd_type: str,
    payload: dict,
    user_id: int | None,
    timeout_ms: int = 3000,
    priority: int = 0,
) -> dict:
    msg_id = str(uuid.uuid4())
    dev_row = crud_device.get_by_uuid(db, device_uuid)
    device_db_id = dev_row.id if dev_row else None

    crud_action_log.create_pending(
        db,
        msg_id=msg_id,
        user_id=user_id,
        device_id=device_db_id,
        action_type=cmd_type,
        payload=payload,
    )

    if not device_registry.is_online(device_uuid):
        crud_action_log.mark_failed(db, msg_id, code="E002", message="device offline")
        return {
            "msg_id": msg_id,
            "device_id": device_uuid,
            "status": "failed",
            "error_code": "E002",
            "error_message": "device offline",
            "result": None,
        }

    envelope = build_envelope(
        msg_id=msg_id,
        user_id=user_id,
        device_uuid=device_uuid,
        payload=payload,
        priority=priority,
        msg_type=cmd_type,
    )

    try:
        await queue_backend.push(device_uuid, envelope)
        crud_action_log.mark_sent(db, msg_id)
    except Exception as e:
        logger.exception(f"[dispatcher] push failed: {e}")
        crud_action_log.mark_failed(db, msg_id, code="E003", message="queue push error")
        return {
            "msg_id": msg_id,
            "device_id": device_uuid,
            "status": "failed",
            "error_code": "E003",
            "error_message": "queue push error",
            "result": None,
        }

    try:
        ack = await ack_tracker.expect(msg_id, timeout=timeout_ms / 1000)
    except asyncio.TimeoutError:
        crud_action_log.mark_failed(db, msg_id, code="E006", message="ack timeout")
        return {
            "msg_id": msg_id,
            "device_id": device_uuid,
            "status": "timeout",
            "error_code": "E006",
            "error_message": "ack timeout",
            "result": None,
        }

    ack_ok, exec_ms, code, message = _parse_ack_payload(ack)
    if ack_ok:
        crud_action_log.mark_acked(db, msg_id, exec_ms)
        return {
            "msg_id": msg_id,
            "device_id": device_uuid,
            "status": "acked",
            "exec_time_ms": exec_ms,
            "result": ack,
        }

    crud_action_log.mark_failed(
        db,
        msg_id,
        code=code or "E000",
        message=message or "failed",
    )
    return {
        "msg_id": msg_id,
        "device_id": device_uuid,
        "status": "failed",
        "error_code": code or "E000",
        "error_message": message or "failed",
        "result": ack,
    }
