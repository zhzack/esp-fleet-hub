"""V2 device timer & scheduled automation CRUD / trigger endpoints."""

from __future__ import annotations

import time
from typing import Any, Literal

from fastapi import APIRouter, Depends, HTTPException, Query
from pydantic import BaseModel, Field
from sqlalchemy.orm import Session

from app.core.config.config import settings
from app.core.security import ensure_device_access, get_current_user
from app.database.crud import device as crud_device
from app.database.crud import macro as crud_macro
from app.database.models.device import Device
from app.database.models.device_timer import DeviceTimer
from app.database.models.user import User
from app.database.session import get_db
from app.services.capabilities import require_device_capability
from app.services.timer_scheduler import (
    ACTION_REQUIRED_CAPS,
    compute_next_run,
    execute_timer_action,
)

router = APIRouter()

ScheduleType = Literal["countdown", "once", "daily", "interval"]
ActionType = Literal["gpio_set", "display_config", "hid_transport", "macro", "reboot"]


class TimerCreateIn(BaseModel):
    device_uuid: str
    name: str = Field(..., min_length=1, max_length=128)
    enabled: bool = True
    schedule_type: ScheduleType
    delay_sec: int = 0
    interval_sec: int = 0
    run_at: int | None = None
    time_hhmm: str = ""
    weekdays: list[int] = Field(default_factory=list)
    action_type: ActionType
    action_payload: dict[str, Any] = Field(default_factory=dict)


class TimerUpdateIn(BaseModel):
    name: str | None = Field(default=None, min_length=1, max_length=128)
    enabled: bool | None = None
    schedule_type: ScheduleType | None = None
    delay_sec: int | None = None
    interval_sec: int | None = None
    run_at: int | None = None
    time_hhmm: str | None = None
    weekdays: list[int] | None = None
    action_type: ActionType | None = None
    action_payload: dict[str, Any] | None = None


class TimerToggleIn(BaseModel):
    enabled: bool


def _serialize_timer(t: DeviceTimer) -> dict[str, Any]:
    return {
        "id": t.id,
        "user_id": t.user_id,
        "device_uuid": t.device_uuid,
        "name": t.name,
        "enabled": bool(t.enabled),
        "schedule_type": t.schedule_type,
        "delay_sec": int(t.delay_sec or 0),
        "interval_sec": int(t.interval_sec or 0),
        "time_hhmm": t.time_hhmm or "",
        "weekdays": list(t.weekdays or []),
        "action_type": t.action_type,
        "action_payload": dict(t.action_payload or {}),
        "next_run_at": t.next_run_at,
        "last_run_at": t.last_run_at,
        "last_status": t.last_status or "pending",
        "last_message": t.last_message or "",
        "run_count": int(t.run_count or 0),
    }


def _validate_timer_spec(
    db: Session,
    user: User,
    dev: Device,
    schedule_type: str,
    delay_sec: int,
    interval_sec: int,
    run_at: int | None,
    time_hhmm: str,
    weekdays: list[int],
    action_type: str,
    action_payload: dict[str, Any],
    enabled: bool,
) -> int | None:
    if action_type in ACTION_REQUIRED_CAPS:
        req_caps, label = ACTION_REQUIRED_CAPS[action_type]
        require_device_capability(dev, req_caps, label)

    if action_type == "gpio_set":
        if "pin" not in action_payload:
            raise HTTPException(400, "gpio_set requires action_payload.pin")
        dur = int(action_payload.get("duration_sec") or 0)
        if dur < 0 or dur > 86400:
            raise HTTPException(400, "duration_sec must be between 0 and 86400")
        if action_payload.get("duty") is not None:
            duty = int(action_payload["duty"])
            if not (0 <= duty <= 100):
                raise HTTPException(400, "duty must be between 0 and 100")
    elif action_type == "hid_transport":
        if action_payload.get("mode") not in ("ble", "usb", "auto"):
            raise HTTPException(400, "hid_transport mode must be ble/usb/auto")
    elif action_type == "macro":
        macro_id = action_payload.get("macro_id")
        if not macro_id:
            raise HTTPException(400, "macro requires action_payload.macro_id")
        macro = crud_macro.get(db, int(macro_id))
        if not macro or (user.role != "admin" and macro.user_id != user.id):
            raise HTTPException(404, "macro not found")

    try:
        next_run = compute_next_run(
            schedule_type,
            delay_sec=delay_sec,
            interval_sec=interval_sec,
            run_at=run_at,
            time_hhmm=time_hhmm,
            weekdays=weekdays,
        )
    except ValueError as e:
        raise HTTPException(400, str(e)) from e

    return next_run if enabled else None


def _get_accessible_timer(db: Session, user: User, timer_id: int) -> DeviceTimer:
    t = db.query(DeviceTimer).filter(DeviceTimer.id == timer_id).first()
    if not t:
        raise HTTPException(404, "timer not found")
    if user.role == "admin" or t.user_id == user.id:
        return t
    ensure_device_access(db, user, t.device_uuid)
    return t


@router.get("")
@router.get("/")
def list_timers(
    device_uuid: str | None = Query(default=None),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    q = db.query(DeviceTimer)
    if device_uuid:
        ensure_device_access(db, user, device_uuid)
        q = q.filter(DeviceTimer.device_uuid == device_uuid)
    elif user.role != "admin":
        devs = crud_device.list_for_user(db, user.id, user.role)
        allowed_uuids = {d.device_uuid for d in devs}
        if not settings.ALLOW_UNCLAIMED_DEVICE_CONTROL:
            allowed_uuids = {d.device_uuid for d in devs if d.owner_user_id == user.id}
        q = q.filter(
            (DeviceTimer.user_id == user.id) | (DeviceTimer.device_uuid.in_(allowed_uuids))
        )
    rows = q.order_by(DeviceTimer.id.desc()).all()
    return [_serialize_timer(r) for r in rows]


@router.post("", status_code=201)
@router.post("/", status_code=201)
def create_timer(
    body: TimerCreateIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    dev = ensure_device_access(db, user, body.device_uuid)
    clean_days = sorted({int(d) for d in body.weekdays if 0 <= int(d) <= 6})
    next_run = _validate_timer_spec(
        db,
        user,
        dev,
        body.schedule_type,
        body.delay_sec,
        body.interval_sec,
        body.run_at,
        body.time_hhmm,
        clean_days,
        body.action_type,
        body.action_payload,
        body.enabled,
    )
    row = DeviceTimer(
        user_id=user.id,
        device_uuid=body.device_uuid,
        name=body.name.strip(),
        enabled=bool(body.enabled),
        schedule_type=body.schedule_type,
        delay_sec=int(body.delay_sec or 0),
        interval_sec=int(body.interval_sec or 0),
        time_hhmm=body.time_hhmm.strip(),
        weekdays=clean_days,
        action_type=body.action_type,
        action_payload=dict(body.action_payload or {}),
        next_run_at=next_run,
        last_status="pending",
    )
    db.add(row)
    db.commit()
    db.refresh(row)
    return _serialize_timer(row)


@router.put("/{timer_id}")
def update_timer(
    timer_id: int,
    body: TimerUpdateIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    row = _get_accessible_timer(db, user, timer_id)
    dev = ensure_device_access(db, user, row.device_uuid)

    schedule_type = body.schedule_type or row.schedule_type
    delay_sec = int(body.delay_sec if body.delay_sec is not None else row.delay_sec or 0)
    interval_sec = int(body.interval_sec if body.interval_sec is not None else row.interval_sec or 0)
    time_hhmm = body.time_hhmm if body.time_hhmm is not None else (row.time_hhmm or "")
    weekdays = (
        sorted({int(d) for d in body.weekdays if 0 <= int(d) <= 6})
        if body.weekdays is not None
        else list(row.weekdays or [])
    )
    action_type = body.action_type or row.action_type
    action_payload = dict(body.action_payload if body.action_payload is not None else (row.action_payload or {}))
    enabled = bool(body.enabled if body.enabled is not None else row.enabled)
    run_at = body.run_at if body.run_at is not None else row.next_run_at

    next_run = _validate_timer_spec(
        db,
        user,
        dev,
        schedule_type,
        delay_sec,
        interval_sec,
        run_at,
        time_hhmm,
        weekdays,
        action_type,
        action_payload,
        enabled,
    )

    if body.name is not None:
        row.name = body.name.strip()
    row.enabled = enabled
    row.schedule_type = schedule_type
    row.delay_sec = delay_sec
    row.interval_sec = interval_sec
    row.time_hhmm = time_hhmm.strip()
    row.weekdays = weekdays
    row.action_type = action_type
    row.action_payload = action_payload
    row.next_run_at = next_run
    db.commit()
    db.refresh(row)
    return _serialize_timer(row)


@router.post("/{timer_id}/toggle")
def toggle_timer(
    timer_id: int,
    body: TimerToggleIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    row = _get_accessible_timer(db, user, timer_id)
    row.enabled = bool(body.enabled)
    if row.enabled:
        try:
            row.next_run_at = compute_next_run(
                row.schedule_type,
                delay_sec=int(row.delay_sec or 0),
                interval_sec=int(row.interval_sec or 0),
                run_at=row.next_run_at or int(time.time()) + 60,
                time_hhmm=row.time_hhmm or "",
                weekdays=row.weekdays or [],
            )
        except ValueError as e:
            raise HTTPException(400, str(e)) from e
    else:
        row.next_run_at = None
    db.commit()
    db.refresh(row)
    return _serialize_timer(row)


@router.post("/{timer_id}/run")
async def run_timer_now(
    timer_id: int,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    row = _get_accessible_timer(db, user, timer_id)
    now_ts = int(time.time())
    row.last_run_at = now_ts
    row.run_count = int(row.run_count or 0) + 1
    ok, msg = await execute_timer_action(db, row)
    row.last_status = "ok" if ok else "failed"
    row.last_message = str(msg or "")[:250]
    db.commit()
    db.refresh(row)
    return {"ok": ok, "message": msg, "timer": _serialize_timer(row)}


@router.delete("/{timer_id}", status_code=204)
def delete_timer(
    timer_id: int,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    row = _get_accessible_timer(db, user, timer_id)
    db.delete(row)
    db.commit()
