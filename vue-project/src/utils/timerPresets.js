export const SCHEDULE_TYPE_OPTIONS = [
  { value: 'countdown', label: '倒计时一次', desc: '延迟 N 秒后自动执行一次' },
  { value: 'interval', label: '固定间隔循环', desc: '每隔 N 秒重复触发执行' },
  { value: 'daily', label: '每日/星期定时', desc: '每天或指定星期几的 HH:MM 触发' },
  { value: 'once', label: '指定日期时刻', desc: '在精确日期时间触发一次' },
]

export const ACTION_TYPE_OPTIONS = [
  { value: 'gpio_set', label: 'GPIO 引脚开关 / PWM', requires: 'supportsGpio' },
  { value: 'display_config', label: '屏幕显示模式 / 背光', requires: 'supportsDisplay' },
  { value: 'hid_transport', label: 'HID 传输通道切换', requires: 'supportsTransport' },
  { value: 'macro', label: '触发自动化宏', requires: 'supportsHid' },
  { value: 'reboot', label: '软重启设备', requires: null },
]

// Matches Python datetime.weekday() (0=Monday .. 6=Sunday)
export const WEEKDAY_OPTIONS = [
  { value: 0, label: '周一', short: '一' },
  { value: 1, label: '周二', short: '二' },
  { value: 2, label: '周三', short: '三' },
  { value: 3, label: '周四', short: '四' },
  { value: 4, label: '周五', short: '五' },
  { value: 5, label: '周六', short: '六' },
  { value: 6, label: '周日', short: '日' },
]

export const GPIO_OP_OPTIONS = [
  { value: 'on', label: '置高开启 (ON)' },
  { value: 'off', label: '置低关闭 (OFF)' },
  { value: 'toggle', label: '电平翻转 (TOGGLE)' },
  { value: 'pwm', label: 'PWM 调光/占空比' },
]

export function clampInt(val, min, max, fallback = min) {
  const n = Number(val)
  if (!Number.isFinite(n)) return fallback
  return Math.min(max, Math.max(min, Math.round(n)))
}

export function getAllowedActionOptions(capFlags = null) {
  if (!capFlags) return ACTION_TYPE_OPTIONS
  return ACTION_TYPE_OPTIONS.filter((opt) => !opt.requires || Boolean(capFlags[opt.requires]))
}

export function createDefaultTimerForm(deviceUuid = '', capFlags = null, seed = null) {
  const allowed = getAllowedActionOptions(capFlags)
  const fallbackAction = allowed[0]?.value || 'reboot'
  const raw = seed && typeof seed === 'object' ? seed : {}
  const ap = raw.action_payload && typeof raw.action_payload === 'object' ? raw.action_payload : {}

  const actionType = allowed.some((o) => o.value === raw.action_type)
    ? raw.action_type
    : fallbackAction

  const runAtSec = Number(raw.next_run_at || 0)
  const defaultOnceDate = runAtSec > 0
    ? new Date(runAtSec * 1000)
    : new Date(Date.now() + 5 * 60 * 1000)

  return {
    id: raw.id || null,
    device_uuid: raw.device_uuid || deviceUuid || '',
    name: raw.name || '',
    enabled: raw.enabled !== undefined ? Boolean(raw.enabled) : true,
    schedule_type: ['countdown', 'interval', 'daily', 'once'].includes(raw.schedule_type)
      ? raw.schedule_type
      : 'countdown',
    delay_sec: clampInt(raw.delay_sec ?? 30, 1, 2592000, 30),
    interval_sec: clampInt(raw.interval_sec ?? 60, 5, 2592000, 60),
    time_hhmm: typeof raw.time_hhmm === 'string' && /^\d{2}:\d{2}$/.test(raw.time_hhmm)
      ? raw.time_hhmm
      : '08:30',
    weekdays: Array.isArray(raw.weekdays) ? [...raw.weekdays] : [],
    once_at_ms: defaultOnceDate.getTime(),
    action_type: actionType,
    // Action-specific sub-fields
    gpio_pin: clampInt(ap.pin ?? 12, 0, 48, 12),
    gpio_op: ['on', 'off', 'toggle', 'pwm'].includes(ap.op) ? ap.op : 'on',
    gpio_duty: clampInt(ap.duty ?? 50, 0, 100, 50),
    gpio_duration_sec: clampInt(ap.duration_sec ?? 0, 0, 86400, 0),
    display_mode: ap.mode || 'monitor',
    display_backlight: clampInt(ap.backlight ?? 80, 0, 100, 80),
    display_text: Array.isArray(ap.lines) ? ap.lines.join('\n') : '',
    transport_mode: ['auto', 'usb', 'ble'].includes(ap.mode) ? ap.mode : 'auto',
    macro_id: ap.macro_id || null,
  }
}

export function buildTimerPayload(form) {
  const stype = form.schedule_type
  const payload = {
    device_uuid: String(form.device_uuid || '').trim(),
    name: String(form.name || '').trim() || defaultTimerTitle(form),
    enabled: Boolean(form.enabled),
    schedule_type: stype,
    delay_sec: stype === 'countdown' ? clampInt(form.delay_sec, 1, 2592000, 30) : 0,
    interval_sec: stype === 'interval' ? clampInt(form.interval_sec, 5, 2592000, 60) : 0,
    run_at: stype === 'once' ? Math.floor(Number(form.once_at_ms || Date.now() + 60000) / 1000) : null,
    time_hhmm: stype === 'daily' ? String(form.time_hhmm || '08:30') : '',
    weekdays: stype === 'daily' && Array.isArray(form.weekdays)
      ? [...new Set(form.weekdays.map(Number).filter((d) => d >= 0 && d <= 6))].sort()
      : [],
    action_type: form.action_type,
    action_payload: buildActionPayload(form),
  }
  return payload
}

export function buildActionPayload(form) {
  switch (form.action_type) {
    case 'gpio_set': {
      const out = {
        pin: clampInt(form.gpio_pin, 0, 48, 0),
        op: form.gpio_op || 'on',
      }
      if (out.op === 'pwm') {
        out.duty = clampInt(form.gpio_duty, 0, 100, 50)
      }
      const dur = clampInt(form.gpio_duration_sec, 0, 86400, 0)
      if (dur > 0 && out.op !== 'off') {
        out.duration_sec = dur
      }
      return out
    }
    case 'display_config': {
      const cfg = {
        mode: form.display_mode || 'monitor',
        backlight: clampInt(form.display_backlight, 0, 100, 80),
        refresh: true,
      }
      if (cfg.mode === 'text' && String(form.display_text || '').trim()) {
        cfg.lines = String(form.display_text)
          .split(/\r?\n/)
          .slice(0, 6)
          .map((s) => s.slice(0, 20))
      }
      return cfg
    }
    case 'hid_transport':
      return { mode: form.transport_mode || 'auto' }
    case 'macro':
      return { macro_id: form.macro_id ? Number(form.macro_id) : null }
    case 'reboot':
    default:
      return {}
  }
}

export function defaultTimerTitle(form) {
  if (form.action_type === 'gpio_set') {
    const dur = Number(form.gpio_duration_sec || 0)
    const opLabel = form.gpio_op === 'pwm' ? `PWM ${form.gpio_duty}%` : (form.gpio_op || 'on').toUpperCase()
    return `GPIO ${form.gpio_pin} ${opLabel}${dur > 0 ? ` (${dur}s)` : ''}`
  }
  if (form.action_type === 'display_config') {
    return `屏幕 ${form.display_mode === 'off' ? '息屏' : `${form.display_mode} ${form.display_backlight}%`}`
  }
  if (form.action_type === 'hid_transport') {
    return `HID 切换 ${String(form.transport_mode || 'auto').toUpperCase()}`
  }
  if (form.action_type === 'macro') {
    return `定时执行宏 #${form.macro_id || '?'}`
  }
  return '定时软重启'
}

export function formatScheduleSummary(timer) {
  if (!timer) return '—'
  if (timer.schedule_type === 'countdown') {
    return `倒计时 ${formatDuration(timer.delay_sec || 0)}`
  }
  if (timer.schedule_type === 'interval') {
    return `每 ${formatDuration(timer.interval_sec || 0)} 循环`
  }
  if (timer.schedule_type === 'daily') {
    const days = Array.isArray(timer.weekdays) && timer.weekdays.length
      ? `周${timer.weekdays.map((d) => WEEKDAY_OPTIONS.find((w) => w.value === d)?.short ?? d).join('、')}`
      : '每天'
    return `${days} ${timer.time_hhmm || '00:00'}`
  }
  if (timer.schedule_type === 'once') {
    return timer.next_run_at ? `单次 ${formatEpoch(timer.next_run_at)}` : '单次指定时刻'
  }
  return timer.schedule_type
}

export function formatActionSummary(timer, macrosMap = null) {
  if (!timer) return '—'
  const ap = timer.action_payload || {}
  if (timer.action_type === 'gpio_set') {
    const op = String(ap.op || (ap.duty !== undefined ? 'pwm' : ap.val ? 'on' : 'off')).toUpperCase()
    const extra = op === 'PWM' ? ` ${ap.duty ?? 0}%` : ''
    const pulse = Number(ap.duration_sec || 0) > 0 ? ` · ${ap.duration_sec}s 后复原` : ''
    return `GPIO #${ap.pin ?? '?'} -> ${op}${extra}${pulse}`
  }
  if (timer.action_type === 'display_config') {
    return `屏幕 -> ${ap.mode || 'monitor'}${ap.backlight !== undefined ? ` (${ap.backlight}%)` : ''}`
  }
  if (timer.action_type === 'hid_transport') {
    return `HID 传输 -> ${String(ap.mode || 'auto').toUpperCase()}`
  }
  if (timer.action_type === 'macro') {
    const mName = macrosMap && ap.macro_id ? macrosMap[ap.macro_id]?.name : ''
    return mName ? `执行宏: ${mName}` : `执行宏 #${ap.macro_id ?? '?'}`
  }
  if (timer.action_type === 'reboot') {
    return '硬件软重启'
  }
  return timer.action_type
}

export function formatDuration(totalSec) {
  const sec = Math.max(0, Math.round(Number(totalSec || 0)))
  if (sec < 60) return `${sec} 秒`
  if (sec < 3600) {
    const m = Math.floor(sec / 60)
    const s = sec % 60
    return s ? `${m} 分 ${s} 秒` : `${m} 分钟`
  }
  const h = Math.floor(sec / 3600)
  const m = Math.floor((sec % 3600) / 60)
  return m ? `${h} 小时 ${m} 分` : `${h} 小时`
}

export function formatCountdown(nextRunAt, nowSec = Math.floor(Date.now() / 1000)) {
  if (!nextRunAt) return '未排程'
  const diff = Number(nextRunAt) - Number(nowSec)
  if (diff <= 0) return '即将执行'
  return `${formatDuration(diff)} 后`
}

export function formatEpoch(epochSec) {
  if (!epochSec) return '—'
  const d = new Date(Number(epochSec) * 1000)
  if (Number.isNaN(d.getTime())) return '—'
  const pad = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
}
