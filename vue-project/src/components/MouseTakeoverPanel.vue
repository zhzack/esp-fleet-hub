<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { ElMessage } from 'element-plus'

import { actionsApi } from '@/api/actions'
import { useWs } from '@/composables/useWs'
import { useAuthStore } from '@/stores/auth'

/**
 * 真实键鼠接管：通过浏览器 Pointer Lock API 捕获控制端电脑的原始鼠标输入，
 * 连同键盘事件一起透传到 ESP32（mouse_move / mouse_down / mouse_up /
 * mouse_scroll / keyboard_down / keyboard_up），再由固件按 active transport
 * 路由到 USB 或 BLE 目标机。与触控板不同，本组件 1:1 直通增量（无加速度曲线，
 * 优先请求 unadjustedMovement 获取未经过 OS 加速的原始计数）。
 */
const props = defineProps({
  deviceUuid: { type: String, default: '' },
  deviceOnline: { type: Boolean, default: false },
  // 目标主机平台（''=未标记 / win / mac / linux）：驱动系统组合键按钮组
  platform: { type: String, default: '' },
})

const auth = useAuthStore()

const MOVE_FLUSH_MS = 10
const SCROLL_FLUSH_MS = 14
const STREAM_RETRY_MS = 4
const STREAM_HTTP_MAX_INFLIGHT = 3
const WHEEL_PIXELS_PER_STEP = 22
const WHEEL_LINE_PIXELS = 33

/* KeyboardEvent.code -> firmware keymap name（字母/数字/F1-F12 走正则，其余特例表） */
const CODE_SPECIALS = {
  Enter: 'KEY_ENTER', Escape: 'KEY_ESC', Backspace: 'KEY_BACKSPACE', Tab: 'KEY_TAB',
  Space: 'KEY_SPACE', Minus: 'KEY_MINUS', Equal: 'KEY_EQUAL',
  ArrowUp: 'KEY_UP', ArrowDown: 'KEY_DOWN', ArrowLeft: 'KEY_LEFT', ArrowRight: 'KEY_RIGHT',
  BracketLeft: 'KEY_LBRACKET', BracketRight: 'KEY_RBRACKET', Backslash: 'KEY_BACKSLASH',
  Semicolon: 'KEY_SEMICOLON', Quote: 'KEY_QUOTE', Backquote: 'KEY_BACKQUOTE',
  Comma: 'KEY_COMMA', Period: 'KEY_DOT', Slash: 'KEY_SLASH', CapsLock: 'KEY_CAPSLOCK',
  Delete: 'KEY_DELETE', Insert: 'KEY_INSERT', Home: 'KEY_HOME', End: 'KEY_END',
  PageUp: 'KEY_PAGEUP', PageDown: 'KEY_PAGEDOWN',
  ControlLeft: 'KEY_LEFTCTRL', ControlRight: 'KEY_RIGHTCTRL',
  ShiftLeft: 'KEY_LEFTSHIFT', ShiftRight: 'KEY_RIGHTSHIFT',
  AltLeft: 'KEY_LEFTALT', AltRight: 'KEY_RIGHTALT',
  MetaLeft: 'KEY_LEFTGUI', MetaRight: 'KEY_RIGHTGUI',
}

/* DOM MouseEvent.button -> HID bitmask（1=左 2=右 4=中） */
const HID_BUTTON = { 0: 1, 1: 4, 2: 2 }

function mapCode(code) {
  if (/^Key[A-Z]$/.test(code)) return `KEY_${code.slice(3)}`
  if (/^Digit\d$/.test(code)) return `KEY_${code.slice(5)}`
  if (/^F([1-9]|1[0-2])$/.test(code)) return `KEY_${code}`
  return CODE_SPECIALS[code] || ''
}

function modsFromEvent(e) {
  return (e.ctrlKey ? 1 : 0) | (e.shiftKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0)
}

const armed = ref(false)
const locked = ref(false)
const sentCount = ref(0)
const sendingCombo = ref(false)
const surfaceRef = ref(null)

/* 系统组合键按钮：物理 Win / Alt+Tab 会被控制端操作系统拦截（弹开始菜单/
 * 切窗口），浏览器根本收不到，只能用按钮把序列直接下发。修饰键经固件
 * 按住状态机携带：先 down 修饰键，再 down/up 主键，最后 up 修饰键。
 * 按目标主机平台标记（props.platform）切换按钮组；未标记显示通用双语组。 */
const seq = (...keys) => {
  // keys: [['down','KEY_X',50], ...] → steps
  return keys.map(([t, key, d]) => ({ t: `keyboard_${t}`, key, d }))
}

const COMBO_GENERIC = [
  { label: 'Win / Cmd 键', steps: seq(['down', 'KEY_LEFTGUI', 80], ['up', 'KEY_LEFTGUI', 40]) },
  { label: 'Cmd+Tab 切应用', steps: seq(['down', 'KEY_LEFTGUI', 50], ['down', 'KEY_TAB', 100], ['up', 'KEY_TAB', 50], ['up', 'KEY_LEFTGUI', 40]) },
  { label: 'Alt+Tab 切窗口', steps: seq(['down', 'KEY_LEFTALT', 50], ['down', 'KEY_TAB', 100], ['up', 'KEY_TAB', 50], ['up', 'KEY_LEFTALT', 40]) },
  { label: 'Cmd+Space', steps: seq(['down', 'KEY_LEFTGUI', 50], ['down', 'KEY_SPACE', 80], ['up', 'KEY_SPACE', 50], ['up', 'KEY_LEFTGUI', 40]) },
]
const COMBO_WIN = [
  { label: 'Win 键', steps: seq(['down', 'KEY_LEFTGUI', 80], ['up', 'KEY_LEFTGUI', 40]) },
  { label: 'Win+Tab 任务视图', steps: seq(['down', 'KEY_LEFTGUI', 50], ['down', 'KEY_TAB', 100], ['up', 'KEY_TAB', 50], ['up', 'KEY_LEFTGUI', 40]) },
  { label: 'Alt+Tab 切窗口', steps: seq(['down', 'KEY_LEFTALT', 50], ['down', 'KEY_TAB', 100], ['up', 'KEY_TAB', 50], ['up', 'KEY_LEFTALT', 40]) },
  { label: 'Win+D 显示桌面', steps: seq(['down', 'KEY_LEFTGUI', 50], ['down', 'KEY_D', 80], ['up', 'KEY_D', 50], ['up', 'KEY_LEFTGUI', 40]) },
]
const COMBO_MAC = [
  { label: 'Cmd 键', steps: seq(['down', 'KEY_LEFTGUI', 80], ['up', 'KEY_LEFTGUI', 40]) },
  { label: 'Cmd+Tab 切应用', steps: seq(['down', 'KEY_LEFTGUI', 50], ['down', 'KEY_TAB', 100], ['up', 'KEY_TAB', 50], ['up', 'KEY_LEFTGUI', 40]) },
  { label: 'Cmd+Space 聚焦搜索', steps: seq(['down', 'KEY_LEFTGUI', 50], ['down', 'KEY_SPACE', 80], ['up', 'KEY_SPACE', 50], ['up', 'KEY_LEFTGUI', 40]) },
  { label: '桌面 ←/→ 切换', steps: seq(['down', 'KEY_LEFTGUI', 50], ['down', 'KEY_LEFTCTRL', 50], ['down', 'KEY_LEFT', 100], ['up', 'KEY_LEFT', 50], ['up', 'KEY_LEFTCTRL', 40], ['up', 'KEY_LEFTGUI', 40]) },
]

const COMBO_KEYS = computed(() => {
  if (props.platform === 'win') return COMBO_WIN
  if (props.platform === 'mac') return COMBO_MAC
  return COMBO_GENERIC
})

async function sendCombo(combo) {
  if (sendingCombo.value || !canControl.value) return
  sendingCombo.value = true
  try {
    for (const step of combo.steps) {
      sendStreamPayload({ type: step.t, key: step.key })
      await new Promise((resolve) => setTimeout(resolve, step.d ?? 40))
    }
  } finally {
    sendingCombo.value = false
  }
}

const moveQueue = reactive({ x: 0, y: 0, timer: 0 })
const scrollPixels = reactive({ x: 0, y: 0 })
const scrollQueue = reactive({ x: 0, y: 0, timer: 0 })
const streamState = reactive({ httpInflight: 0, packetsPerSecond: 0 })

const pressedButtons = new Set()
const pressedKeys = new Set()

const ws = useWs('/ws/client', { token: computed(() => auth.token || '') })
const packetSamples = []

const canControl = computed(() => !!props.deviceUuid && props.deviceOnline)

function makeMsgId() {
  if (window.crypto?.randomUUID) return window.crypto.randomUUID()
  return `takeover-${Date.now()}-${Math.random().toString(36).slice(2, 10)}`
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
  if (!canControl.value) return false
  if (ws.status.value !== 'open') return false
  const ok = ws.send({
    header: {
      msg_id: makeMsgId(),
      type: 'action',
      source: 'web-takeover',
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
  if (ok) { markStreamPacket(); sentCount.value += 1 }
  return ok
}

async function sendHttpPayload(payload) {
  try {
    const response = await actionsApi.send({
      device_id: props.deviceUuid,
      payload,
      timeout_ms: 700,
    })
    const result = response.results?.[0]
    return result?.status === 'acked' || result?.status === 'queued'
  } catch {
    return false
  }
}

function hasQueuedMove() {
  return Math.abs(moveQueue.x) >= 1 || Math.abs(moveQueue.y) >= 1
}

function hasQueuedScroll() {
  return Math.abs(scrollQueue.x) >= 1 || Math.abs(scrollQueue.y) >= 1
}

function sendStreamPayload(payload) {
  if (sendWsPayload(payload)) return true
  if (streamState.httpInflight >= STREAM_HTTP_MAX_INFLIGHT) return false

  streamState.httpInflight += 1
  sentCount.value += 1
  sendHttpPayload(payload)
    .finally(() => {
      streamState.httpInflight = Math.max(0, streamState.httpInflight - 1)
      if (hasQueuedMove()) scheduleMoveFlush(0)
      if (hasQueuedScroll()) scheduleScrollFlush(0)
    })
  return true
}

function clampAxis(value) {
  return Math.max(-127, Math.min(127, value))
}

function takeQueuedAxis(value) {
  const integer = value > 0 ? Math.floor(value) : Math.ceil(value)
  return clampAxis(integer)
}

function scheduleMoveFlush(delay = MOVE_FLUSH_MS) {
  if (moveQueue.timer) return
  moveQueue.timer = window.setTimeout(() => {
    moveQueue.timer = 0
    flushMoveQueue()
  }, delay)
}

function flushMoveQueue() {
  const x = takeQueuedAxis(moveQueue.x)
  const y = takeQueuedAxis(moveQueue.y)
  if (!x && !y) return

  if (!sendStreamPayload({ type: 'mouse_move', x, y })) {
    scheduleMoveFlush(STREAM_RETRY_MS)
    return
  }

  moveQueue.x -= x
  moveQueue.y -= y
  if (hasQueuedMove()) scheduleMoveFlush(0)
}

function scheduleScrollFlush(delay = SCROLL_FLUSH_MS) {
  if (scrollQueue.timer) return
  scrollQueue.timer = window.setTimeout(() => {
    scrollQueue.timer = 0
    flushScrollQueue()
  }, delay)
}

function flushScrollQueue() {
  const x = takeQueuedAxis(scrollQueue.x)
  const y = takeQueuedAxis(scrollQueue.y)
  if (!x && !y) return

  if (!sendStreamPayload({ type: 'mouse_scroll', x, y })) {
    scheduleScrollFlush(STREAM_RETRY_MS)
    return
  }

  scrollQueue.x -= x
  scrollQueue.y -= y
  if (hasQueuedScroll()) scheduleScrollFlush(0)
}

function queueWheelDelta(deltaX, deltaY) {
  scrollPixels.x += deltaX
  scrollPixels.y += deltaY

  const stepsX = scrollPixels.x > 0
    ? Math.floor(scrollPixels.x / WHEEL_PIXELS_PER_STEP)
    : Math.ceil(scrollPixels.x / WHEEL_PIXELS_PER_STEP)
  const stepsY = scrollPixels.y > 0
    ? Math.floor(scrollPixels.y / WHEEL_PIXELS_PER_STEP)
    : Math.ceil(scrollPixels.y / WHEEL_PIXELS_PER_STEP)

  if (!stepsX && !stepsY) return

  scrollPixels.x -= stepsX * WHEEL_PIXELS_PER_STEP
  scrollPixels.y -= stepsY * WHEEL_PIXELS_PER_STEP
  scrollQueue.x += stepsX
  scrollQueue.y += stepsY
  scheduleScrollFlush()
}

function resetQueues() {
  if (moveQueue.timer) { window.clearTimeout(moveQueue.timer); moveQueue.timer = 0 }
  if (scrollQueue.timer) { window.clearTimeout(scrollQueue.timer); scrollQueue.timer = 0 }
  moveQueue.x = 0
  moveQueue.y = 0
  scrollPixels.x = 0
  scrollPixels.y = 0
  scrollQueue.x = 0
  scrollQueue.y = 0
}

/* ---------- Pointer Lock 生命周期 ---------- */

async function engage() {
  if (!canControl.value) {
    ElMessage.warning('Select an online device first.')
    return
  }
  armed.value = true
  await nextTick()
  const el = surfaceRef.value
  if (!el) { armed.value = false; return }

  /* unadjustedMovement: 绕过 OS 鼠标加速度拿原始计数（透传语义更纯正），
   * 不支持时回退普通锁定。 */
  try {
    const p = el.requestPointerLock({ unadjustedMovement: true })
    if (p && typeof p.catch === 'function') await p
  } catch {
    try {
      const p2 = el.requestPointerLock()
      if (p2 && typeof p2.catch === 'function') await p2
    } catch (err) {
      armed.value = false
      ElMessage.error(`Pointer lock failed: ${err?.message || err}`)
    }
  }
}

function disengage() {
  if (document.pointerLockElement) document.exitPointerLock()
}

/** 退出/失焦时清理：清空流式队列，并尽力释放目标机上仍按住的键与按钮，防止粘键。 */
function releaseAll() {
  resetQueues()
  pressedButtons.forEach((button) => { sendWsPayload({ type: 'mouse_up', button }) })
  pressedButtons.clear()
  pressedKeys.forEach((key) => { sendWsPayload({ type: 'keyboard_up', key }) })
  pressedKeys.clear()
}

function onPointerLockChange() {
  locked.value = armed.value && document.pointerLockElement === surfaceRef.value
  if (armed.value && !locked.value) {
    armed.value = false
    releaseAll()
  }
}

function onPointerLockError() {
  armed.value = false
  releaseAll()
  ElMessage.error('Pointer lock was denied by the browser.')
}

/* ---------- 输入事件（锁定期间生效） ---------- */

function onMouseMove(e) {
  if (!locked.value) return
  moveQueue.x += e.movementX || 0
  moveQueue.y += e.movementY || 0
  scheduleMoveFlush()
}

function onMouseDown(e) {
  if (!locked.value) return
  e.preventDefault()
  const button = HID_BUTTON[e.button]
  if (!button) return
  pressedButtons.add(button)
  sendStreamPayload({ type: 'mouse_down', button })
}

function onMouseUp(e) {
  if (!locked.value) return
  const button = HID_BUTTON[e.button]
  if (!button) return
  pressedButtons.delete(button)
  sendStreamPayload({ type: 'mouse_up', button })
}

function onWheel(e) {
  if (!locked.value) return
  e.preventDefault()
  const unit = e.deltaMode === 1 ? WHEEL_LINE_PIXELS : 1
  queueWheelDelta(e.deltaX * unit, e.deltaY * unit)
}

function onKeyDown(e) {
  if (!locked.value) return
  e.preventDefault()
  /* Esc 交给浏览器退出 Pointer Lock（pointerlockchange 里统一清理）。 */
  if (e.code === 'Escape') return
  if (e.repeat) return
  const key = mapCode(e.code)
  if (!key) return
  pressedKeys.add(key)
  sendStreamPayload({ type: 'keyboard_down', key, modifiers: modsFromEvent(e) })
}

function onKeyUp(e) {
  if (!locked.value) return
  e.preventDefault()
  const key = mapCode(e.code)
  if (!key) return
  pressedKeys.delete(key)
  sendStreamPayload({ type: 'keyboard_up', key, modifiers: modsFromEvent(e) })
}

onMounted(() => {
  document.addEventListener('pointerlockchange', onPointerLockChange)
  document.addEventListener('pointerlockerror', onPointerLockError)
  document.addEventListener('keydown', onKeyDown)
  document.addEventListener('keyup', onKeyUp)
})

onBeforeUnmount(() => {
  document.removeEventListener('pointerlockchange', onPointerLockChange)
  document.removeEventListener('pointerlockerror', onPointerLockError)
  document.removeEventListener('keydown', onKeyDown)
  document.removeEventListener('keyup', onKeyUp)
  if (document.pointerLockElement === surfaceRef.value) document.exitPointerLock()
  releaseAll()
  ws.close()
})
</script>

<template>
  <div class="takeover-card">
    <div class="takeover-head">
      <span class="takeover-title">真实键鼠接管</span>
      <el-tooltip
        content="激活后锁定本机指针，真实鼠标的移动 / 点击 / 滚轮与键盘实时透传（USB / BLE 自动路由）。按 Esc 退出。物理 Win / Alt+Tab 会被本机系统截胡，请用下方组合键按钮。"
        placement="top"
      >
        <span class="info-dot">i</span>
      </el-tooltip>
      <span class="takeover-target" :class="{ online: canControl }">
        {{
          props.deviceUuid
            ? `${props.deviceUuid}${props.deviceOnline ? ' · 在线' : ' · 离线'}`
            : '未选择设备'
        }}
      </span>
    </div>

    <div class="takeover-toolbar">
      <el-button type="primary" :disabled="!canControl || armed" @click="engage">
        开始接管
      </el-button>
      <span v-if="armed" class="takeover-live">
        接管中 · {{ streamState.packetsPerSecond }} 包/秒
      </span>
      <span v-else-if="ws.status.value === 'open'" class="takeover-ws">流通道就绪</span>
      <span v-else class="takeover-ws offline">流通道连接中…</span>
    </div>

    <div class="combo-row">
      <span class="combo-label">系统组合键</span>
      <div class="combo-btns">
        <el-button
          v-for="combo in COMBO_KEYS"
          :key="combo.label"
          size="small"
          :disabled="!canControl || sendingCombo"
          @click="sendCombo(combo)"
        >
          {{ combo.label }}
        </el-button>
      </div>
      <el-tooltip content="物理 Win / Alt+Tab 会被控制端系统截胡（弹开始菜单/切本机窗口），浏览器收不到事件；组合键需用按钮直发序列" placement="top">
        <span class="info-dot">i</span>
      </el-tooltip>
    </div>

    <div
      v-if="armed"
      ref="surfaceRef"
      class="takeover-surface"
      @mousemove="onMouseMove"
      @mousedown="onMouseDown"
      @mouseup="onMouseUp"
      @wheel="onWheel"
      @contextmenu.prevent
    >
      <div class="takeover-hud">
        <div class="hud-title">键鼠接管中</div>
        <div class="hud-target">{{ props.deviceUuid }}</div>
        <div class="hud-stats">
          {{ streamState.packetsPerSecond }} packets/s · {{ sentCount }} sent
          <template v-if="streamState.httpInflight"> · HTTP 回退</template>
        </div>
        <div class="hud-exit">按 Esc 退出</div>
        <button type="button" class="hud-quit" @click="disengage">退出接管</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.takeover-card {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.takeover-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  flex-wrap: wrap;
}

.takeover-title {
  font-size: 15px;
  font-weight: 600;
}

.takeover-target {
  font-size: 12px;
  color: var(--text-secondary, #909399);
}

.takeover-target.online {
  color: var(--el-color-success, #67c23a);
}

.takeover-desc {
  margin: 0;
  font-size: 13px;
}

/* 标题旁 info 小圆点（说明收纳进 tooltip） */
.info-dot {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 16px;
  height: 16px;
  border-radius: 50%;
  font-size: 11px;
  font-style: italic;
  font-weight: 600;
  color: var(--text-secondary, #909399);
  background: rgba(144, 147, 153, 0.14);
  cursor: help;
}

.combo-row {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
}

.combo-label {
  font-size: 13px;
  font-weight: 600;
  color: var(--text-primary, #303133);
}

.combo-btns {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
}

.combo-hint {
  font-size: 12px;
  color: var(--text-secondary, #909399);
}

.takeover-desc {
  line-height: 1.6;
  color: var(--text-secondary, #909399);
}

.takeover-toolbar {
  display: flex;
  align-items: center;
  gap: 14px;
  flex-wrap: wrap;
}

.takeover-live {
  font-size: 13px;
  color: var(--el-color-success, #67c23a);
}

.takeover-ws {
  font-size: 12px;
  color: var(--text-secondary, #909399);
}

.takeover-ws.offline {
  color: var(--el-color-warning, #e6a23c);
}

.takeover-surface {
  position: fixed;
  inset: 0;
  z-index: 3000;
  cursor: none;
  background: rgba(15, 18, 25, 0.82);
  display: flex;
  align-items: center;
  justify-content: center;
  user-select: none;
}

.takeover-hud {
  text-align: center;
  color: #fff;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 10px;
  pointer-events: none;
}

.hud-quit {
  pointer-events: auto;
  margin-top: 6px;
  background: rgba(255, 255, 255, 0.12);
  border: 1px solid rgba(255, 255, 255, 0.35);
  color: #fff;
  font-size: 13px;
  border-radius: 8px;
  padding: 6px 18px;
  cursor: pointer;
}

.hud-quit:hover {
  background: rgba(255, 255, 255, 0.22);
}

.hud-title {
  font-size: 22px;
  font-weight: 600;
  letter-spacing: 2px;
}

.hud-target {
  font-size: 14px;
  opacity: 0.85;
}

.hud-stats {
  font-size: 12px;
  opacity: 0.6;
  font-variant-numeric: tabular-nums;
}

.hud-exit {
  margin-top: 14px;
  font-size: 13px;
  opacity: 0.75;
  border: 1px solid rgba(255, 255, 255, 0.35);
  border-radius: 8px;
  padding: 4px 14px;
}
</style>
