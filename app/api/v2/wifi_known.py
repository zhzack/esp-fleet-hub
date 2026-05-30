"""V2 设备 WiFi 已知凭据管理。"""
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel
from sqlalchemy.orm import Session

from app.core.security import get_current_user
from app.database.crud import device as crud_device
from app.database.crud import device_wifi_known as crud_wifi
from app.database.models.user import User
from app.database.session import get_db
from app.services import dispatcher

router = APIRouter()


def _check_owner(db: Session, user: User, device_uuid: str):
    dev = crud_device.get_by_uuid(db, device_uuid)
    if not dev:
        raise HTTPException(404, "device not found")
    if user.role != "admin" and dev.owner_user_id not in (None, user.id):
        raise HTTPException(403, "not your device")
    return dev


class WifiKnownOut(BaseModel):
    ssid: str
    label: Optional[str] = None
    has_password: bool
    last_seen_at: Optional[str] = None


class WifiKnownIn(BaseModel):
    ssid: str
    password: Optional[str] = None
    label: Optional[str] = None


class WifiApplyIn(BaseModel):
    ssid: str


def _to_out(row) -> dict:
    return {
        "ssid": row.ssid,
        "label": row.label,
        "has_password": bool(row.password_enc),
        "last_seen_at": row.last_seen_at.isoformat() if row.last_seen_at else None,
    }


@router.get("/{device_uuid}/wifi/known")
async def list_known(device_uuid: str,
                     db: Session = Depends(get_db),
                     user: User = Depends(get_current_user)):
    dev = _check_owner(db, user, device_uuid)
    rows = crud_wifi.list_for_device(db, dev.id)
    return [_to_out(r) for r in rows]


@router.put("/{device_uuid}/wifi/known")
async def upsert_known(device_uuid: str, body: WifiKnownIn,
                       db: Session = Depends(get_db),
                       user: User = Depends(get_current_user)):
    dev = _check_owner(db, user, device_uuid)
    row = crud_wifi.upsert(db, dev.id, body.ssid,
                           password=body.password, label=body.label)
    return _to_out(row)


@router.delete("/{device_uuid}/wifi/known/{ssid}")
async def delete_known(device_uuid: str, ssid: str,
                       db: Session = Depends(get_db),
                       user: User = Depends(get_current_user)):
    dev = _check_owner(db, user, device_uuid)
    if not crud_wifi.delete(db, dev.id, ssid):
        raise HTTPException(404, "ssid not found in known list")
    return {"ok": True}


@router.post("/{device_uuid}/wifi/apply")
async def apply_known(device_uuid: str, body: WifiApplyIn,
                      db: Session = Depends(get_db),
                      user: User = Depends(get_current_user)):
    """把已知 wifi 凭据下发给设备让它连。"""
    dev = _check_owner(db, user, device_uuid)
    row = crud_wifi.get(db, dev.id, body.ssid)
    if not row:
        raise HTTPException(404, "ssid not in known list")
    pwd = crud_wifi.reveal_password(row)
    if pwd is None and row.password_enc:
        raise HTTPException(500, "password decrypt failed (JWT_SECRET changed?)")
    payload = {"ssid": row.ssid}
    if pwd:
        payload["pass"] = pwd
    return await dispatcher.deliver_command(
        db, device_uuid=device_uuid, cmd_type="wifi.connect",
        payload=payload, user_id=user.id, timeout_ms=20000,
    )
