<script>
export const meta = { title: '实时控制' }
</script>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { ElMessage } from 'element-plus'
import { useRouter } from 'vue-router'

import TrackpadPanel from '@/components/TrackpadPanel.vue'
import MouseTakeoverPanel from '@/components/MouseTakeoverPanel.vue'
import { actionsApi } from '@/api/actions'
import { commandsApi } from '@/api/commands'
import { devicesApi } from '@/api/devices'

const router = useRouter()
const devices = ref([])
const online = ref([])
const selected = ref([]) // 多选：同时下发控制指令的设备 uuid 列表
const text = ref('')
const sending = ref(false)
const lastResult = ref(null)

const bonds = ref([])
const currentPeer = ref('')
const targetPeer = ref('')
const bleConnected = ref(false)
const bleLoading = ref(false)
const transportMode = ref('') // 当前输出通道: 'ble' | 'usb' | 'auto' | ''
const activeTransport = ref('') // auto 模式下实际生效的通道: 'usb' | 'ble'
const usbConnected = ref(false) // USB 线缆是否已枚举到主机

// Categorized keyboard key groups
const NAV_KEYS = [
  { key: 'KEY_UP', label: '↑ Up', icon: '↑' },
  { key: 'KEY_DOWN', label: '↓ Down', icon: '↓' },
  { key: 'KEY_LEFT', label: '← Left', icon: '←' },
  { key: 'KEY_RIGHT', label: '→ Right', icon: '→' },
]

const CONTROL_KEYS = [
  { key: 'KEY_ENTER', label: 'Enter', primary: true },
  { key: 'KEY_SPACE', label: 'Space' },
  { key: 'KEY_TAB', label: 'Tab' },
  { key: 'KEY_ESC', label: 'Esc', danger: true },
  { key: 'KEY_BACKSPACE', label: 'Backspace' },
]

const SHORTCUT_KEYS = [
  { key: 'KEY_GUI_D', label: 'Win + D', hint: 'Desktop' },
  { key: 'KEY_GUI_TAB', label: 'Win + Tab', hint: 'Task View' },
  { key: 'KEY_ALT_TAB', label: 'Alt + Tab', hint: 'App Switch' },
  { key: 'KEY_ALT_SHIFT_TAB', label: 'Alt + Shift + Tab', hint: 'Prev App' },
]

const ALPHA_KEYS = [
  'KEY_A', 'KEY_B', 'KEY_C', 'KEY_D', 'KEY_E', 'KEY_F',
  'KEY_G', 'KEY_H', 'KEY_I', 'KEY_J', 'KEY_K', 'KEY_L',
  'KEY_M', 'KEY_N', 'KEY_O', 'KEY_P', 'KEY_Q', 'KEY_R',
  'KEY_S', 'KEY_T', 'KEY_U', 'KEY_V', 'KEY_W', 'KEY_X',
  'KEY_Y', 'KEY_Z',
]

const QUICK_TEXTS = [
  'Hello World!',
  'ping 127.0.0.1',
  'echo "test"',
  'exit',
]

function extractCommandResult(resp) {
  if (!resp) return null
  let current = resp.result !== undefined ? resp.result : resp.results?.[0]?.result
  if (current === undefined) current = resp
  if (current && typeof current === 'object' && current.result !== undefined) {
    return current.result
  }
  return current
}

function fmtAddr(addr) {
  if (typeof addr === 'string') return addr.toUpperCase()
  if (Array.isArray(addr)) return addr.map((item) => item.toString(16).padStart(2, '0')).join(':').toUpperCase()
  return ''
}

function addrToBytes(value) {
  if (!value) return null
  return value.split(':').map((item) => parseInt(item, 16))
}

// 仅当选中恰好 1 台设备时才有"唯一目标"概念（触控板/输出目标卡片依赖）
const soleSelected = computed(() => (selected.value.length === 1 ? selected.value[0] : ''))
const selectedDevice = computed(() => devices.value.find((item) => item.device_uuid === soleSelected.value) || null)
const isSelectedOnline = computed(() => selected.value.some((uuid) => online.value.includes(uuid)))
const isSoleOnline = computed(() => !!soleSelected.value && online.value.includes(soleSelected.value))

const USB_TARGET = '__usb__' // 输出目标下拉中 USB 主机选项的哨兵值

const targetOptions = computed(() => [
  {
    value: USB_TARGET,
    label: `🔌 USB 主机（线缆直连）${usbConnected.value ? ' ● 已连接' : ' ○ 未连接'}`,
  },
  ...bonds.value.map((item) => ({
    value: item.addrStr,
    label: `📡 ${item.name || item.addrStr}${item.addrStr === currentPeer.value ? '（当前主机）' : ''}`,
  })),
])

const transportLabel = computed(() => {
  if (transportMode.value === 'usb') return `USB${usbConnected.value ? ' · 已挂载' : ' · 未连接'}`
  if (transportMode.value === 'ble') return currentPeer.value ? `BLE · ${currentPeer.value}` : 'BLE · 无活跃连接'
  if (transportMode.value === 'auto') {
    if (activeTransport.value === 'usb') return `auto · 当前走 USB${usbConnected.value ? '' : '（未连接）'}`
    return `auto · 当前走 ${currentPeer.value || 'BLE'}`
  }
  return '未知'
})

const isCurrentTarget = computed(() => {
  if (!targetPeer.value) return false
  if (targetPeer.value === USB_TARGET) return transportMode.value === 'usb'
  return targetPeer.value === currentPeer.value && transportMode.value === 'ble'
})

async function load() {
  devices.value = await devicesApi.list()
  online.value = (await devicesApi.online()).online
  if (!selected.value.length && online.value.length) selected.value = [online.value[0]]
}

async function silentRefresh() {
  try {
    const [list, on] = await Promise.all([devicesApi.list(), devicesApi.online()])
    devices.value = list
    online.value = on.online
  } catch {
    // ignore
  }
}

let pollTimer = null

onMounted(() => {
  load()
  pollTimer = window.setInterval(silentRefresh, 5000)
})

onBeforeUnmount(() => {
  if (pollTimer) window.clearInterval(pollTimer)
})

async function loadTarget() {
  if (!soleSelected.value || !isSoleOnline.value) {
    bonds.value = []
    currentPeer.value = ''
    targetPeer.value = ''
    bleConnected.value = false
    transportMode.value = ''
    activeTransport.value = ''
    usbConnected.value = false
    return
  }

  bleLoading.value = true
  try {
    const uuid = soleSelected.value
    // 四路信息并行拉取；单路失败（如固件不支持 usb.info）不阻塞其余
    const [info, bondResult, transport, usb] = await Promise.all([
      commandsApi.bleInfo(uuid).catch(() => null),
      commandsApi.bleBonds(uuid).catch(() => null),
      commandsApi.transportGet(uuid).catch(() => null),
      commandsApi.usbInfo(uuid).catch(() => null),
    ])

    const infoData = extractCommandResult(info) || {}
    const bondData = extractCommandResult(bondResult) || {}
    const transportData = extractCommandResult(transport) || {}
    const usbData = extractCommandResult(usb) || {}

    transportMode.value = String(transportData.mode || transportData.transport || '')
    activeTransport.value = String(transportData.active || '')
    // 固件 usb.info 返回 {mounted}; transport.get 返回 {usb_mounted} —— 两者皆可判定
    usbConnected.value = !!(usbData.mounted ?? usbData.connected ?? transportData.usb_mounted)
    bleConnected.value = !!infoData.connected
    currentPeer.value = fmtAddr(infoData.peer?.addr || infoData.peer || infoData.current?.addr)

    bonds.value = (bondData.bonds || []).map((item) => ({
      ...item,
      addrStr: fmtAddr(item.addr),
    }))
    // 默认选中当前活跃的输出目标
    targetPeer.value =
      transportMode.value === 'usb'
        ? USB_TARGET
        : currentPeer.value || bonds.value[0]?.addrStr || ''
  } catch {
    bonds.value = []
    currentPeer.value = ''
    targetPeer.value = ''
    bleConnected.value = false
    transportMode.value = ''
    usbConnected.value = false
  } finally {
    bleLoading.value = false
  }
}

watch(soleSelected, loadTarget)

async function switchTarget() {
  const uuid = soleSelected.value
  if (!uuid || !targetPeer.value || isCurrentTarget.value) return

  bleLoading.value = true
  try {
    if (targetPeer.value === USB_TARGET) {
      // USB 主机 = 线缆所连的唯一机器，"切换"即切换输出通道
      await commandsApi.transportSet(uuid, 'usb')
      ElMessage.success('输出通道已切换至 USB 主机')
    } else {
      const target = bonds.value.find((item) => item.addrStr === targetPeer.value)
      if (!target) return ElMessage.warning('目标配对主机未找到')
      if (transportMode.value && transportMode.value !== 'ble' && transportMode.value !== 'auto') {
        await commandsApi.transportSet(uuid, 'ble')
      }
      const response = await commandsApi.bleSwitch(uuid, target.addr_type ?? 0, addrToBytes(targetPeer.value))
      if (response?.status === 'acked' || response?.status === 'ok') {
        ElMessage.success(`已切换至主机 ${target.name || targetPeer.value}`)
      } else {
        ElMessage.warning(`切换失败: ${response?.error_message || '未知错误'}`)
      }
    }
    await loadTarget()
  } finally {
    bleLoading.value = false
  }
}

async function send(payload, { successText = '', timeoutMs = 3000 } = {}) {
  if (!selected.value.length) return ElMessage.warning('请先选择控制目标设备')
  sending.value = true
  try {
    const response = await actionsApi.send({
      target_devices: selected.value, // 多设备并发下发，后端 gather 聚合 results
      payload: {
        ...payload,
        need_ack: true,
      },
      timeout_ms: timeoutMs,
    })
    lastResult.value = {
      ...response,
      _timestamp: new Date().toLocaleTimeString(),
      _type: payload.type,
      _key: payload.key || payload.content,
    }
    const results = response.results || []
    if (results.length <= 1) {
      const result = results[0]
      if (result?.status === 'acked') {
        if (successText) ElMessage.success(successText)
      } else {
        ElMessage.warning(`${result?.status || '执行异常'} ${result?.error_message || ''}`.trim())
      }
    } else {
      const acked = results.filter((r) => r.status === 'acked').length
      if (acked === results.length) {
        if (successText) ElMessage.success(`${successText}（${acked} 台设备全部确认）`)
      } else {
        ElMessage.warning(`${acked}/${results.length} 台设备确认成功`)
      }
    }
  } finally {
    sending.value = false
  }
}

function sendKey(key) {
  send({ type: 'keyboard_tap', key }, { successText: `按键 ${key.replace('KEY_', '')} 发送成功` })
}

function sendText() {
  if (!text.value.trim()) return
  send(
    { type: 'text', content: text.value, interval_ms: 8 },
    { successText: `文本已发送 (${text.value.length} 字符)`, timeoutMs: 6000 },
  )
}

function setQuickText(val) {
  text.value = val
  sendText()
}

function onTrackpadResult(response) {
  lastResult.value = {
    ...response,
    _timestamp: new Date().toLocaleTimeString(),
    _type: 'trackpad',
  }
}

function openTrackpadPage() {
  const route = router.resolve({
    path: '/trackpad',
    query: soleSelected.value ? { device: soleSelected.value } : {},
  })
  window.open(route.href, '_blank', 'noopener')
}

function openPureTrackpadPage() {
  const route = router.resolve({
    path: '/trackpad-pure',
    query: soleSelected.value ? { device: soleSelected.value } : {},
  })
  window.open(route.href, '_blank', 'noopener')
}

function openKioskTrackpadPage() {
  const route = router.resolve({
    path: '/trackpad-kiosk',
    query: soleSelected.value ? { device: soleSelected.value } : {},
  })
  window.open(route.href, '_blank', 'noopener')
}
</script>

<template>
  <div class="console-container">
    <!-- Top Bar with Device Switcher & Quick Launch -->
    <div class="console-header glass-panel">
      <div class="header-left">
        <div class="header-icon-box">
          <svg class="header-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="2" y="3" width="20" height="14" rx="2" ry="2"/>
            <line x1="8" y1="21" x2="16" y2="21"/>
            <line x1="12" y1="17" x2="12" y2="21"/>
          </svg>
        </div>
        <div>
          <h1 class="header-title">实时硬件控制中枢</h1>
          <p class="header-subtitle">Direct Hardware Ingestion & Low-latency Input Dispatcher</p>
        </div>
      </div>

      <div class="header-actions">
        <div class="device-selector-wrapper">
          <span class="device-badge-indicator" :class="{ 'online': isSelectedOnline }"></span>
          <el-select
            v-model="selected"
            placeholder="选择目标设备（可多选并发控制）"
            filterable
            multiple
            collapse-tags
            collapse-tags-tooltip
            class="device-select-custom"
          >
            <el-option
              v-for="device in devices"
              :key="device.id"
              :label="`${device.name || device.device_uuid} ${online.includes(device.device_uuid) ? '● 在线' : '○ 离线'}`"
              :value="device.device_uuid"
            >
              <div class="device-option-row">
                <span class="option-name">{{ device.name || '未命名设备' }}</span>
                <span class="option-uuid">{{ device.device_uuid }}</span>
                <span class="option-status" :class="online.includes(device.device_uuid) ? 'status-on' : 'status-off'">
                  {{ online.includes(device.device_uuid) ? '在线' : '离线' }}
                </span>
              </div>
            </el-option>
          </el-select>
        </div>

        <el-button class="refresh-btn" @click="load">
          <svg class="btn-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/>
          </svg>
          刷新
        </el-button>

        <div class="quick-touchpad-group">
          <button class="action-pill-btn" :disabled="!soleSelected" @click="openPureTrackpadPage" title="纯净手势触控板">
            <span class="pill-dot purple"></span>
            极简触控
          </button>
          <button class="action-pill-btn" :disabled="!soleSelected" @click="openKioskTrackpadPage" title="展厅全屏触控板">
            <span class="pill-dot cyan"></span>
            Kiosk 展厅
          </button>
          <button class="action-pill-btn primary" :disabled="!soleSelected" @click="openTrackpadPage" title="新标签打开全功能触控板">
            <span class="pill-dot blue"></span>
            全屏触控
          </button>
        </div>
      </div>
    </div>

    <!-- Main Grid: Left Control Cards & Right Virtual Input Surface -->
    <div class="console-body">
      <!-- Left Column: BLE Switcher, Hardware Status, Result Terminal -->
      <div class="console-col-side">
        <!-- Unified Output Target Card (USB + BLE) -->
        <div class="console-card glass-panel" v-loading="bleLoading">
          <div class="card-header-bar">
            <div class="card-title-group">
              <svg class="card-title-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                <path d="M6.5 6.5l11 11L12 23V1l5.5 5.5-11 11"/>
              </svg>
              <span class="card-title-text">输出目标主机</span>
            </div>
            <span class="ble-status-badge" :class="{ 'connected': bleConnected || (transportMode === 'usb' && usbConnected) }">
              {{ bleConnected || (transportMode === 'usb' && usbConnected) ? '已连接目标' : '待机空闲' }}
            </span>
          </div>

          <div class="ble-peer-info">
            <span class="peer-label">当前输出通道：</span>
            <code class="peer-code">{{ transportLabel }}</code>
          </div>

          <div class="ble-selector-row">
            <el-select
              v-model="targetPeer"
              placeholder="选择输出目标主机"
              size="default"
              style="flex: 1"
              :disabled="!soleSelected"
            >
              <el-option
                v-for="opt in targetOptions"
                :key="opt.value"
                :label="opt.label"
                :value="opt.value"
              />
            </el-select>
            <el-button
              type="primary"
              :disabled="!targetPeer || isCurrentTarget"
              @click="switchTarget"
            >
              切换
            </el-button>
          </div>

          <div v-if="!bonds.length && soleSelected" class="card-empty-hint">
            <span class="hint-icon">ℹ</span> 该设备尚未发现蓝牙配对主机，可选择 USB 主机（线缆直连）作为输出目标
          </div>
          <div v-if="selected.length > 1" class="card-empty-hint">
            <span class="hint-icon">ℹ</span> 已选中 {{ selected.length }} 台设备：输出目标仅对单选设备可配置，指令将同时下发至全部选中设备
          </div>
        </div>

        <!-- Real-Time Terminal Log -->
        <div class="console-card glass-panel terminal-card">
          <div class="card-header-bar">
            <div class="card-title-group">
              <div class="terminal-dots">
                <span class="t-dot red"></span>
                <span class="t-dot yellow"></span>
                <span class="t-dot green"></span>
              </div>
              <span class="card-title-text">指令流执行控制台</span>
            </div>
            <span v-if="lastResult?._timestamp" class="terminal-time">{{ lastResult._timestamp }}</span>
          </div>

          <div class="terminal-view">
            <div v-if="!lastResult" class="terminal-placeholder">
              <div class="term-cursor">></div>
              <span>等待下发硬件指令...</span>
            </div>
            <div v-else class="terminal-content">
              <div class="term-line status-line">
                <span class="term-prompt">$</span>
                <span class="term-type">{{ lastResult._type || 'action' }}</span>
                <span v-if="lastResult._key" class="term-arg">[{{ lastResult._key }}]</span>
                <span v-if="(lastResult.results || []).length > 1" class="term-arg">
                  × {{ (lastResult.results || []).length }} 台设备
                </span>
              </div>
              <div
                v-for="r in lastResult.results || []"
                :key="r.msg_id"
                class="term-line status-line"
              >
                <span class="term-prompt">›</span>
                <span class="term-type">{{ r.device_id }}</span>
                <span class="term-status-tag" :class="r.status === 'acked' ? 'success' : 'failed'">
                  {{ r.status || 'UNKNOWN' }}
                </span>
                <span v-if="r.exec_time_ms != null" class="term-latency">
                  {{ r.exec_time_ms }} ms
                </span>
                <span v-if="r.error_message" class="term-arg">{{ r.error_message }}</span>
              </div>
              <pre class="term-raw-json">{{ JSON.stringify(lastResult, null, 2) }}</pre>
            </div>
          </div>
        </div>
      </div>

      <!-- Right Column: Virtual Keyboard & Embedded Touchpad Surface -->
      <div class="console-col-main">
        <!-- Top Status Alert if no output target connected -->
        <div v-if="soleSelected && !bleConnected && !usbConnected" class="notice-banner">
          <div class="banner-icon-circle">⚡</div>
          <div class="banner-text">
            <strong>输出目标未就绪：</strong>
            未检测到活跃的 BLE 连接或 USB 主机，输入动作可能无法送达目标机器。请在左侧选择输出目标，或检查线缆连接与蓝牙配对状态。
          </div>
        </div>

        <!-- Virtual Keypad Deck -->
        <div class="console-card glass-panel keypad-deck">
          <div class="card-header-bar">
            <div class="card-title-group">
              <svg class="card-title-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                <rect x="2" y="4" width="20" height="16" rx="2"/>
                <path d="M6 8h.01M10 8h.01M14 8h.01M18 8h.01M6 12h.01M10 12h.01M14 12h.01M18 12h.01M7 16h10"/>
              </svg>
              <span class="card-title-text">仿真机械硬件键位矩阵</span>
            </div>
            <span class="deck-hint">点击即刻触发设备物理按键下发</span>
          </div>

          <!-- Shortcuts & Navigation Strip -->
          <div class="keypad-section">
            <div class="section-label">快捷功能 & 窗口切换</div>
            <div class="key-grid shortcuts-grid">
              <button
                v-for="k in SHORTCUT_KEYS"
                :key="k.key"
                class="mech-key special-key"
                :disabled="sending"
                @click="sendKey(k.key)"
              >
                <span class="key-cap">{{ k.label }}</span>
                <span class="key-sub">{{ k.hint }}</span>
              </button>
            </div>
          </div>

          <!-- Functional Control & Arrows -->
          <div class="keypad-dual-row">
            <div class="keypad-section flex-2">
              <div class="section-label">常用功能键</div>
              <div class="key-grid control-grid">
                <button
                  v-for="k in CONTROL_KEYS"
                  :key="k.key"
                  class="mech-key"
                  :class="{ 'primary-key': k.primary, 'danger-key': k.danger }"
                  :disabled="sending"
                  @click="sendKey(k.key)"
                >
                  <span class="key-cap">{{ k.label }}</span>
                </button>
              </div>
            </div>

            <div class="keypad-section flex-1">
              <div class="section-label">方向导航</div>
              <div class="key-grid nav-grid">
                <button
                  v-for="k in NAV_KEYS"
                  :key="k.key"
                  class="mech-key nav-key"
                  :disabled="sending"
                  @click="sendKey(k.key)"
                >
                  <span class="key-cap">{{ k.label }}</span>
                </button>
              </div>
            </div>
          </div>

          <!-- Alphabetical Matrix (Collapsible or Compact Grid) -->
          <div class="keypad-section">
            <div class="section-label">基础字符 (A - Z)</div>
            <div class="key-grid alpha-grid">
              <button
                v-for="key in ALPHA_KEYS"
                :key="key"
                class="mech-key alpha-key"
                :disabled="sending"
                @click="sendKey(key)"
              >
                <span class="key-cap">{{ key.replace('KEY_', '') }}</span>
              </button>
            </div>
          </div>
        </div>

        <!-- Text Stream Injector -->
        <div class="console-card glass-panel text-injector-card">
          <div class="card-header-bar">
            <div class="card-title-group">
              <svg class="card-title-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                <polyline points="4 7 4 4 20 4 20 7"/>
                <line x1="9" y1="20" x2="15" y2="20"/>
                <line x1="12" y1="4" x2="12" y2="20"/>
              </svg>
              <span class="card-title-text">流式文本批量输入</span>
            </div>
            <div class="quick-preset-chips">
              <span class="chip-label">快捷预设:</span>
              <button
                v-for="preset in QUICK_TEXTS"
                :key="preset"
                class="preset-chip"
                @click="setQuickText(preset)"
              >
                {{ preset }}
              </button>
            </div>
          </div>

          <div class="text-input-row">
            <el-input
              v-model="text"
              placeholder="在此输入需要下发至受控电脑的文本字符串，支持 Enter 回车直接发送..."
              clearable
              size="large"
              class="custom-input-box"
              @keyup.enter="sendText"
            >
              <template #prefix>
                <span class="input-prefix-icon">✍</span>
              </template>
            </el-input>
            <el-button
              type="primary"
              size="large"
              class="send-text-btn"
              :loading="sending"
              @click="sendText"
            >
              <svg class="send-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                <line x1="22" y1="2" x2="11" y2="13"/>
                <polygon points="22 2 15 22 11 13 2 9 22 2"/>
              </svg>
              发送文本
            </el-button>
          </div>
        </div>

        <!-- Real mouse & keyboard takeover via Pointer Lock -->
        <MouseTakeoverPanel
          class="console-card glass-panel"
          :device-uuid="soleSelected"
          :device-online="isSoleOnline"
        />

        <!-- Embedded Touchpad Surface -->
        <div class="embedded-trackpad-wrap">
          <TrackpadPanel
            :device-uuid="soleSelected"
            :device-online="isSoleOnline"
            @result="onTrackpadResult"
          />
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.console-container {
  display: flex;
  flex-direction: column;
  gap: 20px;
  width: 100%;
}

/* Top Header Bar */
.console-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 16px 24px;
  border-radius: var(--radius-lg);
  flex-wrap: wrap;
  gap: 16px;
}

.header-left {
  display: flex;
  align-items: center;
  gap: 14px;
}

.header-icon-box {
  width: 44px;
  height: 44px;
  border-radius: var(--radius-md);
  background: linear-gradient(135deg, rgba(59, 130, 246, 0.2), rgba(139, 92, 246, 0.2));
  border: 1px solid rgba(59, 130, 246, 0.3);
  display: flex;
  align-items: center;
  justify-content: center;
}

.header-icon {
  width: 22px;
  height: 22px;
  color: var(--color-primary);
}

.header-title {
  margin: 0;
  font-size: 18px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.header-subtitle {
  margin: 2px 0 0 0;
  font-size: 12px;
  color: var(--color-text-secondary);
}

.header-actions {
  display: flex;
  align-items: center;
  gap: 12px;
  flex-wrap: wrap;
}

.device-selector-wrapper {
  position: relative;
  display: flex;
  align-items: center;
}

.device-badge-indicator {
  position: absolute;
  left: 12px;
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--color-danger);
  z-index: 2;
  transition: all 0.3s;
}

.device-badge-indicator.online {
  background: var(--color-success);
  box-shadow: 0 0 10px rgba(16, 185, 129, 0.6);
}

.device-select-custom {
  width: 280px;
}

.device-select-custom :deep(.el-input__wrapper) {
  padding-left: 28px;
  background: var(--color-bg-card);
  border-radius: var(--radius-md);
  border: 1px solid var(--color-border);
}

.device-option-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  width: 100%;
  gap: 8px;
}

.option-name {
  font-weight: 600;
  color: var(--color-text-primary);
}

.option-uuid {
  font-size: 11px;
  color: var(--color-text-muted);
  font-family: monospace;
}

.option-status {
  font-size: 11px;
  padding: 2px 6px;
  border-radius: 4px;
}

.option-status.status-on {
  color: #10b981;
  background: rgba(16, 185, 129, 0.1);
}

.option-status.status-off {
  color: #94a3b8;
  background: rgba(148, 163, 184, 0.1);
}

.refresh-btn {
  display: flex;
  align-items: center;
  gap: 6px;
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  color: var(--color-text-secondary);
  border-radius: var(--radius-md);
}

.btn-icon {
  width: 14px;
  height: 14px;
}

.quick-touchpad-group {
  display: flex;
  align-items: center;
  gap: 6px;
}

.action-pill-btn {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 12px;
  border-radius: var(--radius-pill);
  border: 1px solid var(--color-border);
  background: var(--color-bg-card);
  color: var(--color-text-secondary);
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s cubic-bezier(0.16, 1, 0.3, 1);
}

.action-pill-btn:hover:not(:disabled) {
  border-color: var(--color-primary);
  color: var(--color-primary);
  transform: translateY(-1px);
}

.action-pill-btn.primary {
  background: linear-gradient(135deg, rgba(59, 130, 246, 0.2), rgba(139, 92, 246, 0.2));
  border-color: rgba(59, 130, 246, 0.4);
  color: var(--color-primary-glow);
}

.action-pill-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.pill-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
}

.pill-dot.purple { background: #a855f7; }
.pill-dot.cyan { background: #06b6d4; }
.pill-dot.blue { background: #3b82f6; }

/* Console Body Layout */
.console-body {
  display: grid;
  grid-template-columns: 340px 1fr;
  gap: 20px;
  align-items: start;
}

@media (max-width: 1180px) {
  .console-body {
    grid-template-columns: 1fr;
  }
}

.console-col-side {
  display: flex;
  flex-direction: column;
  gap: 20px;
}

.console-col-main {
  display: flex;
  flex-direction: column;
  gap: 20px;
  min-width: 0;
}

/* Card Styling */
.console-card {
  padding: 18px 20px;
  border-radius: var(--radius-lg);
  display: flex;
  flex-direction: column;
  gap: 14px;
}

.card-header-bar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  flex-wrap: wrap;
  gap: 8px;
}

.card-title-group {
  display: flex;
  align-items: center;
  gap: 8px;
}

.card-title-icon {
  width: 16px;
  height: 16px;
  color: var(--color-primary);
}

.card-title-text {
  font-size: 14px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.ble-status-badge {
  font-size: 11px;
  padding: 3px 8px;
  border-radius: var(--radius-pill);
  background: rgba(148, 163, 184, 0.15);
  color: #94a3b8;
  font-weight: 600;
}

.ble-status-badge.connected {
  background: rgba(16, 185, 129, 0.15);
  color: #10b981;
}

.ble-peer-info {
  display: flex;
  flex-direction: column;
  gap: 4px;
  font-size: 12px;
}

.peer-label {
  color: var(--color-text-secondary);
}

.peer-code {
  background: rgba(15, 23, 42, 0.5);
  padding: 6px 10px;
  border-radius: var(--radius-sm);
  border: 1px solid var(--color-border);
  color: #38bdf8;
  font-family: monospace;
  font-size: 12px;
}

.ble-selector-row {
  display: flex;
  gap: 8px;
}

.card-empty-hint {
  font-size: 12px;
  color: var(--color-text-muted);
  background: rgba(148, 163, 184, 0.05);
  padding: 8px 12px;
  border-radius: var(--radius-sm);
}

/* Terminal Card */
.terminal-card {
  background: rgba(15, 23, 42, 0.85);
  border: 1px solid rgba(59, 130, 246, 0.2);
}

.terminal-dots {
  display: flex;
  gap: 5px;
}

.t-dot {
  width: 9px;
  height: 9px;
  border-radius: 50%;
}
.t-dot.red { background: #ef4444; }
.t-dot.yellow { background: #eab308; }
.t-dot.green { background: #22c55e; }

.terminal-time {
  font-size: 11px;
  color: #64748b;
  font-family: monospace;
}

.terminal-view {
  min-height: 180px;
  max-height: 380px;
  overflow-y: auto;
  background: #0f172a;
  border: 1px solid #1e293b;
  border-radius: var(--radius-md);
  padding: 12px;
  font-family: var(--font-mono);
  font-size: 12px;
  color: #f8fafc;
}

.terminal-placeholder {
  display: flex;
  align-items: center;
  gap: 8px;
  color: #94a3b8;
  height: 140px;
  justify-content: center;
}

.term-cursor {
  color: #38bdf8;
  font-weight: bold;
}

.term-line {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 8px;
  flex-wrap: wrap;
  color: #f8fafc;
}

.term-prompt {
  color: #c084fc;
  font-weight: bold;
}

.term-type {
  color: #38bdf8;
  font-weight: 600;
}

.term-arg {
  color: #ffffff;
}

.term-status-tag {
  font-size: 10px;
  font-weight: 700;
  padding: 2px 6px;
  border-radius: 4px;
  text-transform: uppercase;
}

.term-status-tag.success {
  background: rgba(34, 197, 94, 0.2);
  color: #4ade80;
  border: 1px solid rgba(34, 197, 94, 0.4);
}

.term-status-tag.failed {
  background: rgba(239, 68, 68, 0.2);
  color: #f87171;
  border: 1px solid rgba(239, 68, 68, 0.4);
}

.term-latency {
  color: #cbd5e1;
  font-size: 11px;
}

.term-raw-json {
  margin: 0;
  color: #cbd5e1;
  font-size: 11px;
  white-space: pre-wrap;
  word-break: break-all;
}

/* Notice Banner */
.notice-banner {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 16px;
  border-radius: var(--radius-md);
  background: #fffbeb;
  border: 1px solid #fde68a;
  color: #92400e;
  font-size: 13px;
}

.banner-icon-circle {
  font-size: 16px;
  color: #d97706;
}

.banner-text {
  line-height: 1.4;
  color: #78350f;
}

.banner-text strong {
  color: #92400e;
}

/* Keypad Deck & Keys */
.keypad-deck {
  gap: 16px;
}

.deck-hint {
  font-size: 12px;
  color: var(--color-text-muted);
}

.keypad-section {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.section-label {
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  font-weight: 700;
  color: var(--color-text-muted);
}

.keypad-dual-row {
  display: flex;
  gap: 16px;
}

.flex-1 { flex: 1; }
.flex-2 { flex: 2; }

@media (max-width: 768px) {
  .keypad-dual-row {
    flex-direction: column;
  }
}

.key-grid {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

/* Tactile Mechanical Keycaps - High Contrast Clean Styling */
.mech-key {
  position: relative;
  display: inline-flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  padding: 10px 14px;
  min-width: 52px;
  background: linear-gradient(180deg, #ffffff 0%, #f8fafc 100%);
  border: 1px solid #cbd5e1;
  border-bottom: 3px solid #94a3b8;
  border-radius: 8px;
  color: #0f172a;
  cursor: pointer;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.05);
  transition: all 0.12s cubic-bezier(0.16, 1, 0.3, 1);
  user-select: none;
}

.mech-key:hover:not(:disabled) {
  border-color: #3b82f6;
  border-bottom-color: #2563eb;
  color: #2563eb;
  transform: translateY(-2px);
  box-shadow: 0 4px 8px rgba(37, 99, 235, 0.15);
}

.mech-key:active:not(:disabled) {
  transform: translateY(2px);
  border-bottom-width: 1px;
  box-shadow: 0 1px 2px rgba(0, 0, 0, 0.08);
}

.mech-key:disabled {
  opacity: 0.45;
  cursor: not-allowed;
}

.key-cap {
  font-size: 13px;
  font-weight: 700;
  letter-spacing: 0.02em;
  color: inherit;
}

.key-sub {
  font-size: 10px;
  color: #64748b;
  margin-top: 2px;
}

.special-key {
  flex: 1;
  min-width: 110px;
  background: linear-gradient(180deg, #f0f7ff 0%, #e0f2fe 100%);
  border-color: #93c5fd;
  border-bottom-color: #60a5fa;
  color: #0369a1;
}

.special-key .key-cap {
  color: #0369a1;
}

.special-key .key-sub {
  color: #0284c7;
}

.primary-key {
  background: linear-gradient(180deg, #3b82f6 0%, #2563eb 100%);
  border-color: #2563eb;
  border-bottom-color: #1d4ed8;
  color: #ffffff !important;
}

.primary-key .key-cap {
  color: #ffffff !important;
}

.danger-key {
  background: linear-gradient(180deg, #fef2f2 0%, #fee2e2 100%);
  border-color: #fca5a5;
  border-bottom-color: #f87171;
  color: #b91c1c;
}

.danger-key .key-cap {
  color: #b91c1c;
}

.alpha-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(42px, 1fr));
  gap: 6px;
}

.alpha-key {
  min-width: 40px;
  padding: 8px;
}

/* Text Stream Injector */
.text-injector-card {
  gap: 12px;
}

.quick-preset-chips {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-wrap: wrap;
}

.chip-label {
  font-size: 11px;
  color: var(--color-text-muted);
}

.preset-chip {
  background: rgba(148, 163, 184, 0.1);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-pill);
  padding: 2px 8px;
  font-size: 11px;
  color: var(--color-text-secondary);
  cursor: pointer;
  transition: all 0.2s;
}

.preset-chip:hover {
  background: rgba(59, 130, 246, 0.2);
  border-color: var(--color-primary);
  color: var(--color-primary-glow);
}

.text-input-row {
  display: flex;
  gap: 10px;
}

.custom-input-box {
  flex: 1;
}

.input-prefix-icon {
  font-size: 14px;
}

.send-text-btn {
  display: flex;
  align-items: center;
  gap: 6px;
  border-radius: var(--radius-md);
  font-weight: 600;
  padding: 0 20px;
}

.send-icon {
  width: 16px;
  height: 16px;
}

.embedded-trackpad-wrap {
  border-radius: var(--radius-lg);
  overflow: hidden;
}
</style>
