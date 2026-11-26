"""Shared device capability sets and guard helpers."""

from fastapi import HTTPException

BLE_CAPS = {"ble", "ble_hid", "ble_keyboard", "ble_mouse", "ble_scanner"}
USB_CAPS = {"usb", "usb_otg", "usb_hid", "usb_keyboard", "usb_mouse"}
HID_CAPS = BLE_CAPS | USB_CAPS
DISPLAY_CAPS = {"display", "pc_monitor"}
GPIO_CAPS = {"gpio", "gpio_ctrl"}


def normalize_caps(caps) -> set[str]:
    if not isinstance(caps, list):
        return set()
    return {str(c).strip().lower() for c in caps if str(c).strip()}


def require_device_capability(dev, allowed_caps: set[str], label: str) -> None:
    """Reject commands immediately when a device already reported capabilities excluding the target feature."""
    normalized = normalize_caps(getattr(dev, "capabilities", None))
    if normalized and not (normalized & allowed_caps):
        raise HTTPException(
            status_code=400,
            detail=f"设备未启用 {label} 能力（已上报能力: {', '.join(sorted(normalized))}）",
        )
