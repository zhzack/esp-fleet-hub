"""V2 device command HTTP endpoints."""

from typing import Literal

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel
from sqlalchemy.orm import Session

from app.core.security import ensure_device_access, get_current_user
from app.database.models.user import User
from app.database.session import get_db
from app.services import dispatcher

router = APIRouter()

_BLE_CAPS = {"ble", "ble_hid", "ble_keyboard", "ble_mouse", "ble_scanner"}
_USB_CAPS = {"usb", "usb_otg", "usb_hid", "usb_keyboard", "usb_mouse"}
_DISPLAY_CAPS = {"display", "pc_monitor"}


def _require_capability(dev, allowed_caps: set[str], label: str) -> None:
    """Block incompatible commands immediately when a device already reported non-empty capabilities."""
    caps = dev.capabilities
    if not isinstance(caps, list) or not caps:
        return
    normalized = {str(c).strip().lower() for c in caps if str(c).strip()}
    if normalized and not (normalized & allowed_caps):
        raise HTTPException(
            status_code=400,
            detail=f"设备未启用 {label} 能力（已上报能力: {', '.join(sorted(normalized))}）",
        )


async def _send(
    db: Session,
    user: User,
    device_uuid: str,
    cmd_type: str,
    payload: dict | None = None,
    timeout_ms: int = 3000,
    required_caps: set[str] | None = None,
    cap_label: str = "",
) -> dict:
    dev = ensure_device_access(db, user, device_uuid)
    if required_caps:
        _require_capability(dev, required_caps, cap_label)
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
    return await _send(db, user, device_uuid, "ble.info", required_caps=_BLE_CAPS, cap_label="BLE 蓝牙")


@router.get("/{device_uuid}/ble/bonds")
async def ble_list_bonds(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "ble.list_bonds", required_caps=_BLE_CAPS, cap_label="BLE 蓝牙")


@router.post("/{device_uuid}/ble/unpair")
async def ble_unpair(
    device_uuid: str,
    body: BlePeerIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if len(body.addr) != 6:
        raise HTTPException(400, "addr must be 6 bytes")
    return await _send(
        db, user, device_uuid, "ble.unpair", payload=body.model_dump(),
        required_caps=_BLE_CAPS, cap_label="BLE 蓝牙",
    )


@router.post("/{device_uuid}/ble/unpair_all")
async def ble_unpair_all(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "ble.unpair_all", required_caps=_BLE_CAPS, cap_label="BLE 蓝牙")


@router.post("/{device_uuid}/ble/disconnect")
async def ble_disconnect(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "ble.disconnect", required_caps=_BLE_CAPS, cap_label="BLE 蓝牙")


@router.post("/{device_uuid}/ble/switch")
async def ble_switch(
    device_uuid: str,
    body: BlePeerIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if len(body.addr) != 6:
        raise HTTPException(400, "addr must be 6 bytes")
    return await _send(
        db, user, device_uuid, "ble.switch", payload=body.model_dump(),
        required_caps=_BLE_CAPS, cap_label="BLE 蓝牙",
    )


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
        required_caps=_BLE_CAPS,
        cap_label="BLE 蓝牙",
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
    return await _send(
        db, user, device_uuid, "transport.get",
        required_caps=_USB_CAPS | _BLE_CAPS, cap_label="HID 传输",
    )


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
    return await _send(
        db, user, device_uuid, "transport.set", payload=body.model_dump(),
        required_caps=_USB_CAPS | _BLE_CAPS, cap_label="HID 传输",
    )


@router.post("/{device_uuid}/reboot")
async def reboot(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "reboot", timeout_ms=1500)


class DisplayConfigIn(BaseModel):
    mode: Literal["monitor", "text", "status", "off"] | None = None
    backlight: int | None = None
    theme: Literal["cyber", "emerald", "amber", "mono"] | None = None
    header_text: str | None = None
    footer_text: str | None = None
    show_clock: bool | None = None
    show_cpu: bool | None = None
    show_ram: bool | None = None
    show_disk: bool | None = None
    show_net: bool | None = None
    show_uptime: bool | None = None
    lines: list[str] | None = None
    clear: bool | None = None


class DisplayTextIn(BaseModel):
    text: str | None = None
    line0: str | None = None
    line1: str | None = None
    line2: str | None = None
    line3: str | None = None
    line4: str | None = None
    line5: str | None = None
    lines: list[str] | None = None
    backlight: int | None = None
    clear: bool | None = None
    mode: Literal["monitor", "text", "status", "off"] | None = None


def _validate_backlight(backlight: int | None) -> None:
    if backlight is not None and not (0 <= backlight <= 100):
        raise HTTPException(400, "backlight must be between 0 and 100")


@router.get("/{device_uuid}/display/status")
async def display_status(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(
        db,
        user,
        device_uuid,
        "display.status",
        timeout_ms=4000,
        required_caps=_DISPLAY_CAPS,
        cap_label="屏幕显示",
    )


@router.post("/{device_uuid}/display/config")
async def display_config(
    device_uuid: str,
    body: DisplayConfigIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    _validate_backlight(body.backlight)
    return await _send(
        db,
        user,
        device_uuid,
        "display.config",
        payload=body.model_dump(exclude_none=True),
        timeout_ms=4000,
        required_caps=_DISPLAY_CAPS,
        cap_label="屏幕显示",
    )


@router.post("/{device_uuid}/display/text")
async def display_text(
    device_uuid: str,
    body: DisplayTextIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    _validate_backlight(body.backlight)
    return await _send(
        db,
        user,
        device_uuid,
        "display.text",
        payload=body.model_dump(exclude_none=True),
        timeout_ms=4000,
        required_caps=_DISPLAY_CAPS,
        cap_label="屏幕显示",
    )
