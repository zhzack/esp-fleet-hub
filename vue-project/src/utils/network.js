const LOOPBACK_HOSTS = new Set(['localhost', '127.0.0.1', '::1'])
const DEV_SERVER_PORT = (import.meta.env.VITE_DEV_SERVER_PORT || '5173').trim()
const BACKEND_PORT = (import.meta.env.VITE_BACKEND_PORT || '8000').trim()

function trimValue(value) {
  return typeof value === 'string' ? value.trim() : ''
}

function stripTrailingSlash(value) {
  return value.replace(/\/+$/, '')
}

function isLoopbackHost(hostname = location.hostname) {
  return LOOPBACK_HOSTS.has(hostname)
}

function isLoopbackDevPage() {
  return isLoopbackHost() && location.port === DEV_SERVER_PORT
}

function defaultApiLocalBase() {
  return `http://${location.hostname}:${BACKEND_PORT}/api`
}

function defaultWsLocalBase() {
  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${protocol}//${location.hostname}:${BACKEND_PORT}`
}

function normalizeWsBase(value) {
  return stripTrailingSlash(trimValue(value))
}

export function resolveApiBase() {
  const explicitBase = trimValue(import.meta.env.VITE_API_BASE)
  const localBase = trimValue(import.meta.env.VITE_API_LOCAL_BASE)

  if (isLoopbackDevPage()) {
    return stripTrailingSlash(localBase || defaultApiLocalBase())
  }

  return explicitBase || '/api'
}

export function resolveWsBase() {
  const explicitBase = trimValue(import.meta.env.VITE_WS_BASE)
  const localBase = trimValue(import.meta.env.VITE_WS_LOCAL_BASE)

  if (isLoopbackDevPage()) {
    return normalizeWsBase(localBase || defaultWsLocalBase())
  }

  if (explicitBase) {
    return normalizeWsBase(explicitBase)
  }

  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${protocol}//${location.host}`
}

export function buildWsUrl(path, token = '') {
  const normalizedPath = path.startsWith('/') ? path : `/${path}`
  const base = resolveWsBase()
  const query = token ? `?token=${encodeURIComponent(token)}` : ''
  return `${base}${normalizedPath}${query}`
}
