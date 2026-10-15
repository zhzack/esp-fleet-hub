<script setup>
import { computed, ref, watch } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { commandsApi } from '@/api/commands'

const props = defineProps({
  modelValue: { type: Boolean, default: false },
  deviceUuid: { type: String, default: '' },
  deviceName: { type: String, default: '' },
})
const emit = defineEmits(['update:modelValue'])

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

async function call(fn, label) {
  loading.value = true
  try {
    const resp = await fn()
    lastResult.value = resp
    const r = extractResult(resp)
    if (resp?.status && resp.status !== 'acked' && resp.status !== 'ok') {
      ElMessage.warning(`${label}: ${resp.status} ${resp.error_message || ''}`)
    } else {
      ElMessage.success(`${label} 成功`)
    }
    return r
  } catch (e) {
    return null
  } finally {
    loading.value = false
  }
}

async function loadStatus() {
  statusData.value = await call(() => commandsApi.status(props.deviceUuid), '查询硬件状态')
}

async function loadWifiStatus() {
  wifiStatusData.value = await call(() => commandsApi.wifiStatus(props.deviceUuid), 'WiFi 状态')
}

async function doWifiScan() {
  if (!wifiStatusData.value) await loadWifiStatus()
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
  await loadWifiStatus()
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
  await loadWifiStatus()
}

async function loadBleInfo() {
  bleInfoData.value = await call(() => commandsApi.bleInfo(props.deviceUuid), 'BLE 信息')
}

async function loadBonds() {
  const r = await call(() => commandsApi.bleBonds(props.deviceUuid), 'BLE 配对列表')
  bleBonds.value = r?.bonds || (Array.isArray(r) ? r : [])
}

async function doUnpair(b) {
  await call(() => commandsApi.bleUnpair(props.deviceUuid, b.addr_type, b.addr), '取消配对')
  await loadBonds()
}

async function doSwitch(b) {
  await call(() => commandsApi.bleSwitch(props.deviceUuid, b.addr_type, b.addr), '切换主机')
  await loadBleInfo()
}

async function doDisconnect() {
  await call(() => commandsApi.bleDisconnect(props.deviceUuid), 'BLE 断开')
  await loadBleInfo()
}

async function doUnpairAll() {
  try {
    await ElMessageBox.confirm('确定清除当前设备上的所有 BLE 配对记录？', '危险操作', { type: 'warning' })
  } catch { return }
  await call(() => commandsApi.bleUnpairAll(props.deviceUuid), '清除全部配对')
  await loadBonds()
}

async function doScan() {
  if (!bleInfoData.value) await loadBleInfo()
  if (!bleBonds.value.length) await loadBonds()
  const r = await call(() => commandsApi.bleScan(props.deviceUuid, 4000), 'BLE 扫描周边')
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

async function loadTransport() {
  transportData.value = await call(() => commandsApi.transportGet(props.deviceUuid), '查询传输配置')
}

async function setTransport(mode) {
  await call(() => commandsApi.transportSet(props.deviceUuid, mode), `设置传输模式为 ${mode}`)
  await loadTransport()
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

watch(() => props.modelValue, (v) => {
  if (v && props.deviceUuid) {
    tab.value = 'status'
    statusData.value = null
    wifiStatusData.value = null
    wifiScanList.value = []
    bleInfoData.value = null
    bleBonds.value = []
    bleScanList.value = []
    transportData.value = null
    slotsList.value = []
    wifiKnownList.value = []
    lastResult.value = null
    loadStatus()
    loadSlots()
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

        <button class="reboot-pill-btn" @click="doReboot" title="软重启硬件设备">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M18.36 6.64a9 9 0 1 1-12.73 0M12 2v10"/>
          </svg>
          重启硬件
        </button>
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
              <div class="vital-card">
                <div class="vital-label">BLE 状态</div>
                <div class="vital-value text-purple">{{ statusData?.ble?.connected ? '已连主机' : '待机' }}</div>
              </div>
              <div class="vital-card">
                <div class="vital-label">可用内存 (Heap)</div>
                <div class="vital-value text-cyan">{{ statusData?.heap_free ? `${Math.round(statusData.heap_free / 1024)} KB` : '正常' }}</div>
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

        <!-- 3. BLE Tab -->
        <el-tab-pane label="BLE 蓝牙" name="ble">
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

        <!-- 4. Transport Mode Tab -->
        <el-tab-pane label="HID 传输" name="transport">
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
  background: var(--color-bg-sidebar);
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
  background: linear-gradient(135deg, rgba(59, 130, 246, 0.2), rgba(139, 92, 246, 0.2));
  border: 1px solid rgba(59, 130, 246, 0.3);
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
  background: rgba(239, 68, 68, 0.1);
  border: 1px solid rgba(239, 68, 68, 0.3);
  color: #f87171;
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s;
}

.reboot-pill-btn svg {
  width: 14px;
  height: 14px;
}

.reboot-pill-btn:hover {
  background: rgba(239, 68, 68, 0.2);
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

.text-primary { color: #38bdf8; }
.text-purple { color: #c084fc; }
.text-cyan { color: #22d3ee; }
.text-success { color: #4ade80; }
.text-danger { color: #f87171; }
.text-muted { color: #94a3b8; }
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
  background: linear-gradient(135deg, rgba(59, 130, 246, 0.06), rgba(139, 92, 246, 0.04));
  border-color: rgba(59, 130, 246, 0.2);
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
  background: rgba(34, 197, 94, 0.15);
  color: #4ade80;
}

.badge-blue {
  background: rgba(56, 189, 248, 0.15);
  color: #38bdf8;
}

.badge-gray {
  background: rgba(148, 163, 184, 0.15);
  color: #94a3b8;
}

.mac-code {
  font-family: monospace;
  font-size: 12px;
  color: #38bdf8;
}

.slot-badge {
  font-size: 11px;
  font-weight: 700;
  color: #c084fc;
  background: rgba(192, 132, 252, 0.15);
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
  background: linear-gradient(135deg, rgba(59, 130, 246, 0.15), rgba(139, 92, 246, 0.1));
  box-shadow: 0 4px 14px rgba(59, 130, 246, 0.2);
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
  background: rgba(15, 23, 42, 0.85);
  color: #94a3b8;
  padding: 12px;
  border-radius: var(--radius-sm);
  font-family: monospace;
  font-size: 12px;
}
</style>
