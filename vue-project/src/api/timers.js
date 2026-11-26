import http from './http'

export const timersApi = {
  list(deviceUuid = '') {
    const params = deviceUuid ? { device_uuid: deviceUuid } : undefined
    return http.get('/v2/timers', { params })
  },
  create(payload) {
    return http.post('/v2/timers', payload)
  },
  update(timerId, payload) {
    return http.put(`/v2/timers/${timerId}`, payload)
  },
  toggle(timerId, enabled) {
    return http.post(`/v2/timers/${timerId}/toggle`, { enabled: Boolean(enabled) })
  },
  runNow(timerId) {
    return http.post(`/v2/timers/${timerId}/run`)
  },
  remove(timerId) {
    return http.delete(`/v2/timers/${timerId}`)
  },
}
