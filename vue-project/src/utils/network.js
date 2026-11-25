const ENV = import.meta.env || {}
const LOOPBACK_HOSTS = new Set(['localhost', '127.0.0.1', '::1'])
const DEV_SERVER_PORT = (ENV.VITE_DEV_SERVER_PORT || '5173').trim()
const BACKEND_PORT = (ENV.VITE_BACKEND_PORT || '8000').trim()

function trimValue(value) {
  return typeof value === 'string' ? value.trim() : ''
}

function stripTrailingSlash(value) {
  return value.replace(/\/+$/, '')
}

export function isLoopbackHost(hostname = (globalThis.location ? globalThis.location.hostname : '')) {
  return LOOPBACK_HOSTS.has(hostname)
}

function getLocation(loc) {
  if (loc) return loc
  return globalThis.location || { hostname: 'localhost', port: '', protocol: 'http:', host: 'localhost:8000' }
}

function isLoopbackDevPage(loc) {
  const cur = getLocation(loc)
  return isLoopbackHost(cur.hostname) && String(cur.port || '') === DEV_SERVER_PORT
}

function defaultApiLocalBase(loc) {
  const cur = getLocation(loc)
  return `http://${cur.hostname}:${BACKEND_PORT}/api`
}

function defaultWsLocalBase(loc) {
  const cur = getLocation(loc)
  const protocol = cur.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${protocol}//${cur.hostname}:${BACKEND_PORT}`
}

function normalizeWsBase(value) {
  return stripTrailingSlash(trimValue(value))
}

export function resolveApiBase(env = ENV, loc = null) {
  const explicitBase = trimValue(env.VITE_API_BASE)
  const localBase = trimValue(env.VITE_API_LOCAL_BASE)

  if (isLoopbackDevPage(loc)) {
    return stripTrailingSlash(localBase || defaultApiLocalBase(loc))
  }

  return explicitBase || '/api'
}

export function resolveWsBase(env = ENV, loc = null) {
  const explicitBase = trimValue(env.VITE_WS_BASE)
  const localBase = trimValue(env.VITE_WS_LOCAL_BASE)

  if (isLoopbackDevPage(loc)) {
    return normalizeWsBase(localBase || defaultWsLocalBase(loc))
  }

  if (explicitBase) {
    return normalizeWsBase(explicitBase)
  }

  const cur = getLocation(loc)
  const protocol = cur.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${protocol}//${cur.host}`
}

export function buildWsUrl(path, token = '', env = ENV, loc = null) {
  const normalizedPath = path.startsWith('/') ? path : `/${path}`
  const base = resolveWsBase(env, loc)
  const query = token ? `?token=${encodeURIComponent(token)}` : ''
  return `${base}${normalizedPath}${query}`
}
