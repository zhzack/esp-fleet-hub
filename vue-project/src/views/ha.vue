<script>
export const meta = { title: 'Home Assistant 桥接' }
</script>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { ElMessage } from 'element-plus'
import { Refresh, Connection, Setting, Promotion } from '@element-plus/icons-vue'
import { haApi } from '@/api/ha'
import { useAuthStore } from '@/stores/auth'

const auth = useAuthStore()
const isAdmin = computed(() => auth.user?.role === 'admin')

const loading = ref(false)
const testing = ref(false)
const savingCfg = ref(false)
const syncing = ref(false)

const bridge = reactive({
  enabled: false,
  running: false,
  connected: false,
  broker_host: '',
  broker_port: 1883,
  discovery_prefix: 'homeassistant',
  topic_prefix: 'esp_hub',
  published_devices: 0,
  published_entities: 0,
  last_sync_at: 0,
  last_error: '',
})

const cfgForm = reactive({
  enabled: false,
  broker_host: '',
  broker_port: 1883,
  mqtt_user: '',
  mqtt_pass: '',
  has_mqtt_pass: false,
  discovery_prefix: 'homeassistant',
  topic_prefix: 'esp_hub',
  sync_interval_sec: 10,
})

const devices = ref([])

const editDlg = reactive({
  visible: false,
  saving: false,
  device_uuid: '',
  name: '',
  capabilities: [],
  form: {
    enabled: true,
    display_name: '',
    area_name: '',
    expose_hid: true,
    expose_gpio: true,
    expose_display: true,
    expose_camera: true,
    pc_enabled: false,
    pc_name: '',
    pc_ip: '',
    pc_mac: '',
    pc_broadcast_ip: '255.255.255.255',
    pc_power_pin: null,
    pc_power_pulse_ms: 500,
    pc_os: 'windows',
    legacy_topic_prefix: '',
  },
})

const yamlDlg = reactive({
  visible: false,
  device_uuid: '',
  yaml: '',
})

async function loadAll() {
  loading.value = true
  try {
    const [st, dRes] = await Promise.all([haApi.status(), haApi.devices()])
    Object.assign(bridge, st.bridge || {})
    if (st.config) {
      cfgForm.enabled = Boolean(st.config.enabled)
      cfgForm.broker_host = st.config.broker_host || ''
      cfgForm.broker_port = Number(st.config.broker_port || 1883)
      cfgForm.mqtt_user = st.config.mqtt_user || ''
      cfgForm.has_mqtt_pass = Boolean(st.config.has_mqtt_pass)
      cfgForm.mqtt_pass = ''
      cfgForm.discovery_prefix = st.config.discovery_prefix || 'homeassistant'
      cfgForm.topic_prefix = st.config.topic_prefix || 'esp_hub'
      cfgForm.sync_interval_sec = Number(st.config.sync_interval_sec || 10)
    }
    devices.value = dRes.devices || []
  } finally {
    loading.value = false
  }
}

async function testBroker() {
  if (!cfgForm.broker_host.trim()) {
    ElMessage.warning('请填写 MQTT Broker 地址')
    return
  }
  testing.value = true
  try {
    const res = await haApi.testBroker({
      broker_host: cfgForm.broker_host.trim(),
      broker_port: Number(cfgForm.broker_port || 1883),
      mqtt_user: cfgForm.mqtt_user.trim(),
      mqtt_pass: cfgForm.mqtt_pass,
    })
    if (res.ok) ElMessage.success(res.message || 'MQTT 连接成功')
    else ElMessage.error(res.message || 'MQTT 连接失败')
  } finally {
    testing.value = false
  }
}

async function saveBrokerCfg() {
  savingCfg.value = true
  try {
    const payload = {
      enabled: cfgForm.enabled,
      broker_host: cfgForm.broker_host.trim(),
      broker_port: Number(cfgForm.broker_port || 1883),
      mqtt_user: cfgForm.mqtt_user.trim(),
      discovery_prefix: cfgForm.discovery_prefix.trim() || 'homeassistant',
      topic_prefix: cfgForm.topic_prefix.trim() || 'esp_hub',
      sync_interval_sec: Number(cfgForm.sync_interval_sec || 10),
    }
    if (cfgForm.mqtt_pass) payload.mqtt_pass = cfgForm.mqtt_pass
    const res = await haApi.saveConfig(payload)
    Object.assign(bridge, res.bridge || {})
    cfgForm.mqtt_pass = ''
    ElMessage.success('Home Assistant MQTT 配置已保存并热重载')
    setTimeout(loadAll, 800)
  } finally {
    savingCfg.value = false
  }
}

async function forceSync() {
  syncing.value = true
  try {
    const res = await haApi.syncNow()
    Object.assign(bridge, res.bridge || {})
    ElMessage.success(`已同步 ${res.devices || 0} 台设备，共发布 ${res.entities || 0} 个 HA 实体`)
  } finally {
    syncing.value = false
  }
}

function openTargetEditor(row) {
  editDlg.device_uuid = row.device_uuid
  editDlg.name = row.name
  editDlg.capabilities = row.capabilities || []
  Object.assign(editDlg.form, {
    enabled: row.target?.enabled ?? true,
    display_name: row.target?.display_name || '',
    area_name: row.target?.area_name || '',
    expose_hid: row.target?.expose_hid ?? true,
    expose_gpio: row.target?.expose_gpio ?? true,
    expose_display: row.target?.expose_display ?? true,
    expose_camera: row.target?.expose_camera ?? true,
    pc_enabled: Boolean(row.target?.pc_enabled),
    pc_name: row.target?.pc_name || '',
    pc_ip: row.target?.pc_ip || '',
    pc_mac: row.target?.pc_mac || '',
    pc_broadcast_ip: row.target?.pc_broadcast_ip || '255.255.255.255',
    pc_power_pin: row.target?.pc_power_pin ?? null,
    pc_power_pulse_ms: Number(row.target?.pc_power_pulse_ms || 500),
    pc_os: row.target?.pc_os || 'windows',
    legacy_topic_prefix: row.target?.legacy_topic_prefix || '',
  })
  editDlg.visible = true
}

async function saveTarget() {
  editDlg.saving = true
  try {
    await haApi.saveDeviceTarget(editDlg.device_uuid, {
      ...editDlg.form,
      pc_power_pin: editDlg.form.pc_power_pin === '' ? null : editDlg.form.pc_power_pin,
    })
    ElMessage.success(`已更新 ${editDlg.device_uuid} 的 HA 实体与 PC 映射配置`)
    editDlg.visible = false
    await loadAll()
  } finally {
    editDlg.saving = false
  }
}

async function triggerWol(row) {
  await haApi.sendWol(row.device_uuid, { wake_key: true })
  ElMessage.success(`已向 ${row.target?.pc_mac} 发送 WOL 唤醒包与唤醒按键`)
}

async function showAutomations(row) {
  const res = await haApi.automationsYaml(row.device_uuid)
  yamlDlg.device_uuid = row.device_uuid
  yamlDlg.yaml = res.yaml || ''
  yamlDlg.visible = true
}

async function copyYaml() {
  await navigator.clipboard.writeText(yamlDlg.yaml)
  ElMessage.success('已复制 YAML 到剪贴板')
}

onMounted(loadAll)
</script>

<template>
  <div class="ha-page">
    <div class="header-bar glass-panel">
      <div>
        <h2 class="page-title">
          <el-icon><Connection /></el-icon>
          Home Assistant · MQTT 自动发现中枢
        </h2>
        <p class="page-sub">
          基于统一节点架构按设备能力动态注册 GPIO / HID / 多媒体 / 屏幕 / 摄像头 / 台式机 WOL 与定时关机实体，全配置参数可外部热更改
        </p>
      </div>
      <div class="header-stats">
        <el-tag :type="bridge.connected ? 'success' : (bridge.enabled ? 'warning' : 'info')" effect="dark">
          {{ bridge.connected ? 'MQTT 已连接' : (bridge.enabled ? '重连中' : '未启用') }}
        </el-tag>
        <el-tag type="primary" effect="dark">同步设备 {{ bridge.published_devices }}</el-tag>
        <el-tag type="info" effect="dark">发现实体 {{ bridge.published_entities }}</el-tag>
        <el-button v-if="isAdmin" type="primary" plain :loading="syncing" @click="forceSync">
          <el-icon><Promotion /></el-icon> 立即同步实体
        </el-button>
        <el-button :loading="loading" @click="loadAll">
          <el-icon><Refresh /></el-icon> 刷新
        </el-button>
      </div>
    </div>

    <el-alert v-if="bridge.last_error" type="warning" show-icon :closable="false" :title="`MQTT 最近状态: ${bridge.last_error}`" />

    <div class="glass-panel panel-box">
      <h3>1. MQTT Broker 连接与自动发现参数（非写死配置，支持随时迁移新网络）</h3>
      <el-form label-width="135px" size="small" style="margin-top: 12px">
        <div class="form-grid">
          <el-form-item label="启用 MQTT 桥接">
            <el-switch v-model="cfgForm.enabled" :disabled="!isAdmin" />
          </el-form-item>
          <el-form-item label="Broker 地址">
            <el-input v-model="cfgForm.broker_host" :disabled="!isAdmin" placeholder="例如 192.168.68.146 或 mqtt.lan" />
          </el-form-item>
          <el-form-item label="Broker 端口">
            <el-input-number v-model="cfgForm.broker_port" :disabled="!isAdmin" :min="1" :max="65535" />
          </el-form-item>
          <el-form-item label="MQTT 用户名">
            <el-input v-model="cfgForm.mqtt_user" :disabled="!isAdmin" placeholder="匿名 Broker 可留空" />
          </el-form-item>
          <el-form-item label="MQTT 密码">
            <el-input
              v-model="cfgForm.mqtt_pass"
              :disabled="!isAdmin"
              type="password"
              show-password
              :placeholder="cfgForm.has_mqtt_pass ? '已配置加密密码（留空保持不变）' : '匿名 Broker 可留空'"
            />
          </el-form-item>
          <el-form-item label="HA Discovery 前缀">
            <el-input v-model="cfgForm.discovery_prefix" :disabled="!isAdmin" placeholder="homeassistant" />
          </el-form-item>
          <el-form-item label="Hub 主题前缀">
            <el-input v-model="cfgForm.topic_prefix" :disabled="!isAdmin" placeholder="esp_hub" />
          </el-form-item>
          <el-form-item label="状态同步周期(秒)">
            <el-input-number v-model="cfgForm.sync_interval_sec" :disabled="!isAdmin" :min="3" :max="300" />
          </el-form-item>
        </div>
        <div style="display: flex; gap: 10px; margin-top: 6px">
          <el-button v-if="isAdmin" :loading="testing" @click="testBroker">测试 Broker 连通性</el-button>
          <el-button v-if="isAdmin" type="primary" :loading="savingCfg" @click="saveBrokerCfg">保存配置并热重载</el-button>
        </div>
      </el-form>
    </div>

    <div class="glass-panel panel-box">
      <h3>2. 节点能力暴露、WOL 与 PC 控制映射</h3>
      <el-table :data="devices" size="small" style="width: 100%; margin-top: 10px">
        <el-table-column prop="device_uuid" label="设备 UUID" min-width="150" />
        <el-table-column prop="name" label="节点名称" min-width="130" />
        <el-table-column label="状态" width="90">
          <template #default="{ row }">
            <el-tag :type="row.online ? 'success' : 'info'" size="small">{{ row.online ? '在线' : '离线' }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="同步开关" width="90">
          <template #default="{ row }">
            <el-tag :type="row.target?.enabled ? 'primary' : 'info'" size="small">{{ row.target?.enabled ? '启用' : '停用' }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="实体数" prop="entity_count" width="80" />
        <el-table-column label="PC / WOL" min-width="180">
          <template #default="{ row }">
            <span v-if="row.target?.pc_enabled">{{ row.target.pc_name || 'PC' }} ({{ row.target.pc_mac || row.target.pc_ip || 'HID' }})</span>
            <span v-else style="opacity: 0.6">未开启 PC 映射</span>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="280">
          <template #default="{ row }">
            <el-button size="small" type="primary" plain @click="openTargetEditor(row)">
              <el-icon><Setting /></el-icon> 配置映射
            </el-button>
            <el-button v-if="row.target?.pc_mac" size="small" type="success" plain @click="triggerWol(row)">WOL</el-button>
            <el-button size="small" plain @click="showAutomations(row)">语音 YAML</el-button>
          </template>
        </el-table-column>
      </el-table>
    </div>

    <el-dialog v-model="editDlg.visible" :title="`HA 实体与 PC 控制映射: ${editDlg.device_uuid}`" width="600px">
      <el-form label-width="140px" size="small">
        <el-form-item label="暴露至 Home Assistant"><el-switch v-model="editDlg.form.enabled" /></el-form-item>
        <el-form-item label="HA 设备卡片名称"><el-input v-model="editDlg.form.display_name" placeholder="留空使用默认名称" /></el-form-item>
        <el-form-item label="建议房间区域"><el-input v-model="editDlg.form.area_name" placeholder="例如 书房 / 客厅" /></el-form-item>
        <el-form-item label="能力模块暴露">
          <div style="display: flex; gap: 12px; flex-wrap: wrap">
            <el-checkbox v-model="editDlg.form.expose_hid">HID 快捷键/媒体</el-checkbox>
            <el-checkbox v-model="editDlg.form.expose_gpio">GPIO 开关/调光</el-checkbox>
            <el-checkbox v-model="editDlg.form.expose_display">屏幕文字/模式</el-checkbox>
            <el-checkbox v-model="editDlg.form.expose_camera">摄像头抓拍/补光</el-checkbox>
          </div>
        </el-form-item>
        <el-divider content-position="left">台式电脑 / WOL / 定时关机联动</el-divider>
        <el-form-item label="启用 PC 电源与定时实体"><el-switch v-model="editDlg.form.pc_enabled" /></el-form-item>
        <template v-if="editDlg.form.pc_enabled">
          <el-form-item label="电脑实体名称"><el-input v-model="editDlg.form.pc_name" placeholder="台式电脑" /></el-form-item>
          <el-form-item label="PC 局域网 IP (Ping)"><el-input v-model="editDlg.form.pc_ip" placeholder="例如 192.168.68.159" /></el-form-item>
          <el-form-item label="PC 网卡 MAC (WOL)"><el-input v-model="editDlg.form.pc_mac" placeholder="00:E2:69:9C:9A:35" /></el-form-item>
          <el-form-item label="WOL 广播地址"><el-input v-model="editDlg.form.pc_broadcast_ip" /></el-form-item>
          <el-form-item label="目标系统类型">
            <el-select v-model="editDlg.form.pc_os" style="width: 100%">
              <el-option label="Windows (Win+X U U 安全关机)" value="windows" />
              <el-option label="macOS / Linux (Consumer Power)" value="linux" />
            </el-select>
          </el-form-item>
          <el-form-item label="主板 Power SW GPIO">
            <div style="display: flex; gap: 10px; align-items: center">
              <el-input-number v-model="editDlg.form.pc_power_pin" :min="0" :max="48" placeholder="可选" />
              <span style="font-size: 12px; opacity: 0.75">脉冲(ms)</span>
              <el-input-number v-model="editDlg.form.pc_power_pulse_ms" :min="100" :max="5000" :step="100" />
            </div>
          </el-form-item>
          <el-form-item label="兼容旧主题前缀">
            <el-input v-model="editDlg.form.legacy_topic_prefix" placeholder="可选，如填入 pc_ctrl 以兼容既有小爱自动化" />
          </el-form-item>
        </template>
      </el-form>
      <template #footer>
        <el-button @click="editDlg.visible = false">取消</el-button>
        <el-button type="primary" :loading="editDlg.saving" @click="saveTarget">保存并同步实体</el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="yamlDlg.visible" :title="`Home Assistant 小爱语音自动化 YAML (${yamlDlg.device_uuid})`" width="640px">
      <pre class="yaml-box">{{ yamlDlg.yaml }}</pre>
      <template #footer>
        <el-button @click="yamlDlg.visible = false">关闭</el-button>
        <el-button type="primary" @click="copyYaml">复制 YAML</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<style scoped>
.ha-page { display: flex; flex-direction: column; gap: 16px; }
.header-bar { display: flex; justify-content: space-between; align-items: center; padding: 16px 20px; border-radius: 14px; flex-wrap: wrap; gap: 12px; }
.page-title { margin: 0; font-size: 20px; display: flex; align-items: center; gap: 8px; }
.page-sub { margin: 4px 0 0; font-size: 13px; opacity: 0.75; }
.header-stats { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.panel-box { padding: 18px; border-radius: 14px; }
.form-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(310px, 1fr)); gap: 4px 16px; }
.yaml-box { background: #0b1120; color: #cbd5e1; padding: 14px; border-radius: 10px; font-size: 12px; overflow: auto; max-height: 360px; white-space: pre-wrap; }
</style>
