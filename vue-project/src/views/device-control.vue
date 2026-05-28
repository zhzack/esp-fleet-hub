<script setup>
import { ref, watch } from 'vue'
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

const wifiForm = ref({ ssid: '', password: '' })

function close() { emit('update:modelValue', false) }

function extractResult(resp) {
  // dispatcher.deliver_command 返回 { status, results: [...] } 或 { status: 'acked', result: {...} }
  // 取出设备 ACK payload 中的 result 字段
  if (!resp) return null
  if (resp.result !== undefined) return resp.result
  const r = resp.results?.[0]
  if (r?.result !== undefined) return r.result
  return resp
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
  statusData.value = await call(() => commandsApi.status(props.deviceUuid), '查询状态')
}
async function loadWifiStatus() {
  wifiStatusData.value = await call(() => commandsApi.wifiStatus(props.deviceUuid), 'WiFi 状态')
}
async function doWifiScan() {
  const r = await call(() => commandsApi.wifiScan(props.deviceUuid), 'WiFi 扫描')
  wifiScanList.value = r?.aps || r?.list || (Array.isArray(r) ? r : [])
}
async function doWifiConnect() {
  if (!wifiForm.value.ssid) return ElMessage.warning('SSID 必填')
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
    await ElMessageBox.confirm('确定清除所有 BLE 配对？', '确认', { type: 'warning' })
  } catch { return }
  await call(() => commandsApi.bleUnpairAll(props.deviceUuid), '清除全部配对')
  await loadBonds()
}
async function doScan() {
  const r = await call(() => commandsApi.bleScan(props.deviceUuid, 4000), 'BLE 扫描')
  bleScanList.value = r?.devices || (Array.isArray(r) ? r : [])
}
async function loadTransport() {
  transportData.value = await call(() => commandsApi.transportGet(props.deviceUuid), '查询 transport')
}
async function setTransport(mode) {
  await call(() => commandsApi.transportSet(props.deviceUuid, mode), `transport=${mode}`)
  await loadTransport()
}
async function doReboot() {
  try {
    await ElMessageBox.confirm(`重启设备 ${props.deviceName || props.deviceUuid}？`, '确认', { type: 'warning' })
  } catch { return }
  await call(() => commandsApi.reboot(props.deviceUuid), '重启')
}

function fmtAddr(addr) {
  if (!Array.isArray(addr)) return String(addr ?? '')
  return addr.map(b => b.toString(16).padStart(2, '0')).join(':').toUpperCase()
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
    lastResult.value = null
    loadStatus()
  }
})
</script>

<template>
  <el-drawer :model-value="modelValue" @update:model-value="emit('update:modelValue', $event)"
             :title="`设备控制 - ${deviceName || deviceUuid}`" size="720px" direction="rtl">
    <el-tabs v-model="tab" v-loading="loading">
      <el-tab-pane label="状态" name="status">
        <el-button size="small" @click="loadStatus">刷新</el-button>
        <el-button size="small" type="danger" @click="doReboot" style="float:right">重启设备</el-button>
        <pre style="margin-top:12px; max-height:520px; overflow:auto; background:#f5f7fa; padding:12px; font-size:12px">{{ JSON.stringify(statusData, null, 2) }}</pre>
      </el-tab-pane>

      <el-tab-pane label="WiFi" name="wifi">
        <el-space>
          <el-button size="small" @click="loadWifiStatus">查询状态</el-button>
          <el-button size="small" @click="doWifiScan">扫描</el-button>
        </el-space>
        <el-card style="margin-top:12px" header="当前状态">
          <pre style="margin:0; font-size:12px">{{ JSON.stringify(wifiStatusData, null, 2) }}</pre>
        </el-card>
        <el-card style="margin-top:12px" header="连接 WiFi">
          <el-form label-width="80px" inline>
            <el-form-item label="SSID">
              <el-input v-model="wifiForm.ssid" style="width:200px" />
            </el-form-item>
            <el-form-item label="密码">
              <el-input v-model="wifiForm.password" type="password" show-password style="width:200px" />
            </el-form-item>
            <el-button type="primary" @click="doWifiConnect">连接</el-button>
          </el-form>
        </el-card>
        <el-card style="margin-top:12px" header="扫描结果" v-if="wifiScanList.length">
          <el-table :data="wifiScanList" size="small">
            <el-table-column prop="ssid" label="SSID" />
            <el-table-column prop="rssi" label="RSSI" width="80" />
            <el-table-column prop="auth" label="加密" width="100" />
            <el-table-column label="操作" width="80">
              <template #default="{ row }">
                <el-button size="small" @click="wifiForm.ssid = row.ssid">选择</el-button>
              </template>
            </el-table-column>
          </el-table>
        </el-card>
      </el-tab-pane>

      <el-tab-pane label="BLE" name="ble">
        <el-space>
          <el-button size="small" @click="loadBleInfo">BLE 信息</el-button>
          <el-button size="small" @click="loadBonds">配对列表</el-button>
          <el-button size="small" @click="doScan">扫描周边</el-button>
          <el-button size="small" @click="doDisconnect">断开当前</el-button>
          <el-button size="small" type="danger" @click="doUnpairAll">清除全部配对</el-button>
        </el-space>
        <el-card style="margin-top:12px" header="BLE 信息" v-if="bleInfoData">
          <pre style="margin:0; font-size:12px">{{ JSON.stringify(bleInfoData, null, 2) }}</pre>
        </el-card>
        <el-card style="margin-top:12px" header="已配对设备" v-if="bleBonds.length">
          <el-table :data="bleBonds" size="small">
            <el-table-column label="地址">
              <template #default="{ row }">{{ fmtAddr(row.addr) }}</template>
            </el-table-column>
            <el-table-column prop="addr_type" label="类型" width="80" />
            <el-table-column label="操作" width="180">
              <template #default="{ row }">
                <el-button size="small" @click="doSwitch(row)">切换</el-button>
                <el-button size="small" type="danger" @click="doUnpair(row)">取消配对</el-button>
              </template>
            </el-table-column>
          </el-table>
        </el-card>
        <el-card style="margin-top:12px" header="周边可见设备" v-if="bleScanList.length">
          <el-table :data="bleScanList" size="small">
            <el-table-column label="名称">
              <template #default="{ row }">{{ row.name || '(无名)' }}</template>
            </el-table-column>
            <el-table-column label="地址">
              <template #default="{ row }">{{ row.addr || fmtAddr(row.addr) }}</template>
            </el-table-column>
            <el-table-column prop="rssi" label="RSSI" width="80" />
            <el-table-column prop="addr_type" label="类型" width="80" />
          </el-table>
        </el-card>
      </el-tab-pane>

      <el-tab-pane label="传输 / USB" name="transport">
        <el-button size="small" @click="loadTransport">刷新</el-button>
        <el-card style="margin-top:12px" header="HID 传输模式" v-if="transportData">
          <div style="font-size:13px; line-height:1.8">
            <div>当前模式：<el-tag size="small">{{ transportData.mode || '—' }}</el-tag></div>
            <div>实际生效：<el-tag size="small" :type="transportData.active === 'usb' ? 'success' : 'warning'">{{ transportData.active || '—' }}</el-tag></div>
            <div>BLE：<el-tag size="small" :type="transportData.ble_connected ? 'success' : 'info'">{{ transportData.ble_connected ? 'connected' : 'idle' }}</el-tag></div>
            <div>USB：<el-tag size="small" :type="transportData.usb_mounted ? 'success' : 'info'">{{ transportData.usb_mounted ? 'mounted' : 'unplugged' }}</el-tag></div>
          </div>
          <div style="margin-top:12px">
            <el-button-group>
              <el-button size="small" :type="transportData.mode === 'auto' ? 'primary' : ''" @click="setTransport('auto')">auto</el-button>
              <el-button size="small" :type="transportData.mode === 'ble'  ? 'primary' : ''" @click="setTransport('ble')">ble</el-button>
              <el-button size="small" :type="transportData.mode === 'usb'  ? 'primary' : ''" @click="setTransport('usb')">usb</el-button>
            </el-button-group>
          </div>
        </el-card>
        <el-empty v-else description="点击刷新加载" :image-size="80" />
      </el-tab-pane>

      <el-tab-pane label="原始响应" name="raw">
        <pre style="max-height:560px; overflow:auto; background:#f5f7fa; padding:12px; font-size:12px">{{ JSON.stringify(lastResult, null, 2) }}</pre>
      </el-tab-pane>
    </el-tabs>
  </el-drawer>
</template>
