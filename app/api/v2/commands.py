"""V2 设备命令 HTTP 入口（云路径）。

服务端把 HTTP 请求翻译成 V2.1 envelope 下发给设备的 cmd_router，等待 ACK 后
把设备返回的 result 透传给调用方。

命令类型与固件 cmd_router / lan_mode 中的处理保持一一对应。
"""
from typing import Any

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel
from sqlalchemy.orm import Session

from app.core.security import get_current_user
from app.database.crud import device as crud_device
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


async def _send(db: Session, user: User, device_uuid: str, cmd_type: str,
                payload: dict | None = None, timeout_ms: int = 3000) -> dict:
    _check_owner(db, user, device_uuid)
    return await dispatcher.deliver_command(
        db,
        device_uuid=device_uuid,
        cmd_type=cmd_type,
        payload=payload or {},
        user_id=user.id,
        timeout_ms=timeout_ms,
    )


# --- status ---

@router.get("/{device_uuid}/status")
async def get_status(device_uuid: str,
                     db: Session = Depends(get_db),
                     user: User = Depends(get_current_user)):
    return await _send(db, user, device_uuid, "get_status")


# --- wifi ---

class WifiConnectIn(BaseModel):
    ssid: str
    password: str | None = None


@router.get("/{device_uuid}/wifi/status")
async def wifi_status(device_uuid: str,
                      db: Session = Depends(get_db),
                      user: User = Depends(get_current_user)):
    return await _send(db, user, device_uuid, "wifi.status")


@router.post("/{device_uuid}/wifi/scan")
async def wifi_scan(device_uuid: str,
                    db: Session = Depends(get_db),
                    user: User = Depends(get_current_user)):
    return await _send(db, user, device_uuid, "wifi.scan", timeout_ms=8000)


@router.post("/{device_uuid}/wifi/connect")
async def wifi_connect(device_uuid: str, body: WifiConnectIn,
                       db: Session = Depends(get_db),
                       user: User = Depends(get_current_user)):
    return await _send(db, user, device_uuid, "wifi.connect",
                       payload=body.model_dump(exclude_none=True), timeout_ms=20000)


# --- ble ---

class BlePeerIn(BaseModel):
    addr_type: int
    addr: list[int]  # 长度 6


@router.get("/{device_uuid}/ble/info")
async def ble_info(device_uuid: str,
                   db: Session = Depends(get_db),
                   user: User = Depends(get_current_user)):
    return await _send(db, user, device_uuid, "ble.info")


@router.get("/{device_uuid}/ble/bonds")
async def ble_list_bonds(device_uuid: str,
                         db: Session = Depends(get_db),
                         user: User = Depends(get_current_user)):
    return await _send(db, user, device_uuid, "ble.list_bonds")


@router.post("/{device_uuid}/ble/unpair")
async def ble_unpair(device_uuid: str, body: BlePeerIn,
                     db: Session = Depends(get_db),
                     user: User = Depends(get_current_user)):
    if len(body.addr) != 6:
        raise HTTPException(400, "addr must be 6 bytes")
    return await _send(db, user, device_uuid, "ble.unpair", payload=body.model_dump())


@router.post("/{device_uuid}/ble/unpair_all")
async def ble_unpair_all(device_uuid: str,
                         db: Session = Depends(get_db),
                         user: User = Depends(get_current_user)):
    return await _send(db, user, device_uuid, "ble.unpair_all")


@router.post("/{device_uuid}/ble/disconnect")
async def ble_disconnect(device_uuid: str,
                         db: Session = Depends(get_db),
                         user: User = Depends(get_current_user)):
    return await _send(db, user, device_uuid, "ble.disconnect")


@router.post("/{device_uuid}/ble/switch")
async def ble_switch(device_uuid: str, body: BlePeerIn,
                     db: Session = Depends(get_db),
                     user: User = Depends(get_current_user)):
    if len(body.addr) != 6:
        raise HTTPException(400, "addr must be 6 bytes")
    return await _send(db, user, device_uuid, "ble.switch", payload=body.model_dump())


# --- system ---

@router.post("/{device_uuid}/reboot")
async def reboot(device_uuid: str,
                 db: Session = Depends(get_db),
                 user: User = Depends(get_current_user)):
    return await _send(db, user, device_uuid, "reboot", timeout_ms=1500)
