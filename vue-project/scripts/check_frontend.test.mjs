import test from 'node:test'
import assert from 'node:assert/strict'
import { resolveApiBase, resolveWsBase, buildWsUrl } from '../src/utils/network.js'

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
