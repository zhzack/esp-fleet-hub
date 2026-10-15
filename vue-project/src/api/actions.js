import http from './http'

export const actionsApi = {
  send(payload) { return http.post('/v2/action', payload) },
  recentLogs(params = {}) {
    const p = typeof params === 'number' ? { limit: params } : params
    return http.get('/v2/logs', { params: p })
  },
}
