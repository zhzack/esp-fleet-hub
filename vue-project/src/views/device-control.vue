<script setup>
import { computed, ref, watch } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { commandsApi } from '@/api/commands'
import { devicesApi } from '@/api/devices'
import { macrosApi } from '@/api/macros'
import DisplayConfigPanel from '@/components/DisplayConfigPanel.vue'
import TimerConfigPanel from '@/components/TimerConfigPanel.vue'
import { getCapabilityFlags } from '@/utils/capabilities'

const props = defineProps({
  modelValue: { type: Boolean, default: false },
  deviceUuid: { type: String, default: '' },
  deviceName: { type: String, default: '' },
  capabilities: { type: Array, default: () => [] },
})
const emit = defineEmits(['update:modelValue'])

const localCaps = ref([])
const capFlags = computed(() =>
  getCapabilityFlags(props.capabilities?.length ? props.capabilities : localCaps.value),
)

const tab = ref('status')
const loading = ref(false)
const lastResult = ref(null)

const statusData = ref(null)
const wifiStatusData = ref(null)
const wifiScanList = ref([])
const bleInfoData = ref(null)
const bleBonds = ref([])
const bleScanList = ref([])
const transportData = ref(null)
const displayStatusData = ref(null)
const displayErrorMsg = ref('')
const gpioPinsData = ref([])
const gpioBoardName = ref('')
const gpioErrorMsg = ref('')
const gpioPulseSec = ref(0)
const gpioPulseHint = ref('')
const macrosList = ref([])
const timerSeed = ref(null)
const slotsList = ref([])     // {slot_index, addr_type, addr, name, last_used_at}
const wifiKnownList = ref([]) // [{ssid, label, has_password, last_seen_at}]

const wifiForm = ref({ ssid: '', password: '' })
const showRawStatus = ref(false)

function close() { emit('update:modelValue', false) }

function extractResult(resp) {
  if (!resp) return null
  let cur = resp.result !== undefined ? resp.result : resp.results?.[0]?.result
  if (cur === undefined) cur = resp
  if (cur && typeof cur === 'object' && cur.result !== undefined) {
    return cur.result
  }
  return cur
}

async function call(fn, label, { silent = false } = {}) {
  loading.value = true
  try {
    const resp = await fn()
    lastResult.value = resp
    const r = extractResult(resp)
    if (resp?.status && resp.status !== 'acked' && resp.status !== 'ok') {
      if (!silent) ElMessage.warning(`${label}: ${resp.status} ${resp.error_message || ''}`)
    } else {
      if (!silent) ElMessage.success(`${label} 成功`)
    }
    return r
  } catch (e) {
    return null
  } finally {
    loading.value = false
  }
}

async function loadDisplayStatus(silent = true) {
  if (!capFlags.value.supportsDisplay) return
  loading.value = true
  displayErrorMsg.value = ''
  try {
    const resp = await commandsApi.displayStatus(props.deviceUuid)
    lastResult.value = resp
    if (resp?.status && resp.status !== 'acked' && resp.status !== 'ok') {
      const msg = `${resp.error_code || resp.status}: ${resp.error_message || '设备未响应'}`
      displayErrorMsg.value = msg
      if (!silent) ElMessage.warning(`读取屏幕配置失败: ${msg}`)
      return
    }
    displayStatusData.value = extractResult(resp)
    if (!silent) ElMessage.success('已读取当前屏幕配置')
  } catch (e) {
    displayErrorMsg.value = e?.response?.data?.detail || e?.message || '读取屏幕状态失败'
  } finally {
    loading.value = false
  }
}

async function saveDisplayConfig(payload) {
  loading.value = true
  displayErrorMsg.value = ''
  try {
    const resp = await commandsApi.displayConfig(props.deviceUuid, payload)
    lastResult.value = resp
    if (resp?.status && resp.status !== 'acked' && resp.status !== 'ok') {
      const msg = `${resp.error_code || resp.status}: ${resp.error_message || '下发失败'}`
      displayErrorMsg.value = msg
      ElMessage.warning(`屏幕配置未生效 (${msg})`)
      return
    }
    const r = extractResult(resp)
    if (r) displayStatusData.value = r
    ElMessage.success('屏幕配置已保存并应用到设备')
  } catch (e) {
    displayErrorMsg.value = e?.response?.data?.detail || e?.message || '屏幕配置下发失败'
  } finally {
    loading.value = false
  }
}

async function loadGpioPins(silent = true) {
  if (!capFlags.value.supportsGpio) return
  loading.value = true
  gpioErrorMsg.value = ''
  try {
    const resp = await commandsApi.gpioPins(props.deviceUuid)
    lastResult.value = resp
    if (resp?.status && resp.status !== 'acked' && resp.status !== 'ok') {
      const msg = `${resp.error_code || resp.status}: ${resp.error_message || '读取引脚失败'}`
      gpioErrorMsg.value = msg
      if (!silent) ElMessage.warning(`读取 GPIO 引脚失败: ${msg}`)
      return
    }
    const r = extractResult(resp)
    gpioBoardName.value = r?.board || ''
    gpioPinsData.value = (Array.isArray(r?.pins) ? r.pins : []).map((p) => ({
      ...p,
      _draftDuty: p.duty ?? 50,
    }))
    if (!silent) ElMessage.success(`已刷新 ${gpioPinsData.value.length} 个 GPIO 引脚状态`)
  } catch (e) {
    gpioErrorMsg.value = e?.response?.data?.detail || e?.message || '读取 GPIO 状态失败'
  } finally {
    loading.value = false
  }
}

async function applyGpioPin(pinRow, { op = null, val = null, duty = null, durationSec = undefined } = {}) {
  loading.value = true
  gpioErrorMsg.value = ''
  const targetUuid = props.deviceUuid
  const pulse = op === 'off' ? 0 : Number(durationSec !== undefined ? durationSec : gpioPulseSec.value) || 0
  try {
    const resp = await commandsApi.gpioSet(targetUuid, {
      pin: pinRow.pin,
      op,
      val,
      duty,
      duration_sec: pulse,
    })
    lastResult.value = resp
    if (resp?.status && resp.status !== 'acked' && resp.status !== 'ok') {
      const msg = `${resp.error_code || resp.status}: ${resp.error_message || 'GPIO 控制未应答'}`
      gpioErrorMsg.value = msg
      ElMessage.warning(msg)
      return
    }
    if (resp?.pulse_restore_sec > 0) {
      gpioPulseHint.value = `GPIO ${pinRow.pin} 已开启，将在 ${resp.pulse_restore_sec} 秒后自动复原关闭`
      ElMessage.success(gpioPulseHint.value)
      setTimeout(() => {
        if (props.modelValue && props.deviceUuid === targetUuid && tab.value === 'gpio') {
          gpioPulseHint.value = ''
          loadGpioPins(true)
        }
      }, (resp.pulse_restore_sec + 1) * 1000)
    } else {
      gpioPulseHint.value = ''
      ElMessage.success(`GPIO ${pinRow.pin} 状态已更新`)
    }
    await loadGpioPins(true)
  } catch (e) {
    gpioErrorMsg.value = e?.response?.data?.detail || e?.message || 'GPIO 控制失败'
  } finally {
    loading.value = false
  }
}

function scheduleTimerForPin(pinRow, op = 'on') {
  timerSeed.value = {
    _ts: Date.now(),
    name: `${pinRow.label || `GPIO ${pinRow.pin}`} 定时${op === 'off' ? '关闭' : '开启'}`,
    schedule_type: 'countdown',
    delay_sec: 10,
    action_type: 'gpio_set',
    gpio_pin: pinRow.pin,
    gpio_op: op,
    gpio_duty: pinRow._draftDuty ?? 50,
    gpio_duration_sec: gpioPulseSec.value || 0,
  }
  tab.value = 'timers'
}

async function loadMacrosOnce() {
  if (macrosList.value.length) return
  try { macrosList.value = (await macrosApi.list()) || [] }
  catch { macrosList.value = [] }
}

async function loadStatus(silent = true) {
  statusData.value = await call(() => commandsApi.status(props.deviceUuid), '查询硬件状态', { silent })
}

async function loadWifiStatus(silent = true) {
  wifiStatusData.value = await call(() => commandsApi.wifiStatus(props.deviceUuid), 'WiFi 状态', { silent })
}

async function doWifiScan() {
  if (!wifiStatusData.value) await loadWifiStatus(true)
  if (!wifiKnownList.value.length) await loadKnownWifi()
  const r = await call(() => commandsApi.wifiScan(props.deviceUuid), 'WiFi 扫描')
  const list = r?.networks || r?.aps || (Array.isArray(r) ? r : [])
  const cur = wifiStatusData.value?.ssid
  const knownSet = new Set(wifiKnownList.value.map(w => w.ssid))
  wifiScanList.value = list.map(n => ({
    ...n,
    _status: cur && n.ssid === cur ? 'using'
           : knownSet.has(n.ssid) ? 'saved'
           : null,
  }))
}

async function loadKnownWifi() {
  try { wifiKnownList.value = await commandsApi.listKnownWifi(props.deviceUuid) || [] }
  catch { wifiKnownList.value = [] }
}

async function applyKnownWifi(row) {
  await call(() => commandsApi.applyKnownWifi(props.deviceUuid, row.ssid), `连接 ${row.ssid}`)
  await loadWifiStatus(true)
}

async function deleteKnownWifi(row) {
  try {
    await ElMessageBox.confirm(`从已保存列表删除 ${row.ssid}？`, '确认删除', { type: 'warning' })
    await commandsApi.deleteKnownWifi(props.deviceUuid, row.ssid)
    ElMessage.success('已删除')
    await loadKnownWifi()
  } catch { /* canceled */ }
}

async function doWifiConnect() {
  if (!wifiForm.value.ssid) return ElMessage.warning('请填写 WiFi SSID')
  await call(() => commandsApi.wifiConnect(props.deviceUuid, wifiForm.value.ssid, wifiForm.value.password || ''),
             `连接 ${wifiForm.value.ssid}`)
  await loadWifiStatus(true)
}

async function loadBleInfo(silent = true) {
  bleInfoData.value = await call(() => commandsApi.bleInfo(props.deviceUuid), 'BLE 信息', { silent })
}

async function loadBonds(silent = true) {
  const r = await call(() => commandsApi.bleBonds(props.deviceUuid), 'BLE 配对列表', { silent })
  bleBonds.value = r?.bonds || (Array.isArray(r) ? r : [])
  loadHostMeta(true)
}

/* 主机平台标记（win/mac/linux）：存服务端 DeviceHostMeta，按地址关联，
 * 宏的平台变体与控制台快捷键适配都读这里 */
const hostMeta = ref([])

async function loadHostMeta(silent = true) {
  try {
    hostMeta.value = await commandsApi.listHostMeta(props.deviceUuid)
  } catch {
    if (!silent) ElMessage.warning('主机标记加载失败')
  }
}

function platformOf(addr) {
  const row = hostMeta.value.find((m) => m.addr === fmtAddr(addr))
  return row?.platform || ''
}

async function setPlatform(bondRow, platform) {
  try {
    await commandsApi.putHostMeta(props.deviceUuid, fmtAddr(bondRow.addr), {
      platform,
      addr_type: bondRow.addr_type ?? 0,
    })
    await loadHostMeta(true)
    ElMessage.success(platform ? `已标记为 ${platform}` : '已清除平台标记')
  } catch {
    ElMessage.error('平台标记保存失败')
  }
}

async function doUnpair(b) {
  await call(() => commandsApi.bleUnpair(props.deviceUuid, b.addr_type, b.addr), '取消配对')
  await loadBonds(false)
}

async function doSwitch(b) {
  await call(() => commandsApi.bleSwitch(props.deviceUuid, b.addr_type, b.addr), '切换主机')
  await loadBleInfo(true)
}

async function doDisconnect() {
  await call(() => commandsApi.bleDisconnect(props.deviceUuid), 'BLE 断开')
  await loadBleInfo(true)
}

async function doUnpairAll() {
  try {
    await ElMessageBox.confirm('确定清除当前设备上的所有 BLE 配对记录？', '危险操作', { type: 'warning' })
  } catch { return }
  await call(() => commandsApi.bleUnpairAll(props.deviceUuid), '清除全部配对')
  await loadBonds(false)
}

async function doScan() {
  if (!capFlags.value.supportsBle) {
    ElMessage.warning('当前设备未集成 BLE 蓝牙能力')
    return
  }
  const r = await call(() => commandsApi.bleScan(props.deviceUuid, 4000), 'BLE 扫描周边')
  if (!r) return
  await Promise.allSettled([
    !bleInfoData.value ? loadBleInfo(true) : Promise.resolve(),
    !bleBonds.value.length ? loadBonds(true) : Promise.resolve(),
  ])
  const list = r?.devices || (Array.isArray(r) ? r : [])
  const curPeer = String(bleInfoData.value?.peer || '').toUpperCase()
  const bondedAddrs = new Set((bleBonds.value || []).map(b => fmtAddr(b.addr).toUpperCase()))
  bleScanList.value = list.map(d => {
    const addr = String(d.addr || '').toUpperCase()
    let status = null
    if (addr && addr === curPeer) status = 'connected'
    else if (addr && bondedAddrs.has(addr)) status = 'bonded'
    return { ...d, _status: status }
  })
}

async function loadTransport(silent = true) {
  transportData.value = await call(() => commandsApi.transportGet(props.deviceUuid), '查询传输配置', { silent })
}

async function setTransport(mode) {
  await call(() => commandsApi.transportSet(props.deviceUuid, mode), `设置传输模式为 ${mode}`)
  await loadTransport(false)
}

async function loadSlots() {
  try { slotsList.value = await commandsApi.listSlots(props.deviceUuid) || [] }
  catch { slotsList.value = [] }
}

async function assignSlot(bond) {
  try {
    const { value: idx } = await ElMessageBox.prompt(
      `将 ${fmtAddr(bond.addr)} 绑定至哪个槽位？(请输入 1-9)`,
      '分配主机槽位', { inputPattern: /^[1-9]$/, inputErrorMessage: '只能填 1-9' }
    )
    await commandsApi.putSlot(props.deviceUuid, parseInt(idx), {
      addr_type: bond.addr_type ?? 0,
      addr: fmtAddr(bond.addr),
      name: null,
    })
    ElMessage.success(`已分配至 Slot #${idx}`)
    await loadSlots()
  } catch { /* canceled */ }
}

async function clearSlot(s) {
  try {
    await ElMessageBox.confirm(`确定清空 Slot #${s.slot_index}？`, '确认清空', { type: 'warning' })
    await commandsApi.deleteSlot(props.deviceUuid, s.slot_index)
    ElMessage.success(`已清空 Slot #${s.slot_index}`)
    await loadSlots()
  } catch { /* canceled */ }
}

async function doReboot() {
  try {
    await ElMessageBox.confirm(`确定向 ${props.deviceName || props.deviceUuid} 发送硬件重启指令？`, '重启设备', { type: 'warning' })
  } catch { return }
  await call(() => commandsApi.reboot(props.deviceUuid), '重启设备')
}

function fmtAddr(addr) {
  if (!Array.isArray(addr)) return String(addr ?? '')
  return addr.map(b => b.toString(16).padStart(2, '0')).join(':').toUpperCase()
}

function selectWifiRow(row) {
  wifiForm.value.ssid = row.ssid
  ElMessage.info(`已选定 SSID: ${row.ssid}，请输入密码后点击连接`)
}

watch(() => props.modelValue, async (v) => {
  if (v && props.deviceUuid) {
    tab.value = 'status'
    localCaps.value = Array.isArray(props.capabilities) ? [...props.capabilities] : []
    statusData.value = null
    wifiStatusData.value = null
    wifiScanList.value = []
    bleInfoData.value = null
    bleBonds.value = []
    bleScanList.value = []
    transportData.value = null
    displayStatusData.value = null
    displayErrorMsg.value = ''
    gpioPinsData.value = []
    gpioBoardName.value = ''
    gpioErrorMsg.value = ''
    gpioPulseSec.value = 0
    gpioPulseHint.value = ''
    timerSeed.value = null
    slotsList.value = []
    wifiKnownList.value = []
    lastResult.value = null
    if (!localCaps.value.length) {
      try {
        const list = await devicesApi.list()
        const cur = (list || []).find((d) => d.device_uuid === props.deviceUuid)
        if (cur && Array.isArray(cur.capabilities)) {
          localCaps.value = cur.capabilities
        }
      } catch { /* ignore */ }
    }
    loadStatus()
    if (capFlags.value.supportsBle) {
      loadSlots()
    }
  }
})

watch(tab, (t) => {
  if (t === 'display' && props.modelValue && props.deviceUuid && capFlags.value.supportsDisplay && !displayStatusData.value) {
    loadDisplayStatus(true)
  } else if (t === 'gpio' && props.modelValue && props.deviceUuid && capFlags.value.supportsGpio && !gpioPinsData.value.length) {
    loadGpioPins(true)
  } else if (t === 'timers' && props.modelValue && props.deviceUuid) {
    loadMacrosOnce()
    if (capFlags.value.supportsGpio && !gpioPinsData.value.length) {
      loadGpioPins(true)
    }
  }
})
</script>

<template>
  <el-drawer
    :model-value="modelValue"
    @update:model-value="emit('update:modelValue', $event)"
    size="760px"
    direction="rtl"
    class="device-control-drawer"
  >
    <template #header>
      <div class="drawer-header-content">
        <div class="drawer-title-group">
          <div class="drawer-icon-badge">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <circle cx="12" cy="12" r="3"/>
              <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"/>
            </svg>
          </div>
          <div>
            <div class="drawer-title">{{ deviceName || '未命名硬件' }}</div>
            <div class="drawer-uuid">{{ deviceUuid }}</div>
          </div>
        </div>

        <div class="drawer-header-actions">
          <button class="reboot-pill-btn" @click="doReboot" title="软重启硬件设备">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M18.36 6.64a9 9 0 1 1-12.73 0M12 2v10"/>
            </svg>
            重启硬件
          </button>
          <button class="drawer-close-btn" @click="close" title="关闭抽屉">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <line x1="18" y1="6" x2="6" y2="18"/>
              <line x1="6" y1="6" x2="18" y2="18"/>
            </svg>
          </button>
        </div>
      </div>
    </template>

    <div class="drawer-body-container" v-loading="loading">
      <el-tabs v-model="tab" class="modern-device-tabs">
        <!-- 1. Vitals / Status Tab -->
        <el-tab-pane label="硬件状态" name="status">
          <div class="tab-pane-content">
            <div class="action-strip">
              <el-button size="small" @click="loadStatus" class="control-action-btn">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="btn-svg"><path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/></svg>
                刷新状态
              </el-button>
              <el-switch v-model="showRawStatus" active-text="显示原始响应" inactive-text="" />
            </div>

            <!-- Hardware Stat Badges Grid -->
            <div class="vitals-grid">
              <div class="vital-card">
                <div class="vital-label">IP 地址</div>
                <div class="vital-value">{{ statusData?.ip || statusData?.wifi?.ip || '192.168.x.x' }}</div>
              </div>
              <div class="vital-card">
                <div class="vital-label">WiFi SSID</div>
                <div class="vital-value text-primary">{{ statusData?.ssid || statusData?.wifi?.ssid || '已连接' }}</div>
              </div>
              <div v-if="capFlags.supportsBle" class="vital-card">
                <div class="vital-label">BLE 状态</div>
                <div class="vital-value text-purple">{{ statusData?.ble?.connected ? '已连主机' : '待机' }}</div>
              </div>
              <div v-else class="vital-card">
                <div class="vital-label">固件能力</div>
                <div class="vital-value text-purple">{{ capFlags.list.join(', ') || '标准节点' }}</div>
              </div>
              <div class="vital-card">
                <div class="vital-label">可用内存 (Heap)</div>
                <div class="vital-value text-cyan">{{ (statusData?.free_heap ?? statusData?.heap_free) ? `${Math.round((statusData.free_heap ?? statusData.heap_free) / 1024)} KB` : '正常' }}</div>
              </div>
            </div>

            <div class="section-box" v-if="showRawStatus">
              <div class="box-title">原始状态数据</div>
              <pre class="terminal-raw-pre">{{ JSON.stringify(statusData, null, 2) }}</pre>
            </div>
          </div>
        </el-tab-pane>

        <!-- 2. WiFi Tab -->
        <el-tab-pane label="WiFi 管理" name="wifi">
          <div class="tab-pane-content">
            <div class="action-strip">
              <el-button size="small" @click="loadWifiStatus" class="control-action-btn">查询当前状态</el-button>
              <el-button size="small" type="primary" @click="doWifiScan" class="control-action-btn">扫描周边热点</el-button>
            </div>

            <!-- Current WiFi Card -->
            <div class="section-box highlight-box">
              <div class="box-title">当前连接热点</div>
              <div v-if="wifiStatusData" class="wifi-cur-info">
                <div class="wifi-cur-item">
                  <span class="muted">SSID:</span>
                  <span class="bold text-primary">{{ wifiStatusData.ssid || '未连接' }}</span>
                </div>
                <div class="wifi-cur-item" v-if="wifiStatusData.rssi !== undefined">
                  <span class="muted">信号强度:</span>
                  <span class="bold">{{ wifiStatusData.rssi }} dBm</span>
                </div>
                <div class="wifi-cur-item" v-if="wifiStatusData.ip">
                  <span class="muted">分配 IP:</span>
                  <code>{{ wifiStatusData.ip }}</code>
                </div>
              </div>
              <div v-else class="text-muted" style="font-size: 12px">点击“查询当前状态”获取实时 WiFi 信息</div>
            </div>

            <!-- Connect Form -->
            <div class="section-box">
              <div class="box-title">连接新热点</div>
              <el-form label-position="top" inline class="connect-wifi-form">
                <el-form-item label="WiFi SSID">
                  <el-input v-model="wifiForm.ssid" placeholder="输入或从下方扫描选择" style="width: 220px" />
                </el-form-item>
                <el-form-item label="密码">
                  <el-input v-model="wifiForm.password" type="password" show-password placeholder="热点密码" style="width: 220px" />
                </el-form-item>
                <el-form-item label="&nbsp;">
                  <el-button type="primary" @click="doWifiConnect">立即连接</el-button>
                </el-form-item>
              </el-form>
            </div>

            <!-- Saved Networks -->
            <div class="section-box">
              <div class="box-header-row">
                <div class="box-title">已保存的热点凭证</div>
                <el-button size="small" link @click="loadKnownWifi">刷新凭证</el-button>
              </div>
              <el-empty v-if="!wifiKnownList.length" description="暂无落库的热点记录" :image-size="50" />
              <el-table v-else :data="wifiKnownList" size="small" class="custom-tab-table">
                <el-table-column prop="ssid" label="SSID" />
                <el-table-column label="有无密码" width="90">
                  <template #default="{ row }">
                    <span class="badge-tag" :class="row.has_password ? 'badge-green' : 'badge-gray'">
                      {{ row.has_password ? '已保存' : '无密码' }}
                    </span>
                  </template>
                </el-table-column>
                <el-table-column prop="last_seen_at" label="上次同步" width="160" />
                <el-table-column label="操作" width="160">
                  <template #default="{ row }">
                    <el-button size="small" link type="primary" @click="applyKnownWifi(row)">连接此网络</el-button>
                    <el-button size="small" link type="danger" @click="deleteKnownWifi(row)">删除</el-button>
                  </template>
                </el-table-column>
              </el-table>
            </div>

            <!-- Scan Results -->
            <div class="section-box" v-if="wifiScanList.length">
              <div class="box-title">周边扫描到 {{ wifiScanList.length }} 个热点</div>
              <el-table :data="wifiScanList" size="small" class="custom-tab-table" max-height="300">
                <el-table-column prop="ssid" label="SSID" />
                <el-table-column prop="rssi" label="信号强度 (RSSI)" width="120">
                  <template #default="{ row }">
                    <span :class="row.rssi > -65 ? 'text-success' : row.rssi > -80 ? 'text-primary' : 'text-danger'">
                      {{ row.rssi }} dBm
                    </span>
                  </template>
                </el-table-column>
                <el-table-column prop="auth" label="加密模式" width="100" />
                <el-table-column label="状态" width="90">
                  <template #default="{ row }">
                    <span v-if="row._status === 'using'" class="badge-tag badge-green">当前使用</span>
                    <span v-else-if="row._status === 'saved'" class="badge-tag badge-blue">已存凭证</span>
                  </template>
                </el-table-column>
                <el-table-column label="操作" width="80">
                  <template #default="{ row }">
                    <el-button size="small" link type="primary" @click="selectWifiRow(row)">选择</el-button>
                  </template>
                </el-table-column>
              </el-table>
            </div>
          </div>
        </el-tab-pane>

        <!-- 3. Configurable Screen / Display Tab -->
        <el-tab-pane v-if="capFlags.supportsDisplay" label="屏幕配置" name="display">
          <div class="tab-pane-content">
            <DisplayConfigPanel
              :status="displayStatusData"
              :device-name="deviceName || deviceUuid"
              :loading="loading"
              :pc-monitor-default="capFlags.list.includes('pc_monitor')"
              :error-message="displayErrorMsg"
              @refresh="loadDisplayStatus(false)"
              @save="saveDisplayConfig"
            />
          </div>
        </el-tab-pane>

        <!-- 3.5 GPIO & Pulse Control Tab -->
        <el-tab-pane v-if="capFlags.supportsGpio" label="GPIO 外设" name="gpio">
          <div class="tab-pane-content">
            <div class="action-strip gpio-toolbar">
              <el-button size="small" type="primary" @click="loadGpioPins(false)" :loading="loading" class="control-action-btn">
                读取引脚状态
              </el-button>
              <div class="gpio-pulse-picker">
                <span class="gpio-pulse-label">脉冲自动关闭:</span>
                <el-select v-model="gpioPulseSec" size="small" style="width: 126px">
                  <el-option :value="0" label="保持常驻 (0s)" />
                  <el-option :value="2" label="2 秒脉冲" />
                  <el-option :value="5" label="5 秒脉冲" />
                  <el-option :value="10" label="10 秒脉冲" />
                  <el-option :value="30" label="30 秒脉冲" />
                  <el-option :value="60" label="60 秒脉冲" />
                </el-select>
              </div>
              <span v-if="gpioBoardName" class="badge-tag badge-blue">{{ gpioBoardName }}</span>
            </div>

            <el-alert v-if="gpioErrorMsg" :title="gpioErrorMsg" type="warning" show-icon :closable="false" />
            <el-alert v-if="gpioPulseHint" :title="gpioPulseHint" type="success" show-icon :closable="false" />

            <div v-if="gpioPinsData.length" class="gpio-grid">
              <div v-for="pin in gpioPinsData" :key="pin.pin" class="gpio-card" :class="{ active: pin.val || pin.duty > 0 }">
                <div class="gpio-card-top">
                  <div>
                    <div class="gpio-pin-title">GPIO {{ pin.pin }} <span class="gpio-pin-alias">{{ pin.label || '引脚' }}</span></div>
                    <div class="gpio-pin-tags">
                      <span class="badge-tag badge-blue">{{ pin.pwm ? 'PWM' : '数字输出' }}</span>
                      <span v-if="pin.active_low" class="badge-tag badge-green">低有效</span>
                    </div>
                  </div>
                  <div class="gpio-state-badge" :class="(pin.val || pin.duty > 0) ? 'on' : 'off'">
                    {{ pin.duty > 0 ? `PWM ${pin.duty}%` : pin.val ? 'ON (HIGH)' : 'OFF (LOW)' }}
                  </div>
                </div>

                <div class="gpio-btn-row">
                  <el-button size="small" type="success" :disabled="loading || !pin.output" @click="applyGpioPin(pin, { op: 'on' })">
                    {{ gpioPulseSec > 0 ? `开启(${gpioPulseSec}s)` : '开启' }}
                  </el-button>
                  <el-button size="small" type="info" :disabled="loading || !pin.output" @click="applyGpioPin(pin, { op: 'off' })">
                    关闭
                  </el-button>
                  <el-button size="small" :disabled="loading || !pin.output" @click="applyGpioPin(pin, { op: 'toggle' })">
                    翻转
                  </el-button>
                  <el-button size="small" link type="primary" @click="scheduleTimerForPin(pin, 'on')">
                    设定时
                  </el-button>
                </div>

                <div v-if="pin.pwm" class="gpio-pwm-row">
                  <span class="gpio-pwm-label">PWM:</span>
                  <el-slider v-model="pin._draftDuty" :min="0" :max="100" size="small" style="flex: 1" />
                  <el-button size="small" type="primary" plain :disabled="loading" @click="applyGpioPin(pin, { op: 'pwm', duty: pin._draftDuty })">
                    {{ pin._draftDuty }}%
                  </el-button>
                </div>
              </div>
            </div>

            <el-empty v-else description="点击“读取引脚状态”获取当前芯片 GPIO 列表" :image-size="56" />
          </div>
        </el-tab-pane>

        <!-- 4. BLE Tab (仅在支持 BLE 或尚未上报能力时显示) -->
        <el-tab-pane v-if="capFlags.supportsBle" label="BLE 蓝牙" name="ble">
          <div class="tab-pane-content">
            <div class="action-strip">
              <el-button size="small" @click="loadBleInfo" class="control-action-btn">BLE 信息</el-button>
              <el-button size="small" @click="loadBonds" class="control-action-btn">配对列表</el-button>
              <el-button size="small" @click="doScan" class="control-action-btn">扫描周边</el-button>
              <el-button size="small" @click="doDisconnect" class="control-action-btn">断开当前</el-button>
              <el-button size="small" type="danger" @click="doUnpairAll" class="control-action-btn">清除全部配对</el-button>
            </div>

            <!-- Bonded Hosts List -->
            <div class="section-box">
              <div class="box-title">已配对主机列表</div>
              <el-empty v-if="!bleBonds.length" description="暂无配对记录" :image-size="50" />
              <el-table v-else :data="bleBonds" size="small" class="custom-tab-table">
                <el-table-column label="MAC 地址">
                  <template #default="{ row }">
                    <code class="mac-code">{{ fmtAddr(row.addr) }}</code>
                    <el-tag v-if="row.connected" size="small" type="success" style="margin-left: 6px">在线</el-tag>
                  </template>
                </el-table-column>
                <el-table-column label="平台" width="130">
                  <template #default="{ row }">
                    <el-select
                      :model-value="platformOf(row.addr)"
                      size="small"
                      style="width: 110px"
                      @change="(val) => setPlatform(row, val)"
                    >
                      <el-option label="未标记" value="" />
                      <el-option label="🪟 Windows" value="win" />
                      <el-option label=" macOS" value="mac" />
                      <el-option label="🐧 Linux" value="linux" />
                    </el-select>
                  </template>
                </el-table-column>
                <el-table-column prop="addr_type" label="地址类型" width="90" />
                <el-table-column label="管理操作" width="240">
                  <template #default="{ row }">
                    <el-button size="small" type="primary" link @click="doSwitch(row)">切换为主机</el-button>
                    <el-button size="small" link @click="assignSlot(row)">绑定 Slot</el-button>
                    <el-button size="small" type="danger" link @click="doUnpair(row)">解除配对</el-button>
                  </template>
                </el-table-column>
              </el-table>
            </div>

            <!-- Slots Grid/Table -->
            <div class="section-box">
              <div class="box-header-row">
                <div>
                  <div class="box-title">主机槽位配置 (Slots 1-9)</div>
                  <div class="box-desc">绑定槽位后可在宏指令或控制台中通过编号快速定向控制目标电脑</div>
                </div>
                <el-button size="small" link @click="loadSlots">刷新槽位</el-button>
              </div>

              <el-empty v-if="!slotsList.length" description="尚未分配任何 Slot 槽位" :image-size="50" />
              <el-table v-else :data="slotsList" size="small" class="custom-tab-table">
                <el-table-column prop="slot_index" label="槽位" width="80">
                  <template #default="{ row }">
                    <span class="slot-badge">Slot #{{ row.slot_index }}</span>
                  </template>
                </el-table-column>
                <el-table-column prop="addr" label="主机 MAC">
                  <template #default="{ row }">
                    <code class="mac-code">{{ row.addr }}</code>
                  </template>
                </el-table-column>
                <el-table-column prop="name" label="主机别名" width="120" />
                <el-table-column prop="last_used_at" label="最近触发" width="160" />
                <el-table-column label="操作" width="80">
                  <template #default="{ row }">
                    <el-button size="small" type="danger" link @click="clearSlot(row)">清空</el-button>
                  </template>
                </el-table-column>
              </el-table>
            </div>

            <!-- BLE Scan List -->
            <div class="section-box" v-if="bleScanList.length">
              <div class="box-title">周边可见蓝牙设备 ({{ bleScanList.length }})</div>
              <el-table :data="bleScanList" size="small" class="custom-tab-table" max-height="260">
                <el-table-column label="广播名称">
                  <template #default="{ row }">{{ row.name || '(未命名广播)' }}</template>
                </el-table-column>
                <el-table-column label="地址">
                  <template #default="{ row }">
                    <code class="mac-code">{{ row.addr || fmtAddr(row.addr) }}</code>
                  </template>
                </el-table-column>
                <el-table-column prop="rssi" label="RSSI" width="90" />
                <el-table-column label="状态" width="100">
                  <template #default="{ row }">
                    <span v-if="row._status === 'connected'" class="badge-tag badge-green">当前主机</span>
                    <span v-else-if="row._status === 'bonded'" class="badge-tag badge-blue">已配对</span>
                  </template>
                </el-table-column>
              </el-table>
            </div>
          </div>
        </el-tab-pane>

        <!-- 4. Transport Mode Tab (仅在启用 USB/BLE HID 或未上报能力时显示) -->
        <el-tab-pane v-if="capFlags.supportsTransport" label="HID 传输" name="transport">
          <div class="tab-pane-content">
            <div class="action-strip">
              <el-button size="small" @click="loadTransport" class="control-action-btn">刷新传输状态</el-button>
            </div>

            <div class="section-box" v-if="transportData">
              <div class="box-title">外设输出模式切换</div>
              <div class="transport-cards-row">
                <div
                  class="transport-mode-card"
                  :class="{ active: transportData.mode === 'auto' }"
                  @click="setTransport('auto')"
                >
                  <div class="tm-title">Auto 智能自适应</div>
                  <div class="tm-desc">优先探测 USB 有线插拔，若未挂载则无缝回退至 BLE 蓝牙广播</div>
                </div>
                <div
                  class="transport-mode-card"
                  :class="{ active: transportData.mode === 'usb' }"
                  @click="setTransport('usb')"
                >
                  <div class="tm-title">USB 独占模式</div>
                  <div class="tm-desc">锁定物理 USB HID 管道输出，隔离蓝牙无线信号</div>
                </div>
                <div
                  class="transport-mode-card"
                  :class="{ active: transportData.mode === 'ble' }"
                  @click="setTransport('ble')"
                >
                  <div class="tm-title">BLE 蓝牙无线模式</div>
                  <div class="tm-desc">锁定蓝牙 HID 管道，支持多主机动态槽位快速穿透切换</div>
                </div>
              </div>

              <!-- Hardware Interface Link Vitals -->
              <div class="link-vitals-box">
                <div class="vital-card">
                  <div class="vital-label">生效中的输出通道</div>
                  <div class="vital-value text-primary">{{ transportData.active || '—' }}</div>
                </div>
                <div class="vital-card">
                  <div class="vital-label">USB 挂载状态</div>
                  <div class="vital-value" :class="transportData.usb_mounted ? 'text-success' : 'text-muted'">
                    {{ transportData.usb_mounted ? 'Mounted (已就绪)' : 'Unplugged' }}
                  </div>
                </div>
                <div class="vital-card">
                  <div class="vital-label">BLE 连接通道</div>
                  <div class="vital-value" :class="transportData.ble_connected ? 'text-purple' : 'text-muted'">
                    {{ transportData.ble_connected ? 'Connected' : 'Idle' }}
                  </div>
                </div>
              </div>
            </div>

            <el-empty v-else description="点击上方刷新加载当前硬件传输状态" :image-size="60" />
          </div>
        </el-tab-pane>

        <!-- 4.5 Timers & Automation Tab -->
        <el-tab-pane label="定时任务" name="timers">
          <div class="tab-pane-content">
            <TimerConfigPanel
              :device-uuid="deviceUuid"
              :device-caps="capFlags.list"
              :gpio-pins="gpioPinsData"
              :macros="macrosList"
              :seed-form="timerSeed"
              compact
            />
          </div>
        </el-tab-pane>

        <!-- 5. Raw Terminal Tab -->
        <el-tab-pane label="响应终端" name="raw">
          <div class="tab-pane-content">
            <div class="section-box terminal-box">
              <div class="box-title">最后一次下发指令原始响应</div>
              <pre class="terminal-raw-pre">{{ JSON.stringify(lastResult, null, 2) }}</pre>
            </div>
          </div>
        </el-tab-pane>
      </el-tabs>
    </div>
  </el-drawer>
</template>

<style scoped>
.device-control-drawer :deep(.el-drawer__header) {
  margin-bottom: 0;
  padding: 18px 24px;
  background: var(--color-bg-card);
  border-bottom: 1px solid var(--color-border);
}

.device-control-drawer :deep(.el-drawer__body) {
  padding: 20px 24px;
  background: var(--color-bg-base);
  color: var(--color-text-primary);
}

.drawer-header-content {
  display: flex;
  justify-content: space-between;
  align-items: center;
  width: 100%;
}

.drawer-title-group {
  display: flex;
  align-items: center;
  gap: 12px;
}

.drawer-icon-badge {
  width: 36px;
  height: 36px;
  border-radius: var(--radius-sm);
  background: #eff6ff;
  border: 1px solid #bfdbfe;
  display: flex;
  align-items: center;
  justify-content: center;
}

.drawer-icon-badge svg {
  width: 20px;
  height: 20px;
  color: var(--color-primary);
}

.drawer-title {
  font-size: 16px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.drawer-uuid {
  font-size: 11px;
  font-family: monospace;
  color: var(--color-text-muted);
}

.reboot-pill-btn {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 14px;
  border-radius: var(--radius-pill);
  background: #fee2e2;
  border: 1px solid #fecaca;
  color: #dc2626;
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s;
}

.reboot-pill-btn svg {
  width: 14px;
  height: 14px;
}

.drawer-header-actions {
  display: flex;
  align-items: center;
  gap: 10px;
}

.drawer-close-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 32px;
  height: 32px;
  border-radius: 8px;
  border: 1px solid var(--color-border);
  background: var(--color-bg-base);
  color: var(--color-text-muted);
  cursor: pointer;
  transition: all 0.2s;
}

.drawer-close-btn svg {
  width: 16px;
  height: 16px;
}

.drawer-close-btn:hover {
  background: #fee2e2;
  color: #dc2626;
  border-color: #fca5a5;
}

.reboot-pill-btn:hover {
  background: #fca5a5;
  transform: translateY(-1px);
}

/* Tabs */
.modern-device-tabs :deep(.el-tabs__header) {
  margin-bottom: 20px;
}

.modern-device-tabs :deep(.el-tabs__item) {
  font-size: 14px;
  font-weight: 600;
  color: var(--color-text-secondary);
}

.modern-device-tabs :deep(.el-tabs__item.is-active) {
  color: var(--color-primary);
}

.tab-pane-content {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.action-strip {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.control-action-btn {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-sm);
  color: var(--color-text-secondary);
}

.btn-svg {
  width: 12px;
  height: 12px;
}

/* Vitals Badges */
.vitals-grid {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: 12px;
}

.vital-card {
  padding: 14px 16px;
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-md);
}

.vital-label {
  font-size: 11px;
  color: var(--color-text-muted);
  text-transform: uppercase;
  margin-bottom: 4px;
}

.vital-value {
  font-size: 16px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.text-primary { color: #2563eb; }
.text-purple { color: #7c3aed; }
.text-cyan { color: #0891b2; }
.text-success { color: #16a34a; }
.text-danger { color: #dc2626; }
.text-muted { color: #64748b; }
.bold { font-weight: 600; }

/* Section Boxes */
.section-box {
  padding: 16px;
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-md);
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.highlight-box {
  background: #f8fafc;
  border-color: #cbd5e1;
}

.box-header-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.box-title {
  font-size: 13px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.box-desc {
  font-size: 11px;
  color: var(--color-text-muted);
  margin-top: 2px;
}

.wifi-cur-info {
  display: flex;
  gap: 24px;
  font-size: 13px;
  flex-wrap: wrap;
}

.wifi-cur-item {
  display: flex;
  align-items: center;
  gap: 6px;
}

.connect-wifi-form {
  margin-bottom: 0;
}

.custom-tab-table {
  border-radius: var(--radius-sm);
  background: transparent !important;
}

.badge-tag {
  font-size: 11px;
  padding: 2px 8px;
  border-radius: var(--radius-pill);
  font-weight: 600;
}

.badge-green {
  background: #dcfce7;
  color: #15803d;
  border: 1px solid #bbf7d0;
}

.badge-blue {
  background: #e0f2fe;
  color: #0369a1;
  border: 1px solid #bae6fd;
}

.badge-gray {
  background: #f1f5f9;
  color: #64748b;
  border: 1px solid #e2e8f0;
}

.mac-code {
  font-family: monospace;
  font-size: 12px;
  color: #0369a1;
  background: #f0f9ff;
  padding: 2px 6px;
  border-radius: 4px;
  border: 1px solid #bae6fd;
}

.slot-badge {
  font-size: 11px;
  font-weight: 700;
  color: #7e22ce;
  background: #f3e8ff;
  border: 1px solid #e9d5ff;
  padding: 2px 8px;
  border-radius: 4px;
}

/* Transport Cards */
.transport-cards-row {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 10px;
}

@media (max-width: 680px) {
  .transport-cards-row {
    grid-template-columns: 1fr;
  }
}

.transport-mode-card {
  padding: 14px;
  border-radius: var(--radius-md);
  border: 1px solid var(--color-border);
  background: var(--color-bg-base);
  cursor: pointer;
  transition: all 0.2s cubic-bezier(0.16, 1, 0.3, 1);
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.transport-mode-card:hover {
  border-color: var(--color-primary);
  transform: translateY(-2px);
}

.transport-mode-card.active {
  border-color: var(--color-primary);
  background: #eff6ff;
  box-shadow: 0 4px 14px rgba(37, 99, 235, 0.12);
}

.tm-title {
  font-size: 13px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.tm-desc {
  font-size: 11px;
  color: var(--color-text-secondary);
  line-height: 1.4;
}

.link-vitals-box {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 10px;
  margin-top: 10px;
}

/* Terminal Raw Pre */
.terminal-raw-pre {
  margin: 0;
  max-height: 480px;
  overflow-y: auto;
  background: #0f172a;
  color: #e2e8f0;
  border: 1px solid #1e293b;
  padding: 12px;
  border-radius: var(--radius-sm);
  font-family: monospace;
  font-size: 12px;
}

.gpio-toolbar {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 10px;
}

.gpio-pulse-picker {
  display: flex;
  align-items: center;
  gap: 6px;
}

.gpio-pulse-label {
  font-size: 12px;
  color: var(--color-text-secondary);
}

.gpio-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
  gap: 12px;
}

.gpio-card {
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-md);
  padding: 12px 14px;
  display: flex;
  flex-direction: column;
  gap: 10px;
  transition: border-color 0.2s, box-shadow 0.2s;
}

.gpio-card.active {
  border-color: #86efac;
  background: linear-gradient(180deg, #f0fdf4 0%, #ffffff 100%);
}

.gpio-card-top {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  gap: 8px;
}

.gpio-pin-title {
  font-weight: 700;
  font-size: 14px;
  color: var(--color-text-primary);
}

.gpio-pin-alias {
  font-weight: 500;
  font-size: 12px;
  color: var(--color-text-muted);
  margin-left: 4px;
}

.gpio-pin-tags {
  display: flex;
  gap: 6px;
  margin-top: 4px;
}

.gpio-state-badge {
  font-family: var(--font-mono);
  font-size: 11px;
  font-weight: 700;
  padding: 3px 8px;
  border-radius: 999px;
}

.gpio-state-badge.on {
  background: #dcfce7;
  color: #15803d;
}

.gpio-state-badge.off {
  background: #f1f5f9;
  color: #64748b;
}

.gpio-btn-row {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 6px;
}

.gpio-pwm-row {
  display: flex;
  align-items: center;
  gap: 10px;
  padding-top: 4px;
  border-top: 1px dashed var(--color-border);
}

.gpio-pwm-label {
  font-size: 12px;
  color: var(--color-text-secondary);
}
</style>
