<script>
export const meta = { title: '实时控制' }
</script>

<script setup>
import { onMounted, ref, watch } from 'vue'
import { ElMessage } from 'element-plus'
import { devicesApi } from '@/api/devices'
import { actionsApi } from '@/api/actions'
import { commandsApi } from '@/api/commands'

const devices = ref([])
const online = ref([])
const selected = ref('')
const text = ref('')
const sending = ref(false)
const lastResult = ref(null)

const bonds = ref([])         // [{ addr, addr_type }]
const currentPeer = ref('')   // 当前 active peer 的 addr 字符串
const targetPeer = ref('')    // 用户选择要切到的 peer addr 字符串
const bleConnected = ref(false)
const bleLoading = ref(false)

const KEYS = ['KEY_A','KEY_B','KEY_C','KEY_ENTER','KEY_SPACE','KEY_TAB','KEY_ESC','KEY_BACKSPACE',
  'KEY_UP','KEY_DOWN','KEY_LEFT','KEY_RIGHT']

function fmtAddr(addr) {
  if (typeof addr === 'string') return addr.toUpperCase()
  if (Array.isArray(addr)) return addr.map(b => b.toString(16).padStart(2,'0')).join(':').toUpperCase()
  return ''
}
function addrToBytes(s) {
  if (!s) return null
  return s.split(':').map(x => parseInt(x, 16))
}

async function load() {
  devices.value = await devicesApi.list()
  online.value = (await devicesApi.online()).online
  if (!selected.value && online.value.length) selected.value = online.value[0]
}
onMounted(load)

async function loadBleTarget() {
  if (!selected.value || !online.value.includes(selected.value)) {
    bonds.value = []; currentPeer.value = ''; targetPeer.value = ''; bleConnected.value = false
    return
  }
  bleLoading.value = true
  try {
    const info = await commandsApi.bleInfo(selected.value)
    const ir = info?.result?.result || info?.result || {}
    bleConnected.value = !!ir.connected
    currentPeer.value = fmtAddr(ir.peer || ir.current?.addr)

    const bondsResp = await commandsApi.bleBonds(selected.value)
    const br = bondsResp?.result?.result || bondsResp?.result || {}
    bonds.value = (br.bonds || []).map(b => ({ ...b, addrStr: fmtAddr(b.addr) }))
    targetPeer.value = currentPeer.value || (bonds.value[0]?.addrStr ?? '')
  } catch (e) {
    // 离线/暂不可用，不打扰主流程
  } finally {
    bleLoading.value = false
  }
}
watch(selected, loadBleTarget)

async function switchTarget() {
  if (!targetPeer.value || targetPeer.value === currentPeer.value) return
  const b = bonds.value.find(x => x.addrStr === targetPeer.value)
  if (!b) return ElMessage.warning('未找到该已配对设备')
  bleLoading.value = true
  try {
    const addr = addrToBytes(targetPeer.value)
    const resp = await commandsApi.bleSwitch(selected.value, b.addr_type ?? 0, addr)
    if (resp?.status === 'acked' || resp?.status === 'ok') {
      ElMessage.success(`已切换到 ${targetPeer.value}`)
      await loadBleTarget()
    } else {
      ElMessage.warning(`切换失败：${resp?.error_message || ''}`)
    }
  } finally {
    bleLoading.value = false
  }
}

async function send(payload) {
  if (!selected.value) return ElMessage.warning('请先选择目标设备')
  sending.value = true
  try {
    lastResult.value = await actionsApi.send({ device_id: selected.value, payload, timeout_ms: 3000 })
    const r = lastResult.value.results?.[0]
    if (r?.status === 'acked') ElMessage.success(`成功 ${r.exec_time_ms}ms`)
    else ElMessage.warning(`${r?.status} ${r?.error_message || ''}`)
  } finally {
    sending.value = false
  }
}

function sendKey(k)   { send({ type: 'keyboard_tap', key: k }) }
function sendText()   { if (text.value) send({ type: 'text', content: text.value, interval_ms: 10 }) }
function mouseMove(x, y) { send({ type: 'mouse_move', x, y }) }
function mouseClick(b)   { send({ type: 'mouse_click', button: b }) }
</script>

<template>
  <el-row :gutter="16">
    <el-col :span="8">
      <el-card header="设备">
        <el-select v-model="selected" placeholder="选择设备" style="width:100%">
          <el-option v-for="d in devices" :key="d.id" :label="`${d.name || d.device_uuid} ${online.includes(d.device_uuid) ? '🟢' : '⚪'}`" :value="d.device_uuid" />
        </el-select>
        <el-button @click="load" style="margin-top:8px" size="small">刷新</el-button>
      </el-card>

      <el-card style="margin-top:16px" v-loading="bleLoading">
        <template #header>
          <span>目标主机（BLE）</span>
          <el-tag size="small" :type="bleConnected ? 'success' : 'info'" style="margin-left:8px">
            {{ bleConnected ? 'connected' : 'idle' }}
          </el-tag>
        </template>
        <div style="font-size:12px; color:#909399; margin-bottom:8px">
          当前：{{ currentPeer || '—' }}
        </div>
        <el-select v-model="targetPeer" placeholder="选择已配对主机" style="width:100%" :disabled="!bonds.length">
          <el-option v-for="b in bonds" :key="b.addrStr"
            :label="b.addrStr + (b.addrStr === currentPeer ? '  (当前)' : '')"
            :value="b.addrStr" />
        </el-select>
        <div style="margin-top:8px; display:flex; gap:8px">
          <el-button size="small" @click="loadBleTarget">刷新</el-button>
          <el-button size="small" type="primary" :disabled="!targetPeer || targetPeer === currentPeer" @click="switchTarget">切换</el-button>
        </div>
        <el-empty v-if="!bonds.length && selected" description="暂无已配对主机" :image-size="60" />
      </el-card>

      <el-card header="最近结果" style="margin-top:16px">
        <pre style="max-height:240px; overflow:auto; margin:0; font-size:12px">{{ JSON.stringify(lastResult, null, 2) }}</pre>
      </el-card>
    </el-col>

    <el-col :span="16">
      <el-alert v-if="selected && !bleConnected" type="warning" :closable="false"
        title="目标主机未连接：请先在 PC/手机上完成与该设备的 BLE 配对" style="margin-bottom:16px" show-icon />

      <el-card header="键盘">
        <el-button v-for="k in KEYS" :key="k" :loading="sending" size="small" style="margin:4px" @click="sendKey(k)">{{ k }}</el-button>
      </el-card>

      <el-card header="文本输入" style="margin-top:16px">
        <el-input v-model="text" placeholder="输入要发送的文本" @keyup.enter="sendText">
          <template #append>
            <el-button @click="sendText" :loading="sending">发送</el-button>
          </template>
        </el-input>
      </el-card>

      <el-card header="鼠标" style="margin-top:16px">
        <el-space wrap>
          <el-button @click="mouseMove(0, -20)">↑ 20</el-button>
          <el-button @click="mouseMove(0,  20)">↓ 20</el-button>
          <el-button @click="mouseMove(-20, 0)">← 20</el-button>
          <el-button @click="mouseMove( 20, 0)">→ 20</el-button>
          <el-divider direction="vertical" />
          <el-button type="primary" @click="mouseClick(1)">左键</el-button>
          <el-button type="warning" @click="mouseClick(2)">右键</el-button>
        </el-space>
      </el-card>
    </el-col>
  </el-row>
</template>
