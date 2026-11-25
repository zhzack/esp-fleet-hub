import http from './http'

export const authApi = {
  bootstrapStatus() {
    return http.get('/v1/auth/bootstrap-status')
  },
  login(username, password) {
    return http.post('/v1/auth/login', { username, password })
  },
  register(username, password, role = 'user') {
    return http.post('/v1/auth/register', { username, password, role })
  },
  me() {
    return http.get('/v1/auth/me')
  },
  changePassword(oldPassword, newPassword) {
    return http.post('/v1/auth/change-password', {
      old_password: oldPassword,
      new_password: newPassword,
    })
  },
}
