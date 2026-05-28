import http from './http'

const base = (uuid) => `/v2/devices/${encodeURIComponent(uuid)}`

export const commandsApi = {
  status(uuid)        { return http.get(`${base(uuid)}/status`) },

  wifiStatus(uuid)    { return http.get(`${base(uuid)}/wifi/status`) },
  wifiScan(uuid)      { return http.post(`${base(uuid)}/wifi/scan`, null, { timeout: 12000 }) },
  wifiConnect(uuid, ssid, password) {
    return http.post(`${base(uuid)}/wifi/connect`, { ssid, password }, { timeout: 25000 })
  },

  bleInfo(uuid)       { return http.get(`${base(uuid)}/ble/info`) },
  bleBonds(uuid)      { return http.get(`${base(uuid)}/ble/bonds`) },
  bleUnpair(uuid, addrType, addr)   { return http.post(`${base(uuid)}/ble/unpair`,    { addr_type: addrType, addr }) },
  bleUnpairAll(uuid)  { return http.post(`${base(uuid)}/ble/unpair_all`) },
  bleDisconnect(uuid) { return http.post(`${base(uuid)}/ble/disconnect`) },
  bleSwitch(uuid, addrType, addr)   { return http.post(`${base(uuid)}/ble/switch`,    { addr_type: addrType, addr }) },
  bleScan(uuid, durationMs = 4000)  { return http.post(`${base(uuid)}/ble/scan`,      { duration_ms: durationMs }, { timeout: durationMs + 8000 }) },

  usbInfo(uuid)        { return http.get(`${base(uuid)}/usb/info`) },
  transportGet(uuid)   { return http.get(`${base(uuid)}/transport`) },
  transportSet(uuid, mode) { return http.post(`${base(uuid)}/transport`, { mode }) },

  reboot(uuid)        { return http.post(`${base(uuid)}/reboot`) },
}
