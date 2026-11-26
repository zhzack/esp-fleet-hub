import test from 'node:test'
import assert from 'node:assert/strict'
import { resolveApiBase, resolveWsBase, buildWsUrl } from '../src/utils/network.js'
import { getCapabilityFlags } from '../src/utils/capabilities.js'
import {
  buildDisplayPayload,
  getPanelDimensions,
  hasRedInk,
  isEpdPanel,
  isLineRed,
  normalizeDisplayConfig,
} from '../src/utils/displayPresets.js'
import {
  buildTimerPayload,
  createDefaultTimerForm,
  formatActionSummary,
  formatCountdown,
  formatScheduleSummary,
  getAllowedActionOptions,
} from '../src/utils/timerPresets.js'

test('resolveApiBase uses backend port on loopback dev server and relative /api in prod', () => {
  const devLoc = { hostname: 'localhost', port: '5173', protocol: 'http:', host: 'localhost:5173' }
  assert.equal(resolveApiBase({}, devLoc), 'http://localhost:8000/api')

  const prodLoc = { hostname: 'hub.example.org', port: '', protocol: 'https:', host: 'hub.example.org' }
  assert.equal(resolveApiBase({}, prodLoc), '/api')
  assert.equal(resolveApiBase({ VITE_API_BASE: 'https://api.example.org/api' }, prodLoc), 'https://api.example.org/api')
})

test('resolveWsBase and buildWsUrl upgrade to wss on HTTPS and URL-encode tokens', () => {
  const prodHttps = { hostname: 'hub.example.org', port: '', protocol: 'https:', host: 'hub.example.org' }
  assert.equal(resolveWsBase({}, prodHttps), 'wss://hub.example.org')
  assert.equal(
    buildWsUrl('/ws/client', 'a+b=c/d', {}, prodHttps),
    'wss://hub.example.org/ws/client?token=a%2Bb%3Dc%2Fd',
  )
})

test('getCapabilityFlags hides BLE/transport for C3 non-HID nodes and enables appropriately for S3/BLE', () => {
  const c3 = getCapabilityFlags(['gpio', 'display', 'ota'])
  assert.equal(c3.hasBle, false)
  assert.equal(c3.supportsBle, false)
  assert.equal(c3.supportsTransport, false)
  assert.equal(c3.supportsTrackpad, false)

  const s3UsbOnly = getCapabilityFlags(['usb_keyboard', 'usb_mouse', 'gpio', 'display', 'ota'])
  assert.equal(s3UsbOnly.hasBle, false)
  assert.equal(s3UsbOnly.supportsBle, false)
  assert.equal(s3UsbOnly.supportsTransport, true)
  assert.equal(s3UsbOnly.supportsTrackpad, true)

  const dualHid = getCapabilityFlags(['usb_keyboard', 'ble_keyboard', 'ble_mouse'])
  assert.equal(dualHid.supportsBle, true)
  assert.equal(dualHid.supportsTransport, true)
  assert.equal(dualHid.supportsTrackpad, true)

  const c3Mon = getCapabilityFlags(['gpio', 'display', 'pc_monitor', 'joystick', 'ota'])
  assert.equal(c3Mon.hasDisplay, true)
  assert.equal(c3Mon.supportsDisplay, true)

  const headless = getCapabilityFlags(['gpio', 'ota'])
  assert.equal(headless.hasDisplay, false)
  assert.equal(headless.supportsDisplay, false)

  const unreported = getCapabilityFlags([])
  assert.equal(unreported.supportsBle, true)
  assert.equal(unreported.supportsTransport, true)
  assert.equal(unreported.supportsDisplay, true)
})

test('displayPresets normalizeDisplayConfig and buildDisplayPayload clamp and structure fields', () => {
  const norm = normalizeDisplayConfig({
    mode: 'text',
    backlight: 150,
    theme: 'emerald',
    header_text: 'DESK-C3',
    show_disk: false,
    lines: ['A', 'B'],
  })
  assert.equal(norm.mode, 'text')
  assert.equal(norm.backlight, 100)
  assert.equal(norm.theme, 'emerald')
  assert.equal(norm.show_disk, false)
  assert.equal(norm.lines.length, 6)
  assert.equal(norm.lines[0], 'A')
  assert.equal(norm.lines[2], '')

  const payload = buildDisplayPayload({ ...norm, backlight: -5 })
  assert.equal(payload.backlight, 0)
  assert.equal(payload.header_text, 'DESK-C3')
  assert.equal(payload.lines.length, 6)
})

test('getCapabilityFlags supportsGpio & timerPresets helpers shape schedules/actions', () => {
  const c3 = getCapabilityFlags(['gpio', 'display'])
  assert.equal(c3.hasGpio, true)
  assert.equal(c3.supportsGpio, true)

  const displayOnly = getCapabilityFlags(['display'])
  assert.equal(displayOnly.hasGpio, false)
  assert.equal(displayOnly.supportsGpio, false)
  assert.equal(getCapabilityFlags([]).supportsGpio, true)

  const allowedDisplayOnly = getAllowedActionOptions(displayOnly).map((o) => o.value)
  assert.equal(allowedDisplayOnly.includes('gpio_set'), false)
  assert.equal(allowedDisplayOnly.includes('display_config'), true)
  assert.equal(allowedDisplayOnly.includes('reboot'), true)

  const form = createDefaultTimerForm('esp32c3-72f320', c3, {
    schedule_type: 'countdown',
    delay_sec: 15,
    action_type: 'gpio_set',
    action_payload: { pin: 12, op: 'on', duration_sec: 5 },
  })
  const built = buildTimerPayload(form)
  assert.equal(built.device_uuid, 'esp32c3-72f320')
  assert.equal(built.delay_sec, 15)
  assert.deepEqual(built.action_payload, { pin: 12, op: 'on', duration_sec: 5 })
  assert.match(formatScheduleSummary(built), /15 秒/)
  assert.match(formatActionSummary(built), /GPIO #12 -> ON · 5s 后复原/)
  assert.equal(formatCountdown(105, 100), '5 秒 后')
})

test('epd presets support 2.13 BW, 2.13 BWR, and 4.2 BWR dimensions, rotation, and red highlights', () => {
  assert.equal(isEpdPanel('epd_213_bw'), true)
  assert.equal(hasRedInk('epd_213_bw'), false)
  assert.equal(hasRedInk('epd_213_bwr'), true)
  assert.equal(hasRedInk('epd_420_bwr'), true)
  assert.equal(isEpdPanel('st7789_240'), false)

  assert.deepEqual(getPanelDimensions('epd_213_bwr', 0), { w: 122, h: 250 })
  assert.deepEqual(getPanelDimensions('epd_213_bwr', 90), { w: 250, h: 122 })
  assert.deepEqual(getPanelDimensions('epd_420_bwr', 0), { w: 400, h: 300 })
  assert.deepEqual(getPanelDimensions('epd_420_bwr', 270), { w: 300, h: 400 })

  const s3Default = normalizeDisplayConfig({
    panel: 'epd_420_bwr',
    rotation: 90,
    invert: true,
    refresh_sec: 120,
    red_mask: 0x42,
    pins: { clk: 9, mosi: 10, rst: 11, dc: 12, cs: 13, busy: 14 },
    lines: ['Header', '[R]Alert', 'Normal', '', '', ''],
  }, false)
  assert.equal(s3Default.panel, 'epd_420_bwr')
  assert.equal(s3Default.rotation, 90)
  assert.equal(s3Default.pins.clk, 9)
  assert.equal(s3Default.pins.busy, 14)
  assert.equal(isLineRed(s3Default, 1), true)
  assert.equal(isLineRed(s3Default, 2), false)

  const epdPayload = buildDisplayPayload(s3Default, { full_refresh: true })
  assert.equal(epdPayload.panel, 'epd_420_bwr')
  assert.equal(epdPayload.full_refresh, true)
  assert.equal(epdPayload.red_mask, 0x42)
})
