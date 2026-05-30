import http from './http'

export const macrosApi = {
  list()              { return http.get('/v1/macros') },
  get(id)             { return http.get(`/v1/macros/${id}`) },
  create(payload)     { return http.post('/v1/macros', payload) },
  update(id, payload) { return http.put(`/v1/macros/${id}`, payload) },
  remove(id)          { return http.delete(`/v1/macros/${id}`) },
}
