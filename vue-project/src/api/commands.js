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

  // slot CRUD
  listSlots(uuid)                    { return http.get(`${base(uuid)}/slots`) },
  putSlot(uuid, idx, body)           { return http.put(`${base(uuid)}/slots/${idx}`, body) },
  deleteSlot(uuid, idx)              { return http.delete(`${base(uuid)}/slots/${idx}`) },

  // 多目标编排发送
  send(uuid, body)                   { return http.post(`${base(uuid)}/send`, body, { timeout: 30000 }) },

  // WiFi 已知凭据
  listKnownWifi(uuid)                { return http.get(`${base(uuid)}/wifi/known`) },
  upsertKnownWifi(uuid, body)        { return http.put(`${base(uuid)}/wifi/known`, body) },
  deleteKnownWifi(uuid, ssid)        { return http.delete(`${base(uuid)}/wifi/known/${encodeURIComponent(ssid)}`) },
  applyKnownWifi(uuid, ssid)         { return http.post(`${base(uuid)}/wifi/apply`, { ssid }, { timeout: 25000 }) },
}
