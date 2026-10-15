<script>
export const meta = { title: '设备组管理' }
</script>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { deviceGroupsApi } from '@/api/device-groups'
import { devicesApi } from '@/api/devices'

const groups = ref([])
const allDevices = ref([])
const onlineDeviceUuids = ref([])
const loading = ref(false)

const createDialog = reactive({ visible: false, name: '' })
const editDialog = reactive({ visible: false, id: null, name: '' })
const devicesDialog = reactive({ visible: false, groupId: null, groupName: '', deviceIds: [] })
const addDevicesDialog = reactive({ visible: false, groupId: null, selectedIds: [] })
const dispatchDialog = reactive({ visible: false, groupId: null, groupName: '', payload: '', timeout: 3000, preset: 'enter' })
const resultsDialog = reactive({ visible: false, results: [], summary: {} })

const DISPATCH_PRESETS = [
  { id: 'enter', label: '敲击 Enter 回车', payload: { type: 'keyboard_tap', key: 'KEY_ENTER', need_ack: true } },
  { id: 'win_d', label: '切换桌面 (Win+D)', payload: { type: 'keyboard_tap', key: 'KEY_GUI_D', need_ack: true } },
  { id: 'space', label: '敲击 Space 空格', payload: { type: 'keyboard_tap', key: 'KEY_SPACE', need_ack: true } },
  { id: 'text', label: '广播问候文本', payload: { type: 'text', content: 'Broadcast from HID Gateway', interval_ms: 10, need_ack: true } },
  { id: 'click', label: '模拟鼠标左键', payload: { type: 'mouse_click', button: 1, need_ack: true } },
  { id: 'custom', label: '自定义 JSON', payload: { type: 'custom', data: {} } },
]

async function loadGroups() {
  loading.value = true
  try {
    groups.value = await deviceGroupsApi.list()
  } finally {
    loading.value = false
  }
}

async function loadAllDevices() {
  try {
    const [devs, on] = await Promise.all([devicesApi.list(), devicesApi.online()])
    allDevices.value = devs
    onlineDeviceUuids.value = on.online || []
  } catch {
    allDevices.value = []
    onlineDeviceUuids.value = []
  }
}

// KPI Stats
const totalGroupsCount = computed(() => groups.value.length)
const totalDevicesCount = computed(() => allDevices.value.length)
const onlineDevicesCount = computed(() => allDevices.value.filter(d => onlineDeviceUuids.value.includes(d.device_uuid)).length)

async function createGroup() {
  if (!createDialog.name.trim()) return ElMessage.warning('请输入分组名称')
  await deviceGroupsApi.create({ name: createDialog.name.trim() })
  ElMessage.success('设备组创建成功')
  createDialog.visible = false
  createDialog.name = ''
  loadGroups()
}

function openEdit(row) {
  editDialog.id = row.id
  editDialog.name = row.name
  editDialog.visible = true
}

async function submitEdit() {
  if (!editDialog.name.trim()) return ElMessage.warning('分组名称不能为空')
  await deviceGroupsApi.update(editDialog.id, { name: editDialog.name.trim() })
  ElMessage.success('分组名称更新成功')
  editDialog.visible = false
  loadGroups()
}

async function deleteGroup(row) {
  await ElMessageBox.confirm(`确认删除分组「${row.name}」？分组内的设备不会被删除。`, '删除分组', { type: 'warning' })
  await deviceGroupsApi.delete(row.id)
  ElMessage.success('分组删除成功')
  loadGroups()
}

async function viewDevices(row) {
  const res = await deviceGroupsApi.getDevices(row.id)
  devicesDialog.groupId = row.id
  devicesDialog.groupName = row.name
  devicesDialog.deviceIds = res.devices || []
  devicesDialog.visible = true
}

function openAddDevices(row) {
  addDevicesDialog.groupId = row.id
  addDevicesDialog.selectedIds = []
  addDevicesDialog.visible = true
}

async function submitAddDevices() {
  if (addDevicesDialog.selectedIds.length === 0) return ElMessage.warning('请勾选至少一个设备')
  await deviceGroupsApi.addDevices(addDevicesDialog.groupId, addDevicesDialog.selectedIds)
  ElMessage.success('设备已成功加入分组')
  addDevicesDialog.visible = false
  if (devicesDialog.visible) viewDevices({ id: devicesDialog.groupId, name: devicesDialog.groupName })
}

async function removeDeviceFromGroup(deviceId) {
  await deviceGroupsApi.removeDevice(devicesDialog.groupId, deviceId)
  ElMessage.success('设备已从该分组移除')
  viewDevices({ id: devicesDialog.groupId, name: devicesDialog.groupName })
}

function openDispatch(row) {
  dispatchDialog.groupId = row.id
  dispatchDialog.groupName = row.name
  dispatchDialog.preset = 'enter'
  dispatchDialog.payload = JSON.stringify(DISPATCH_PRESETS[0].payload, null, 2)
  dispatchDialog.timeout = 3000
  dispatchDialog.visible = true
}

function onSelectPreset(preset) {
  dispatchDialog.preset = preset.id
  dispatchDialog.payload = JSON.stringify(preset.payload, null, 2)
}

async function submitDispatch() {
  try {
    const payload = JSON.parse(dispatchDialog.payload)
    const res = await deviceGroupsApi.dispatch(dispatchDialog.groupId, payload, dispatchDialog.timeout)
    dispatchDialog.visible = false
    resultsDialog.results = res.results || []
    resultsDialog.summary = res.summary || { total: 0, success: 0, failed: 0 }
    resultsDialog.visible = true
  } catch (e) {
    ElMessage.error('JSON 格式错误或下发网络异常')
  }
}

onMounted(() => {
  loadGroups()
  loadAllDevices()
})
</script>

<template>
  <div class="groups-container">
    <!-- Top Header -->
    <div class="groups-header glass-panel">
      <div class="header-left">
        <div class="header-icon-badge">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2"/>
            <circle cx="9" cy="7" r="4"/>
            <path d="M23 21v-2a4 4 0 0 0-3-3.87"/>
            <path d="M16 3.13a4 4 0 0 1 0 7.75"/>
          </svg>
        </div>
        <div>
          <h1 class="header-title">设备组协同中枢</h1>
          <p class="header-subtitle">Device Clustering, Synchronous Broadcast & Batch Dispatching</p>
        </div>
      </div>

      <div class="header-actions">
        <el-button class="refresh-btn" :loading="loading" @click="loadGroups">
          <svg class="btn-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/>
          </svg>
          刷新
        </el-button>
        <el-button type="primary" class="create-btn" @click="createDialog.visible = true">
          <svg class="btn-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <line x1="12" y1="5" x2="12" y2="19"/>
            <line x1="5" y1="12" x2="19" y2="12"/>
          </svg>
          新建分组
        </el-button>
      </div>
    </div>

    <!-- KPI Row -->
    <div class="kpi-grid">
      <div class="kpi-card glass-panel">
        <div class="kpi-label">已编排设备组</div>
        <div class="kpi-value text-blue">{{ totalGroupsCount }}</div>
        <div class="kpi-sub">支持按业务/机房划分组别</div>
      </div>
      <div class="kpi-card glass-panel">
        <div class="kpi-label">全网接入设备</div>
        <div class="kpi-value text-purple">{{ totalDevicesCount }}</div>
        <div class="kpi-sub">受控 ESP32 硬件终端</div>
      </div>
      <div class="kpi-card glass-panel">
        <div class="kpi-label">当前活跃在线</div>
        <div class="kpi-value text-green">{{ onlineDevicesCount }}</div>
        <div class="kpi-sub">WebSocket 长连接正常</div>
      </div>
    </div>

    <!-- Groups Table Card -->
    <div class="table-card glass-panel" v-loading="loading">
      <el-empty v-if="!groups.length && !loading" description="暂无设备分组，点击上方新建分组开始协同调度" :image-size="80" />
      <el-table v-else :data="groups" size="default" class="modern-group-table">
        <el-table-column prop="id" label="分组 ID" width="90">
          <template #default="{ row }">
            <span class="group-id-badge">#{{ row.id }}</span>
          </template>
        </el-table-column>

        <el-table-column prop="name" label="分组名称" min-width="180">
          <template #default="{ row }">
            <div class="group-name-cell">
              <span class="group-name-text">{{ row.name }}</span>
            </div>
          </template>
        </el-table-column>

        <el-table-column prop="created_at" label="创建时间" width="200">
          <template #default="{ row }">
            <span class="timestamp-text">{{ row.created_at ? new Date(row.created_at).toLocaleString() : '-' }}</span>
          </template>
        </el-table-column>

        <el-table-column label="协同操作" width="380" fixed="right">
          <template #default="{ row }">
            <div class="table-action-group">
              <el-button size="small" link type="primary" @click="viewDevices(row)">
                查看成员
              </el-button>
              <el-button size="small" link type="primary" @click="openAddDevices(row)">
                分配设备
              </el-button>
              <el-button size="small" link class="dispatch-link-btn" @click="openDispatch(row)">
                ⚡ 批量下发
              </el-button>
              <el-button size="small" link @click="openEdit(row)">
                编辑
              </el-button>
              <el-button size="small" link type="danger" @click="deleteGroup(row)">
                删除
              </el-button>
            </div>
          </template>
        </el-table-column>
      </el-table>
    </div>

    <!-- Dialog: Create Group -->
    <el-dialog v-model="createDialog.visible" title="新建设备协同组" width="440px" class="modern-dialog">
      <div class="dialog-content-box">
        <div class="field-label">分组名称</div>
        <el-input v-model="createDialog.name" placeholder="例如：测试机房 A 组、多机联动编组" size="large" @keyup.enter="createGroup" />
      </div>
      <template #footer>
        <el-button @click="createDialog.visible = false">取消</el-button>
        <el-button type="primary" @click="createGroup">确认创建</el-button>
      </template>
    </el-dialog>

    <!-- Dialog: Edit Group -->
    <el-dialog v-model="editDialog.visible" title="编辑设备组名称" width="440px" class="modern-dialog">
      <div class="dialog-content-box">
        <div class="field-label">分组名称</div>
        <el-input v-model="editDialog.name" placeholder="输入新的分组名称" size="large" @keyup.enter="submitEdit" />
      </div>
      <template #footer>
        <el-button @click="editDialog.visible = false">取消</el-button>
        <el-button type="primary" @click="submitEdit">保存修改</el-button>
      </template>
    </el-dialog>

    <!-- Dialog: View Devices in Group -->
    <el-dialog v-model="devicesDialog.visible" :title="`组内成员设备 · ${devicesDialog.groupName}`" width="680px" class="modern-dialog">
      <div class="dialog-table-wrap">
        <el-empty
          v-if="!allDevices.filter(d => devicesDialog.deviceIds.includes(d.id)).length"
          description="该分组当前暂无关联设备，请点击“分配设备”添加"
          :image-size="60"
        />
        <el-table v-else :data="allDevices.filter(d => devicesDialog.deviceIds.includes(d.id))" size="small">
          <el-table-column prop="id" label="ID" width="60" />
          <el-table-column prop="name" label="设备名称" />
          <el-table-column prop="device_uuid" label="UUID" />
          <el-table-column label="在线状态" width="100">
            <template #default="{ row }">
              <span class="status-pill" :class="onlineDeviceUuids.includes(row.device_uuid) ? 'pill-on' : 'pill-off'">
                {{ onlineDeviceUuids.includes(row.device_uuid) ? '● 在线' : '○ 离线' }}
              </span>
            </template>
          </el-table-column>
          <el-table-column label="操作" width="90">
            <template #default="{ row }">
              <el-button size="small" type="danger" link @click="removeDeviceFromGroup(row.id)">移出分组</el-button>
            </template>
          </el-table-column>
        </el-table>
      </div>
    </el-dialog>

    <!-- Dialog: Add Devices to Group -->
    <el-dialog v-model="addDevicesDialog.visible" title="批量添加设备至分组" width="640px" class="modern-dialog">
      <div class="add-devices-selection-list">
        <el-checkbox-group v-model="addDevicesDialog.selectedIds">
          <div
            v-for="dev in allDevices"
            :key="dev.id"
            class="device-checkbox-card"
            :class="{ active: addDevicesDialog.selectedIds.includes(dev.id) }"
          >
            <el-checkbox :label="dev.id" :value="dev.id">
              <div class="dev-check-meta">
                <span class="dev-check-name">{{ dev.name || '未命名设备' }}</span>
                <span class="dev-check-uuid">UUID: {{ dev.device_uuid }}</span>
                <span
                  class="dev-check-status"
                  :class="onlineDeviceUuids.includes(dev.device_uuid) ? 'status-on' : 'status-off'"
                >
                  {{ onlineDeviceUuids.includes(dev.device_uuid) ? '在线' : '离线' }}
                </span>
              </div>
            </el-checkbox>
          </div>
        </el-checkbox-group>
      </div>
      <template #footer>
        <el-button @click="addDevicesDialog.visible = false">取消</el-button>
        <el-button type="primary" @click="submitAddDevices">确认添加 ({{ addDevicesDialog.selectedIds.length }})</el-button>
      </template>
    </el-dialog>

    <!-- Dialog: Batch Dispatch -->
    <el-dialog v-model="dispatchDialog.visible" :title="`批量指令下发 · ${dispatchDialog.groupName}`" width="700px" class="modern-dialog">
      <div class="dispatch-form-body">
        <div class="preset-strip">
          <span class="preset-label">动作快捷模板:</span>
          <button
            v-for="p in DISPATCH_PRESETS"
            :key="p.id"
            class="preset-pill-btn"
            :class="{ active: dispatchDialog.preset === p.id }"
            @click="onSelectPreset(p)"
          >
            {{ p.label }}
          </button>
        </div>

        <div class="field-label" style="margin-top: 14px">下发 Payload (JSON)</div>
        <el-input
          v-model="dispatchDialog.payload"
          type="textarea"
          :rows="8"
          class="json-editor-textarea"
          placeholder="请输入标准动作 JSON 载荷"
        />

        <div class="dispatch-timeout-row">
          <span class="field-label">下发超时时间 (ms):</span>
          <el-input-number v-model="dispatchDialog.timeout" :min="500" :max="15000" :step="500" controls-position="right" />
        </div>
      </div>
      <template #footer>
        <el-button @click="dispatchDialog.visible = false">取消</el-button>
        <el-button type="primary" @click="submitDispatch">立即全组广播下发</el-button>
      </template>
    </el-dialog>

    <!-- Dialog: Batch Results -->
    <el-dialog v-model="resultsDialog.visible" title="批量下发执行结果报告" width="720px" class="modern-dialog">
      <div class="results-report-body">
        <div class="report-summary-banner">
          <div class="summary-pill total">
            总下发设备: <strong>{{ resultsDialog.summary?.total ?? 0 }}</strong>
          </div>
          <div class="summary-pill success">
            成功 ACK: <strong>{{ resultsDialog.summary?.success ?? 0 }}</strong>
          </div>
          <div class="summary-pill failed">
            失败: <strong>{{ resultsDialog.summary?.failed ?? 0 }}</strong>
          </div>
        </div>

        <el-table :data="resultsDialog.results" max-height="360" size="small" class="report-table">
          <el-table-column prop="device_id" label="设备 UUID" min-width="180" />
          <el-table-column label="执行状态" width="110">
            <template #default="{ row }">
              <span
                class="status-pill"
                :class="{
                  'pill-acked': row.status === 'acked',
                  'pill-failed': row.status !== 'acked' && row.status !== 'queued',
                  'pill-queued': row.status === 'queued'
                }"
              >
                {{ row.status }}
              </span>
            </template>
          </el-table-column>
          <el-table-column label="耗时" width="90">
            <template #default="{ row }">
              <span v-if="row.exec_time_ms != null">{{ row.exec_time_ms }} ms</span>
              <span v-else class="text-muted">—</span>
            </template>
          </el-table-column>
          <el-table-column label="详情 / 错误" min-width="160">
            <template #default="{ row }">
              <span v-if="row.error_message" class="text-danger">{{ row.error_message }}</span>
              <span v-else class="text-muted">正常完成</span>
            </template>
          </el-table-column>
        </el-table>
      </div>
      <template #footer>
        <el-button type="primary" @click="resultsDialog.visible = false">完成</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<style scoped>
.groups-container {
  display: flex;
  flex-direction: column;
  gap: 20px;
  width: 100%;
}

/* Header */
.groups-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 18px 24px;
  border-radius: var(--radius-lg);
  flex-wrap: wrap;
  gap: 16px;
}

.header-left {
  display: flex;
  align-items: center;
  gap: 14px;
}

.header-icon-badge {
  width: 44px;
  height: 44px;
  border-radius: var(--radius-md);
  background: linear-gradient(135deg, rgba(59, 130, 246, 0.2), rgba(168, 85, 247, 0.2));
  border: 1px solid rgba(59, 130, 246, 0.3);
  display: flex;
  align-items: center;
  justify-content: center;
}

.header-icon-badge svg {
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

.create-btn {
  display: flex;
  align-items: center;
  gap: 6px;
  border-radius: var(--radius-md);
}

.btn-svg {
  width: 14px;
  height: 14px;
}

/* KPI Row */
.kpi-grid {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 16px;
}

@media (max-width: 860px) {
  .kpi-grid {
    grid-template-columns: 1fr;
  }
}

.kpi-card {
  padding: 16px 20px;
  border-radius: var(--radius-lg);
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.kpi-label {
  font-size: 11px;
  font-weight: 600;
  text-transform: uppercase;
  color: var(--color-text-muted);
}

.kpi-value {
  font-size: 26px;
  font-weight: 800;
  color: var(--color-text-primary);
  line-height: 1.2;
}

.kpi-sub {
  font-size: 11px;
  color: var(--color-text-secondary);
}

.text-blue { color: #38bdf8; }
.text-purple { color: #c084fc; }
.text-green { color: #4ade80; }
.text-muted { color: #94a3b8; }
.text-danger { color: #f87171; }

/* Table Card */
.table-card {
  padding: 8px;
  border-radius: var(--radius-lg);
  overflow: hidden;
}

.modern-group-table {
  background: transparent !important;
}

.modern-group-table :deep(.el-table__header-wrapper th) {
  background: var(--color-bg-base) !important;
  color: var(--color-text-muted);
  font-weight: 600;
  font-size: 12px;
}

.group-id-badge {
  font-size: 12px;
  font-family: monospace;
  color: var(--color-text-muted);
}

.group-name-cell {
  display: flex;
  align-items: center;
  gap: 8px;
}

.group-name-text {
  font-weight: 700;
  color: var(--color-text-primary);
  font-size: 14px;
}

.timestamp-text {
  font-size: 12px;
  color: var(--color-text-muted);
}

.table-action-group {
  display: flex;
  align-items: center;
  gap: 10px;
}

.dispatch-link-btn {
  color: #f59e0b !important;
  font-weight: 600;
}

/* Dialogs */
.dialog-content-box {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.field-label {
  font-size: 12px;
  font-weight: 600;
  color: var(--color-text-secondary);
}

.dialog-table-wrap {
  min-height: 160px;
}

.status-pill {
  font-size: 11px;
  font-weight: 600;
  padding: 2px 8px;
  border-radius: var(--radius-pill);
}

.pill-on {
  color: #10b981;
  background: rgba(16, 185, 129, 0.1);
}

.pill-off {
  color: #94a3b8;
  background: rgba(148, 163, 184, 0.1);
}

.pill-acked {
  color: #4ade80;
  background: rgba(34, 197, 94, 0.15);
}

.pill-failed {
  color: #f87171;
  background: rgba(239, 68, 68, 0.15);
}

.pill-queued {
  color: #38bdf8;
  background: rgba(56, 189, 248, 0.15);
}

/* Add Devices Selection */
.add-devices-selection-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
  max-height: 380px;
  overflow-y: auto;
}

.device-checkbox-card {
  padding: 10px 14px;
  border-radius: var(--radius-md);
  border: 1px solid var(--color-border);
  background: var(--color-bg-card);
  transition: all 0.2s;
}

.device-checkbox-card.active {
  border-color: var(--color-primary);
  background: rgba(59, 130, 246, 0.05);
}

.dev-check-meta {
  display: flex;
  align-items: center;
  gap: 12px;
  font-size: 13px;
}

.dev-check-name {
  font-weight: 700;
  color: var(--color-text-primary);
}

.dev-check-uuid {
  font-size: 11px;
  font-family: monospace;
  color: var(--color-text-muted);
}

.dev-check-status {
  font-size: 11px;
  padding: 2px 6px;
  border-radius: 4px;
}

.status-on {
  color: #10b981;
  background: rgba(16, 185, 129, 0.1);
}

.status-off {
  color: #94a3b8;
  background: rgba(148, 163, 184, 0.1);
}

/* Dispatch Form */
.dispatch-form-body {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.preset-strip {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.preset-label {
  font-size: 12px;
  font-weight: 600;
  color: var(--color-text-muted);
}

.preset-pill-btn {
  padding: 4px 10px;
  border-radius: var(--radius-pill);
  border: 1px solid var(--color-border);
  background: var(--color-bg-card);
  color: var(--color-text-secondary);
  font-size: 11px;
  cursor: pointer;
  transition: all 0.2s;
}

.preset-pill-btn:hover {
  border-color: var(--color-primary);
  color: var(--color-primary);
}

.preset-pill-btn.active {
  background: linear-gradient(135deg, rgba(59, 130, 246, 0.2), rgba(139, 92, 246, 0.2));
  border-color: var(--color-primary);
  color: var(--color-primary-glow);
  font-weight: 600;
}

.json-editor-textarea :deep(textarea) {
  font-family: 'JetBrains Mono', Consolas, Monaco, monospace;
  font-size: 12px;
  background: rgba(15, 23, 42, 0.8);
  color: #38bdf8;
  border-radius: var(--radius-sm);
}

.dispatch-timeout-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
}

/* Results Report */
.results-report-body {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.report-summary-banner {
  display: flex;
  gap: 12px;
}

.summary-pill {
  padding: 8px 16px;
  border-radius: var(--radius-md);
  font-size: 13px;
  flex: 1;
  text-align: center;
}

.summary-pill.total {
  background: rgba(56, 189, 248, 0.1);
  color: #38bdf8;
}

.summary-pill.success {
  background: rgba(34, 197, 94, 0.1);
  color: #4ade80;
}

.summary-pill.failed {
  background: rgba(239, 68, 68, 0.1);
  color: #f87171;
}
</style>
