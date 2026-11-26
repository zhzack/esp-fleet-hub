<script>
export const meta = { title: '统一芯片 & 云编译中心' }
</script>

<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import {
  Cpu, Connection, Refresh, Setting, VideoCamera, Monitor, Upload, Promotion, Delete, Check,
} from '@element-plus/icons-vue'
import { unifiedApi } from '@/api/unified'
import DisplayConfigPanel from '@/components/DisplayConfigPanel.vue'
import { useAuthStore } from '@/stores/auth'
import { buildUnifiedConfigPatch, normalizeWsConfigForm } from '@/utils/wsConfig'

const auth = useAuthStore()
const isAdmin = computed(() => auth.user?.role === 'admin')

const activeTab = ref('devices')
const loading = ref(false)
const devices = ref([])
const auditLogs = ref([])
const profiles = ref([])
const ports = ref([])
const firmwares = ref([])

let devicesTimer = null
let taskTimer = null

// Compiler & Serial Flash Form
const buildForm = reactive({
  name: 'esp32s3_display_hid',
  target: 'esp32s3',
  description: '',
  hardware: { cpu_freq_mhz: 240, flash_size: '8MB', spiram: true },
  features: {
    http_server: true,
    web_ui: true,
    rest_api: true,
    ota: true,
    serial_cli: true,
    fleet_agent: true,
    gpio_ctrl: true,
    camera: false,
    storage: true,
    display: true,
    usb_otg: true,
    pc_monitor: false,
    joystick: false,
    ws_gateway: true,
  },
  defaults: {
    wifi_ssid: '1',
    wifi_pass: '11111111',
    mdns_host: 'esp-node',
    device_id: '',
    ws_gateway_url: '',
  },
})

const flashForm = reactive({
  port: 'COM11',
  baud: 921600,
  file: 'esp32c3_pc_monitor.bin',
  target: 'esp32c3',
  full: true,
  auto_provision: true,
  wifi_ssid: '1',
  wifi_pass: '11111111',
  mdns_host: 'esp-node',
})

const taskState = reactive({
  task_id: '',
  kind: 'idle',
  label: '',
  status: 'idle',
  log: '',
  artifact_name: '',
  artifact_size: 0,
  tasks: [],
})
const activeTaskId = ref('')

// Device Inspector Drawer
const inspect = reactive({
  visible: false,
  device: null,
  pins: [],
  storageInfo: null,
  displayStatus: null,
  displayLoading: false,
  displayError: '',
  hidText: '',
  camImgUrl: '',
  cfgForm: {
    name: '',
    wifi_ssid: '',
    wifi_pass: '',
    mdns_host: '',
    hub_url: '',
    ws_gateway_url: '',
    ws_enabled: true,
    ws_reconnect_ms: 5000,
    ws_heartbeat_ms: 10000,
    hub_fallback_mdns: true,
    hb_interval_ms: 12000,
    auth_strict: false,
    api_token: '',
  },
})

const inspectHasDisplay = computed(() =>
  Boolean(
    inspect.device?.capabilities?.display?.enabled ||
      inspect.device?.capabilities?.display?.supported ||
      inspect.device?.capabilities?.pc_monitor?.enabled,
  ),
)

// Batch OTA Modal
const otaDialog = reactive({
  visible: false,
  file: '',
  selectedIds: [],
  submitting: false,
})

// LAN Scan & Key Adoption Modal
const scanDialog = reactive({
  visible: false,
  scanning: false,
  subnet: '',
  use_mdns: true,
  use_http_scan: true,
  results: [],
  adoptingIp: '',
  apiKey: '',
  setHubUrl: true,
  setWsGateway: true,
  wsGatewayUrl: '',
  wsEnabled: true,
  manualIp: '',
  manualPort: 80,
})

const onlineCount = computed(() => devices.value.filter((d) => d.status === 'online').length)
const activeCapsList = (caps) => {
  if (!caps || typeof caps !== 'object') return []
  return Object.entries(caps)
    .filter(([, v]) => v && v.enabled)
    .map(([k]) => k)
}

async function refreshAll() {
  loading.value = true
  try {
    const [dRes, fRes, pRes, cRes] = await Promise.all([
      unifiedApi.devices(),
      unifiedApi.firmwares(),
      unifiedApi.profiles(),
      isAdmin.value ? unifiedApi.ports() : Promise.resolve({ ports: [] }),
    ])
    devices.value = dRes.devices || []
    auditLogs.value = dRes.audit || []
    firmwares.value = fRes.firmwares || []
    profiles.value = pRes.profiles || []
    ports.value = cRes.ports || []
    if (ports.value.length && !ports.value.some((x) => x.port === flashForm.port)) {
      flashForm.port = ports.value[0].port
    }
    if (firmwares.value.length && !firmwares.value.some((x) => x.file === flashForm.file)) {
      flashForm.file = firmwares.value[0].file
      flashForm.target = firmwares.value[0].target
    }
  } finally {
    loading.value = false
  }
}

async function pollDevices() {
  try {
    const res = await unifiedApi.devices()
    devices.value = res.devices || []
    auditLogs.value = res.audit || []
  } catch {
    // ignore background polling error
  }
}

function selectProfile(p) {
  buildForm.name = p.name
  buildForm.target = p.target || 'esp32'
  buildForm.description = p.description || ''
  Object.assign(buildForm.hardware, { cpu_freq_mhz: 160, flash_size: '4MB', spiram: false }, p.hardware || {})
  for (const k of Object.keys(buildForm.features)) {
    buildForm.features[k] = Boolean((p.features || {})[k])
  }
  if (buildForm.features.fleet_agent === false && p.features?.fleet_agent === undefined) {
    buildForm.features.fleet_agent = true
  }
  if (p.defaults) {
    buildForm.defaults.wifi_ssid = p.defaults.wifi_ssid || '1'
    buildForm.defaults.wifi_pass = p.defaults.wifi_pass || '11111111'
    buildForm.defaults.mdns_host = p.defaults.mdns_host || p.name
    buildForm.defaults.device_id = p.defaults.device_id || ''
    buildForm.defaults.ws_gateway_url = p.defaults.ws_gateway_url || ''
  }
}

async function pollTask() {
  try {
    const st = await unifiedApi.taskStatus(activeTaskId.value)
    Object.assign(taskState, st)
    const anyRunning = (st.tasks || []).some((t) => t.status === 'building' || t.status === 'flashing')
    if (st.status !== 'building' && st.status !== 'flashing' && !anyRunning) {
      clearInterval(taskTimer)
      taskTimer = null
      refreshAll()
    }
  } catch {
    // ignore
  }
}

function selectTask(tid) {
  activeTaskId.value = tid
  pollTask()
}

function ensureTaskPolling() {
  if (!taskTimer) {
    taskTimer = setInterval(pollTask, 1200)
  }
}

async function runBuild() {
  const payload = JSON.parse(JSON.stringify(buildForm))
  const res = await unifiedApi.startBuild(payload)
  if (res?.task_id) activeTaskId.value = res.task_id
  ElMessage.success(`云端编译已启动: ${payload.name}`)
  ensureTaskPolling()
}

async function runFlash() {
  const res = await unifiedApi.flashSerial({ ...flashForm })
  if (res?.task_id) activeTaskId.value = res.task_id
  ElMessage.success(`开始串口烧录到 ${flashForm.port}`)
  ensureTaskPolling()
}

async function loadInspectDisplay(dev = inspect.device) {
  if (!dev) return
  inspect.displayLoading = true
  inspect.displayError = ''
  try {
    inspect.displayStatus = await unifiedApi.proxyGet(dev.device_id, '/api/display/status', true)
  } catch (e) {
    inspect.displayError = e?.response?.data?.detail || e?.message || '读取屏幕配置失败'
  } finally {
    inspect.displayLoading = false
  }
}

async function openInspect(dev) {
  inspect.device = dev
  inspect.visible = true
  inspect.pins = []
  inspect.storageInfo = null
  inspect.displayStatus = null
  inspect.displayError = ''
  if (inspect.camImgUrl) {
    URL.revokeObjectURL(inspect.camImgUrl)
    inspect.camImgUrl = ''
  }
  inspect.cfgForm.name = dev.name || dev.device_id
  inspect.cfgForm.mdns_host = dev.mdns_host || ''
  inspect.cfgForm.wifi_ssid = dev.capabilities?.wifi?.ssid || '1'
  inspect.cfgForm.wifi_pass = ''
  inspect.cfgForm.api_token = ''
  Object.assign(
    inspect.cfgForm,
    normalizeWsConfigForm({
      hub_url: dev.capabilities?.fleet_agent?.active_hub || '',
      hub_fallback_mdns: dev.capabilities?.hub_fallback_mdns,
      ws_enabled: dev.capabilities?.ws_gateway?.enabled,
      ws_gateway_url:
        dev.capabilities?.ws_gateway?.configured_url || dev.capabilities?.ws_gateway?.active_url || '',
      auth_strict: dev.capabilities?.auth_strict,
    }),
  )

  if (dev.status !== 'online' && !dev.ws_online) return
  try {
    const cfg = await unifiedApi.proxyGet(dev.device_id, '/api/system/config')
    Object.assign(inspect.cfgForm, normalizeWsConfigForm({ ...inspect.cfgForm, ...cfg }))
    if (cfg.wifi_ssid) inspect.cfgForm.wifi_ssid = cfg.wifi_ssid
  } catch { /* ignore */ }

  if (dev.capabilities?.gpio?.enabled) {
    try {
      const g = await unifiedApi.proxyGet(dev.device_id, '/api/gpio/pins')
      inspect.pins = g.pins || []
    } catch { /* ignore */ }
  }
  if (dev.capabilities?.storage?.enabled) {
    try {
      inspect.storageInfo = await unifiedApi.proxyGet(dev.device_id, '/api/storage/list')
    } catch { /* ignore */ }
  }
  if (
    dev.capabilities?.display?.enabled ||
    dev.capabilities?.display?.supported ||
    dev.capabilities?.pc_monitor?.enabled
  ) {
    await loadInspectDisplay(dev)
  }
}

async function togglePin(pin, nextVal) {
  const dev = inspect.device
  await unifiedApi.sendCommand(dev.device_id, 'gpio_set', { pin: pin.pin, val: nextVal })
  const g = await unifiedApi.proxyGet(dev.device_id, '/api/gpio/pins')
  inspect.pins = g.pins || []
  ElMessage.success(`GPIO ${pin.pin} -> ${nextVal}`)
}

async function saveShadowConfig() {
  const dev = inspect.device
  const patch = buildUnifiedConfigPatch(inspect.cfgForm)
  const res = await unifiedApi.pushConfig(dev.device_id, patch)
  const metaPayload = { claimed: true }
  let needMeta = false
  if (inspect.cfgForm.name && inspect.cfgForm.name !== dev.name) {
    metaPayload.name = inspect.cfgForm.name
    needMeta = true
  }
  if (inspect.cfgForm.api_token && inspect.cfgForm.api_token.trim()) {
    metaPayload.api_token = inspect.cfgForm.api_token.trim()
    needMeta = true
  }
  if (needMeta) {
    await unifiedApi.claimDevice(dev.device_id, metaPayload)
  }
  ElMessage.success(res.pushed_live ? '配置已实时生效并同步影子' : '配置已写入影子队列')
  pollDevices()
}

async function refreshSnapshot() {
  if (!inspect.device) return
  try {
    const blob = await unifiedApi.captureSnapshot(inspect.device.device_id)
    if (inspect.camImgUrl) URL.revokeObjectURL(inspect.camImgUrl)
    inspect.camImgUrl = URL.createObjectURL(blob)
  } catch {
    ElMessage.warning('摄像头抓拍失败')
  }
}

async function toggleFlashLed(duty) {
  await unifiedApi.sendCommand(inspect.device.device_id, 'camera_flash', { duty })
  ElMessage.success(`补光灯占空比设为 ${duty}`)
}

async function saveUnifiedDisplayConfig(payload) {
  if (!inspect.device) return
  inspect.displayLoading = true
  inspect.displayError = ''
  try {
    const res = await unifiedApi.sendCommand(inspect.device.device_id, 'display_config', payload)
    if (res?.response) inspect.displayStatus = res.response
    else await loadInspectDisplay(inspect.device)
    ElMessage.success('屏幕配置已保存并生效')
  } catch (e) {
    inspect.displayError = e?.response?.data?.detail || e?.message || '屏幕配置下发失败'
  } finally {
    inspect.displayLoading = false
  }
}

async function sendUnifiedDisplayImage(payload) {
  if (!inspect.device) return
  inspect.displayLoading = true
  inspect.displayError = ''
  try {
    const res = await unifiedApi.sendCommand(inspect.device.device_id, 'display_image', payload)
    if (res?.response) inspect.displayStatus = res.response
    else await loadInspectDisplay(inspect.device)
    ElMessage.success('图片已推送到墨水屏')
  } catch (e) {
    inspect.displayError = e?.response?.data?.detail || e?.message || '墨水屏图片推送失败'
  } finally {
    inspect.displayLoading = false
  }
}

async function sendHidText() {
  if (!inspect.hidText) return
  await unifiedApi.sendCommand(inspect.device.device_id, 'hid_text', { text: inspect.hidText })
  ElMessage.success('已触发 USB-OTG HID 文本输出')
}

async function claimDev(dev) {
  await unifiedApi.claimDevice(dev.device_id, { name: dev.name || dev.device_id, claimed: true })
  ElMessage.success(`已认领 ${dev.device_id}`)
  pollDevices()
}

async function removeDev(dev) {
  await ElMessageBox.confirm(`确认删除设备 ${dev.device_id}？`, '确认')
  await unifiedApi.deleteDevice(dev.device_id)
  ElMessage.success('设备已移除')
  pollDevices()
}

async function publishToMainRepo(fw) {
  await unifiedApi.publishFirmware(fw.file, { version: fw.fw_version })
  ElMessage.success(`已将 ${fw.file} 发布至“固件与 OTA”主仓库！`)
}

function openBatchOta(fw) {
  otaDialog.file = fw.file
  otaDialog.selectedIds = devices.value
    .filter((d) => d.status === 'online')
    .map((d) => d.device_id)
  otaDialog.visible = true
}

async function submitBatchOta() {
  if (!otaDialog.selectedIds.length) {
    ElMessage.warning('请至少选择一台在线设备')
    return
  }
  otaDialog.submitting = true
  try {
    const res = await unifiedApi.batchOta(otaDialog.file, otaDialog.selectedIds)
    const okCnt = (res.results || []).filter((r) => r.ok).length
    ElMessage.success(`批量 OTA 已下发 (${okCnt}/${(res.results || []).length})`)
    otaDialog.visible = false
    setTimeout(pollDevices, 800)
  } finally {
    otaDialog.submitting = false
  }
}

function openScanDialog() {
  scanDialog.visible = true
  if (!scanDialog.results.length && !scanDialog.scanning) {
    runLanDiscover()
  }
}

async function runLanDiscover() {
  scanDialog.scanning = true
  try {
    const res = await unifiedApi.discoverLan({
      subnet: scanDialog.subnet.trim(),
      use_mdns: scanDialog.use_mdns,
      use_http_scan: scanDialog.use_http_scan,
    })
    scanDialog.results = res.devices || []
    ElMessage.success(`局域网扫描完成：发现 ${scanDialog.results.length} 台统一节点`)
  } finally {
    scanDialog.scanning = false
  }
}

async function adoptScannedDevice(row) {
  const ip = String(row?.ip || scanDialog.manualIp || '').trim()
  const port = Number(row?.port || scanDialog.manualPort || 80)
  if (!ip) {
    ElMessage.warning('请输入目标设备 IPv4 地址')
    return
  }
  scanDialog.adoptingIp = ip
  try {
    const res = await unifiedApi.adoptDevice({
      ip,
      port,
      api_token: scanDialog.apiKey,
      name: row?.local_name || row?.mdns_host || undefined,
      claimed: true,
      set_hub_url: scanDialog.setHubUrl,
      set_ws_gateway: scanDialog.setWsGateway,
      ws_gateway_url: scanDialog.wsGatewayUrl.trim() || undefined,
      ws_enabled: scanDialog.wsEnabled,
    })
    ElMessage.success(`已通过密钥校验并接入 ${res.device?.device_id || ip}`)
    await pollDevices()
  } finally {
    scanDialog.adoptingIp = ''
  }
}

onMounted(() => {
  refreshAll()
  devicesTimer = setInterval(pollDevices, 8000)
})

onBeforeUnmount(() => {
  if (devicesTimer) clearInterval(devicesTimer)
  if (taskTimer) clearInterval(taskTimer)
  if (inspect.camImgUrl) URL.revokeObjectURL(inspect.camImgUrl)
})
</script>

<template>
  <div class="unified-page">
    <div class="header-bar glass-panel">
      <div>
        <h2 class="page-title">
          <el-icon><Connection /></el-icon>
          跨芯片统一 IoT 与云端编译中心
        </h2>
        <p class="page-sub">
          统一纳管 ESP32-C3 / ESP32-S3 / ESP32-CAM / ESP8266 节点、LuatOS 风格裁剪编译与出厂 NVS 预置烧录
        </p>
      </div>
      <div class="header-stats">
        <el-tag type="info" effect="dark">总节点 {{ devices.length }}</el-tag>
        <el-tag type="success" effect="dark">在线 {{ onlineCount }}</el-tag>
        <el-tag type="primary" effect="dark">固件产物 {{ firmwares.length }}</el-tag>
        <el-button type="primary" plain @click="openScanDialog">
          <el-icon><Connection /></el-icon> 局域网扫描 / 密钥接入
        </el-button>
        <el-button :loading="loading" @click="refreshAll">
          <el-icon><Refresh /></el-icon> 刷新
        </el-button>
      </div>
    </div>

    <el-tabs v-model="activeTab" class="main-tabs">
      <!-- Tab 1: Unified Multi-Chip Fleet -->
      <el-tab-pane label="统一节点大盘" name="devices">
        <div v-if="!devices.length" class="empty-box glass-panel">
          暂无统一节点接入（串口烧录启用“自动出厂 NVS 预置”或设备加入同局域网后将自动通过 _esp-hub._tcp 注册到此）
        </div>
        <div class="device-grid">
          <div v-for="d in devices" :key="d.device_id" class="dev-card glass-panel">
            <div class="dev-top">
              <div>
                <div class="dev-name">{{ d.name || d.device_id }}</div>
                <div class="dev-id">{{ d.device_id }} · {{ d.mac }}</div>
              </div>
              <div style="display: flex; gap: 6px; align-items: center">
                <el-tag v-if="d.ws_online" type="primary" size="small" effect="plain">WS 隧道</el-tag>
                <el-tag :type="d.status === 'online' ? 'success' : 'warning'" size="small">
                  {{ d.status === 'online' ? '在线' : d.status }}
                </el-tag>
              </div>
            </div>

            <div class="dev-meta">
              <div>芯片：<b>{{ d.chip || '未知' }}</b></div>
              <div>网络：{{ d.ip || '未上线' }} · RSSI {{ d.rssi }} dBm</div>
              <div>存储：Flash {{ d.flash_mb }}MB / PSRAM {{ d.psram_mb || 0 }}MB / Heap {{ Math.round((d.free_heap || 0) / 1024) }}KB</div>
              <div>影子版本：v{{ d.shadow?.reported_ver || 1 }} -> v{{ d.shadow?.desired_ver || 1 }} ({{ d.shadow?.sync_status || 'synced' }})</div>
            </div>

            <div
              v-if="d.ota_progress && ['starting', 'downloading', 'verifying', 'rebooting', 'failed'].includes(d.ota_progress.status)"
              class="ota-progress-box"
            >
              <div style="display: flex; justify-content: space-between; font-size: 12px; margin-bottom: 4px">
                <span>OTA 状态: {{ d.ota_progress.status }}</span>
                <span>{{ d.ota_progress.progress }}%</span>
              </div>
              <el-progress
                :percentage="d.ota_progress.progress"
                :status="d.ota_progress.status === 'failed' ? 'exception' : (d.ota_progress.progress === 100 ? 'success' : '')"
                :stroke-width="6"
              />
            </div>

            <div class="caps-row">
              <el-tag v-for="c in activeCapsList(d.capabilities)" :key="c" size="small" effect="plain">
                {{ c }}
              </el-tag>
            </div>

            <div class="dev-actions">
              <el-button type="primary" size="small" @click="openInspect(d)">
                <el-icon><Setting /></el-icon> 控制与配置
              </el-button>
              <el-button v-if="!d.claimed" type="success" size="small" plain @click="claimDev(d)">
                <el-icon><Check /></el-icon> 认领
              </el-button>
              <el-button type="danger" size="small" plain @click="removeDev(d)">
                <el-icon><Delete /></el-icon>
              </el-button>
            </div>
          </div>
        </div>
      </el-tab-pane>

      <!-- Tab 2: Cloud Compiler & Factory Provision Flasher -->
      <el-tab-pane label="LuatOS 云端裁剪编译与串口烧录" name="compiler">
        <div class="compiler-grid">
          <div class="glass-panel panel-box">
            <h3>1. 选择芯片模板与功能裁剪</h3>
            <div class="preset-pills">
              <el-button
                v-for="p in profiles"
                :key="p.id"
                size="small"
                :type="buildForm.name === p.name ? 'primary' : 'default'"
                @click="selectProfile(p)"
              >
                {{ p.name }} ({{ p.target }})
              </el-button>
            </div>

            <el-form label-width="110px" size="small" style="margin-top: 12px">
              <el-form-item label="配置名称">
                <el-input v-model="buildForm.name" />
              </el-form-item>
              <el-form-item label="目标芯片">
                <el-select v-model="buildForm.target" style="width: 100%">
                  <el-option label="ESP32-S3 (双核 / PSRAM / USB-OTG)" value="esp32s3" />
                  <el-option label="ESP32-C3 (RISC-V / PC Monitor)" value="esp32c3" />
                  <el-option label="ESP32 (经典 / ESP32-CAM)" value="esp32" />
                  <el-option label="ESP8266 (1MB 轻量 PlatformIO)" value="esp8266" />
                </el-select>
              </el-form-item>
              <el-form-item label="Flash / PSRAM">
                <div style="display: flex; gap: 10px; align-items: center">
                  <el-select v-model="buildForm.hardware.flash_size" style="width: 110px">
                    <el-option label="2MB" value="2MB" />
                    <el-option label="4MB" value="4MB" />
                    <el-option label="8MB" value="8MB" />
                    <el-option label="16MB" value="16MB" />
                  </el-select>
                  <el-checkbox v-model="buildForm.hardware.spiram">启用 PSRAM</el-checkbox>
                </div>
              </el-form-item>
              <el-form-item label="功能模块">
                <div class="feat-checks">
                  <el-checkbox v-model="buildForm.features.web_ui">Web UI</el-checkbox>
                  <el-checkbox v-model="buildForm.features.ota">防砖 OTA</el-checkbox>
                  <el-checkbox v-model="buildForm.features.fleet_agent">Fleet Agent</el-checkbox>
                  <el-checkbox v-model="buildForm.features.gpio_ctrl">GPIO 矩阵</el-checkbox>
                  <el-checkbox v-model="buildForm.features.camera">OV2640 摄像头</el-checkbox>
                  <el-checkbox v-model="buildForm.features.storage">SPIFFS 存储</el-checkbox>
                  <el-checkbox v-model="buildForm.features.display">SPI 显示屏</el-checkbox>
                  <el-checkbox v-model="buildForm.features.usb_otg">USB-OTG HID</el-checkbox>
                  <el-checkbox v-model="buildForm.features.pc_monitor">C3 监控屏</el-checkbox>
                  <el-checkbox v-model="buildForm.features.ws_gateway">HID 网关 WS</el-checkbox>
                </div>
              </el-form-item>
              <el-form-item label="默认 Wi-Fi">
                <div style="display: flex; gap: 8px">
                  <el-input v-model="buildForm.defaults.wifi_ssid" placeholder="SSID" />
                  <el-input v-model="buildForm.defaults.wifi_pass" placeholder="Password" />
                </div>
              </el-form-item>
              <el-form-item label="HID WS 网关">
                <el-input v-model="buildForm.defaults.ws_gateway_url" placeholder="例如 ws://192.168.68.159:8000/ws/device (留空按 Hub 推导)" />
              </el-form-item>
            </el-form>
            <el-button type="primary" :disabled="!isAdmin" :loading="taskState.status === 'building'" @click="runBuild">
              启动云端裁剪编译
            </el-button>
          </div>

          <div class="glass-panel panel-box">
            <h3>2. 本机串口烧录 + 自动 NVS 出厂预置</h3>
            <el-form label-width="110px" size="small">
              <el-form-item label="串口设备">
                <el-select v-model="flashForm.port" style="width: 100%" :disabled="!isAdmin">
                  <el-option v-for="p in ports" :key="p.port" :label="`${p.port} (${p.desc})`" :value="p.port" />
                </el-select>
              </el-form-item>
              <el-form-item label="选择固件">
                <el-select
                  v-model="flashForm.file"
                  style="width: 100%"
                  @change="(val) => { const f = firmwares.find(x => x.file === val); if (f) flashForm.target = f.target }"
                >
                  <el-option v-for="f in firmwares" :key="f.file" :label="`${f.file} [${f.target}]`" :value="f.file" />
                </el-select>
              </el-form-item>
              <el-form-item label="预置凭据">
                <el-checkbox v-model="flashForm.auto_provision">生成唯一 device_id / HMAC 密钥并刷入 0x9000 NVS</el-checkbox>
              </el-form-item>
              <el-form-item label="Wi-Fi / 波特率">
                <div style="display: flex; gap: 8px">
                  <el-input v-model="flashForm.wifi_ssid" placeholder="SSID" />
                  <el-input v-model="flashForm.wifi_pass" placeholder="密码" />
                  <el-select v-model="flashForm.baud" style="width: 120px">
                    <el-option :value="921600" label="921600" />
                    <el-option :value="460800" label="460800" />
                    <el-option :value="115200" label="115200" />
                  </el-select>
                </div>
              </el-form-item>
            </el-form>
            <el-button type="success" :disabled="!isAdmin" :loading="taskState.status === 'flashing'" @click="runFlash">
              一键芯片指纹校对并烧录
            </el-button>

            <div v-if="taskState.tasks?.length" style="display: flex; flex-wrap: wrap; gap: 6px; margin-top: 10px">
              <el-tag
                v-for="t in taskState.tasks.slice(0, 6)"
                :key="t.task_id"
                size="small"
                style="cursor: pointer"
                :type="t.status === 'success' ? 'success' : (t.status === 'failed' ? 'danger' : 'warning')"
                :effect="taskState.task_id === t.task_id ? 'dark' : 'plain'"
                @click="selectTask(t.task_id)"
              >
                {{ t.label || t.kind }} · {{ t.status }}
              </el-tag>
            </div>

            <pre class="console-log">{{ taskState.log || '等待构建或烧录任务...' }}</pre>
          </div>
        </div>
      </el-tab-pane>

      <!-- Tab 3: Firmware Repo & Batch OTA -->
      <el-tab-pane label="统一固件制品与批量 OTA" name="firmwares">
        <div class="glass-panel panel-box">
          <el-table :data="firmwares" style="width: 100%">
            <el-table-column prop="file" label="固件文件名" min-width="200" />
            <el-table-column prop="target" label="目标架构" width="110" />
            <el-table-column label="大小" width="110">
              <template #default="{ row }">{{ (row.size / 1024).toFixed(1) }} KB</template>
            </el-table-column>
            <el-table-column prop="fw_version" label="版本" width="140" />
            <el-table-column label="SHA-256" min-width="180">
              <template #default="{ row }">
                <code>{{ (row.sha256 || '').slice(0, 16) }}...</code>
              </template>
            </el-table-column>
            <el-table-column label="操作" width="300">
              <template #default="{ row }">
                <el-button size="small" type="primary" :disabled="!isAdmin" @click="openBatchOta(row)">
                  <el-icon><Promotion /></el-icon> 批量 Pull OTA
                </el-button>
                <el-button size="small" type="warning" plain :disabled="!isAdmin" @click="publishToMainRepo(row)">
                  <el-icon><Upload /></el-icon> 发布至主 HID 固件库
                </el-button>
              </template>
            </el-table-column>
          </el-table>
        </div>
      </el-tab-pane>
    </el-tabs>

    <!-- Device Control Drawer -->
    <el-drawer v-model="inspect.visible" :title="`节点控制: ${inspect.device?.device_id || ''}`" size="500px">
      <template v-if="inspect.device">
        <el-divider content-position="left">影子配置与网关绑定</el-divider>
        <el-form label-width="100px" size="small">
          <el-form-item label="显示名称"><el-input v-model="inspect.cfgForm.name" /></el-form-item>
          <el-form-item label="mDNS 域名"><el-input v-model="inspect.cfgForm.mdns_host" /></el-form-item>
          <el-form-item label="Wi-Fi SSID"><el-input v-model="inspect.cfgForm.wifi_ssid" /></el-form-item>
          <el-form-item label="Wi-Fi 密码"><el-input v-model="inspect.cfgForm.wifi_pass" type="password" placeholder="留空不修改" /></el-form-item>
          <el-form-item label="Hub URL"><el-input v-model="inspect.cfgForm.hub_url" /></el-form-item>
          <el-form-item label="Hub mDNS回退">
            <div style="display: flex; gap: 12px; align-items: center">
              <el-switch v-model="inspect.cfgForm.hub_fallback_mdns" />
              <span style="font-size: 12px; opacity: 0.75">心跳周期(ms)</span>
              <el-input-number v-model="inspect.cfgForm.hb_interval_ms" :min="2000" :max="120000" :step="1000" />
            </div>
          </el-form-item>
          <el-form-item label="WS 出站客户端">
            <div style="display: flex; gap: 12px; align-items: center">
              <el-switch v-model="inspect.cfgForm.ws_enabled" />
              <span style="font-size: 12px; opacity: 0.75">重连(ms)</span>
              <el-input-number v-model="inspect.cfgForm.ws_reconnect_ms" :min="500" :max="60000" :step="500" />
            </div>
          </el-form-item>
          <el-form-item label="WS 网关 URL"><el-input v-model="inspect.cfgForm.ws_gateway_url" placeholder="ws://<host>:8000/ws/device (留空按 Hub 推导)" /></el-form-item>
          <el-form-item label="WS 心跳(ms)">
            <el-input-number v-model="inspect.cfgForm.ws_heartbeat_ms" :min="2000" :max="120000" :step="1000" />
          </el-form-item>
          <el-form-item label="严格密钥鉴权">
            <div style="display: flex; gap: 12px; align-items: center; width: 100%">
              <el-switch v-model="inspect.cfgForm.auth_strict" />
              <el-input v-model="inspect.cfgForm.api_token" type="password" show-password placeholder="设置/更新访问 Key (留空不改)" />
            </div>
          </el-form-item>
          <el-button type="primary" size="small" @click="saveShadowConfig">下发配置并同步影子</el-button>
        </el-form>

        <template v-if="inspect.pins.length">
          <el-divider content-position="left">安全 GPIO 控制</el-divider>
          <div v-for="p in inspect.pins" :key="p.pin" class="pin-row">
            <span>{{ p.label }} (GPIO{{ p.pin }})</span>
            <el-button size="small" :type="p.val ? 'success' : 'info'" @click="togglePin(p, p.val ? 0 : 1)">
              {{ p.val ? 'ON (点击关闭)' : 'OFF (点击开启)' }}
            </el-button>
          </div>
        </template>

        <template v-if="inspect.device.capabilities?.camera?.enabled">
          <el-divider content-position="left">OV2640 摄像头</el-divider>
          <div style="display: flex; gap: 8px; margin-bottom: 8px">
            <el-button size="small" type="primary" @click="refreshSnapshot"><el-icon><VideoCamera /></el-icon> 实时抓拍</el-button>
            <el-button size="small" @click="toggleFlashLed(128)">补光灯半亮</el-button>
            <el-button size="small" @click="toggleFlashLed(0)">关灯</el-button>
          </div>
          <img v-if="inspect.camImgUrl" :src="inspect.camImgUrl" style="width: 100%; border-radius: 8px" />
        </template>

        <template v-if="inspectHasDisplay">
          <el-divider content-position="left">屏幕显示配置化</el-divider>
          <DisplayConfigPanel
            compact
            :status="inspect.displayStatus"
            :device-name="inspect.device?.name || inspect.device?.device_id || ''"
            :loading="inspect.displayLoading"
            :pc-monitor-default="Boolean(inspect.device?.capabilities?.pc_monitor?.enabled)"
            :error-message="inspect.displayError"
            @refresh="loadInspectDisplay()"
            @save="saveUnifiedDisplayConfig"
            @send-image="sendUnifiedDisplayImage"
          />
        </template>

        <template v-if="inspect.storageInfo">
          <el-divider content-position="left">SPIFFS 文件存储 ({{ inspect.storageInfo.used_bytes }} / {{ inspect.storageInfo.total_bytes }} B)</el-divider>
          <div v-for="f in (inspect.storageInfo.files || [])" :key="f.name" class="pin-row">
            <span>{{ f.name }}</span><span>{{ f.size }} B</span>
          </div>
        </template>

        <template v-if="inspect.device.capabilities?.usb_otg?.enabled">
          <el-divider content-position="left">USB-OTG HID 测试 (支持 WS 接入“动作控制台”)</el-divider>
          <div style="display: flex; gap: 8px">
            <el-input v-model="inspect.hidText" size="small" placeholder="输入测试文本" />
            <el-button size="small" type="primary" @click="sendHidText">发送键盘输入</el-button>
          </div>
        </template>
      </template>
    </el-drawer>

    <!-- Batch OTA Dialog -->
    <el-dialog v-model="otaDialog.visible" title="批量安全 Pull OTA (SHA-256 + 90s 自动回滚)" width="460px">
      <p>目标固件：<b>{{ otaDialog.file }}</b></p>
      <el-checkbox-group v-model="otaDialog.selectedIds">
        <el-checkbox v-for="d in devices" :key="d.device_id" :label="d.device_id">
          {{ d.name || d.device_id }} ({{ d.chip }} · {{ d.status }})
        </el-checkbox>
      </el-checkbox-group>
      <template #footer>
        <el-button @click="otaDialog.visible = false">取消</el-button>
        <el-button type="primary" :loading="otaDialog.submitting" @click="submitBatchOta">确认下发</el-button>
      </template>
    </el-dialog>

    <!-- LAN Scan & Key Adoption Dialog -->
    <el-dialog v-model="scanDialog.visible" title="局域网发现与密钥跨网接入 (_esp-unified._tcp)" width="760px">
      <el-form label-width="110px" size="small">
        <el-form-item label="扫描网段 / 方式">
          <div style="display: flex; gap: 10px; align-items: center; flex-wrap: wrap; width: 100%">
            <el-input v-model="scanDialog.subnet" placeholder="可选 CIDR (如 192.168.68.0/24, 留空自动识别)" style="max-width: 280px" />
            <el-checkbox v-model="scanDialog.use_mdns">mDNS (_esp-unified._tcp)</el-checkbox>
            <el-checkbox v-model="scanDialog.use_http_scan">HTTP 网段探测</el-checkbox>
            <el-button type="primary" :loading="scanDialog.scanning" @click="runLanDiscover">开始扫描</el-button>
          </div>
        </el-form-item>
        <el-form-item label="接入凭证 Key">
          <el-input v-model="scanDialog.apiKey" type="password" show-password placeholder="设备 X-API-Key / api_token / device_secret（无密码设备可留空）" />
        </el-form-item>
        <el-form-item label="重绑定目标网关">
          <div style="display: flex; gap: 10px; align-items: center; flex-wrap: wrap; width: 100%">
            <el-checkbox v-model="scanDialog.setHubUrl">绑定当前 Hub</el-checkbox>
            <el-checkbox v-model="scanDialog.setWsGateway">绑定 WS 客户端网关</el-checkbox>
            <el-switch v-model="scanDialog.wsEnabled" active-text="启用 WS" />
            <el-input v-model="scanDialog.wsGatewayUrl" placeholder="自定义 ws://host:port/ws/device (留空随 Hub 推导)" style="flex: 1; min-width: 240px" />
          </div>
        </el-form-item>
      </el-form>

      <el-table :data="scanDialog.results" size="small" max-height="260" style="width: 100%; margin-bottom: 12px">
        <el-table-column prop="device_id" label="设备 ID" min-width="145" />
        <el-table-column label="地址" width="150">
          <template #default="{ row }">{{ row.ip }}:{{ row.port || 80 }}</template>
        </el-table-column>
        <el-table-column prop="chip" label="芯片" width="95" />
        <el-table-column label="发现/鉴权" width="165">
          <template #default="{ row }">
            <el-tag size="small" effect="plain">{{ (row.via || []).join('+') }}</el-tag>
            <el-tag size="small" :type="row.auth_strict ? 'danger' : (row.auth_required ? 'warning' : 'success')" style="margin-left: 4px">
              {{ row.auth_strict ? '严格 Key' : (row.auth_required ? '写鉴权' : '开放') }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="140">
          <template #default="{ row }">
            <el-button
              size="small"
              type="success"
              :disabled="row.can_adopt === false"
              :loading="scanDialog.adoptingIp === row.ip"
              @click="adoptScannedDevice(row)"
            >
              校验并接入
            </el-button>
          </template>
        </el-table-column>
      </el-table>

      <div style="display: flex; gap: 8px; align-items: center; padding-top: 4px">
        <span style="font-size: 12px; opacity: 0.8; white-space: nowrap">手动直连 IP：</span>
        <el-input v-model="scanDialog.manualIp" size="small" placeholder="例如 192.168.68.143" style="max-width: 190px" />
        <el-input-number v-model="scanDialog.manualPort" size="small" :min="1" :max="65535" />
        <el-button size="small" type="primary" plain :loading="Boolean(scanDialog.adoptingIp)" @click="adoptScannedDevice(null)">
          校验密钥并接入
        </el-button>
      </div>
    </el-dialog>
  </div>
</template>

<style scoped>
.unified-page { display: flex; flex-direction: column; gap: 16px; }
.header-bar { display: flex; justify-content: space-between; align-items: center; padding: 16px 20px; border-radius: 14px; flex-wrap: wrap; gap: 12px; }
.page-title { margin: 0; font-size: 20px; display: flex; align-items: center; gap: 8px; }
.page-sub { margin: 4px 0 0; font-size: 13px; opacity: 0.75; }
.header-stats { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.device-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 14px; }
.dev-card { padding: 16px; border-radius: 14px; display: flex; flex-direction: column; gap: 10px; }
.dev-top { display: flex; justify-content: space-between; align-items: flex-start; }
.dev-name { font-weight: 700; font-size: 16px; }
.dev-id { font-size: 12px; opacity: 0.68; }
.dev-meta { font-size: 12.5px; line-height: 1.6; opacity: 0.9; }
.caps-row { display: flex; flex-wrap: wrap; gap: 6px; }
.dev-actions { display: flex; gap: 8px; margin-top: auto; padding-top: 6px; }
.compiler-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
@media (max-width: 980px) { .compiler-grid { grid-template-columns: 1fr; } }
.panel-box { padding: 18px; border-radius: 14px; }
.preset-pills { display: flex; flex-wrap: wrap; gap: 6px; }
.feat-checks { display: flex; flex-wrap: wrap; gap: 6px 14px; }
.console-log { margin-top: 12px; background: #0b1120; color: #cbd5e1; padding: 12px; border-radius: 10px; max-height: 290px; overflow: auto; font-size: 12px; white-space: pre-wrap; }
.pin-row { display: flex; justify-content: space-between; align-items: center; padding: 6px 0; border-bottom: 1px solid rgba(148, 163, 184, 0.2); }
.empty-box { padding: 28px; text-align: center; border-radius: 14px; opacity: 0.8; }
</style>
