import http from './http'

export const haApi = {
  status() {
    return http.get('/v2/ha/status')
  },
  getConfig() {
    return http.get('/v2/ha/config')
  },
  saveConfig(payload) {
    return http.put('/v2/ha/config', payload)
  },
  testBroker(payload) {
    return http.post('/v2/ha/test', payload, { timeout: 10000 })
  },
  syncNow() {
    return http.post('/v2/ha/sync', null, { timeout: 15000 })
  },
  devices() {
    return http.get('/v2/ha/devices')
  },
  saveDeviceTarget(deviceUuid, payload) {
    return http.put(`/v2/ha/devices/${encodeURIComponent(deviceUuid)}`, payload)
  },
  sendWol(deviceUuid, payload = {}) {
    return http.post(`/v2/ha/devices/${encodeURIComponent(deviceUuid)}/wol`, payload)
  },
  automationsYaml(deviceUuid) {
    return http.get(`/v2/ha/devices/${encodeURIComponent(deviceUuid)}/automations`)
  },
}
