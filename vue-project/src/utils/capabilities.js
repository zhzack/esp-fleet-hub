const BLE_CAPS = new Set(['ble', 'ble_hid', 'ble_keyboard', 'ble_mouse', 'ble_scanner'])
const USB_CAPS = new Set(['usb', 'usb_otg', 'usb_hid', 'usb_keyboard', 'usb_mouse'])

export function normalizeCapabilities(caps) {
  if (!Array.isArray(caps)) return []
  return caps.map((c) => String(c || '').trim().toLowerCase()).filter(Boolean)
}

export function getCapabilityFlags(caps) {
  const list = normalizeCapabilities(caps)
  const hasReported = list.length > 0
  const hasBle = list.some((c) => BLE_CAPS.has(c) || c.startsWith('ble_'))
  const hasUsb = list.some((c) => USB_CAPS.has(c) || c.startsWith('usb_'))
  const hasHid = hasBle || hasUsb
  return {
    list,
    hasReported,
    hasBle,
    hasUsb,
    hasHid,
    supportsBle: !hasReported || hasBle,
    supportsTransport: !hasReported || hasHid,
    supportsTrackpad: !hasReported || hasHid,
  }
}
