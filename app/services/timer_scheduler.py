"""Persistent timer schedule computation, action execution, and asyncio scheduler loop."""

from __future__ import annotations

import asyncio
from datetime import datetime, timedelta
import re
import time
from typing import Any

from sqlalchemy.orm import Session

from app.core.logger.logger import logger
from app.database.crud import device as crud_device
from app.database.crud import macro as crud_macro
from app.database.models.device_timer import DeviceTimer
from app.database.session import SessionLocal
from app.services import dispatcher
from app.services.capabilities import DISPLAY_CAPS, GPIO_CAPS, HID_CAPS, normalize_caps
from app.services.device_executor import execute_device_command, execute_gpio_control

_HHMM_RE = re.compile(r"^([01]\d|2[0-3]):([0-5]\d)$")
_SCHEDULER_TASK: asyncio.Task | None = None

ACTION_REQUIRED_CAPS: dict[str, tuple[set[str], str]] = {
    "gpio_set": (GPIO_CAPS, "GPIO 控制"),
    "display_config": (DISPLAY_CAPS, "屏幕显示"),
    "hid_transport": (HID_CAPS, "HID 传输"),
    "macro": (HID_CAPS, "HID 宏控制"),
}


def parse_hhmm(time_hhmm: str) -> tuple[int, int]:
    m = _HHMM_RE.match(str(time_hhmm or "").strip())
    if not m:
        raise ValueError("time_hhmm must match HH:MM (00:00..23:59)")
    return int(m.group(1)), int(m.group(2))


def compute_next_run(
    schedule_type: str,
    *,
    now_ts: int | None = None,
    delay_sec: int = 0,
    interval_sec: int = 0,
    run_at: int | None = None,
    time_hhmm: str = "",
    weekdays: list[int] | None = None,
) -> int:
    """Compute the next UTC epoch timestamp (in seconds) for a timer rule."""
    cur_ts = int(now_ts if now_ts is not None else time.time())
    stype = str(schedule_type or "").strip().lower()

    if stype == "countdown":
        d = int(delay_sec or 0)
        if d < 1 or d > 86400 * 30:
            raise ValueError("delay_sec must be between 1 and 2592000")
        return cur_ts + d

    if stype == "interval":
        iv = int(interval_sec or 0)
        if iv < 5 or iv > 86400 * 30:
            raise ValueError("interval_sec must be between 5 and 2592000")
        return cur_ts + iv

    if stype == "once":
        if run_at is None:
            raise ValueError("run_at epoch timestamp is required for once schedule")
        ts = int(run_at)
        if ts < 1:
            raise ValueError("run_at must be a positive epoch timestamp")
        return ts

    if stype == "daily":
        hh, mm = parse_hhmm(time_hhmm)
        valid_days = {int(d) for d in (weekdays or []) if 0 <= int(d) <= 6}
        now_dt = datetime.fromtimestamp(cur_ts)
        for day_offset in range(0, 8):
            cand = (now_dt + timedelta(days=day_offset)).replace(
                hour=hh, minute=mm, second=0, microsecond=0
            )
            cand_ts = int(cand.timestamp())
            if cand_ts <= cur_ts:
                continue
            if valid_days and cand.weekday() not in valid_days:
                continue
            return cand_ts
        raise ValueError("no matching weekday found for daily schedule")

    raise ValueError(f"unsupported schedule_type: {schedule_type}")


async def execute_timer_action(db: Session, timer: DeviceTimer) -> tuple[bool, str]:
    """Execute one DeviceTimer action against its target device."""
    dev = crud_device.get_by_uuid(db, timer.device_uuid)
    atype = str(timer.action_type or "").strip()
    pl: dict[str, Any] = dict(timer.action_payload or {})

    if dev and atype in ACTION_REQUIRED_CAPS:
        req_caps, label = ACTION_REQUIRED_CAPS[atype]
        caps = normalize_caps(dev.capabilities)
        if caps and not (caps & req_caps):
            return False, f"设备未启用 {label} 能力"

    if atype == "gpio_set":
        if "pin" not in pl:
            return False, "缺少 pin 参数"
        res = await execute_gpio_control(
            db,
            user_id=timer.user_id,
            device_uuid=timer.device_uuid,
            pin=int(pl["pin"]),
            val=pl.get("val"),
            duty=pl.get("duty"),
            op=pl.get("op"),
            duration_sec=int(pl.get("duration_sec") or 0),
        )
    elif atype == "display_config":
        res = await execute_device_command(
            db,
            user_id=timer.user_id,
            device_uuid=timer.device_uuid,
            cmd_type="display.config",
            payload=pl,
            timeout_ms=4000,
        )
    elif atype == "hid_transport":
        mode = str(pl.get("mode", "auto"))
        if mode not in ("ble", "usb", "auto"):
            return False, "mode 必须为 ble/usb/auto"
        res = await execute_device_command(
            db,
            user_id=timer.user_id,
            device_uuid=timer.device_uuid,
            cmd_type="transport.set",
            payload={"mode": mode},
            timeout_ms=4000,
        )
    elif atype == "macro":
        macro_id = pl.get("macro_id")
        if not macro_id:
            return False, "缺少 macro_id"
        macro = crud_macro.get(db, int(macro_id))
        if not macro:
            return False, f"宏 #{macro_id} 不存在"
        res = await dispatcher.deliver_to_device(
            db,
            device_uuid=timer.device_uuid,
            payload={"type": "macro", "steps": macro.steps or [], "need_ack": True},
            user_id=timer.user_id,
            timeout_ms=8000,
        )
    elif atype == "reboot":
        res = await execute_device_command(
            db,
            user_id=timer.user_id,
            device_uuid=timer.device_uuid,
            cmd_type="reboot",
            payload={},
            timeout_ms=2500,
        )
    else:
        return False, f"不支持的动作类型: {atype}"

    if res.get("status") == "acked":
        extra = f" ({res['pulse_restore_sec']}s后恢复)" if res.get("pulse_restore_sec") else ""
        return True, f"acked ({res.get('via', 'ws')}){extra}"
    err_code = res.get("error_code") or res.get("status") or "E000"
    err_msg = res.get("error_message") or "failed"
    return False, f"{err_code}: {err_msg}"


async def run_due_timers(db: Session, *, now_ts: int | None = None) -> int:
    """Advance schedules and fire all enabled timers whose next_run_at <= now_ts."""
    cur_ts = int(now_ts if now_ts is not None else time.time())
    due = (
        db.query(DeviceTimer)
        .filter(
            DeviceTimer.enabled.is_(True),
            DeviceTimer.next_run_at.isnot(None),
            DeviceTimer.next_run_at <= cur_ts,
        )
        .order_by(DeviceTimer.next_run_at.asc(), DeviceTimer.id.asc())
        .all()
    )
    if not due:
        return 0

    executed = 0
    for timer in due:
        # Advance/disable BEFORE awaiting I/O so slow timeouts cannot re-fire on next 1s tick.
        if timer.schedule_type in ("countdown", "once"):
            timer.enabled = False
            timer.next_run_at = None
        elif timer.schedule_type == "interval":
            timer.next_run_at = cur_ts + max(5, int(timer.interval_sec or 5))
        elif timer.schedule_type == "daily":
            timer.next_run_at = compute_next_run(
                "daily",
                now_ts=cur_ts + 1,
                time_hhmm=timer.time_hhmm,
                weekdays=timer.weekdays,
            )
        timer.last_run_at = cur_ts
        timer.run_count = int(timer.run_count or 0) + 1
        db.commit()

        try:
            ok, msg = await execute_timer_action(db, timer)
        except Exception as exc:
            ok, msg = False, str(exc)

        timer.last_status = "ok" if ok else "failed"
        timer.last_message = str(msg or "")[:250]
        db.commit()
        executed += 1
        logger.info(
            f"[timer] #{timer.id} '{timer.name}' -> {timer.device_uuid} "
            f"{timer.action_type} ok={ok} msg={timer.last_message}"
        )
    return executed


async def _scheduler_loop() -> None:
    logger.info("⏰ Device timer scheduler started (1s tick)")
    while True:
        try:
            await asyncio.sleep(1.0)
            with SessionLocal() as db:
                await run_due_timers(db)
        except asyncio.CancelledError:
            logger.info("⏰ Device timer scheduler stopped")
            return
        except Exception as exc:
            logger.warning(f"[timer] scheduler tick error: {exc}")


def start_timer_scheduler() -> None:
    global _SCHEDULER_TASK
    if _SCHEDULER_TASK is not None and not _SCHEDULER_TASK.done():
        return
    _SCHEDULER_TASK = asyncio.create_task(_scheduler_loop())


def stop_timer_scheduler() -> None:
    global _SCHEDULER_TASK
    if _SCHEDULER_TASK is not None and not _SCHEDULER_TASK.done():
        _SCHEDULER_TASK.cancel()
    _SCHEDULER_TASK = None
