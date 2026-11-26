<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { timersApi } from '@/api/timers'
import { devicesApi } from '@/api/devices'
import { macrosApi } from '@/api/macros'
import { commandsApi } from '@/api/commands'
import { getCapabilityFlags } from '@/utils/capabilities'
import { DISPLAY_MODE_OPTIONS } from '@/utils/displayPresets'
import {
  GPIO_OP_OPTIONS,
  SCHEDULE_TYPE_OPTIONS,
  WEEKDAY_OPTIONS,
  buildTimerPayload,
  createDefaultTimerForm,
  formatActionSummary,
  formatCountdown,
  formatEpoch,
  formatScheduleSummary,
  getAllowedActionOptions,
} from '@/utils/timerPresets'

const props = defineProps({
  deviceUuid: { type: String, default: '' },
  deviceCaps: { type: Array, default: () => [] },
  devices: { type: Array, default: () => [] },
  gpioPins: { type: Array, default: () => [] },
  macros: { type: Array, default: () => [] },
  seedForm: { type: Object, default: null },
  compact: { type: Boolean, default: false },
})

const emit = defineEmits(['changed'])

const loading = ref(false)
const saving = ref(false)
const timers = ref([])
const showEditor = ref(false)
const localDevices = ref([])
const localMacros = ref([])
const localGpioPins = ref([])
const nowSec = ref(Math.floor(Date.now() / 1000))
let tickHandle = null
let autoRefreshArmedFor = 0

const deviceList = computed(() => (props.devices?.length ? props.devices : localDevices.value))
const macroList = computed(() => (props.macros?.length ? props.macros : localMacros.value))
const macrosMap = computed(() => {
  const m = {}
  for (const row of macroList.value) m[row.id] = row
  return m
})

const form = reactive(createDefaultTimerForm(props.deviceUuid))

const targetDeviceObj = computed(() =>
  deviceList.value.find((d) => d.device_uuid === (form.device_uuid || props.deviceUuid)) || null,
)

const targetCapFlags = computed(() => {
  const rawCaps = props.deviceUuid
    ? (props.deviceCaps?.length ? props.deviceCaps : targetDeviceObj.value?.capabilities || [])
    : (targetDeviceObj.value?.capabilities || [])
  return getCapabilityFlags(rawCaps)
})

const allowedActionOptions = computed(() => getAllowedActionOptions(targetCapFlags.value))

const effectiveGpioPins = computed(() => {
  if (props.deviceUuid && props.deviceUuid === form.device_uuid && props.gpioPins?.length) {
    return props.gpioPins
  }
  return localGpioPins.value
})

const stats = computed(() => {
  const total = timers.value.length
  const active = timers.value.filter((t) => t.enabled).length
  const ran = timers.value.reduce((acc, t) => acc + Number(t.run_count || 0), 0)
  return { total, active, ran }
})

async function loadMetaIfNeeded() {
  const tasks = []
  if (!props.devices?.length && !localDevices.value.length) {
    tasks.push(devicesApi.list().then((r) => { localDevices.value = r || [] }).catch(() => {}))
  }
  if (!props.macros?.length && !localMacros.value.length) {
    tasks.push(macrosApi.list().then((r) => { localMacros.value = r || [] }).catch(() => {}))
  }
  if (tasks.length) await Promise.all(tasks)
}

async function loadGpioPinsForTarget(uuid) {
  if (!uuid) {
    localGpioPins.value = []
    return
  }
  const caps = getCapabilityFlags(
    deviceList.value.find((d) => d.device_uuid === uuid)?.capabilities || props.deviceCaps || [],
  )
  if (!caps.supportsGpio) {
    localGpioPins.value = []
    return
  }
  try {
    const resp = await commandsApi.gpioPins(uuid)
    const r = resp?.result?.result || resp?.result || resp
    localGpioPins.value = Array.isArray(r?.pins) ? r.pins : []
  } catch {
    localGpioPins.value = []
  }
}

async function loadTimers() {
  loading.value = true
  try {
    timers.value = (await timersApi.list(props.deviceUuid || '')) || []
    autoRefreshArmedFor = 0
  } catch {
    timers.value = []
  } finally {
    loading.value = false
  }
}

function openCreate(seed = null) {
  const targetUuid = form.device_uuid || props.deviceUuid || deviceList.value[0]?.device_uuid || ''
  const next = createDefaultTimerForm(targetUuid, targetCapFlags.value, seed)
  Object.assign(form, next)
  if (seed?.gpio_pin !== undefined) form.gpio_pin = seed.gpio_pin
  if (seed?.gpio_op) form.gpio_op = seed.gpio_op
  if (seed?.name) form.name = seed.name
  showEditor.value = true
  if (form.device_uuid && !props.gpioPins?.length) {
    loadGpioPinsForTarget(form.device_uuid)
  }
}

function openEdit(row) {
  const next = createDefaultTimerForm(row.device_uuid, null, row)
  Object.assign(form, next)
  showEditor.value = true
  if (form.device_uuid) {
    loadGpioPinsForTarget(form.device_uuid)
  }
}

function applyQuickPreset(preset) {
  openCreate(preset)
}

async function submitTimer() {
  if (!form.device_uuid) {
    ElMessage.warning('请选择目标设备')
    return
  }
  if (form.action_type === 'macro' && !form.macro_id) {
    ElMessage.warning('请选择要执行的自动化宏')
    return
  }
  saving.value = true
  try {
    const payload = buildTimerPayload(form)
    if (form.id) {
      await timersApi.update(form.id, payload)
      ElMessage.success('定时规则已更新')
    } else {
      await timersApi.create(payload)
      ElMessage.success('定时任务已创建并进入调度队列')
    }
    showEditor.value = false
    await loadTimers()
    emit('changed')
  } finally {
    saving.value = false
  }
}

async function toggleTimer(row, val) {
  try {
    await timersApi.toggle(row.id, val)
    ElMessage.success(val ? '定时器已启用' : '定时器已暂停')
    await loadTimers()
    emit('changed')
  } catch {
    await loadTimers()
  }
}

async function runNow(row) {
  loading.value = true
  try {
    const resp = await timersApi.runNow(row.id)
    if (resp?.ok) {
      ElMessage.success(`已立即执行「${row.name}」: ${resp.message || 'ok'}`)
    } else {
      ElMessage.warning(`执行异常: ${resp?.message || 'failed'}`)
    }
    await loadTimers()
    emit('changed')
  } finally {
    loading.value = false
  }
}

async function removeTimer(row) {
  try {
    await ElMessageBox.confirm(`确认删除定时任务「${row.name}」？`, '删除定时器', { type: 'warning' })
    await timersApi.remove(row.id)
    ElMessage.success('已删除定时任务')
    await loadTimers()
    emit('changed')
  } catch { /* canceled */ }
}

function deviceLabel(uuid) {
  const d = deviceList.value.find((item) => item.device_uuid === uuid)
  return d?.name ? `${d.name} (${uuid})` : uuid
}

function statusTagType(status) {
  if (status === 'acked' || status === 'executed' || status === 'ok') return 'success'
  if (status === 'error' || status === 'failed' || status === 'offline') return 'danger'
  return 'info'
}

watch(
  () => props.deviceUuid,
  async (uuid) => {
    form.device_uuid = uuid || form.device_uuid
    await loadMetaIfNeeded()
    await loadTimers()
  },
  { immediate: true },
)

watch(
  () => form.device_uuid,
  (uuid) => {
    if (showEditor.value && uuid) {
      if (!allowedActionOptions.value.some((o) => o.value === form.action_type)) {
        form.action_type = allowedActionOptions.value[0]?.value || 'reboot'
      }
      loadGpioPinsForTarget(uuid)
    }
  },
)

watch(
  () => props.seedForm,
  (seed) => {
    if (seed && seed._ts) {
      openCreate(seed)
    }
  },
)

onMounted(() => {
  tickHandle = setInterval(() => {
    const cur = Math.floor(Date.now() / 1000)
    nowSec.value = cur
    const dueRow = timers.value.find((t) => t.enabled && t.next_run_at && cur >= t.next_run_at + 2)
    if (dueRow && autoRefreshArmedFor !== dueRow.next_run_at && !loading.value) {
      autoRefreshArmedFor = dueRow.next_run_at
      loadTimers()
    }
  }, 1000)
})

onBeforeUnmount(() => {
  if (tickHandle) clearInterval(tickHandle)
})
</script>

<template>
  <div class="timer-panel" :class="{ compact }">
    <!-- Header toolbar + stats -->
    <div class="tp-header">
      <div class="tp-stats">
        <span class="tp-stat-chip">规则总数 <b>{{ stats.total }}</b></span>
        <span class="tp-stat-chip active">运行中 <b>{{ stats.active }}</b></span>
        <span class="tp-stat-chip">累计执行 <b>{{ stats.ran }}</b> 次</span>
      </div>
      <div class="tp-actions">
        <el-button size="small" @click="loadTimers" :loading="loading">刷新</el-button>
        <el-button size="small" type="primary" @click="openCreate()">
          + 新建定时规则
        </el-button>
      </div>
    </div>

    <!-- Quick Presets Strip -->
    <div class="tp-presets">
      <span class="tp-presets-label">快捷模板:</span>
      <el-button
        v-if="targetCapFlags.supportsGpio"
        size="small"
        plain
        @click="applyQuickPreset({
          name: '10秒后点亮脉冲 5秒',
          schedule_type: 'countdown',
          delay_sec: 10,
          action_type: 'gpio_set',
          action_payload: { pin: effectiveGpioPins[0]?.pin ?? 12, op: 'on', duration_sec: 5 },
        })"
      >
        10s 后脉冲开 5s
      </el-button>
      <el-button
        v-if="targetCapFlags.supportsGpio"
        size="small"
        plain
        @click="applyQuickPreset({
          name: '每日 08:30 开启 GPIO',
          schedule_type: 'daily',
          time_hhmm: '08:30',
          action_type: 'gpio_set',
          action_payload: { pin: effectiveGpioPins[0]?.pin ?? 12, op: 'on' },
        })"
      >
        每日 08:30 开启
      </el-button>
      <el-button
        v-if="targetCapFlags.supportsDisplay"
        size="small"
        plain
        @click="applyQuickPreset({
          name: '每日 23:30 定时息屏',
          schedule_type: 'daily',
          time_hhmm: '23:30',
          action_type: 'display_config',
          action_payload: { mode: 'off', backlight: 0 },
        })"
      >
        夜间 23:30 息屏
      </el-button>
      <el-button
        size="small"
        plain
        @click="applyQuickPreset({
          name: '30秒后软重启校准',
          schedule_type: 'countdown',
          delay_sec: 30,
          action_type: 'reboot',
          action_payload: {},
        })"
      >
        30s 后重启
      </el-button>
    </div>

    <!-- Create / Edit Form Box -->
    <div v-if="showEditor" class="tp-editor-box">
      <div class="tp-editor-head">
        <span class="tp-editor-title">{{ form.id ? '编辑定时任务' : '新建模块定时 / 周期任务' }}</span>
        <el-button size="small" link @click="showEditor = false">收起</el-button>
      </div>

      <el-form label-position="top" size="small" class="tp-form-grid">
        <el-form-item v-if="!deviceUuid" label="目标硬件设备">
          <el-select v-model="form.device_uuid" placeholder="选择设备" style="width: 100%" filterable>
            <el-option
              v-for="d in deviceList"
              :key="d.device_uuid"
              :label="`${d.name || d.device_uuid} (${(d.online ?? d.status === 'online') ? '在线' : '离线'})`"
              :value="d.device_uuid"
            />
          </el-select>
        </el-form-item>

        <el-form-item label="任务名称">
          <el-input v-model="form.name" placeholder="留空将按动作自动命名" />
        </el-form-item>

        <el-form-item label="触发方式">
          <el-select v-model="form.schedule_type" style="width: 100%">
            <el-option
              v-for="opt in SCHEDULE_TYPE_OPTIONS"
              :key="opt.value"
              :label="`${opt.label} — ${opt.desc}`"
              :value="opt.value"
            />
          </el-select>
        </el-form-item>

        <el-form-item v-if="form.schedule_type === 'countdown'" label="倒计时延迟 (秒)">
          <el-input-number v-model="form.delay_sec" :min="1" :max="2592000" style="width: 100%" />
        </el-form-item>

        <el-form-item v-if="form.schedule_type === 'interval'" label="循环周期间隔 (秒，最小 5s)">
          <el-input-number v-model="form.interval_sec" :min="5" :max="2592000" style="width: 100%" />
        </el-form-item>

        <el-form-item v-if="form.schedule_type === 'daily'" label="触发时刻 (HH:MM)">
          <el-time-select
            v-model="form.time_hhmm"
            start="00:00"
            step="00:15"
            end="23:45"
            format="HH:mm"
            editable
            style="width: 100%"
          />
        </el-form-item>

        <el-form-item v-if="form.schedule_type === 'once'" label="精确触发日期时间">
          <el-date-picker
            v-model="form.once_at_ms"
            type="datetime"
            value-format="x"
            placeholder="选择执行时间"
            style="width: 100%"
          />
        </el-form-item>

        <el-form-item
          v-if="form.schedule_type === 'daily'"
          label="重复星期 (全不选 = 每天)"
          class="tp-col-span-2"
        >
          <el-checkbox-group v-model="form.weekdays">
            <el-checkbox-button v-for="w in WEEKDAY_OPTIONS" :key="w.value" :value="w.value">
              {{ w.label }}
            </el-checkbox-button>
          </el-checkbox-group>
        </el-form-item>

        <el-form-item label="执行硬件动作" class="tp-col-span-2">
          <el-select v-model="form.action_type" style="width: 100%">
            <el-option
              v-for="opt in allowedActionOptions"
              :key="opt.value"
              :label="opt.label"
              :value="opt.value"
            />
          </el-select>
        </el-form-item>

        <!-- Action Payload: GPIO -->
        <template v-if="form.action_type === 'gpio_set'">
          <el-form-item label="目标 GPIO 引脚">
            <el-select
              v-if="effectiveGpioPins.length"
              v-model="form.gpio_pin"
              filterable
              allow-create
              default-first-option
              style="width: 100%"
            >
              <el-option
                v-for="p in effectiveGpioPins"
                :key="p.pin"
                :label="`GPIO ${p.pin} · ${p.label || '引脚'}${p.pwm ? ' [PWM]' : ''}`"
                :value="p.pin"
              />
            </el-select>
            <el-input-number v-else v-model="form.gpio_pin" :min="0" :max="48" style="width: 100%" />
          </el-form-item>

          <el-form-item label="目标动作">
            <el-select v-model="form.gpio_op" style="width: 100%">
              <el-option
                v-for="op in GPIO_OP_OPTIONS"
                :key="op.value"
                :label="op.label"
                :value="op.value"
              />
            </el-select>
          </el-form-item>

          <el-form-item v-if="form.gpio_op === 'pwm'" label="PWM 占空比 (%)">
            <el-slider v-model="form.gpio_duty" :min="0" :max="100" show-input />
          </el-form-item>

          <el-form-item
            v-if="form.gpio_op !== 'off'"
            label="开启后自动复原关闭时长 (秒，0=常驻)"
          >
            <el-input-number v-model="form.gpio_duration_sec" :min="0" :max="86400" style="width: 100%" />
          </el-form-item>
        </template>

        <!-- Action Payload: Display -->
        <template v-else-if="form.action_type === 'display_config'">
          <el-form-item label="目标屏幕模式">
            <el-select v-model="form.display_mode" style="width: 100%">
              <el-option
                v-for="m in DISPLAY_MODE_OPTIONS"
                :key="m.value"
                :label="m.label"
                :value="m.value"
              />
            </el-select>
          </el-form-item>
          <el-form-item label="目标背光亮度 (%)">
            <el-slider v-model="form.display_backlight" :min="0" :max="100" show-input />
          </el-form-item>
          <el-form-item
            v-if="form.display_mode === 'text'"
            label="显示文本行 (最多 6 行，每行回车分隔)"
            class="tp-col-span-2"
          >
            <el-input v-model="form.display_text" type="textarea" :rows="3" />
          </el-form-item>
        </template>

        <!-- Action Payload: HID Transport -->
        <template v-else-if="form.action_type === 'hid_transport'">
          <el-form-item label="目标传输模式" class="tp-col-span-2">
            <el-radio-group v-model="form.transport_mode">
              <el-radio-button value="auto">Auto 智能自适应</el-radio-button>
              <el-radio-button value="usb">USB 独占</el-radio-button>
              <el-radio-button value="ble">BLE 无线</el-radio-button>
            </el-radio-group>
          </el-form-item>
        </template>

        <!-- Action Payload: Macro -->
        <template v-else-if="form.action_type === 'macro'">
          <el-form-item label="选择要触发的宏编排" class="tp-col-span-2">
            <el-select v-model="form.macro_id" placeholder="选择自动化宏" style="width: 100%" filterable>
              <el-option
                v-for="m in macroList"
                :key="m.id"
                :label="`${m.name} (#${m.id})`"
                :value="m.id"
              />
            </el-select>
          </el-form-item>
        </template>
      </el-form>

      <div class="tp-editor-foot">
        <el-checkbox v-model="form.enabled">保存后立即启用调度</el-checkbox>
        <div style="display: flex; gap: 8px">
          <el-button size="small" @click="showEditor = false">取消</el-button>
          <el-button size="small" type="primary" :loading="saving" @click="submitTimer">
            {{ form.id ? '保存修改' : '创建并排程' }}
          </el-button>
        </div>
      </div>
    </div>

    <!-- Timers Table -->
    <el-empty v-if="!timers.length && !loading" description="暂无模块定时或自动化规则，点击右上角创建" :image-size="56" />
    <el-table v-else :data="timers" size="small" class="tp-table" v-loading="loading">
      <el-table-column label="启用" width="66" align="center">
        <template #default="{ row }">
          <el-switch
            :model-value="row.enabled"
            size="small"
            @change="(val) => toggleTimer(row, val)"
          />
        </template>
      </el-table-column>

      <el-table-column label="规则名称" min-width="140">
        <template #default="{ row }">
          <div class="tp-row-name">{{ row.name }}</div>
          <div v-if="!deviceUuid" class="tp-row-sub">{{ deviceLabel(row.device_uuid) }}</div>
        </template>
      </el-table-column>

      <el-table-column label="执行动作" min-width="150">
        <template #default="{ row }">
          <span class="tp-action-pill">{{ formatActionSummary(row, macrosMap) }}</span>
        </template>
      </el-table-column>

      <el-table-column label="调度计划 / 倒计时" min-width="155">
        <template #default="{ row }">
          <div>{{ formatScheduleSummary(row) }}</div>
          <div class="tp-row-sub" v-if="row.enabled && row.next_run_at">
            下次: {{ formatCountdown(row.next_run_at, nowSec) }} ({{ formatEpoch(row.next_run_at).slice(11) }})
          </div>
          <div class="tp-row-sub" v-else>已暂停 / 单次已结束</div>
        </template>
      </el-table-column>

      <el-table-column label="最近状态" width="120">
        <template #default="{ row }">
          <el-tag size="small" :type="statusTagType(row.last_status)">
            {{ row.last_status || 'pending' }}
          </el-tag>
          <div class="tp-row-sub" :title="row.last_message">
            已运行 {{ row.run_count || 0 }} 次
          </div>
        </template>
      </el-table-column>

      <el-table-column label="操作" width="150" fixed="right">
        <template #default="{ row }">
          <el-button size="small" link type="primary" @click="runNow(row)">立即执行</el-button>
          <el-button size="small" link @click="openEdit(row)">编辑</el-button>
          <el-button size="small" link type="danger" @click="removeTimer(row)">删除</el-button>
        </template>
      </el-table-column>
    </el-table>
  </div>
</template>

<style scoped>
.timer-panel {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.tp-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 8px;
}

.tp-stats {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.tp-stat-chip {
  font-size: 12px;
  color: var(--color-text-secondary);
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  border-radius: 6px;
  padding: 4px 10px;
}

.tp-stat-chip b {
  color: var(--color-text-primary);
  margin-left: 3px;
}

.tp-stat-chip.active b {
  color: var(--color-primary);
}

.tp-actions {
  display: flex;
  gap: 8px;
}

.tp-presets {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
  padding: 10px 12px;
  background: var(--color-bg-card);
  border: 1px dashed var(--color-border);
  border-radius: var(--radius-sm);
}

.tp-presets-label {
  font-size: 12px;
  color: var(--color-text-muted);
}

.tp-editor-box {
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-md);
  padding: 14px 16px;
}

.tp-editor-head {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 10px;
}

.tp-editor-title {
  font-size: 13.5px;
  font-weight: 600;
  color: var(--color-text-primary);
}

.tp-form-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 0 14px;
}

.tp-col-span-2 {
  grid-column: span 2;
}

.tp-editor-foot {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-top: 8px;
  padding-top: 10px;
  border-top: 1px solid var(--color-border);
}

.tp-row-name {
  font-weight: 600;
  color: var(--color-text-primary);
  font-size: 13px;
}

.tp-row-sub {
  font-size: 11.5px;
  color: var(--color-text-muted);
  margin-top: 2px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: 220px;
}

.tp-action-pill {
  font-family: var(--font-mono);
  font-size: 11.5px;
  background: #f1f5f9;
  color: #334155;
  padding: 2px 7px;
  border-radius: 4px;
}
</style>
