<script>
export const meta = { title: '实时控制' }
</script>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { ElMessage } from 'element-plus'

import TrackpadPanel from '@/components/TrackpadPanel.vue'
import { actionsApi } from '@/api/actions'
import { commandsApi } from '@/api/commands'
import { devicesApi } from '@/api/devices'

const devices = ref([])
const online = ref([])
const selected = ref('')
const text = ref('')
const sending = ref(false)
const lastResult = ref(null)

const bonds = ref([])
const currentPeer = ref('')
const targetPeer = ref('')
const bleConnected = ref(false)
const bleLoading = ref(false)

const KEYS = [
  'KEY_A', 'KEY_B', 'KEY_C', 'KEY_ENTER', 'KEY_SPACE', 'KEY_TAB', 'KEY_ESC', 'KEY_BACKSPACE',
  'KEY_UP', 'KEY_DOWN', 'KEY_LEFT', 'KEY_RIGHT',
  'KEY_ALT_TAB', 'KEY_ALT_SHIFT_TAB', 'KEY_GUI_D', 'KEY_GUI_TAB',
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

const selectedDevice = computed(() => devices.value.find((item) => item.device_uuid === selected.value) || null)
const isSelectedOnline = computed(() => !!selected.value && online.value.includes(selected.value))

async function load() {
  devices.value = await devicesApi.list()
  online.value = (await devicesApi.online()).online
  if (!selected.value && online.value.length) selected.value = online.value[0]
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

async function loadBleTarget() {
  if (!selected.value || !isSelectedOnline.value) {
    bonds.value = []
    currentPeer.value = ''
    targetPeer.value = ''
    bleConnected.value = false
    return
  }

  bleLoading.value = true
  try {
    const info = extractCommandResult(await commandsApi.bleInfo(selected.value)) || {}
    bleConnected.value = !!info.connected
    currentPeer.value = fmtAddr(info.peer?.addr || info.peer || info.current?.addr)

    const bondResult = extractCommandResult(await commandsApi.bleBonds(selected.value)) || {}
    bonds.value = (bondResult.bonds || []).map((item) => ({
      ...item,
      addrStr: fmtAddr(item.addr),
    }))
    targetPeer.value = currentPeer.value || bonds.value[0]?.addrStr || ''
  } catch {
    bonds.value = []
    currentPeer.value = ''
    targetPeer.value = ''
    bleConnected.value = false
  } finally {
    bleLoading.value = false
  }
}

watch(selected, loadBleTarget)

async function switchTarget() {
  if (!targetPeer.value || targetPeer.value === currentPeer.value) return
  const target = bonds.value.find((item) => item.addrStr === targetPeer.value)
  if (!target) return ElMessage.warning('Target bond not found.')

  bleLoading.value = true
  try {
    const response = await commandsApi.bleSwitch(selected.value, target.addr_type ?? 0, addrToBytes(targetPeer.value))
    if (response?.status === 'acked' || response?.status === 'ok') {
      ElMessage.success(`Switched to ${targetPeer.value}`)
      await loadBleTarget()
    } else {
      ElMessage.warning(`Switch failed: ${response?.error_message || 'unknown error'}`)
    }
  } finally {
    bleLoading.value = false
  }
}

async function send(payload, { successText = '', timeoutMs = 3000 } = {}) {
  if (!selected.value) return ElMessage.warning('Select a device first.')
  sending.value = true
  try {
    const response = await actionsApi.send({
      device_id: selected.value,
      payload,
      timeout_ms: timeoutMs,
    })
    lastResult.value = response
    const result = response.results?.[0]
    if (result?.status === 'acked') {
      if (successText) ElMessage.success(successText)
    } else {
      ElMessage.warning(`${result?.status || 'failed'} ${result?.error_message || ''}`.trim())
    }
  } finally {
    sending.value = false
  }
}

function sendKey(key) {
  send({ type: 'keyboard_tap', key }, { successText: `${key} sent` })
}

function sendText() {
  if (!text.value.trim()) return
  send(
    { type: 'text', content: text.value, interval_ms: 10 },
    { successText: 'Text sent', timeoutMs: 6000 },
  )
}

function onTrackpadResult(response) {
  lastResult.value = response
}
</script>

<template>
  <el-row :gutter="16">
    <el-col :span="8">
      <el-card header="Device">
        <el-select v-model="selected" placeholder="Select device" style="width: 100%">
          <el-option
            v-for="device in devices"
            :key="device.id"
            :label="`${device.name || device.device_uuid} ${online.includes(device.device_uuid) ? '[online]' : '[offline]'}`"
            :value="device.device_uuid"
          />
        </el-select>
        <div style="margin-top: 8px; display: flex; gap: 8px">
          <el-button size="small" @click="load">Refresh</el-button>
          <el-tag size="small" :type="isSelectedOnline ? 'success' : 'info'">
            {{ isSelectedOnline ? 'online' : 'offline' }}
          </el-tag>
        </div>
      </el-card>

      <el-card style="margin-top: 16px" v-loading="bleLoading">
        <template #header>
          <span>BLE Target Host</span>
          <el-tag size="small" :type="bleConnected ? 'success' : 'info'" style="margin-left: 8px">
            {{ bleConnected ? 'connected' : 'idle' }}
          </el-tag>
        </template>

        <div style="font-size: 12px; color: #909399; margin-bottom: 8px">
          Current peer: {{ currentPeer || '-' }}
        </div>

        <el-select
          v-model="targetPeer"
          placeholder="Choose a bonded host"
          style="width: 100%"
          :disabled="!bonds.length"
        >
          <el-option
            v-for="bond in bonds"
            :key="bond.addrStr"
            :label="bond.addrStr + (bond.addrStr === currentPeer ? ' (current)' : '')"
            :value="bond.addrStr"
          />
        </el-select>

        <div style="margin-top: 8px; display: flex; gap: 8px">
          <el-button size="small" @click="loadBleTarget">Refresh</el-button>
          <el-button
            size="small"
            type="primary"
            :disabled="!targetPeer || targetPeer === currentPeer"
            @click="switchTarget"
          >
            Switch
          </el-button>
        </div>

        <el-empty v-if="!bonds.length && selected" description="No bonded hosts on this device yet." :image-size="60" />
      </el-card>

      <el-card header="Last Result" style="margin-top: 16px">
        <pre style="max-height: 320px; overflow: auto; margin: 0; font-size: 12px">{{ JSON.stringify(lastResult, null, 2) }}</pre>
      </el-card>
    </el-col>

    <el-col :span="16">
      <el-alert
        v-if="selected && !bleConnected"
        type="info"
        :closable="false"
        title="BLE host is not connected. Keyboard macros can still work over USB, but BLE host switching requires a paired and connected peer."
        style="margin-bottom: 16px"
        show-icon
      />

      <el-card header="Keyboard">
        <el-button
          v-for="key in KEYS"
          :key="key"
          :loading="sending"
          size="small"
          style="margin: 4px"
          @click="sendKey(key)"
        >
          {{ key }}
        </el-button>
      </el-card>

      <el-card header="Text Input" style="margin-top: 16px">
        <el-input v-model="text" placeholder="Type text to send to the selected device" @keyup.enter="sendText">
          <template #append>
            <el-button @click="sendText" :loading="sending">Send</el-button>
          </template>
        </el-input>
      </el-card>

      <div style="margin-top: 16px">
        <TrackpadPanel
          :device-uuid="selected"
          :device-online="isSelectedOnline"
          @result="onTrackpadResult"
        />
      </div>
    </el-col>
  </el-row>
</template>
