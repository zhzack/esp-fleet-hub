export const DISPLAY_MODE_OPTIONS = [
  { value: 'status', label: '节点状态', desc: '本地展示 WiFi / IP / RSSI / Heap 与备注' },
  { value: 'text', label: '自定义文本', desc: '固定展示最多 6 行自定义看板文字，墨水屏断电持久保留' },
  { value: 'monitor', label: 'PC 监控', desc: '实时展示 CPU / 内存 / 磁盘 / 网速 / 运行时长' },
  { value: 'off', label: '清屏/休眠', desc: '墨水屏清屏进入深睡休眠 / LCD 关闭背光' },
]

export const DISPLAY_THEME_OPTIONS = [
  { value: 'cyber', label: '红头白纸 / 赛博青紫', header: '#22d3ee', a1: '#22d3ee', a2: '#e879f9', a3: '#fb923c', ok: '#4ade80' },
  { value: 'red_bar', label: '红顶栏醒目看板 (三色屏)', header: '#b91c1c', a1: '#b91c1c', a2: '#111827', a3: '#475569', ok: '#15803d' },
  { value: 'paper', label: '纯净纸白黑字', header: '#18181b', a1: '#b91c1c', a2: '#18181b', a3: '#52525b', ok: '#18181b' },
  { value: 'emerald', label: '翡翠矩阵', header: '#4ade80', a1: '#4ade80', a2: '#22d3ee', a3: '#facc15', ok: '#4ade80' },
  { value: 'amber', label: '琥珀终端', header: '#facc15', a1: '#fb923c', a2: '#facc15', a3: '#f8fafc', ok: '#4ade80' },
  { value: 'mono', label: '高对比纯黑白', header: '#f8fafc', a1: '#cbd5e1', a2: '#f8fafc', a3: '#facc15', ok: '#f8fafc' },
]

export const DISPLAY_PANEL_OPTIONS = [
  {
    value: 'epd_213_bwr',
    label: '2.13" 黑白红三色墨水屏 (122×250)',
    shortLabel: '2.13" 三色 (122×250)',
    chip: 'SSD1683 / GDEY0213Z98 (FPC-A002)',
    w: 122,
    h: 250,
    isEpd: true,
    hasRed: true,
    partial: false,
  },
  {
    value: 'epd_213_bw',
    label: '2.13" 黑白双色墨水屏 (122×250 · 支持局部快刷)',
    shortLabel: '2.13" 黑白 (122×250)',
    chip: 'SSD1683 / GDEY0213B74',
    w: 122,
    h: 250,
    isEpd: true,
    hasRed: false,
    partial: true,
  },
  {
    value: 'epd_420_bwr',
    label: '4.2" 黑白红三色大墨水屏 (400×300)',
    shortLabel: '4.2" 三色 (400×300)',
    chip: 'SSD1683 / GDEY042Z98',
    w: 400,
    h: 300,
    isEpd: true,
    hasRed: true,
    partial: false,
  },
  {
    value: 'st7789_240',
    label: 'ST7789 240×240 SPI 彩色 TFT',
    shortLabel: 'ST7789 240×240 LCD',
    chip: 'ST7789 RGB565',
    w: 240,
    h: 240,
    isEpd: false,
    hasRed: false,
    partial: false,
  },
  {
    value: 'st7735_160',
    label: 'Air101 ST7735 80×160 监控彩屏',
    shortLabel: 'ST7735 80×160 LCD',
    chip: 'ST7735 RGB565',
    w: 80,
    h: 160,
    isEpd: false,
    hasRed: false,
    partial: false,
  },
]

export const DISPLAY_ROTATION_OPTIONS = [
  { value: 0, label: '0° 默认' },
  { value: 90, label: '90° 横向旋转' },
  { value: 180, label: '180° 上下倒置' },
  { value: 270, label: '270° 反向旋转' },
]

export const EPD_DEFAULT_PINS = {
  mosi: 11,
  clk: 12,
  cs: 10,
  dc: 13,
  rst: 9,
  busy: 14,
}

export const EPD_WIRING_GUIDE = [
  { boardPin: 'GND', s3Pin: 'GND', note: '共地' },
  { boardPin: '3V3', s3Pin: '3.3V', note: '仅接 3.3V，严禁接 5V' },
  { boardPin: 'SCK', s3Pin: 'GPIO 12', note: 'SPI 时钟 (可自定义)' },
  { boardPin: 'SDA', s3Pin: 'GPIO 11', note: 'SPI MOSI (可自定义)' },
  { boardPin: 'CS1', s3Pin: 'GPIO 10', note: '主片选 (CS2 悬空不接)' },
  { boardPin: 'DC', s3Pin: 'GPIO 13', note: '数据/命令切换' },
  { boardPin: 'RST', s3Pin: 'GPIO 9', note: '硬件复位' },
  { boardPin: 'BUSY', s3Pin: 'GPIO 14', note: '高电平忙检测' },
]

export function getPanelSpec(panel) {
  return DISPLAY_PANEL_OPTIONS.find((p) => p.value === panel) || DISPLAY_PANEL_OPTIONS[0]
}

export function isEpdPanel(panel) {
  return String(panel || '').startsWith('epd_')
}

export function hasRedInk(panel) {
  return getPanelSpec(panel).hasRed
}

export function getPanelDimensions(panel, rotation = 0) {
  const spec = getPanelSpec(panel)
  const rot = Number(rotation) || 0
  if (rot === 90 || rot === 270) {
    return { w: spec.h, h: spec.w }
  }
  return { w: spec.w, h: spec.h }
}

export function stripRedPrefix(text) {
  const s = String(text ?? '')
  const m = s.match(/^\[(r|red)\]\s*(.*)$/i)
  if (m) return { isRedTag: true, text: m[2] }
  return { isRedTag: false, text: s }
}

export function isLineRed(form, idx) {
  if (!form || !hasRedInk(form.panel)) return false
  if (form.theme === 'mono') return false
  if ((Number(form.red_mask || 0) & (1 << idx)) !== 0) return true
  return stripRedPrefix(form.lines?.[idx]).isRedTag
}

export function createDefaultDisplayConfig(pcMonitor = true) {
  return {
    mode: pcMonitor ? 'monitor' : 'status',
    backlight: 100,
    theme: pcMonitor ? 'cyber' : 'red_bar',
    panel: pcMonitor ? 'st7735_160' : 'epd_213_bwr',
    rotation: 0,
    invert: false,
    partial_refresh: true,
    refresh_sec: 0,
    red_mask: 0x40,
    pins: { mosi: -1, clk: -1, cs: -1, dc: -1, rst: -1, busy: -1 },
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
  const validPanel = DISPLAY_PANEL_OPTIONS.some((p) => p.value === raw.panel)
  const bl = Number(raw.backlight)
  const rotNum = Number(raw.rotation)
  const validRot = [0, 90, 180, 270].includes(rotNum) ? rotNum : base.rotation
  const rsec = Number(raw.refresh_sec)
  const rmask = Number(raw.red_mask)
  const rawPins = raw.pins && typeof raw.pins === 'object' ? raw.pins : {}
  const pinVal = (k) => {
    const n = Number(rawPins[k])
    return Number.isFinite(n) && n >= -1 && n <= 48 ? Math.round(n) : -1
  }
  const lines = Array.from({ length: 6 }, (_, i) => {
    const arrVal = Array.isArray(raw.lines) ? raw.lines[i] : undefined
    const lineKey = raw[`line${i}`]
    return String(arrVal !== undefined ? arrVal : (lineKey ?? base.lines[i] ?? ''))
  })
  return {
    mode: validMode ? raw.mode : base.mode,
    backlight: Number.isFinite(bl) ? Math.min(100, Math.max(0, Math.round(bl))) : base.backlight,
    theme: validTheme ? raw.theme : base.theme,
    panel: validPanel ? raw.panel : base.panel,
    rotation: validRot,
    invert: raw.invert !== undefined ? Boolean(raw.invert) : base.invert,
    partial_refresh: raw.partial_refresh !== undefined ? Boolean(raw.partial_refresh) : base.partial_refresh,
    refresh_sec: Number.isFinite(rsec) ? Math.min(86400, Math.max(0, Math.round(rsec))) : base.refresh_sec,
    red_mask: Number.isFinite(rmask) ? (Math.round(rmask) & 0xff) : base.red_mask,
    pins: {
      mosi: pinVal('mosi'),
      clk: pinVal('clk'),
      cs: pinVal('cs'),
      dc: pinVal('dc'),
      rst: pinVal('rst'),
      busy: pinVal('busy'),
    },
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

export function buildDisplayPayload(cfg, extras = null) {
  const n = normalizeDisplayConfig(cfg)
  const payload = {
    mode: n.mode,
    backlight: n.backlight,
    theme: n.theme,
    panel: n.panel,
    rotation: n.rotation,
    invert: n.invert,
    partial_refresh: n.partial_refresh,
    refresh_sec: n.refresh_sec,
    red_mask: n.red_mask,
    pins: { ...n.pins },
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
  if (extras && typeof extras === 'object') {
    Object.assign(payload, extras)
  }
  return payload
}

export function clipScreenText(text, maxChars = 10) {
  return String(text || '').slice(0, maxChars)
}
