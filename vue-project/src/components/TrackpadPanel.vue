<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { ElMessage } from 'element-plus'

import { actionsApi } from '@/api/actions'
import { macrosApi } from '@/api/macros'
import { useAuthStore } from '@/stores/auth'

const props = defineProps({
  deviceUuid: { type: String, default: '' },
  deviceOnline: { type: Boolean, default: false },
})

const emit = defineEmits(['result'])

const auth = useAuthStore()

const MOVE_SCALE = 1.15
const MOVE_FLUSH_MS = 28
const SCROLL_FLUSH_MS = 35
const SCROLL_PIXELS_PER_STEP = 22
const TAP_DISTANCE = 18
const TAP_MAX_MS = 320
const SWIPE_MIN_DISTANCE = 56
const STATUS_HINT = 'One finger moves the pointer, one tap clicks left, two fingers scroll, and multi-finger gestures can trigger defaults or macros.'

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

const settingsVisible = ref(false)
const macros = ref([])
const macrosLoading = ref(false)
const activeTouches = ref(0)
const status = reactive({ text: STATUS_HINT, type: 'info' })
const bindings = reactive(loadBindings())

const moveQueue = reactive({ x: 0, y: 0, sending: false, timer: 0 })
const scrollPixels = reactive({ x: 0, y: 0 })
const scrollQueue = reactive({ x: 0, y: 0, sending: false, timer: 0 })

const canControl = computed(() => !!props.deviceUuid && props.deviceOnline)
const surfaceState = computed(() => {
  if (!props.deviceUuid) return 'Select an online device first.'
  if (!props.deviceOnline) return 'The selected device is offline.'
  return STATUS_HINT
})

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

watch(bindings, saveBindings, { deep: true })

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

async function sendPayload(payload, { silent = false, record = true, timeoutMs = 1500 } = {}) {
  if (!props.deviceUuid || !props.deviceOnline) {
    if (!silent) ElMessage.warning('Select an online device first.')
    return null
  }

  try {
    const response = await actionsApi.send({
      device_id: props.deviceUuid,
      payload,
      timeout_ms: timeoutMs,
    })
    const result = response.results?.[0]
    if (record) emit('result', response)

    if (result?.status === 'acked') {
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
  if (moveQueue.sending) {
    scheduleMoveFlush(MOVE_FLUSH_MS)
    return
  }

  const x = takeQueuedAxis(moveQueue.x)
  const y = takeQueuedAxis(moveQueue.y)
  if (!x && !y) return

  moveQueue.x -= x
  moveQueue.y -= y
  moveQueue.sending = true
  try {
    await sendPayload({ type: 'mouse_move', x, y }, { silent: true, record: false, timeoutMs: 900 })
  } finally {
    moveQueue.sending = false
    if (Math.abs(moveQueue.x) >= 1 || Math.abs(moveQueue.y) >= 1) {
      scheduleMoveFlush(0)
    }
  }
}

function queueMove(dx, dy) {
  moveQueue.x += dx * MOVE_SCALE
  moveQueue.y += dy * MOVE_SCALE
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
  if (scrollQueue.sending) {
    scheduleScrollFlush(SCROLL_FLUSH_MS)
    return
  }

  const x = takeQueuedAxis(scrollQueue.x)
  const y = takeQueuedAxis(scrollQueue.y)
  if (!x && !y) return

  scrollQueue.x -= x
  scrollQueue.y -= y
  scrollQueue.sending = true
  try {
    await sendPayload({ type: 'mouse_scroll', x, y }, { silent: true, record: false, timeoutMs: 900 })
  } finally {
    scrollQueue.sending = false
    if (Math.abs(scrollQueue.x) >= 1 || Math.abs(scrollQueue.y) >= 1) {
      scheduleScrollFlush(0)
    }
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
    execution_mode: 'network_debug',
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
    setStatus(surfaceState.value, canControl.value ? 'info' : 'warning')
  },
  { immediate: true },
)

onMounted(loadMacros)

onBeforeUnmount(() => {
  clearTimer(moveQueue.timer)
  clearTimer(scrollQueue.timer)
  resetInteraction()
})
</script>

<template>
  <el-card class="trackpad-card">
    <template #header>
      <div class="trackpad-header">
        <div>
          <div class="trackpad-title">Touchpad</div>
          <div class="trackpad-subtitle">Mouse move, real wheel scroll, Windows-style gesture defaults, and macro bindings.</div>
        </div>
        <div class="trackpad-actions">
          <el-button size="small" @click="loadMacros" :loading="macrosLoading">Refresh Macros</el-button>
          <el-button size="small" type="primary" @click="settingsVisible = true">Gesture Bindings</el-button>
        </div>
      </div>
    </template>

    <div
      class="trackpad-surface"
      :class="{ disabled: !canControl, active: activeTouches > 0 }"
      @pointerdown="onPointerDown"
      @pointermove="onPointerMove"
      @pointerup="onPointerUp"
      @pointercancel="onPointerCancel"
      @contextmenu.prevent
    >
      <div class="surface-hint">
        <div class="surface-title">{{ canControl ? 'Touch Surface' : 'Unavailable' }}</div>
        <div class="surface-text">{{ surfaceState }}</div>
      </div>
      <el-tag v-if="activeTouches > 0" size="small" type="success" class="touch-counter">
        {{ activeTouches }} finger{{ activeTouches > 1 ? 's' : '' }}
      </el-tag>
    </div>

    <div class="trackpad-toolbar">
      <el-button-group>
        <el-button @click="clickButton(1, 'Left click')" :disabled="!canControl">Left</el-button>
        <el-button @click="clickButton(2, 'Right click')" :disabled="!canControl">Right</el-button>
        <el-button @click="clickButton(4, 'Middle click')" :disabled="!canControl">Middle</el-button>
      </el-button-group>
      <span class="toolbar-note">Mouse move and scroll require USB transport on the device side.</span>
    </div>

    <el-alert
      :title="status.text"
      :type="status.type === 'error' ? 'error' : (status.type === 'success' ? 'success' : 'info')"
      :closable="false"
      show-icon
      style="margin-top: 12px"
    />

    <div class="gesture-summary">
      <div class="summary-item"><strong>1 finger</strong><span>Move pointer, tap for left click.</span></div>
      <div class="summary-item"><strong>2 fingers</strong><span>Scroll, tap for right click.</span></div>
      <div class="summary-item"><strong>3 fingers</strong><span>Tap for middle click, swipe for app/task actions.</span></div>
      <div class="summary-item"><strong>4 fingers</strong><span>Swipe left or right to switch desktops.</span></div>
    </div>
  </el-card>

  <el-dialog v-model="settingsVisible" title="Gesture Bindings" width="860px">
    <el-alert
      type="info"
      :closable="false"
      title="Bindings are stored locally for the current signed-in browser user. Clear the binding to fall back to the built-in default action."
      style="margin-bottom: 16px"
    />

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
}

.toolbar-note {
  font-size: 12px;
  color: #909399;
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
