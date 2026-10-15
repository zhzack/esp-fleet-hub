<script>
export const meta = { title: '总览仪表盘' }
</script>

<script setup>
import { computed, onMounted, onBeforeUnmount, ref } from 'vue'
import { useRouter } from 'vue-router'
import {
  Cpu, Connection, CircleCheck, Refresh, Aim, VideoCamera, MagicStick,
  ArrowRight, Lightning, TopRight
} from '@element-plus/icons-vue'
import { devicesApi } from '@/api/devices'
import { actionsApi } from '@/api/actions'
import { useAuthStore } from '@/stores/auth'

const router = useRouter()
const auth = useAuthStore()

const devices = ref([])
const online = ref([])
const logs = ref([])
const loading = ref(false)
let timer = null

const onlineDevices = computed(() => {
  return devices.value.filter((d) => online.value.includes(d.device_uuid))
})

const successCount = computed(() => {
  return logs.value.filter((l) => l.status === 'acked' || l.status === 'ok').length
})

const successRate = computed(() => {
  if (!logs.value.length) return 100
  return Math.round((successCount.value / logs.value.length) * 100)
})

const avgLatency = computed(() => {
  const valid = logs.value.filter((l) => typeof l.exec_time_ms === 'number' && l.exec_time_ms > 0)
  if (!valid.length) return 0
  const sum = valid.reduce((acc, cur) => acc + cur.exec_time_ms, 0)
  return Math.round(sum / valid.length)
})

async function loadData() {
  loading.value = true
  try {
    const [devs, onRes, recentLogs] = await Promise.all([
      devicesApi.list(),
      devicesApi.online(),
      actionsApi.recentLogs(25),
    ])
    devices.value = devs || []
    online.value = onRes?.online || []
    logs.value = recentLogs || []
  } finally {
    loading.value = false
  }
}

function getActionTagType(type) {
  if (!type) return 'info'
  if (type.includes('keyboard')) return 'primary'
  if (type.includes('mouse')) return 'success'
  if (type.includes('macro')) return 'warning'
  return 'info'
}

onMounted(() => {
  loadData()
  timer = setInterval(loadData, 5000)
})

onBeforeUnmount(() => {
  if (timer) clearInterval(timer)
})
</script>

<template>
  <div class="dashboard-page">
    <!-- Welcome Header Banner -->
    <div class="welcome-banner">
      <div class="banner-left">
        <div class="greeting-row">
          <span class="greeting-hello">你好，{{ auth.user?.username || '开发者' }}</span>
          <el-tag size="small" effect="dark" type="primary" class="role-tag">
            {{ auth.user?.role === 'admin' ? '系统管理员' : '标准控制员' }}
          </el-tag>
        </div>
        <p class="banner-desc">
          HID Gateway 平台运行正常。所有下发指令将经由 ESP32 硬件直连模拟输出到受控宿主机。
        </p>
      </div>

      <div class="banner-actions">
        <el-button
          :icon="Refresh"
          circle
          :loading="loading"
          title="刷新数据"
          @click="loadData"
        />
        <el-button
          type="primary"
          class="quick-trackpad-btn"
          @click="router.push('/trackpad')"
        >
          <el-icon style="margin-right: 6px"><Aim /></el-icon>
          进入触控板
        </el-button>
      </div>
    </div>

    <!-- 4 Primary KPI Cards -->
    <div class="kpi-grid">
      <!-- Total Devices -->
      <div class="kpi-card">
        <div class="kpi-header">
          <span class="kpi-title">设备总数</span>
          <div class="kpi-icon-wrap blue">
            <el-icon><Cpu /></el-icon>
          </div>
        </div>
        <div class="kpi-value-row">
          <span class="kpi-number">{{ devices.length }}</span>
          <span class="kpi-sub">已注册节点</span>
        </div>
        <div class="kpi-footer">
          <span class="footer-link" @click="router.push('/devices')">
            管理设备列表 <el-icon><TopRight /></el-icon>
          </span>
        </div>
      </div>

      <!-- Online Devices -->
      <div class="kpi-card">
        <div class="kpi-header">
          <span class="kpi-title">在线硬件</span>
          <div class="kpi-icon-wrap emerald">
            <el-icon><Connection /></el-icon>
          </div>
        </div>
        <div class="kpi-value-row">
          <span class="kpi-number color-emerald">{{ online.length }}</span>
          <div class="online-pill">
            <span class="status-dot" :class="online.length > 0 ? 'online' : 'offline'"></span>
            <span>{{ online.length > 0 ? '活跃通信中' : '无在线设备' }}</span>
          </div>
        </div>
        <div class="kpi-footer">
          <span class="kpi-hint">WS 传输正常就绪</span>
        </div>
      </div>

      <!-- Success Rate -->
      <div class="kpi-card">
        <div class="kpi-header">
          <span class="kpi-title">指令成功率</span>
          <div class="kpi-icon-wrap purple">
            <el-icon><CircleCheck /></el-icon>
          </div>
        </div>
        <div class="kpi-value-row">
          <span class="kpi-number">{{ successRate }}%</span>
          <span class="kpi-sub">ACK 状态比率</span>
        </div>
        <div class="kpi-footer">
          <el-progress
            :percentage="successRate"
            :show-text="false"
            :stroke-width="6"
            color="#10b981"
          />
        </div>
      </div>

      <!-- Average Latency -->
      <div class="kpi-card">
        <div class="kpi-header">
          <span class="kpi-title">平均执行耗时</span>
          <div class="kpi-icon-wrap amber">
            <el-icon><Lightning /></el-icon>
          </div>
        </div>
        <div class="kpi-value-row">
          <span class="kpi-number">{{ avgLatency }}</span>
          <span class="kpi-sub">ms / 毫秒</span>
        </div>
        <div class="kpi-footer">
          <span class="kpi-hint">端到端链路响应平稳</span>
        </div>
      </div>
    </div>

    <!-- Middle Section: Quick Access + Active Devices -->
    <div class="content-split">
      <!-- Active Devices Panel -->
      <div class="panel-card active-devices-card">
        <div class="panel-header">
          <div class="panel-title-wrap">
            <h3 class="panel-title">在线活跃设备</h3>
            <span class="panel-subtitle">可直接发起键鼠或触控接管</span>
          </div>
          <el-button text size="small" type="primary" @click="router.push('/devices')">
            查看全部 ({{ devices.length }})
          </el-button>
        </div>

        <div v-if="!onlineDevices.length" class="empty-state">
          <el-icon :size="36" color="#94a3b8"><Cpu /></el-icon>
          <p>暂无处于在线状态的 ESP32 设备</p>
          <span class="empty-hint">请确保 ESP32 已接通电源并连接同一 Wi-Fi</span>
        </div>

        <div v-else class="device-chips-grid">
          <div
            v-for="d in onlineDevices"
            :key="d.device_uuid"
            class="device-chip"
          >
            <div class="chip-info">
              <div class="chip-name-row">
                <span class="status-dot online"></span>
                <strong class="chip-name">{{ d.name || 'ESP32 Device' }}</strong>
              </div>
              <span class="chip-uuid">{{ d.device_uuid }}</span>
            </div>

            <div class="chip-actions">
              <el-button
                size="small"
                type="primary"
                plain
                @click="router.push({ path: '/trackpad', query: { device: d.device_uuid } })"
              >
                触控板
              </el-button>
              <el-button
                size="small"
                @click="router.push({ path: '/console', query: { device: d.device_uuid } })"
              >
                控制台
              </el-button>
            </div>
          </div>
        </div>
      </div>

      <!-- Quick Shortcuts Grid -->
      <div class="panel-card shortcuts-card">
        <div class="panel-header">
          <h3 class="panel-title">功能快捷入口</h3>
        </div>

        <div class="shortcuts-list">
          <div class="shortcut-item" @click="router.push('/trackpad')">
            <div class="shortcut-icon-box blue">
              <el-icon><Aim /></el-icon>
            </div>
            <div class="shortcut-content">
              <div class="shortcut-name">全屏触控板 (Touchpad)</div>
              <div class="shortcut-desc">双指缩放/旋转 + 侧边滚轮摇杆</div>
            </div>
            <el-icon class="arrow-icon"><ArrowRight /></el-icon>
          </div>

          <div class="shortcut-item" @click="router.push('/console')">
            <div class="shortcut-icon-box indigo">
              <el-icon><VideoCamera /></el-icon>
            </div>
            <div class="shortcut-content">
              <div class="shortcut-name">动作控制台 (Console)</div>
              <div class="shortcut-desc">单键下发、文本输入与快捷按键</div>
            </div>
            <el-icon class="arrow-icon"><ArrowRight /></el-icon>
          </div>

          <div class="shortcut-item" @click="router.push('/macros')">
            <div class="shortcut-icon-box purple">
              <el-icon><MagicStick /></el-icon>
            </div>
            <div class="shortcut-content">
              <div class="shortcut-name">宏自动化库 (Macros)</div>
              <div class="shortcut-desc">编排按键序列与回放录制脚本</div>
            </div>
            <el-icon class="arrow-icon"><ArrowRight /></el-icon>
          </div>
        </div>
      </div>
    </div>

    <!-- Recent Action Logs Table -->
    <div class="panel-card logs-panel">
      <div class="panel-header">
        <div class="panel-title-wrap">
          <h3 class="panel-title">最近指令执行流水 (Action Logs)</h3>
          <span class="panel-subtitle">最近 25 条设备端 ACK 回执与执行记录</span>
        </div>
        <el-button text size="small" type="primary" @click="router.push('/logs')">
          查看完整日志
        </el-button>
      </div>

      <el-table
        :data="logs"
        v-loading="loading"
        class="custom-table"
        empty-text="暂无动作指令执行记录"
      >
        <el-table-column prop="id" label="序号" width="70" />

        <el-table-column prop="action_type" label="指令类型" min-width="140">
          <template #default="{ row }">
            <el-tag :type="getActionTagType(row.action_type)" size="small" effect="plain">
              {{ row.action_type || 'action' }}
            </el-tag>
          </template>
        </el-table-column>

        <el-table-column prop="device_id" label="目标设备" min-width="140">
          <template #default="{ row }">
            <span class="table-mono">{{ row.device_id || '-' }}</span>
          </template>
        </el-table-column>

        <el-table-column prop="status" label="执行状态" width="120">
          <template #default="{ row }">
            <el-tag
              :type="row.status === 'acked' || row.status === 'ok' ? 'success' : row.status === 'failed' ? 'danger' : 'info'"
              size="small"
            >
              {{ row.status === 'acked' ? 'ACK 成功' : row.status === 'failed' ? '失败' : row.status }}
            </el-tag>
          </template>
        </el-table-column>

        <el-table-column prop="exec_time_ms" label="硬件耗时" width="110">
          <template #default="{ row }">
            <span
              v-if="row.exec_time_ms != null"
              class="latency-badge"
              :class="row.exec_time_ms < 60 ? 'fast' : row.exec_time_ms < 200 ? 'medium' : 'slow'"
            >
              {{ row.exec_time_ms }} ms
            </span>
            <span v-else color="#94a3b8">-</span>
          </template>
        </el-table-column>

        <el-table-column prop="error_message" label="错误描述 / 详情" min-width="180">
          <template #default="{ row }">
            <span :class="{ 'color-danger': !!row.error_message }">
              {{ row.error_message || '正常完成' }}
            </span>
          </template>
        </el-table-column>

        <el-table-column prop="created_at" label="触发时间" width="170">
          <template #default="{ row }">
            <span class="table-time">{{ row.created_at || '-' }}</span>
          </template>
        </el-table-column>
      </el-table>
    </div>
  </div>
</template>

<style scoped>
.dashboard-page {
  display: flex;
  flex-direction: column;
  gap: 20px;
}

/* Banner */
.welcome-banner {
  background: linear-gradient(135deg, #1e3a8a 0%, #2563eb 55%, #3b82f6 100%);
  border-radius: var(--radius-lg);
  padding: 24px 28px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  border: 1px solid #1d4ed8;
  box-shadow: 0 8px 20px -4px rgba(37, 99, 235, 0.25);
  color: #ffffff;
}

.greeting-row {
  display: flex;
  align-items: center;
  gap: 12px;
}

.greeting-hello {
  font-size: 20px;
  font-weight: 700;
  letter-spacing: -0.3px;
  color: #ffffff;
}

.role-tag {
  border-radius: 6px;
}

.banner-desc {
  margin: 6px 0 0;
  font-size: 13px;
  color: rgba(255, 255, 255, 0.9);
  max-width: 600px;
  line-height: 1.5;
}

.banner-actions {
  display: flex;
  align-items: center;
  gap: 12px;
}

.quick-trackpad-btn {
  border-radius: 10px;
  font-weight: 600;
}

/* KPI Grid */
.kpi-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 16px;
}

.kpi-card {
  background: #ffffff;
  border-radius: var(--radius-md);
  padding: 20px;
  border: 1px solid #e2e8f0;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.04);
  display: flex;
  flex-direction: column;
  transition: transform 0.2s ease, box-shadow 0.2s ease;
}

.kpi-card:hover {
  transform: translateY(-2px);
  box-shadow: 0 6px 16px -2px rgba(0, 0, 0, 0.08);
}

.kpi-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.kpi-title {
  font-size: 13px;
  color: #64748b;
  font-weight: 600;
}

.kpi-icon-wrap {
  width: 36px;
  height: 36px;
  border-radius: 10px;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 18px;
}

.kpi-icon-wrap.blue { background: #eff6ff; color: #3b82f6; }
.kpi-icon-wrap.emerald { background: #ecfdf5; color: #10b981; }
.kpi-icon-wrap.purple { background: #faf5ff; color: #a855f7; }
.kpi-icon-wrap.amber { background: #fffbeb; color: #f59e0b; }

.kpi-value-row {
  margin-top: 14px;
  display: flex;
  align-items: baseline;
  gap: 8px;
}

.kpi-number {
  font-size: 32px;
  font-weight: 700;
  color: #0f172a;
  line-height: 1;
  font-feature-settings: "tnum";
}

.kpi-number.color-emerald {
  color: #059669;
}

.kpi-sub {
  font-size: 12px;
  color: #64748b;
}

.online-pill {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: #059669;
  background: #ecfdf5;
  border: 1px solid #a7f3d0;
  padding: 2px 8px;
  border-radius: 9999px;
  margin-left: 4px;
}

.kpi-footer {
  margin-top: 16px;
  font-size: 12px;
  color: #64748b;
}

.footer-link {
  color: var(--color-primary);
  cursor: pointer;
  display: inline-flex;
  align-items: center;
  gap: 4px;
  font-weight: 600;
}

.footer-link:hover {
  text-decoration: underline;
}

/* Content Split */
.content-split {
  display: grid;
  grid-template-columns: 1.4fr 1fr;
  gap: 16px;
}

@media (max-width: 992px) {
  .content-split {
    grid-template-columns: 1fr;
  }
}

.panel-card {
  background: #ffffff;
  border-radius: var(--radius-md);
  padding: 20px 22px;
  border: 1px solid var(--color-border);
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.04);
}

.panel-header {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  margin-bottom: 16px;
}

.panel-title {
  margin: 0;
  font-size: 16px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.panel-subtitle {
  font-size: 12px;
  color: #64748b;
  margin-top: 2px;
  display: block;
}

/* Active Devices list */
.empty-state {
  text-align: center;
  padding: 32px 16px;
  color: #64748b;
}

.empty-state p {
  margin: 10px 0 4px;
  font-size: 14px;
  font-weight: 500;
}

.empty-hint {
  font-size: 12px;
  color: #94a3b8;
}

.device-chips-grid {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.device-chip {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 12px 14px;
  border-radius: 10px;
  background: #f8fafc;
  border: 1px solid #e2e8f0;
  transition: all 0.2s ease;
}

.device-chip:hover {
  background: #f1f5f9;
  border-color: #cbd5e1;
}

.chip-name-row {
  display: flex;
  align-items: center;
  gap: 8px;
}

.chip-name {
  font-size: 14px;
  color: #1e293b;
}

.chip-uuid {
  font-size: 11px;
  color: #94a3b8;
  font-family: var(--font-mono);
  margin-top: 2px;
  display: block;
}

.chip-actions {
  display: flex;
  gap: 8px;
}

/* Shortcuts list */
.shortcuts-list {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.shortcut-item {
  display: flex;
  align-items: center;
  gap: 14px;
  padding: 12px 14px;
  border-radius: 12px;
  background: #f8fafc;
  border: 1px solid #e2e8f0;
  cursor: pointer;
  transition: all 0.2s ease;
}

.shortcut-item:hover {
  background: #eff6ff;
  border-color: #bfdbfe;
  transform: translateX(3px);
}

.shortcut-icon-box {
  width: 40px;
  height: 40px;
  border-radius: 10px;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 20px;
  flex-shrink: 0;
}

.shortcut-icon-box.blue { background: #dbeafe; color: #2563eb; }
.shortcut-icon-box.indigo { background: #e0e7ff; color: #4f46e5; }
.shortcut-icon-box.purple { background: #f3e8ff; color: #9333ea; }

.shortcut-content {
  flex: 1;
}

.shortcut-name {
  font-size: 14px;
  font-weight: 600;
  color: #1e293b;
}

.shortcut-desc {
  font-size: 12px;
  color: #64748b;
  margin-top: 2px;
}

.arrow-icon {
  color: #cbd5e1;
  font-size: 16px;
  transition: transform 0.2s ease;
}

.shortcut-item:hover .arrow-icon {
  color: #3b82f6;
  transform: translateX(2px);
}

/* Logs Panel & Table */
.custom-table :deep(th.el-table__cell) {
  background-color: #f8fafc;
  color: #475569;
  font-weight: 600;
  font-size: 12px;
}

.table-mono {
  font-family: var(--font-mono);
  font-size: 12px;
  color: #475569;
}

.table-time {
  font-size: 12px;
  color: #64748b;
}

.latency-badge {
  font-size: 11px;
  font-weight: 600;
  padding: 2px 6px;
  border-radius: 4px;
}

.latency-badge.fast { background: #ecfdf5; color: #059669; }
.latency-badge.medium { background: #fffbeb; color: #d97706; }
.latency-badge.slow { background: #fef2f2; color: #dc2626; }

.color-danger {
  color: #ef4444;
  font-size: 12px;
}
</style>
