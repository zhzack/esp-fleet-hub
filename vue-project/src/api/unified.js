import http from './http'

export const unifiedApi = {
  devices() {
    return http.get('/v2/unified/devices')
  },
  claimDevice(deviceId, payload) {
    return http.post(`/v2/unified/devices/${encodeURIComponent(deviceId)}/claim`, payload)
  },
  pushConfig(deviceId, config) {
    return http.post(`/v2/unified/devices/${encodeURIComponent(deviceId)}/config`, { config })
  },
  sendCommand(deviceId, cmdType, payload = {}) {
    return http.post(`/v2/unified/devices/${encodeURIComponent(deviceId)}/command`, {
      cmd_type: cmdType,
      payload,
    })
  },
  deleteDevice(deviceId) {
    return http.delete(`/v2/unified/devices/${encodeURIComponent(deviceId)}`)
  },
  proxyGet(deviceId, path) {
    return http.get(`/v2/unified/devices/${encodeURIComponent(deviceId)}/proxy`, {
      params: { path },
    })
  },
  proxyPost(deviceId, path, payload = {}) {
    return http.post(`/v2/unified/devices/${encodeURIComponent(deviceId)}/proxy`, payload, {
      params: { path },
    })
  },
  captureSnapshot(deviceId) {
    return http.get(`/v2/unified/devices/${encodeURIComponent(deviceId)}/proxy`, {
      params: { path: '/capture' },
      responseType: 'blob',
      timeout: 10000,
    })
  },
  profiles() {
    return http.get('/v2/unified/profiles')
  },
  ports() {
    return http.get('/v2/unified/ports')
  },
  firmwares() {
    return http.get('/v2/unified/firmwares')
  },
  publishFirmware(fileName, payload = {}) {
    return http.post(`/v2/unified/firmwares/${encodeURIComponent(fileName)}/publish`, payload)
  },
  batchOta(file, deviceIds) {
    return http.post('/v2/unified/ota/batch', { file, device_ids: deviceIds })
  },
  startBuild(payload) {
    return http.post('/v2/unified/build', payload)
  },
  taskStatus() {
    return http.get('/v2/unified/task')
  },
  flashSerial(payload) {
    return http.post('/v2/unified/flash_serial', payload)
  },
}
