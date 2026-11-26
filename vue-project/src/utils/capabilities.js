const BLE_CAPS = new Set(['ble', 'ble_hid', 'ble_keyboard', 'ble_mouse', 'ble_scanner'])
const USB_CAPS = new Set(['usb', 'usb_otg', 'usb_hid', 'usb_keyboard', 'usb_mouse'])
const DISPLAY_CAPS = new Set(['display', 'pc_monitor'])
const GPIO_CAPS = new Set(['gpio', 'gpio_ctrl'])

export function normalizeCapabilities(caps) {
  if (!Array.isArray(caps)) return []
  return caps.map((c) => String(c || '').trim().toLowerCase()).filter(Boolean)
}

export function getCapabilityFlags(caps) {
  const list = normalizeCapabilities(caps)
  const hasReported = list.length > 0
  const hasBle = list.some((c) => BLE_CAPS.has(c) || c.startsWith('ble_'))
  const hasUsb = list.some((c) => USB_CAPS.has(c) || c.startsWith('usb_'))
  const hasDisplay = list.some((c) => DISPLAY_CAPS.has(c))
  const hasGpio = list.some((c) => GPIO_CAPS.has(c))
  const hasHid = hasBle || hasUsb
  return {
    list,
    hasReported,
    hasBle,
    hasUsb,
    hasDisplay,
    hasGpio,
    hasHid,
    supportsBle: !hasReported || hasBle,
    supportsTransport: !hasReported || hasHid,
    supportsTrackpad: !hasReported || hasHid,
    supportsDisplay: !hasReported || hasDisplay,
    supportsGpio: !hasReported || hasGpio,
  }
}
