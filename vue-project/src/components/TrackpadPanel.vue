<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { ElMessage } from 'element-plus'

import { actionsApi } from '@/api/actions'
import { macrosApi } from '@/api/macros'
import { useWs } from '@/composables/useWs'
import { useAuthStore } from '@/stores/auth'

const props = defineProps({
  deviceUuid: { type: String, default: '' },
  deviceOnline: { type: Boolean, default: false },
  fullscreen: { type: Boolean, default: false },
  minimal: { type: Boolean, default: false },
  immersive: { type: Boolean, default: false },
})

const emit = defineEmits(['result'])

const auth = useAuthStore()

const MOVE_FLUSH_MS = 10
const SCROLL_FLUSH_MS = 14
const STREAM_RETRY_MS = 4
const STREAM_HTTP_MAX_INFLIGHT = 3
const SCROLL_PIXELS_PER_STEP = 22
const TAP_DISTANCE = 18
const TAP_MAX_MS = 320
const SWIPE_MIN_DISTANCE = 56
const STATUS_HINT = 'One finger moves the pointer, one tap clicks left, two fingers scroll, and multi-finger gestures can trigger defaults or macros.'

const POINTER_PROFILES = [
  {
    id: 'linear',
    label: 'Linear',
    description: 'Fixed ratio, no acceleration.',
    baseScale: 1.15,
    accelPerPixel: 0,
    maxScale: 1.15,
  },
  {
    id: 'balanced',
    label: 'Balanced',
    description: 'Default. Short swipes stay precise, long swipes speed up.',
    baseScale: 1.02,
    accelPerPixel: 0.024,
    maxScale: 2.2,
  },
  {
    id: 'strong',
    label: 'Strong',
    description: 'Higher acceleration for large displays and longer throws.',
    baseScale: 0.96,
    accelPerPixel: 0.04,
    maxScale: 3.0,
  },
]

const GESTURE_DEFS = [
  {
    id: 'two_finger_tap',
    label: 'Two-finger tap',
    defaultLabel: 'Right click',
    defaultPayload: { type: 'mouse_click', button: 2 },
  },
  {
    id: 'three_finger_tap',
    label: 'Three-finger tap',
    defaultLabel: 'Middle click',
    defaultPayload: { type: 'mouse_click', button: 4 },
  },
  {
    id: 'three_finger_swipe_left',
    label: 'Three-finger swipe left',
    defaultLabel: 'Previous app',
    defaultPayload: { type: 'keyboard_tap', key: 'KEY_ALT_SHIFT_TAB' },
  },
  {
    id: 'three_finger_swipe_right',
    label: 'Three-finger swipe right',
    defaultLabel: 'Next app',
    defaultPayload: { type: 'keyboard_tap', key: 'KEY_ALT_TAB' },
  },
  {
    id: 'three_finger_swipe_up',
    label: 'Three-finger swipe up',
    defaultLabel: 'Task view',
    defaultPayload: { type: 'keyboard_tap', key: 'KEY_GUI_TAB' },
  },
  {
    id: 'three_finger_swipe_down',
    label: 'Three-finger swipe down',
    defaultLabel: 'Show desktop',
    defaultPayload: { type: 'keyboard_tap', key: 'KEY_GUI_D' },
  },
  {
    id: 'four_finger_swipe_left',
    label: 'Four-finger swipe left',
    defaultLabel: 'Previous desktop',
    defaultPayload: { type: 'keyboard_tap', key: 'KEY_GUI_CTRL_LEFT' },
  },
  {
    id: 'four_finger_swipe_right',
    label: 'Four-finger swipe right',
    defaultLabel: 'Next desktop',
    defaultPayload: { type: 'keyboard_tap', key: 'KEY_GUI_CTRL_RIGHT' },
  },
]

function userStorageKey() {
  const suffix = auth.user?.id ?? auth.user?.username ?? 'anonymous'
  return `hid-gateway:trackpad-bindings:${suffix}`
}

function loadBindings() {
  try {
    const raw = localStorage.getItem(userStorageKey())
    if (!raw) return {}
    const parsed = JSON.parse(raw)
    return parsed && typeof parsed === 'object' ? parsed : {}
  } catch {
    return {}
  }
}

function hintStorageKey() {
  const suffix = auth.user?.id ?? auth.user?.username ?? 'anonymous'
  return `hid-gateway:trackpad-hint-dismissed:${suffix}`
}

function loadHintDismissed() {
  try {
    return localStorage.getItem(hintStorageKey()) === '1'
  } catch {
    return false
  }
}

function prefsStorageKey() {
  const suffix = auth.user?.id ?? auth.user?.username ?? 'anonymous'
  return `hid-gateway:trackpad-prefs:${suffix}`
}

function loadPrefs() {
  try {
    const raw = localStorage.getItem(prefsStorageKey())
    if (!raw) return { pointerProfile: 'balanced' }
    const parsed = JSON.parse(raw)
    if (!parsed || typeof parsed !== 'object') return { pointerProfile: 'balanced' }
    return {
      pointerProfile: typeof parsed.pointerProfile === 'string' ? parsed.pointerProfile : 'balanced',
    }
  } catch {
    return { pointerProfile: 'balanced' }
  }
}

const settingsVisible = ref(false)
const macros = ref([])
const macrosLoading = ref(false)
const activeTouches = ref(0)
const status = reactive({ text: STATUS_HINT, type: 'info' })
const bindings = reactive(loadBindings())
const hintDismissed = ref(loadHintDismissed())
const prefs = reactive(loadPrefs())

const moveQueue = reactive({ x: 0, y: 0, timer: 0 })
const scrollPixels = reactive({ x: 0, y: 0 })
const scrollQueue = reactive({ x: 0, y: 0, timer: 0 })
const streamState = reactive({ httpInflight: 0, packetsPerSecond: 0 })

const ws = useWs('/ws/client', { token: computed(() => auth.token || '') })
const packetSamples = []

const canControl = computed(() => !!props.deviceUuid && props.deviceOnline)
const streamTransport = computed(() => (ws.status.value === 'open' ? 'ws' : 'http'))
const activePointerProfile = computed(() => (
  POINTER_PROFILES.find((item) => item.id === prefs.pointerProfile) || POINTER_PROFILES[1]
))
const surfaceState = computed(() => {
  if (!props.deviceUuid) return 'Select an online device first.'
  if (!props.deviceOnline) return 'The selected device is offline.'
  return STATUS_HINT
})
const showSurfaceHint = computed(() => !canControl.value || !hintDismissed.value)
const showTouchCounter = computed(() => activeTouches.value > 0 && !props.immersive)
const cardBodyStyle = computed(() => (
  (props.fullscreen || props.minimal)
    ? {
      display: 'flex',
      flexDirection: 'column',
      flex: '1 1 auto',
      minHeight: '0',
      padding: props.minimal ? '0' : undefined,
    }
    : undefined
))

const gestureRows = computed(() =>
  GESTURE_DEFS.map((def) => ({
    ...def,
    macroId: bindings[def.id] ?? '',
    macroName: macros.value.find((row) => String(row.id) === String(bindings[def.id]))?.name || '',
  })),
)

function saveBindings() {
  localStorage.setItem(userStorageKey(), JSON.stringify(bindings))
}

function dismissHint() {
  if (hintDismissed.value) return
  hintDismissed.value = true
  try {
    localStorage.setItem(hintStorageKey(), '1')
  } catch {
    // ignore
  }
}

watch(bindings, saveBindings, { deep: true })

watch(
  prefs,
  (value) => {
    try {
      localStorage.setItem(prefsStorageKey(), JSON.stringify(value))
    } catch {
      // ignore
    }
  },
  { deep: true },
)

async function loadMacros() {
  macrosLoading.value = true
  try {
    macros.value = await macrosApi.list()
  } catch (error) {
    macros.value = []
    ElMessage.warning(error.message || 'Failed to load macros.')
  } finally {
    macrosLoading.value = false
  }
}

function setStatus(text, type = 'info') {
  status.text = text
  status.type = type
}

function clampAxis(value) {
  return Math.max(-127, Math.min(127, value))
}

function takeQueuedAxis(value) {
  const integer = value > 0 ? Math.floor(value) : Math.ceil(value)
  return clampAxis(integer)
}

async function sendPayload(payload, { silent = false, record = true, timeoutMs = 1500, needAck = false } = {}) {
  if (!props.deviceUuid || !props.deviceOnline) {
    if (!silent) ElMessage.warning('Select an online device first.')
    return null
  }

  try {
    const response = await actionsApi.send({
      device_id: props.deviceUuid,
      payload: {
        ...payload,
        need_ack: needAck,
      },
      timeout_ms: timeoutMs,
    })
    const result = response.results?.[0]
    if (record) emit('result', response)

    if (result?.status === 'acked' || result?.status === 'queued') {
      return response
    }

    const detail = `${result?.status || 'failed'} ${result?.error_message || ''}`.trim()
    setStatus(detail, 'error')
    if (!silent) ElMessage.warning(detail)
    return null
  } catch (error) {
    setStatus(error.message || 'Network error', 'error')
    if (!silent) ElMessage.error(error.message || 'Network error')
    return null
  }
}

function makeMsgId() {
  if (window.crypto?.randomUUID) return window.crypto.randomUUID()
  return `trackpad-${Date.now()}-${Math.random().toString(36).slice(2, 10)}`
}

function markStreamPacket() {
  const now = performance.now()
  packetSamples.push(now)
  refreshPacketRate(now)
}

function refreshPacketRate(now = performance.now()) {
  while (packetSamples.length && packetSamples[0] < now - 1000) packetSamples.shift()
  streamState.packetsPerSecond = packetSamples.length
}

function sendWsPayload(payload) {
  if (!props.deviceUuid || !props.deviceOnline) return false
  if (ws.status.value !== 'open') return false
  const ok = ws.send({
    header: {
      msg_id: makeMsgId(),
      type: 'action',
      source: 'web-trackpad',
      timestamp: Math.floor(Date.now() / 1000),
      timeout_ms: 250,
    },
    routing: {
      target_devices: [props.deviceUuid],
      broadcast: false,
      group: null,
    },
    payload,
  })
  if (ok) markStreamPacket()
  return ok
}

function hasQueuedMove() {
  return Math.abs(moveQueue.x) >= 1 || Math.abs(moveQueue.y) >= 1
}

function hasQueuedScroll() {
  return Math.abs(scrollQueue.x) >= 1 || Math.abs(scrollQueue.y) >= 1
}

function pointerScaleForDelta(dx, dy) {
  const profile = activePointerProfile.value
  const distance = Math.hypot(dx, dy)
  if (profile.accelPerPixel <= 0) return profile.baseScale

  const boosted = profile.baseScale + (distance * profile.accelPerPixel)
  return Math.min(profile.maxScale, boosted)
}

function sendStreamPayload(payload) {
  if (sendWsPayload(payload)) return true
  if (streamState.httpInflight >= STREAM_HTTP_MAX_INFLIGHT) return false

  streamState.httpInflight += 1
  markStreamPacket()
  sendPayload(payload, { silent: true, record: false, timeoutMs: 700 })
    .finally(() => {
      streamState.httpInflight = Math.max(0, streamState.httpInflight - 1)
      if (hasQueuedMove()) scheduleMoveFlush(0)
      if (hasQueuedScroll()) scheduleScrollFlush(0)
    })
  return true
}

function clearTimer(id) {
  if (id) window.clearTimeout(id)
}

function scheduleMoveFlush(delay = MOVE_FLUSH_MS) {
  if (moveQueue.timer) return
  moveQueue.timer = window.setTimeout(() => {
    moveQueue.timer = 0
    flushMoveQueue()
  }, delay)
}

async function flushMoveQueue() {
  const x = takeQueuedAxis(moveQueue.x)
  const y = takeQueuedAxis(moveQueue.y)
  if (!x && !y) return

  if (!sendStreamPayload({ type: 'mouse_move', x, y })) {
    scheduleMoveFlush(STREAM_RETRY_MS)
    return
  }

  moveQueue.x -= x
  moveQueue.y -= y
  if (hasQueuedMove()) {
    scheduleMoveFlush(0)
  }
}

function queueMove(dx, dy) {
  const scale = pointerScaleForDelta(dx, dy)
  moveQueue.x += dx * scale
  moveQueue.y += dy * scale
  scheduleMoveFlush()
}

function scheduleScrollFlush(delay = SCROLL_FLUSH_MS) {
  if (scrollQueue.timer) return
  scrollQueue.timer = window.setTimeout(() => {
    scrollQueue.timer = 0
    flushScrollQueue()
  }, delay)
}

async function flushScrollQueue() {
  const x = takeQueuedAxis(scrollQueue.x)
  const y = takeQueuedAxis(scrollQueue.y)
  if (!x && !y) return

  if (!sendStreamPayload({ type: 'mouse_scroll', x, y })) {
    scheduleScrollFlush(STREAM_RETRY_MS)
    return
  }

  scrollQueue.x -= x
  scrollQueue.y -= y
  if (hasQueuedScroll()) {
    scheduleScrollFlush(0)
  }
}

function queueScrollFromPixels(dx, dy) {
  scrollPixels.x += dx
  scrollPixels.y += dy

  const stepsX = scrollPixels.x > 0
    ? Math.floor(scrollPixels.x / SCROLL_PIXELS_PER_STEP)
    : Math.ceil(scrollPixels.x / SCROLL_PIXELS_PER_STEP)
  const stepsY = scrollPixels.y > 0
    ? Math.floor(scrollPixels.y / SCROLL_PIXELS_PER_STEP)
    : Math.ceil(scrollPixels.y / SCROLL_PIXELS_PER_STEP)

  if (!stepsX && !stepsY) return

  scrollPixels.x -= stepsX * SCROLL_PIXELS_PER_STEP
  scrollPixels.y -= stepsY * SCROLL_PIXELS_PER_STEP
  scrollQueue.x += stepsX
  scrollQueue.y += -stepsY
  scheduleScrollFlush()
}

function resetQueues() {
  moveQueue.x = 0
  moveQueue.y = 0
  scrollPixels.x = 0
  scrollPixels.y = 0
  scrollQueue.x = 0
  scrollQueue.y = 0
}

function normalizeMacroConfig(config = {}) {
  return {
    execution_mode: 'ble_targeted',
    action_timeout_ms: 10000,
    ...config,
  }
}

async function runMacroBinding(macro, gestureLabel) {
  const steps = Array.isArray(macro?.content_json?.steps) ? macro.content_json.steps : []
  if (!steps.length) {
    ElMessage.warning(`Macro "${macro?.name || 'unknown'}" has no steps.`)
    return
  }

  const config = normalizeMacroConfig(macro.content_json?.config)
  const payload = {
    type: 'macro',
    steps,
  }
  if (config.execution_mode === 'network_debug') {
    payload.debug_no_hid = true
    payload.skip_link_check = true
  }

  setStatus(`${gestureLabel} -> macro: ${macro.name}`, 'success')
  await sendPayload(payload, {
    silent: true,
    record: true,
    timeoutMs: config.action_timeout_ms || 10000,
    needAck: true,
  })
}

async function triggerGesture(gestureId) {
  const gesture = GESTURE_DEFS.find((item) => item.id === gestureId)
  if (!gesture || !canControl.value) return

  const macroId = bindings[gesture.id]
  const macro = macros.value.find((row) => String(row.id) === String(macroId))
  if (macro) {
    await runMacroBinding(macro, gesture.label)
    return
  }

  setStatus(`${gesture.label} -> ${gesture.defaultLabel}`, 'success')
  await sendPayload(gesture.defaultPayload, { silent: true, record: true, timeoutMs: 2000 })
}

async function triggerPrimaryTap() {
  setStatus('One-finger tap -> left click', 'success')
  await sendPayload({ type: 'mouse_click', button: 1 }, { silent: true, record: true, timeoutMs: 1200 })
}

async function clickButton(button, label) {
  setStatus(label, 'success')
  await sendPayload({ type: 'mouse_click', button }, { silent: true, record: true, timeoutMs: 1200 })
}

let session = null
const pointers = new Map()

function centroidFromPointers() {
  let x = 0
  let y = 0
  pointers.forEach((point) => {
    x += point.x
    y += point.y
  })
  const count = pointers.size || 1
  return { x: x / count, y: y / count }
}

function beginSession() {
  session = {
    startedAt: performance.now(),
    maxPointers: 0,
    startCentroid: null,
    prevCentroid: null,
    totalDx: 0,
    totalDy: 0,
    oneFingerMoved: false,
    twoFingerScrolled: false,
  }
}

function resetInteraction() {
  pointers.clear()
  activeTouches.value = 0
  session = null
  resetQueues()
}

function updateSessionCentroid() {
  if (!session || !pointers.size) return null
  const centroid = centroidFromPointers()
  if (!session.startCentroid) session.startCentroid = centroid
  session.prevCentroid ||= centroid
  session.totalDx = centroid.x - session.startCentroid.x
  session.totalDy = centroid.y - session.startCentroid.y
  return centroid
}

function finalizeGesture() {
  if (!session) return

  const elapsed = performance.now() - session.startedAt
  const fingers = session.maxPointers
  const distance = Math.hypot(session.totalDx, session.totalDy)
  const horizontal = Math.abs(session.totalDx)
  const vertical = Math.abs(session.totalDy)

  if (fingers === 1 && !session.oneFingerMoved && distance <= TAP_DISTANCE && elapsed <= TAP_MAX_MS) {
    triggerPrimaryTap()
  } else if (
    fingers === 2 &&
    !session.twoFingerScrolled &&
    distance <= TAP_DISTANCE &&
    elapsed <= TAP_MAX_MS
  ) {
    triggerGesture('two_finger_tap')
  } else if (fingers === 3 || fingers === 4) {
    let gestureId = ''
    if (distance <= TAP_DISTANCE && elapsed <= TAP_MAX_MS && fingers === 3) {
      gestureId = 'three_finger_tap'
    } else if (Math.max(horizontal, vertical) >= SWIPE_MIN_DISTANCE) {
      const direction = horizontal >= vertical
        ? (session.totalDx > 0 ? 'right' : 'left')
        : (session.totalDy > 0 ? 'down' : 'up')
      gestureId = `${fingers === 3 ? 'three' : 'four'}_finger_swipe_${direction}`
    }

    if (gestureId) triggerGesture(gestureId)
  }

  session = null
}

function updatePointer(event) {
  const point = pointers.get(event.pointerId)
  if (!point) return null
  point.prevX = point.x
  point.prevY = point.y
  point.x = event.clientX
  point.y = event.clientY
  return point
}

function onPointerDown(event) {
  if (!canControl.value) return
  event.preventDefault()
  dismissHint()

  if (!pointers.size) beginSession()

  const point = {
    x: event.clientX,
    y: event.clientY,
    prevX: event.clientX,
    prevY: event.clientY,
    startX: event.clientX,
    startY: event.clientY,
  }
  pointers.set(event.pointerId, point)
  activeTouches.value = pointers.size

  session.maxPointers = Math.max(session.maxPointers, pointers.size)
  updateSessionCentroid()
  event.currentTarget?.setPointerCapture?.(event.pointerId)
}

function onPointerMove(event) {
  if (!canControl.value || !session) return
  if (!pointers.has(event.pointerId)) return
  event.preventDefault()

  const point = updatePointer(event)
  if (!point) return

  const count = pointers.size
  const centroid = updateSessionCentroid()
  if (!centroid) return

  if (count === 1 && session.maxPointers === 1) {
    const dx = point.x - point.prevX
    const dy = point.y - point.prevY
    if (Math.hypot(point.x - point.startX, point.y - point.startY) > TAP_DISTANCE) {
      session.oneFingerMoved = true
    }
    if (dx || dy) queueMove(dx, dy)
    return
  }

  if (count === 2) {
    const prev = session.prevCentroid || centroid
    const dx = centroid.x - prev.x
    const dy = centroid.y - prev.y
    if (dx || dy) {
      session.twoFingerScrolled = session.twoFingerScrolled
        || Math.abs(session.totalDx) > 10
        || Math.abs(session.totalDy) > 10
      queueScrollFromPixels(dx, dy)
    }
  }

  session.prevCentroid = centroid
}

function finishPointer(event) {
  if (!pointers.has(event.pointerId)) return
  updatePointer(event)
  pointers.delete(event.pointerId)
  activeTouches.value = pointers.size

  if (!pointers.size) {
    finalizeGesture()
    resetQueues()
  } else {
    const centroid = updateSessionCentroid()
    if (centroid) session.prevCentroid = centroid
  }
}

function onPointerUp(event) {
  if (!session) return
  event.preventDefault()
  finishPointer(event)
}

function onPointerCancel(event) {
  if (!session) return
  finishPointer(event)
}

watch(
  () => [props.deviceUuid, props.deviceOnline],
  () => {
    resetInteraction()
    packetSamples.length = 0
    streamState.packetsPerSecond = 0
    setStatus(surfaceState.value, canControl.value ? 'info' : 'warning')
  },
  { immediate: true },
)

watch(
  () => ws.status.value,
  (value) => {
    if (value === 'open') {
      if (hasQueuedMove()) scheduleMoveFlush(0)
      if (hasQueuedScroll()) scheduleScrollFlush(0)
    }
  },
)

let packetTimer = null

onMounted(() => {
  loadMacros()
  packetTimer = window.setInterval(() => refreshPacketRate(), 250)
})

onBeforeUnmount(() => {
  clearTimer(moveQueue.timer)
  clearTimer(scrollQueue.timer)
  if (packetTimer) window.clearInterval(packetTimer)
  resetInteraction()
})
</script>

<template>
  <el-card class="trackpad-card" :class="{ fullscreen, minimal }" :body-style="cardBodyStyle">
    <template v-if="!minimal" #header>
      <div class="trackpad-header">
        <div>
          <div class="trackpad-title">Touchpad</div>
          <div class="trackpad-subtitle">
            Mouse move, real wheel scroll, Windows-style gesture defaults, and macro bindings.
            The device can now emit real mouse HID over BLE or USB.
          </div>
        </div>
        <div class="trackpad-actions">
          <el-button size="small" @click="loadMacros" :loading="macrosLoading">Refresh Macros</el-button>
          <el-button size="small" type="primary" @click="settingsVisible = true">Touchpad Settings</el-button>
        </div>
      </div>
    </template>

    <div class="trackpad-body" :class="{ fullscreen }">
      <div
        class="trackpad-surface"
        :class="{ disabled: !canControl, active: activeTouches > 0 }"
        @pointerdown="onPointerDown"
        @pointermove="onPointerMove"
        @pointerup="onPointerUp"
        @pointercancel="onPointerCancel"
        @contextmenu.prevent
      >
        <div v-if="showSurfaceHint" class="surface-hint">
          <div class="surface-title">{{ canControl ? 'Touch Surface' : 'Unavailable' }}</div>
          <div class="surface-text">{{ surfaceState }}</div>
        </div>
        <el-tag v-if="showTouchCounter" size="small" type="success" class="touch-counter">
          {{ activeTouches }} finger{{ activeTouches > 1 ? 's' : '' }}
        </el-tag>
      </div>

      <div v-if="!minimal" class="trackpad-toolbar">
        <el-button-group>
          <el-button @click="clickButton(1, 'Left click')" :disabled="!canControl">Left</el-button>
          <el-button @click="clickButton(2, 'Right click')" :disabled="!canControl">Right</el-button>
          <el-button @click="clickButton(4, 'Middle click')" :disabled="!canControl">Middle</el-button>
        </el-button-group>
        <div class="toolbar-meta">
          <el-tag size="small" :type="streamTransport === 'ws' ? 'success' : 'info'">
            {{ streamTransport.toUpperCase() }}
          </el-tag>
          <span class="toolbar-note">
            Stream ~{{ streamState.packetsPerSecond }}/s, inflight {{ streamState.httpInflight }}.
          </span>
        </div>
      </div>

      <el-alert
        v-if="!minimal"
        :title="status.text"
        :type="status.type === 'error' ? 'error' : (status.type === 'success' ? 'success' : 'info')"
        :closable="false"
        show-icon
        style="margin-top: 12px"
      />

      <div v-if="!fullscreen" class="gesture-summary">
        <div class="summary-item"><strong>1 finger</strong><span>Move pointer, tap for left click.</span></div>
        <div class="summary-item"><strong>2 fingers</strong><span>Scroll, tap for right click.</span></div>
        <div class="summary-item"><strong>3 fingers</strong><span>Tap for middle click, swipe for app/task actions.</span></div>
        <div class="summary-item"><strong>4 fingers</strong><span>Swipe left or right to switch desktops.</span></div>
      </div>
    </div>
  </el-card>

  <el-dialog v-model="settingsVisible" title="Touchpad Settings" width="860px">
    <el-alert
      type="info"
      :closable="false"
      title="Pointer profile and gesture bindings are stored locally for the current signed-in browser user."
      style="margin-bottom: 16px"
    />

    <el-card shadow="never" class="settings-card">
      <div class="settings-title">Pointer Motion</div>
      <div class="settings-copy">
        Choose how one-finger movement accelerates. Pure and kiosk pages use the same saved profile.
      </div>
      <el-radio-group v-model="prefs.pointerProfile" class="profile-group">
        <el-radio-button
          v-for="profile in POINTER_PROFILES"
          :key="profile.id"
          :label="profile.id"
        >
          {{ profile.label }}
        </el-radio-button>
      </el-radio-group>
      <div class="settings-meta">
        {{ activePointerProfile.label }}: {{ activePointerProfile.description }}
      </div>
    </el-card>

    <el-table :data="gestureRows" size="small" v-loading="macrosLoading">
      <el-table-column prop="label" label="Gesture" min-width="200" />
      <el-table-column prop="defaultLabel" label="Default Action" min-width="180" />
      <el-table-column label="Macro Override" min-width="280">
        <template #default="{ row }">
          <el-select v-model="bindings[row.id]" clearable filterable placeholder="Use default" style="width: 100%">
            <el-option
              v-for="macro in macros"
              :key="macro.id"
              :label="macro.name"
              :value="macro.id"
            />
          </el-select>
        </template>
      </el-table-column>
    </el-table>

    <template #footer>
      <el-button @click="settingsVisible = false">Close</el-button>
    </template>
  </el-dialog>
</template>

<style scoped>
.trackpad-card {
  min-height: 100%;
}

.trackpad-card.fullscreen {
  height: 100%;
  display: flex;
  flex-direction: column;
}

.trackpad-card.minimal {
  height: 100%;
  border: 0;
  background: transparent;
  box-shadow: none;
}

.trackpad-card.immersive {
  height: 100%;
  border: 0;
  background: transparent;
  box-shadow: none;
}

.trackpad-body {
  display: block;
}

.trackpad-body.fullscreen {
  flex: 1 1 auto;
  min-height: 0;
  display: flex;
  flex-direction: column;
}

.trackpad-header {
  display: flex;
  justify-content: space-between;
  gap: 16px;
  align-items: flex-start;
}

.trackpad-title {
  font-size: 16px;
  font-weight: 600;
  line-height: 1.2;
}

.trackpad-subtitle {
  margin-top: 6px;
  font-size: 12px;
  color: #909399;
  max-width: 540px;
}

.trackpad-actions {
  display: flex;
  gap: 8px;
  flex-shrink: 0;
}

.trackpad-surface {
  position: relative;
  min-height: 280px;
  border-radius: 18px;
  border: 1px solid #d9ecff;
  background:
    radial-gradient(circle at top left, rgba(64, 158, 255, 0.16), transparent 44%),
    linear-gradient(160deg, #f8fbff 0%, #eef5ff 52%, #e7f0ff 100%);
  overflow: hidden;
  touch-action: none;
  user-select: none;
}

.trackpad-card.fullscreen .trackpad-surface {
  flex: 1 1 auto;
  min-height: 0;
}

.trackpad-card.minimal .trackpad-surface {
  flex: 1 1 auto;
  min-height: 0;
}

.trackpad-card.immersive .trackpad-surface {
  flex: 1 1 auto;
  min-height: 0;
  border: 0;
  border-radius: 0;
  background:
    radial-gradient(circle at top left, rgba(64, 158, 255, 0.18), transparent 40%),
    linear-gradient(180deg, #eff6ff 0%, #e7f0ff 48%, #edf3ff 100%);
}

.trackpad-card.immersive .trackpad-surface::after {
  display: none;
}

.trackpad-surface::after {
  content: '';
  position: absolute;
  inset: 12px;
  border-radius: 12px;
  border: 1px dashed rgba(64, 158, 255, 0.2);
}

.trackpad-surface.active {
  box-shadow: inset 0 0 0 1px rgba(64, 158, 255, 0.28);
}

.trackpad-surface.disabled {
  opacity: 0.68;
}

.surface-hint {
  position: absolute;
  left: 20px;
  right: 20px;
  bottom: 20px;
  z-index: 1;
  padding: 14px 16px;
  border-radius: 14px;
  background: rgba(255, 255, 255, 0.76);
  backdrop-filter: blur(10px);
  pointer-events: none;
}

.surface-title {
  font-size: 15px;
  font-weight: 600;
}

.surface-text {
  margin-top: 6px;
  font-size: 12px;
  color: #606266;
  line-height: 1.5;
}

.touch-counter {
  position: absolute;
  top: 16px;
  right: 16px;
  z-index: 1;
}

.trackpad-toolbar {
  margin-top: 14px;
  display: flex;
  justify-content: space-between;
  gap: 12px;
  align-items: center;
  flex-wrap: wrap;
}

.toolbar-meta {
  display: flex;
  gap: 8px;
  align-items: center;
  flex-wrap: wrap;
}

.toolbar-note {
  font-size: 12px;
  color: #909399;
}

.settings-card {
  margin-bottom: 16px;
}

.settings-title {
  font-size: 14px;
  font-weight: 600;
}

.settings-copy {
  margin-top: 6px;
  font-size: 12px;
  line-height: 1.5;
  color: #667085;
}

.profile-group {
  margin-top: 12px;
}

.settings-meta {
  margin-top: 10px;
  font-size: 12px;
  color: #606266;
}

.gesture-summary {
  margin-top: 14px;
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
  gap: 10px;
}

.summary-item {
  padding: 12px 14px;
  border-radius: 12px;
  background: #f8fbff;
  border: 1px solid #e4efff;
}

.summary-item strong {
  display: block;
  margin-bottom: 6px;
  font-size: 13px;
}

.summary-item span {
  font-size: 12px;
  color: #606266;
  line-height: 1.45;
}
</style>
