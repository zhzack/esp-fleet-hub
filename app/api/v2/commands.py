"""V2 device command HTTP endpoints."""

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel
from sqlalchemy.orm import Session

from app.core.security import ensure_device_access, get_current_user
from app.database.models.user import User
from app.database.session import get_db
from app.services import dispatcher

router = APIRouter()


async def _send(
    db: Session,
    user: User,
    device_uuid: str,
    cmd_type: str,
    payload: dict | None = None,
    timeout_ms: int = 3000,
) -> dict:
    ensure_device_access(db, user, device_uuid)
    return await dispatcher.deliver_command(
        db,
        device_uuid=device_uuid,
        cmd_type=cmd_type,
        payload=payload or {},
        user_id=user.id,
        timeout_ms=timeout_ms,
    )


@router.get("/{device_uuid}/status")
async def get_status(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "get_status")


class WifiConnectIn(BaseModel):
    ssid: str
    password: str | None = None


@router.get("/{device_uuid}/wifi/status")
async def wifi_status(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "wifi.status")


@router.post("/{device_uuid}/wifi/scan")
async def wifi_scan(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "wifi.scan", timeout_ms=8000)


@router.post("/{device_uuid}/wifi/connect")
async def wifi_connect(
    device_uuid: str,
    body: WifiConnectIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    payload = {"ssid": body.ssid}
    if body.password is not None:
        payload["pass"] = body.password

    resp = await _send(
        db,
        user,
        device_uuid,
        "wifi.connect",
        payload=payload,
        timeout_ms=20000,
    )

    try:
        if resp.get("status") == "acked":
            from app.database.crud import device as _crud_dev
            from app.database.crud import device_wifi_known as _crud_wifi

            dev = _crud_dev.get_by_uuid(db, device_uuid)
            if dev:
                _crud_wifi.upsert(db, dev.id, body.ssid, password=body.password)
    except Exception:
        pass

    return resp


class BlePeerIn(BaseModel):
    addr_type: int
    addr: list[int]


@router.get("/{device_uuid}/ble/info")
async def ble_info(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "ble.info")


@router.get("/{device_uuid}/ble/bonds")
async def ble_list_bonds(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "ble.list_bonds")


@router.post("/{device_uuid}/ble/unpair")
async def ble_unpair(
    device_uuid: str,
    body: BlePeerIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if len(body.addr) != 6:
        raise HTTPException(400, "addr must be 6 bytes")
    return await _send(db, user, device_uuid, "ble.unpair", payload=body.model_dump())


@router.post("/{device_uuid}/ble/unpair_all")
async def ble_unpair_all(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "ble.unpair_all")


@router.post("/{device_uuid}/ble/disconnect")
async def ble_disconnect(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "ble.disconnect")


@router.post("/{device_uuid}/ble/switch")
async def ble_switch(
    device_uuid: str,
    body: BlePeerIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if len(body.addr) != 6:
        raise HTTPException(400, "addr must be 6 bytes")
    return await _send(db, user, device_uuid, "ble.switch", payload=body.model_dump())


class BleScanIn(BaseModel):
    duration_ms: int | None = 4000


@router.post("/{device_uuid}/ble/scan")
async def ble_scan(
    device_uuid: str,
    body: BleScanIn | None = None,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    payload = body.model_dump(exclude_none=True) if body else {}
    dur = int(payload.get("duration_ms") or 4000)
    return await _send(
        db,
        user,
        device_uuid,
        "ble.scan",
        payload=payload,
        timeout_ms=max(dur + 4000, 8000),
    )


@router.get("/{device_uuid}/usb/info")
async def usb_info(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "usb.info")


@router.get("/{device_uuid}/transport")
async def transport_get(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "transport.get")


class TransportIn(BaseModel):
    mode: str


@router.post("/{device_uuid}/transport")
async def transport_set(
    device_uuid: str,
    body: TransportIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if body.mode not in ("ble", "usb", "auto"):
        raise HTTPException(400, "mode must be ble/usb/auto")
    return await _send(db, user, device_uuid, "transport.set", payload=body.model_dump())


@router.post("/{device_uuid}/reboot")
async def reboot(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "reboot", timeout_ms=1500)
