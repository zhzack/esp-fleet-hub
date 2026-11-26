export function normalizeWsConfigForm(src = {}) {
  const clamp = (v, min, max, fallback) => {
    const n = Number(v)
    if (!Number.isFinite(n)) return fallback
    return Math.max(min, Math.min(max, Math.round(n)))
  }
  return {
    ws_enabled: src.ws_enabled !== undefined ? Boolean(src.ws_enabled) : true,
    ws_gateway_url: String(src.ws_gateway_url || ''),
    ws_reconnect_ms: clamp(src.ws_reconnect_ms, 500, 60000, 5000),
    ws_heartbeat_ms: clamp(src.ws_heartbeat_ms, 2000, 120000, 10000),
    hub_url: String(src.hub_url || ''),
    hub_fallback_mdns: src.hub_fallback_mdns !== undefined ? Boolean(src.hub_fallback_mdns) : true,
    hb_interval_ms: clamp(src.hb_interval_ms, 2000, 120000, 12000),
    auth_strict: Boolean(src.auth_strict),
  }
}

export function buildUnifiedConfigPatch(form = {}) {
  const norm = normalizeWsConfigForm(form)
  const patch = {
    mdns_host: String(form.mdns_host || '').trim(),
    hub_url: norm.hub_url.trim(),
    ws_gateway_url: norm.ws_gateway_url.trim(),
    ws_enabled: norm.ws_enabled,
    ws_reconnect_ms: norm.ws_reconnect_ms,
    ws_heartbeat_ms: norm.ws_heartbeat_ms,
    hub_fallback_mdns: norm.hub_fallback_mdns,
    hb_interval_ms: norm.hb_interval_ms,
    auth_strict: norm.auth_strict,
  }
  if (form.wifi_ssid && String(form.wifi_ssid).trim()) {
    patch.wifi_ssid = String(form.wifi_ssid).trim()
  }
  if (form.wifi_pass) {
    patch.wifi_pass = String(form.wifi_pass)
  }
  if (form.api_token && String(form.api_token).trim()) {
    patch.api_token = String(form.api_token).trim()
  }
  return patch
}
