import http from './http'

export const firmwareApi = {
  list(params) {
    return http.get('/v2/firmwares', { params })
  },
  get(id) {
    return http.get(`/v2/firmwares/${id}`)
  },
  upload(formData, onProgress) {
    return http.post('/v2/firmwares/upload', formData, {
      headers: { 'Content-Type': 'multipart/form-data' },
      onUploadProgress: onProgress,
    })
  },
  delete(id) {
    return http.delete(`/v2/firmwares/${id}`)
  },
  pushOta(firmwareId, deviceUuids, customUrl = null) {
    return http.post(`/v2/firmwares/${firmwareId}/push`, {
      device_uuids: deviceUuids,
      custom_url: customUrl,
    })
  },
  getProgress(deviceUuid) {
    return http.get(`/v2/firmwares/progress/${deviceUuid}`)
  },
  getDeviceStatus(deviceUuid) {
    return http.get(`/v2/firmwares/devices/${deviceUuid}/status`)
  },
}
