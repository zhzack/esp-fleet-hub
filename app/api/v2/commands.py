"""V2 device command HTTP endpoints."""

from typing import Any, Literal

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, ConfigDict
from sqlalchemy.orm import Session

from app.core.security import ensure_device_access, get_current_user
from app.database.models.user import User
from app.database.session import get_db
from app.services.capabilities import (
    BLE_CAPS as _BLE_CAPS,
    DISPLAY_CAPS as _DISPLAY_CAPS,
    GPIO_CAPS as _GPIO_CAPS,
    USB_CAPS as _USB_CAPS,
    require_device_capability as _require_capability,
)
from app.services.device_executor import execute_device_command, execute_gpio_control

router = APIRouter()


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
    return await execute_device_command(
        db,
        user_id=user.id,
        device_uuid=device_uuid,
        cmd_type=cmd_type,
        payload=payload or {},
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


class HidActionIn(BaseModel):
    model_config = ConfigDict(extra="allow")
    type: str | None = None
    steps: list[dict[str, Any]] | None = None
    need_ack: bool = True


@router.post("/{device_uuid}/hid/action")
async def hid_action(
    device_uuid: str,
    body: HidActionIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    payload = body.model_dump(exclude_none=True)
    if not payload.get("type"):
        if isinstance(payload.get("steps"), list) and payload["steps"]:
            payload["type"] = "macro"
        else:
            raise HTTPException(400, "action payload requires 'type' or non-empty 'steps'")
    return await _send(
        db,
        user,
        device_uuid,
        "hid.action",
        payload=payload,
        timeout_ms=8000,
        required_caps=_USB_CAPS | _BLE_CAPS,
        cap_label="HID 控制",
    )


class WsConfigIn(BaseModel):
    ws_enabled: bool | None = None
    ws_gateway_url: str | None = None
    ws_reconnect_ms: int | None = None
    ws_heartbeat_ms: int | None = None
    hub_url: str | None = None
    hub_fallback_mdns: bool | None = None
    hb_interval_ms: int | None = None
    auth_strict: bool | None = None


@router.get("/{device_uuid}/ws/config")
async def ws_config_get(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(db, user, device_uuid, "ws.config.get", timeout_ms=4000)


@router.post("/{device_uuid}/ws/config")
async def ws_config_set(
    device_uuid: str,
    body: WsConfigIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if body.ws_gateway_url is not None:
        ws_u = body.ws_gateway_url.strip()
        if ws_u and not (ws_u.startswith("ws://") or ws_u.startswith("wss://")):
            raise HTTPException(400, "ws_gateway_url must start with ws:// or wss://")
    if body.hub_url is not None:
        hub_u = body.hub_url.strip()
        if hub_u and not (hub_u.startswith("http://") or hub_u.startswith("https://")):
            raise HTTPException(400, "hub_url must start with http:// or https://")
    if body.ws_reconnect_ms is not None and not (500 <= body.ws_reconnect_ms <= 60000):
        raise HTTPException(400, "ws_reconnect_ms must be between 500 and 60000")
    if body.ws_heartbeat_ms is not None and not (2000 <= body.ws_heartbeat_ms <= 120000):
        raise HTTPException(400, "ws_heartbeat_ms must be between 2000 and 120000")
    if body.hb_interval_ms is not None and not (2000 <= body.hb_interval_ms <= 120000):
        raise HTTPException(400, "hb_interval_ms must be between 2000 and 120000")
    payload = body.model_dump(exclude_none=True)
    return await _send(db, user, device_uuid, "ws.config.set", payload=payload, timeout_ms=5000)


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


DisplayPanel = Literal["auto", "st7735_160", "st7789_240", "epd_213_bw", "epd_213_bwr", "epd_420_bwr"]
DisplayMode = Literal["monitor", "text", "status", "image", "off"]


class DisplayConfigIn(BaseModel):
    mode: DisplayMode | None = None
    backlight: int | None = None
    theme: Literal["cyber", "emerald", "amber", "mono", "paper", "red_bar"] | None = None
    panel: DisplayPanel | None = None
    rotation: Literal[0, 90, 180, 270] | None = None
    invert: bool | None = None
    partial_refresh: bool | None = None
    refresh_sec: int | None = None
    red_mask: int | None = None
    red_lines: list[int] | None = None
    refresh: bool | None = None
    full_refresh: bool | None = None
    pins: dict[str, int] | None = None
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
    mode: DisplayMode | None = None
    panel: DisplayPanel | None = None
    rotation: Literal[0, 90, 180, 270] | None = None
    invert: bool | None = None
    partial_refresh: bool | None = None
    red_mask: int | None = None
    refresh: bool | None = None
    full_refresh: bool | None = None


class DisplayImageIn(BaseModel):
    width: int
    height: int
    color_mode: Literal["bw", "bwr"] = "bwr"
    encoding: Literal["raw", "rle"] = "rle"
    bw_b64: str
    red_b64: str | None = None
    panel: DisplayPanel | None = None
    rotation: Literal[0, 90, 180, 270] | None = None
    invert: bool | None = None
    full_refresh: bool | None = True


def _validate_backlight(backlight: int | None) -> None:
    if backlight is not None and not (0 <= backlight <= 100):
        raise HTTPException(400, "backlight must be between 0 and 100")


def _validate_display_extras(body) -> None:
    _validate_backlight(getattr(body, "backlight", None))
    rsec = getattr(body, "refresh_sec", None)
    if rsec is not None and not (0 <= rsec <= 86400):
        raise HTTPException(400, "refresh_sec must be between 0 and 86400")
    rmask = getattr(body, "red_mask", None)
    if rmask is not None and not (0 <= rmask <= 255):
        raise HTTPException(400, "red_mask must be between 0 and 255")
    pins = getattr(body, "pins", None)
    if pins is not None:
        for k, v in pins.items():
            if k not in ("mosi", "clk", "cs", "dc", "rst", "busy"):
                raise HTTPException(400, f"unsupported display pin key: {k}")
            if not (-1 <= int(v) <= 48):
                raise HTTPException(400, f"pin {k} out of range (-1..48)")


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
    _validate_display_extras(body)
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
    _validate_display_extras(body)
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


@router.post("/{device_uuid}/display/image")
async def display_image(
    device_uuid: str,
    body: DisplayImageIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if not (1 <= body.width <= 400 and 1 <= body.height <= 400):
        raise HTTPException(400, "width and height must be between 1 and 400")
    plane_len = ((body.width + 7) // 8) * body.height
    if plane_len > 15000:
        raise HTTPException(400, "image buffer exceeds maximum EPD framebuffer size (15000 bytes)")
    if not body.bw_b64:
        raise HTTPException(400, "bw_b64 is required")
    return await _send(
        db,
        user,
        device_uuid,
        "display.image",
        payload=body.model_dump(exclude_none=True),
        timeout_ms=8000,
        required_caps=_DISPLAY_CAPS,
        cap_label="屏幕显示",
    )


class GpioSetIn(BaseModel):
    pin: int
    val: int | None = None
    duty: int | None = None
    op: Literal["on", "off", "toggle", "pwm"] | None = None
    duration_sec: int = 0


@router.get("/{device_uuid}/gpio/pins")
async def gpio_pins(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    return await _send(
        db,
        user,
        device_uuid,
        "gpio.pins",
        timeout_ms=3500,
        required_caps=_GPIO_CAPS,
        cap_label="GPIO 外设",
    )


@router.post("/{device_uuid}/gpio/set")
async def gpio_set(
    device_uuid: str,
    body: GpioSetIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if body.pin < 0 or body.pin > 48:
        raise HTTPException(400, "pin must be between 0 and 48")
    if body.duty is not None and not (0 <= body.duty <= 100):
        raise HTTPException(400, "duty must be between 0 and 100")
    if body.duration_sec < 0 or body.duration_sec > 86400:
        raise HTTPException(400, "duration_sec must be between 0 and 86400")
    dev = ensure_device_access(db, user, device_uuid)
    _require_capability(dev, _GPIO_CAPS, "GPIO 外设")
    return await execute_gpio_control(
        db,
        user_id=user.id,
        device_uuid=device_uuid,
        pin=body.pin,
        val=body.val,
        duty=body.duty,
        op=body.op,
        duration_sec=body.duration_sec,
    )
