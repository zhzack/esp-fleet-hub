export const DISPLAY_MODE_OPTIONS = [
  { value: 'monitor', label: 'PC 监控', desc: '实时展示 CPU / 内存 / 磁盘 / 网速 / 运行时长' },
  { value: 'text', label: '自定义文本', desc: '固定展示最多 6 行自定义看板文字，不需推流' },
  { value: 'status', label: '节点状态', desc: '本地展示 WiFi / IP / RSSI / Heap 与备注' },
  { value: 'off', label: '息屏待机', desc: '关闭背光与刷新，保留当前已存配置' },
]

export const DISPLAY_THEME_OPTIONS = [
  { value: 'cyber', label: '赛博青紫', header: '#22d3ee', a1: '#22d3ee', a2: '#e879f9', a3: '#fb923c', ok: '#4ade80' },
  { value: 'emerald', label: '翡翠矩阵', header: '#4ade80', a1: '#4ade80', a2: '#22d3ee', a3: '#facc15', ok: '#4ade80' },
  { value: 'amber', label: '琥珀终端', header: '#facc15', a1: '#fb923c', a2: '#facc15', a3: '#f8fafc', ok: '#4ade80' },
  { value: 'mono', label: '高对比白', header: '#f8fafc', a1: '#cbd5e1', a2: '#f8fafc', a3: '#facc15', ok: '#f8fafc' },
]

export function createDefaultDisplayConfig(pcMonitor = true) {
  return {
    mode: pcMonitor ? 'monitor' : 'text',
    backlight: 100,
    theme: 'cyber',
    header_text: '',
    footer_text: '',
    show_clock: true,
    show_cpu: true,
    show_ram: true,
    show_disk: true,
    show_net: true,
    show_uptime: true,
    lines: ['ESP Unified', 'Ready', '', '', '', ''],
  }
}

export function normalizeDisplayConfig(raw, pcMonitor = true) {
  const base = createDefaultDisplayConfig(pcMonitor)
  if (!raw || typeof raw !== 'object') return base
  const validMode = DISPLAY_MODE_OPTIONS.some((m) => m.value === raw.mode)
  const validTheme = DISPLAY_THEME_OPTIONS.some((t) => t.value === raw.theme)
  const bl = Number(raw.backlight)
  const lines = Array.from({ length: 6 }, (_, i) => {
    const arrVal = Array.isArray(raw.lines) ? raw.lines[i] : undefined
    const lineKey = raw[`line${i}`]
    return String(arrVal !== undefined ? arrVal : (lineKey ?? base.lines[i] ?? ''))
  })
  return {
    mode: validMode ? raw.mode : base.mode,
    backlight: Number.isFinite(bl) ? Math.min(100, Math.max(0, Math.round(bl))) : base.backlight,
    theme: validTheme ? raw.theme : base.theme,
    header_text: String(raw.header_text ?? raw.header ?? ''),
    footer_text: String(raw.footer_text ?? raw.footer ?? ''),
    show_clock: raw.show_clock !== undefined ? Boolean(raw.show_clock) : base.show_clock,
    show_cpu: raw.show_cpu !== undefined ? Boolean(raw.show_cpu) : base.show_cpu,
    show_ram: raw.show_ram !== undefined ? Boolean(raw.show_ram) : base.show_ram,
    show_disk: raw.show_disk !== undefined ? Boolean(raw.show_disk) : base.show_disk,
    show_net: raw.show_net !== undefined ? Boolean(raw.show_net) : base.show_net,
    show_uptime: raw.show_uptime !== undefined ? Boolean(raw.show_uptime) : base.show_uptime,
    lines,
  }
}

export function buildDisplayPayload(cfg) {
  const n = normalizeDisplayConfig(cfg)
  return {
    mode: n.mode,
    backlight: n.backlight,
    theme: n.theme,
    header_text: n.header_text,
    footer_text: n.footer_text,
    show_clock: n.show_clock,
    show_cpu: n.show_cpu,
    show_ram: n.show_ram,
    show_disk: n.show_disk,
    show_net: n.show_net,
    show_uptime: n.show_uptime,
    lines: n.lines.map((s) => String(s || '').slice(0, 39)),
  }
}

export function clipScreenText(text, maxChars = 10) {
  return String(text || '').slice(0, maxChars)
}
