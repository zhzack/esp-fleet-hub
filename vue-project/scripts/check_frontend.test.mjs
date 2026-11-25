import test from 'node:test'
import assert from 'node:assert/strict'
import { resolveApiBase, resolveWsBase, buildWsUrl } from '../src/utils/network.js'
import { getCapabilityFlags } from '../src/utils/capabilities.js'

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

  const unreported = getCapabilityFlags([])
  assert.equal(unreported.supportsBle, true)
  assert.equal(unreported.supportsTransport, true)
})
