<script>
export const meta = { title: '动作日志' }
</script>

<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { ElMessage } from 'element-plus'
import { actionsApi } from '@/api/actions'

const rows = ref([])
const loading = ref(false)
const autoRefresh = ref(false)
let autoRefreshTimer = null

const filters = reactive({
  limit: 100,
  status: '',
  action_type: '',
})

const ACTION_TYPES = [
  { value: '', label: '全部类型' },
  { value: 'keyboard_tap', label: '按键敲击 (keyboard_tap)' },
  { value: 'text', label: '文本输入 (text)' },
  { value: 'mouse_move', label: '鼠标位移 (mouse_move)' },
  { value: 'mouse_click', label: '鼠标点击 (mouse_click)' },
  { value: 'mouse_scroll', label: '滚轮滚动 (mouse_scroll)' },
  { value: 'macro', label: '宏编排 (macro)' },
]

const STATUS_OPTIONS = [
  { value: '', label: '全部状态' },
  { value: 'acked', label: '已确认 (acked)' },
  { value: 'failed', label: '失败 (failed)' },
  { value: 'pending', label: '等待中 (pending)' },
  { value: 'sent', label: '已发送 (sent)' },
]

// Detail Drawer
const detailDrawer = reactive({
  visible: false,
  log: null,
})

async function load() {
  loading.value = true
  try {
    const params = {
      limit: filters.limit,
    }
    if (filters.status) params.status = filters.status
    if (filters.action_type) params.action_type = filters.action_type

    rows.value = await actionsApi.recentLogs(params)
  } catch (err) {
    ElMessage.error('加载日志列表失败')
  } finally {
    loading.value = false
  }
}

// KPI Computations
const totalCount = computed(() => rows.value.length)
const ackedCount = computed(() => rows.value.filter(r => r.status === 'acked').length)
const failedCount = computed(() => rows.value.filter(r => r.status === 'failed').length)
const successRate = computed(() => totalCount.value ? Math.round((ackedCount.value / totalCount.value) * 100) : 100)
const avgLatency = computed(() => {
  const acked = rows.value.filter(r => r.status === 'acked' && r.exec_time_ms != null)
  if (!acked.length) return 0
  const sum = acked.reduce((acc, cur) => acc + cur.exec_time_ms, 0)
  return Math.round(sum / acked.length)
})

function viewDetail(row) {
  detailDrawer.log = row
  detailDrawer.visible = true
}

function copyText(val) {
  navigator.clipboard.writeText(typeof val === 'object' ? JSON.stringify(val, null, 2) : String(val))
  ElMessage.success('已复制到剪贴板')
}

function toggleAutoRefresh(val) {
  if (val) {
    autoRefreshTimer = window.setInterval(load, 3000)
    ElMessage.success('已开启 3 秒实时轮询')
  } else {
    if (autoRefreshTimer) {
      window.clearInterval(autoRefreshTimer)
      autoRefreshTimer = null
    }
  }
}

onMounted(load)

onBeforeUnmount(() => {
  if (autoRefreshTimer) window.clearInterval(autoRefreshTimer)
})
</script>

<template>
  <div class="logs-container">
    <!-- Top Bar -->
    <div class="logs-header glass-panel">
      <div class="header-left">
        <div class="header-icon-badge">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/>
          </svg>
        </div>
        <div>
          <h1 class="header-title">外设动作审计日志</h1>
          <p class="header-subtitle">Hardware Action Logs, Latency Analytics & Traceability</p>
        </div>
      </div>

      <div class="header-actions">
        <el-switch
          v-model="autoRefresh"
          active-text="实时监控 (3s)"
          inactive-text=""
          @change="toggleAutoRefresh"
          class="auto-refresh-switch"
        />
        <el-button class="refresh-btn" :loading="loading" @click="load">
          <svg class="btn-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/>
          </svg>
          刷新
        </el-button>
      </div>
    </div>

    <!-- KPI Summary Row -->
    <div class="kpi-grid">
      <div class="kpi-card glass-panel">
        <div class="kpi-label">已捕获日志总量</div>
        <div class="kpi-value text-blue">{{ totalCount }}</div>
        <div class="kpi-sub">当前筛选记录数</div>
      </div>
      <div class="kpi-card glass-panel">
        <div class="kpi-label">指令执行成功率</div>
        <div class="kpi-value text-green">{{ successRate }}%</div>
        <div class="kpi-sub">{{ ackedCount }} 次 ACK 响应</div>
      </div>
      <div class="kpi-card glass-panel">
        <div class="kpi-label">硬件执行平均耗时</div>
        <div class="kpi-value text-purple">{{ avgLatency }} <span class="unit">ms</span></div>
        <div class="kpi-sub">端到端传输与响应</div>
      </div>
      <div class="kpi-card glass-panel">
        <div class="kpi-label">异常与失败</div>
        <div class="kpi-value" :class="failedCount > 0 ? 'text-red' : 'text-muted'">{{ failedCount }}</div>
        <div class="kpi-sub">超时或硬件校验报错</div>
      </div>
    </div>

    <!-- Filter Toolbar -->
    <div class="filter-toolbar glass-panel">
      <div class="filter-group">
        <span class="filter-label">执行状态:</span>
        <el-select v-model="filters.status" placeholder="全部状态" size="default" style="width: 140px" @change="load">
          <el-option v-for="item in STATUS_OPTIONS" :key="item.value" :label="item.label" :value="item.value" />
        </el-select>
      </div>

      <div class="filter-group">
        <span class="filter-label">动作类型:</span>
        <el-select v-model="filters.action_type" placeholder="全部类型" size="default" style="width: 180px" @change="load">
          <el-option v-for="item in ACTION_TYPES" :key="item.value" :label="item.label" :value="item.value" />
        </el-select>
      </div>

      <div class="filter-group">
        <span class="filter-label">显示数量:</span>
        <el-select v-model="filters.limit" size="default" style="width: 110px" @change="load">
          <el-option :value="50" label="50 条" />
          <el-option :value="100" label="100 条" />
          <el-option :value="200" label="200 条" />
          <el-option :value="500" label="500 条" />
        </el-select>
      </div>
    </div>

    <!-- Modern Data Table -->
    <div class="table-card glass-panel" v-loading="loading">
      <el-table :data="rows" size="default" class="modern-log-table" empty-text="未找到匹配的动作日志">
        <el-table-column prop="id" label="ID" width="70" />

        <el-table-column label="动作类型" width="160">
          <template #default="{ row }">
            <span class="action-type-badge">
              {{ row.action_type || 'custom' }}
            </span>
          </template>
        </el-table-column>

        <el-table-column label="状态" width="110">
          <template #default="{ row }">
            <span
              class="status-badge"
              :class="{
                'badge-acked': row.status === 'acked',
                'badge-failed': row.status === 'failed',
                'badge-pending': row.status === 'pending' || row.status === 'sent'
              }"
            >
              <span class="status-dot"></span>
              {{ row.status }}
            </span>
          </template>
        </el-table-column>

        <el-table-column label="耗时" width="110">
          <template #default="{ row }">
            <span
              v-if="row.exec_time_ms != null"
              class="latency-badge"
              :class="{
                'speed-fast': row.exec_time_ms <= 15,
                'speed-mid': row.exec_time_ms > 15 && row.exec_time_ms <= 60,
                'speed-slow': row.exec_time_ms > 60
              }"
            >
              {{ row.exec_time_ms }} ms
            </span>
            <span v-else class="text-muted">—</span>
          </template>
        </el-table-column>

        <el-table-column label="消息凭证 (msg_id)" min-width="200">
          <template #default="{ row }">
            <code class="msg-id-code" @click="copyText(row.msg_id)" title="点击复制">{{ row.msg_id }}</code>
          </template>
        </el-table-column>

        <el-table-column prop="device_id" label="设备 ID" width="90">
          <template #default="{ row }">
            <span class="device-id-tag">{{ row.device_id ?? '-' }}</span>
          </template>
        </el-table-column>

        <el-table-column label="错误信息" min-width="180">
          <template #default="{ row }">
            <span v-if="row.error_message || row.error_code" class="text-danger-msg">
              <span class="err-code">{{ row.error_code }}</span>
              {{ row.error_message }}
            </span>
            <span v-else class="text-muted">—</span>
          </template>
        </el-table-column>

        <el-table-column prop="created_at" label="时间戳" width="170">
          <template #default="{ row }">
            <span class="timestamp-text">{{ row.created_at ? new Date(row.created_at).toLocaleString() : '-' }}</span>
          </template>
        </el-table-column>

        <el-table-column label="操作" width="90" fixed="right">
          <template #default="{ row }">
            <el-button size="small" link type="primary" @click="viewDetail(row)">详情</el-button>
          </template>
        </el-table-column>
      </el-table>
    </div>

    <!-- Detail Drawer -->
    <el-drawer
      v-model="detailDrawer.visible"
      title="动作执行审计详情"
      size="520px"
      direction="rtl"
      class="log-detail-drawer"
    >
      <div v-if="detailDrawer.log" class="drawer-detail-body">
        <div class="detail-hero-card">
          <div class="dh-status-row">
            <span
              class="status-badge large"
              :class="{
                'badge-acked': detailDrawer.log.status === 'acked',
                'badge-failed': detailDrawer.log.status === 'failed',
                'badge-pending': detailDrawer.log.status === 'pending' || detailDrawer.log.status === 'sent'
              }"
            >
              {{ detailDrawer.log.status }}
            </span>
            <span class="dh-latency" v-if="detailDrawer.log.exec_time_ms != null">
              执行耗时: <strong>{{ detailDrawer.log.exec_time_ms }} ms</strong>
            </span>
          </div>
          <div class="dh-msg-id">
            <span class="dh-label">msg_id:</span>
            <code>{{ detailDrawer.log.msg_id }}</code>
            <el-button size="small" link @click="copyText(detailDrawer.log.msg_id)">复制</el-button>
          </div>
        </div>

        <div class="detail-props-list">
          <div class="dp-row">
            <span class="dp-label">日志 ID:</span>
            <span class="dp-val">#{{ detailDrawer.log.id }}</span>
          </div>
          <div class="dp-row">
            <span class="dp-label">动作类型:</span>
            <span class="dp-val text-primary">{{ detailDrawer.log.action_type }}</span>
          </div>
          <div class="dp-row">
            <span class="dp-label">目标设备 ID:</span>
            <span class="dp-val">{{ detailDrawer.log.device_id ?? '未绑定' }}</span>
          </div>
          <div class="dp-row">
            <span class="dp-label">操作用户 ID:</span>
            <span class="dp-val">{{ detailDrawer.log.user_id ?? '系统' }}</span>
          </div>
          <div class="dp-row">
            <span class="dp-label">创建时间:</span>
            <span class="dp-val">{{ detailDrawer.log.created_at ? new Date(detailDrawer.log.created_at).toLocaleString() : '-' }}</span>
          </div>
          <div class="dp-row" v-if="detailDrawer.log.error_code || detailDrawer.log.error_message">
            <span class="dp-label text-danger">异常信息:</span>
            <span class="dp-val text-danger">[{{ detailDrawer.log.error_code }}] {{ detailDrawer.log.error_message }}</span>
          </div>
        </div>

        <div class="payload-box">
          <div class="payload-header">
            <span>指令载荷 Payload (JSON)</span>
            <el-button size="small" link @click="copyText(detailDrawer.log.payload_json)">复制 JSON</el-button>
          </div>
          <pre class="terminal-raw-pre">{{ JSON.stringify(detailDrawer.log.payload_json, null, 2) }}</pre>
        </div>
      </div>
    </el-drawer>
  </div>
</template>

<style scoped>
.logs-container {
  display: flex;
  flex-direction: column;
  gap: 20px;
  width: 100%;
}

/* Header */
.logs-header {
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
  background: linear-gradient(135deg, rgba(59, 130, 246, 0.2), rgba(16, 185, 129, 0.2));
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
  gap: 16px;
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

.btn-svg {
  width: 14px;
  height: 14px;
}

/* KPI Grid */
.kpi-grid {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 16px;
}

@media (max-width: 960px) {
  .kpi-grid {
    grid-template-columns: repeat(2, 1fr);
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

.kpi-value .unit {
  font-size: 14px;
  font-weight: 500;
  color: var(--color-text-muted);
}

.kpi-sub {
  font-size: 11px;
  color: var(--color-text-secondary);
}

.text-blue { color: #38bdf8; }
.text-green { color: #4ade80; }
.text-purple { color: #c084fc; }
.text-red { color: #f87171; }
.text-muted { color: #94a3b8; }

/* Filter Toolbar */
.filter-toolbar {
  display: flex;
  align-items: center;
  gap: 20px;
  padding: 14px 20px;
  border-radius: var(--radius-md);
  flex-wrap: wrap;
}

.filter-group {
  display: flex;
  align-items: center;
  gap: 8px;
}

.filter-label {
  font-size: 12px;
  color: var(--color-text-secondary);
  font-weight: 600;
}

/* Table Card */
.table-card {
  padding: 8px;
  border-radius: var(--radius-lg);
  overflow: hidden;
}

.modern-log-table {
  background: transparent !important;
}

.modern-log-table :deep(.el-table__header-wrapper th) {
  background: var(--color-bg-base) !important;
  color: var(--color-text-muted);
  font-weight: 600;
  font-size: 12px;
}

.action-type-badge {
  display: inline-block;
  font-size: 12px;
  font-weight: 600;
  color: #38bdf8;
  background: rgba(56, 189, 248, 0.1);
  padding: 2px 8px;
  border-radius: 4px;
}

.status-badge {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  font-size: 11px;
  font-weight: 700;
  padding: 3px 8px;
  border-radius: var(--radius-pill);
  text-transform: uppercase;
}

.status-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
}

.badge-acked {
  background: rgba(34, 197, 94, 0.15);
  color: #4ade80;
}
.badge-acked .status-dot { background: #4ade80; }

.badge-failed {
  background: rgba(239, 68, 68, 0.15);
  color: #f87171;
}
.badge-failed .status-dot { background: #f87171; }

.badge-pending {
  background: rgba(148, 163, 184, 0.15);
  color: #94a3b8;
}
.badge-pending .status-dot { background: #94a3b8; }

.latency-badge {
  font-size: 11px;
  font-weight: 700;
  padding: 2px 6px;
  border-radius: 4px;
}

.speed-fast {
  background: rgba(34, 197, 94, 0.1);
  color: #4ade80;
}
.speed-mid {
  background: rgba(56, 189, 248, 0.1);
  color: #38bdf8;
}
.speed-slow {
  background: rgba(234, 179, 8, 0.1);
  color: #facc15;
}

.msg-id-code {
  font-family: monospace;
  font-size: 11px;
  color: var(--color-text-secondary);
  cursor: pointer;
}
.msg-id-code:hover {
  color: var(--color-primary);
  text-decoration: underline;
}

.device-id-tag {
  font-size: 11px;
  font-weight: 600;
  color: var(--color-text-secondary);
}

.text-danger-msg {
  color: #f87171;
  font-size: 11px;
}
.err-code {
  font-weight: 700;
  margin-right: 4px;
}

.timestamp-text {
  font-size: 11px;
  color: var(--color-text-muted);
}

/* Detail Drawer */
.log-detail-drawer :deep(.el-drawer__body) {
  padding: 20px;
  background: var(--color-bg-base);
}

.drawer-detail-body {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.detail-hero-card {
  padding: 16px;
  border-radius: var(--radius-md);
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.dh-status-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.status-badge.large {
  font-size: 13px;
  padding: 4px 12px;
}

.dh-latency {
  font-size: 12px;
  color: var(--color-text-secondary);
}

.dh-msg-id {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 12px;
}

.dh-msg-id code {
  font-family: monospace;
  color: #38bdf8;
  word-break: break-all;
}

.detail-props-list {
  padding: 16px;
  border-radius: var(--radius-md);
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.dp-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 12px;
}

.dp-label {
  color: var(--color-text-muted);
}

.dp-val {
  color: var(--color-text-primary);
  font-weight: 600;
}

.payload-box {
  padding: 16px;
  border-radius: var(--radius-md);
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.payload-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 12px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.terminal-raw-pre {
  margin: 0;
  max-height: 400px;
  overflow-y: auto;
  background: rgba(15, 23, 42, 0.85);
  color: #94a3b8;
  padding: 12px;
  border-radius: var(--radius-sm);
  font-family: monospace;
  font-size: 12px;
}
</style>
