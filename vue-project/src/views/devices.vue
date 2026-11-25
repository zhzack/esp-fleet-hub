<script>
export const meta = { title: '设备管理' }
</script>

<script setup>
import { computed, onMounted, onBeforeUnmount, reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import {
  Cpu, Plus, Refresh, Search, Aim, VideoCamera, Edit, Key,
  Grid, List, Check, Connection, Delete,
} from '@element-plus/icons-vue'
import { devicesApi } from '@/api/devices'
import { getCapabilityFlags } from '@/utils/capabilities'
import DeviceControl from './device-control.vue'

const router = useRouter()

const rows = ref([])
const online = ref([])
const loading = ref(false)
const searchQuery = ref('')
const filterTab = ref('all') // 'all' | 'online' | 'offline' | 'claimed'
const viewMode = ref('card') // 'card' | 'table'

const dialog = reactive({
  visible: false,
  form: { device_uuid: '', name: '', capabilities: ['usb_keyboard', 'usb_mouse', 'ble_keyboard', 'ble_mouse'] },
})

const editDialog = reactive({
  visible: false,
  form: { id: null, name: '', capabilities: [], owner_user_id: null },
})

const control = reactive({ visible: false, deviceUuid: '', deviceName: '', capabilities: [] })

const capOptions = [
  { label: 'USB 键盘', value: 'usb_keyboard' },
  { label: 'USB 鼠标', value: 'usb_mouse' },
  { label: 'BLE 蓝牙键盘', value: 'ble_keyboard' },
  { label: 'BLE 蓝牙鼠标', value: 'ble_mouse' },
  { label: 'GPIO 控制', value: 'gpio' },
  { label: '屏幕显示', value: 'display' },
  { label: 'PC 监控', value: 'pc_monitor' },
  { label: '5向摇杆', value: 'joystick' },
  { label: '摄像头', value: 'camera' },
  { label: 'OTA 升级', value: 'ota' },
]

function canTrackpad(row) {
  return getCapabilityFlags(row?.capabilities).supportsTrackpad
}

function isOnline(uuid) {
  return online.value.includes(uuid)
}

const filteredDevices = computed(() => {
  let list = rows.value
  if (searchQuery.value.trim()) {
    const q = searchQuery.value.trim().toLowerCase()
    list = list.filter((item) =>
      (item.name || '').toLowerCase().includes(q) ||
      (item.device_uuid || '').toLowerCase().includes(q)
    )
  }
  if (filterTab.value === 'online') {
    list = list.filter((item) => isOnline(item.device_uuid))
  } else if (filterTab.value === 'offline') {
    list = list.filter((item) => !isOnline(item.device_uuid))
  } else if (filterTab.value === 'claimed') {
    list = list.filter((item) => !!item.owner_user_id)
  }
  return list
})

async function load() {
  loading.value = true
  try {
    const [list, on] = await Promise.all([devicesApi.list(), devicesApi.online()])
    rows.value = list || []
    online.value = on?.online || []
  } finally {
    loading.value = false
  }
}

async function silentRefresh() {
  try {
    const [list, on] = await Promise.all([devicesApi.list(), devicesApi.online()])
    rows.value = list || []
    online.value = on?.online || []
  } catch {
    // ignore
  }
}

let pollTimer = null

onMounted(() => {
  load()
  pollTimer = setInterval(silentRefresh, 5000)
})

onBeforeUnmount(() => {
  if (pollTimer) clearInterval(pollTimer)
})

function openControl(row) {
  control.deviceUuid = row.device_uuid
  control.deviceName = row.name || row.device_uuid
  control.capabilities = Array.isArray(row.capabilities) ? [...row.capabilities] : []
  control.visible = true
}

function openTrackpad(row) {
  router.push({ path: '/trackpad', query: { device: row.device_uuid } })
}

async function submit() {
  const uuid = dialog.form.device_uuid.trim()
  if (!uuid) return ElMessage.warning('设备 UUID 为必填项')
  try {
    await devicesApi.create(dialog.form)
    ElMessage.success('设备添加成功')
    dialog.visible = false
    dialog.form = { device_uuid: '', name: '', capabilities: ['usb_keyboard', 'usb_mouse', 'ble_keyboard', 'ble_mouse'] }
    load()
  } catch (err) {
    ElMessage.error(err.response?.data?.detail || err.message || '添加失败')
  }
}

function openEdit(row) {
  editDialog.form = {
    id: row.id,
    name: row.name,
    capabilities: [...(row.capabilities || [])],
    owner_user_id: row.owner_user_id,
  }
  editDialog.visible = true
}

async function submitEdit() {
  try {
    await devicesApi.update(editDialog.form.id, {
      name: editDialog.form.name,
      capabilities: editDialog.form.capabilities,
    })
    ElMessage.success('更新设备成功')
    editDialog.visible = false
    load()
  } catch (err) {
    ElMessage.error(err.response?.data?.detail || err.message || '更新失败')
  }
}

async function claimDevice(row) {
  try {
    await devicesApi.claim(row.id)
    ElMessage.success('设备认领成功')
    load()
  } catch (err) {
    ElMessage.error(err.response?.data?.detail || err.message || '认领失败')
  }
}
</script>

<template>
  <div class="devices-page">
    <!-- Top Action Bar -->
    <div class="devices-toolbar">
      <div class="toolbar-left">
        <el-radio-group v-model="filterTab" size="default" class="filter-tabs">
          <el-radio-button label="all">全部 ({{ rows.length }})</el-radio-button>
          <el-radio-button label="online">在线 ({{ online.length }})</el-radio-button>
          <el-radio-button label="offline">离线 ({{ rows.length - online.length }})</el-radio-button>
          <el-radio-button label="claimed">已认领</el-radio-button>
        </el-radio-group>

        <el-input
          v-model="searchQuery"
          :prefix-icon="Search"
          placeholder="搜索设备名称 / UUID..."
          clearable
          style="width: 260px"
        />
      </div>

      <div class="toolbar-right">
        <el-button-group class="view-toggle">
          <el-button
            :type="viewMode === 'card' ? 'primary' : 'default'"
            :icon="Grid"
            @click="viewMode = 'card'"
            title="卡片视图"
          />
          <el-button
            :type="viewMode === 'table' ? 'primary' : 'default'"
            :icon="List"
            @click="viewMode = 'table'"
            title="列表视图"
          />
        </el-button-group>

        <el-button :icon="Refresh" circle :loading="loading" @click="load" title="刷新列表" />
        <el-button type="primary" :icon="Plus" @click="dialog.visible = true">添加设备</el-button>
      </div>
    </div>

    <!-- Card Grid View -->
    <div v-if="viewMode === 'card'" class="device-card-grid" v-loading="loading">
      <div v-if="!filteredDevices.length" class="empty-panel">
        <el-icon :size="48" color="#94a3b8"><Cpu /></el-icon>
        <p>未找到符合条件的设备</p>
        <el-button type="primary" size="small" @click="dialog.visible = true">立即添加新设备</el-button>
      </div>

      <div
        v-for="d in filteredDevices"
        :key="d.device_uuid"
        class="device-card"
        :class="{ 'is-online': isOnline(d.device_uuid) }"
      >
        <!-- Card Header -->
        <div class="card-header">
          <div class="card-title-group">
            <div class="device-avatar" :class="isOnline(d.device_uuid) ? 'online' : 'offline'">
              <el-icon><Cpu /></el-icon>
            </div>
            <div class="title-meta">
              <span class="device-name">{{ d.name || 'ESP32 Device' }}</span>
              <span class="device-uuid">{{ d.device_uuid }}</span>
            </div>
          </div>

          <div class="status-badge" :class="isOnline(d.device_uuid) ? 'online' : 'offline'">
            <span class="status-dot" :class="isOnline(d.device_uuid) ? 'online' : 'offline'"></span>
            <span>{{ isOnline(d.device_uuid) ? '在线' : '离线' }}</span>
          </div>
        </div>

        <!-- Capabilities tags -->
        <div class="capabilities-row">
          <span class="cap-label">能力:</span>
          <div class="cap-tags">
            <el-tag
              v-for="c in (d.capabilities || [])"
              :key="c"
              size="small"
              class="cap-tag"
              effect="plain"
            >
              {{ c.replace(/_/g, ' ') }}
            </el-tag>
            <span v-if="!d.capabilities?.length" class="no-cap">未配置</span>
          </div>
        </div>

        <!-- Card Meta Details -->
        <div class="card-details">
          <div class="detail-item">
            <span class="item-label">设备归属:</span>
            <el-tag v-if="d.owner_user_id" type="success" size="small" effect="light">已认领</el-tag>
            <el-tag v-else type="info" size="small" effect="light">公共/待认领</el-tag>
          </div>
          <div class="detail-item">
            <span class="item-label">最后活跃:</span>
            <span class="item-val">{{ d.last_online_at || '未曾在线' }}</span>
          </div>
        </div>

        <!-- Card Footer Actions -->
        <div class="card-footer">
          <div class="footer-left">
            <el-button
              v-if="!d.owner_user_id"
              size="small"
              type="primary"
              plain
              @click="claimDevice(d)"
            >
              认领
            </el-button>
            <el-button size="small" @click="openEdit(d)">编辑</el-button>
          </div>

          <div class="footer-right">
            <el-button
              size="small"
              type="success"
              plain
              :icon="Aim"
              :disabled="!isOnline(d.device_uuid) || !canTrackpad(d)"
              :title="canTrackpad(d) ? '打开虚拟触控板' : '当前设备未启用 HID 键鼠能力'"
              @click="openTrackpad(d)"
            >
              触控
            </el-button>
            <el-button
              size="small"
              type="primary"
              :disabled="!isOnline(d.device_uuid)"
              @click="openControl(d)"
            >
              控制
            </el-button>
          </div>
        </div>
      </div>
    </div>

    <!-- Table View -->
    <el-card v-else shadow="never" class="table-card">
      <el-table :data="filteredDevices" v-loading="loading" class="device-table">
        <el-table-column prop="id" label="ID" width="70" />
        <el-table-column label="状态" width="100">
          <template #default="{ row }">
            <div class="table-status-cell">
              <span class="status-dot" :class="isOnline(row.device_uuid) ? 'online' : 'offline'"></span>
              <span>{{ isOnline(row.device_uuid) ? '在线' : '离线' }}</span>
            </div>
          </template>
        </el-table-column>
        <el-table-column prop="name" label="设备名称" min-width="140">
          <template #default="{ row }">
            <strong>{{ row.name || '未命名' }}</strong>
          </template>
        </el-table-column>
        <el-table-column prop="device_uuid" label="设备 UUID" min-width="180">
          <template #default="{ row }">
            <span class="table-mono">{{ row.device_uuid }}</span>
          </template>
        </el-table-column>
        <el-table-column label="能力特性" min-width="220">
          <template #default="{ row }">
            <el-tag
              v-for="c in (row.capabilities || [])"
              :key="c"
              size="small"
              style="margin-right: 4px; margin-bottom: 2px"
            >
              {{ c }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="归属" width="100">
          <template #default="{ row }">
            <el-tag v-if="row.owner_user_id" type="success" size="small">已认领</el-tag>
            <el-tag v-else type="info" size="small">待认领</el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="last_online_at" label="最后在线" width="170" />
        <el-table-column label="操作" width="280" fixed="right">
          <template #default="{ row }">
            <el-button v-if="!row.owner_user_id" size="small" type="primary" plain @click="claimDevice(row)">认领</el-button>
            <el-button size="small" @click="openEdit(row)">编辑</el-button>
            <el-button size="small" type="success" plain :disabled="!isOnline(row.device_uuid) || !canTrackpad(row)" :title="canTrackpad(row) ? '打开虚拟触控板' : '当前设备未启用 HID 键鼠能力'" @click="openTrackpad(row)">触控</el-button>
            <el-button size="small" type="primary" :disabled="!isOnline(row.device_uuid)" @click="openControl(row)">控制</el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <!-- Dialog: Add Device -->
    <el-dialog v-model="dialog.visible" title="添加新 ESP32 设备" width="500px">
      <el-form label-position="top">
        <el-form-item label="设备唯一标识 UUID (与固件 NVS 对齐)">
          <el-input v-model="dialog.form.device_uuid" placeholder="例如: esp32_test 或 84F70308A5E0" />
        </el-form-item>
        <el-form-item label="设备友好名称">
          <el-input v-model="dialog.form.name" placeholder="例如: 客厅电脑主控板" />
        </el-form-item>
        <el-form-item label="支持能力特性">
          <el-checkbox-group v-model="dialog.form.capabilities">
            <el-checkbox
              v-for="c in capOptions"
              :key="c.value"
              :label="c.value"
            >
              {{ c.label }}
            </el-checkbox>
          </el-checkbox-group>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="dialog.visible = false">取消</el-button>
        <el-button type="primary" @click="submit">确认添加</el-button>
      </template>
    </el-dialog>

    <!-- Dialog: Edit Device -->
    <el-dialog v-model="editDialog.visible" title="编辑设备配置" width="500px">
      <el-form label-position="top">
        <el-form-item label="设备名称">
          <el-input v-model="editDialog.form.name" />
        </el-form-item>
        <el-form-item label="能力特性">
          <el-checkbox-group v-model="editDialog.form.capabilities">
            <el-checkbox
              v-for="c in capOptions"
              :key="c.value"
              :label="c.value"
            >
              {{ c.label }}
            </el-checkbox>
          </el-checkbox-group>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="editDialog.visible = false">取消</el-button>
        <el-button type="primary" @click="submitEdit">保存更改</el-button>
      </template>
    </el-dialog>

    <DeviceControl
      v-model="control.visible"
      :device-uuid="control.deviceUuid"
      :device-name="control.deviceName"
      :capabilities="control.capabilities"
    />
  </div>
</template>

<style scoped>
.devices-page {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.devices-toolbar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  flex-wrap: wrap;
  gap: 12px;
  background: #ffffff;
  padding: 14px 18px;
  border-radius: var(--radius-md);
  border: 1px solid #e2e8f0;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.04);
}

.toolbar-left, .toolbar-right {
  display: flex;
  align-items: center;
  gap: 12px;
}

.view-toggle {
  margin-right: 4px;
}

/* Card Grid */
.device-card-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(330px, 1fr));
  gap: 16px;
}

.empty-panel {
  grid-column: 1 / -1;
  background: #ffffff;
  border-radius: var(--radius-md);
  padding: 48px;
  text-align: center;
  color: #64748b;
  border: 1px dashed #cbd5e1;
}

.empty-panel p {
  margin: 12px 0 16px;
  font-size: 15px;
}

.device-card {
  background: #ffffff;
  border-radius: var(--radius-md);
  border: 1px solid #e2e8f0;
  padding: 18px;
  display: flex;
  flex-direction: column;
  gap: 14px;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.03);
  transition: all 0.2s ease;
}

.device-card:hover {
  transform: translateY(-2px);
  box-shadow: 0 8px 20px -4px rgba(0, 0, 0, 0.08);
  border-color: #cbd5e1;
}

.device-card.is-online {
  border-top: 3px solid #10b981;
}

.card-header {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
}

.card-title-group {
  display: flex;
  align-items: center;
  gap: 12px;
}

.device-avatar {
  width: 42px;
  height: 42px;
  border-radius: 12px;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 20px;
}

.device-avatar.online { background: #ecfdf5; color: #10b981; }
.device-avatar.offline { background: #f1f5f9; color: #94a3b8; }

.title-meta {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.device-name {
  font-size: 15px;
  font-weight: 700;
  color: #0f172a;
}

.device-uuid {
  font-size: 11px;
  color: #94a3b8;
  font-family: var(--font-mono);
}

.status-badge {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  padding: 3px 8px;
  border-radius: 9999px;
  font-weight: 500;
}

.status-badge.online { background: #ecfdf5; color: #059669; }
.status-badge.offline { background: #f1f5f9; color: #94a3b8; }

.capabilities-row {
  display: flex;
  align-items: flex-start;
  gap: 8px;
  font-size: 12px;
}

.cap-label {
  color: #64748b;
  flex-shrink: 0;
  margin-top: 2px;
}

.cap-tags {
  display: flex;
  flex-wrap: wrap;
  gap: 4px;
}

.cap-tag {
  border-radius: 6px;
  font-size: 11px;
}

.no-cap {
  color: #94a3b8;
  font-style: italic;
}

.card-details {
  display: flex;
  flex-direction: column;
  gap: 6px;
  padding: 10px 12px;
  background: #f8fafc;
  border-radius: 8px;
  font-size: 12px;
}

.detail-item {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.item-label {
  color: #64748b;
}

.item-val {
  color: #1e293b;
  font-feature-settings: "tnum";
}

.card-footer {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding-top: 4px;
  border-top: 1px solid #f1f5f9;
}

.footer-left, .footer-right {
  display: flex;
  gap: 8px;
}

/* Table Card */
.table-card {
  border-radius: var(--radius-md);
  border: 1px solid #e2e8f0;
}

.table-status-cell {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
}

.table-mono {
  font-family: var(--font-mono);
  font-size: 12px;
  color: #475569;
}
</style>
