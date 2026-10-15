<script>
export const meta = { title: '宏管理' }
</script>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import draggable from 'vuedraggable'

import { actionsApi } from '@/api/actions'
import { commandsApi } from '@/api/commands'
import { devicesApi } from '@/api/devices'
import { macrosApi } from '@/api/macros'

const KEYS = [
  'KEY_A', 'KEY_B', 'KEY_C', 'KEY_D', 'KEY_E', 'KEY_F', 'KEY_G', 'KEY_H', 'KEY_I', 'KEY_J',
  'KEY_K', 'KEY_L', 'KEY_M', 'KEY_N', 'KEY_O', 'KEY_P', 'KEY_Q', 'KEY_R', 'KEY_S', 'KEY_T',
  'KEY_U', 'KEY_V', 'KEY_W', 'KEY_X', 'KEY_Y', 'KEY_Z',
  'KEY_0', 'KEY_1', 'KEY_2', 'KEY_3', 'KEY_4', 'KEY_5', 'KEY_6', 'KEY_7', 'KEY_8', 'KEY_9',
  'KEY_ENTER', 'KEY_SPACE', 'KEY_TAB', 'KEY_ESC', 'KEY_BACKSPACE',
  'KEY_UP', 'KEY_DOWN', 'KEY_LEFT', 'KEY_RIGHT',
  'KEY_F1', 'KEY_F2', 'KEY_F3', 'KEY_F4', 'KEY_F5', 'KEY_F6', 'KEY_F7', 'KEY_F8', 'KEY_F9', 'KEY_F10', 'KEY_F11', 'KEY_F12',
  'KEY_LCTRL', 'KEY_LSHIFT', 'KEY_LALT', 'KEY_LMETA',
  'KEY_ALT_TAB', 'KEY_ALT_SHIFT_TAB', 'KEY_GUI_D', 'KEY_GUI_TAB', 'KEY_GUI_CTRL_LEFT', 'KEY_GUI_CTRL_RIGHT',
]

const STEP_TYPES = [
  { value: 'keyboard_tap', label: '按键敲击', category: 'key', icon: '⌨' },
  { value: 'keyboard_down', label: '按键按下', category: 'key', icon: '⬇' },
  { value: 'keyboard_up', label: '按键抬起', category: 'key', icon: '⬆' },
  { value: 'text', label: '文本串输入', category: 'text', icon: '✍' },
  { value: 'mouse_move', label: '鼠标相对位移', category: 'mouse', icon: '✛' },
  { value: 'mouse_click', label: '鼠标按键点击', category: 'mouse', icon: '🖱' },
  { value: 'mouse_scroll', label: '滚轮水平/垂直', category: 'mouse', icon: '⇕' },
  { value: 'delay', label: '等待延迟 (ms)', category: 'delay', icon: '⏱' },
]

const EXECUTION_MODES = [
  {
    value: 'ble_targeted',
    label: 'BLE 定向物理输出 (推荐)',
    description: '通过蓝牙切换至绑定的主机 Slot 槽位，并发出真实的物理键盘/鼠标 HID 信号。',
  },
  {
    value: 'network_debug',
    label: '网络调试模式 (No-HID)',
    description: '仅校验 WebSocket 协议帧与 ACK 回执逻辑，不产生真实外设物理信号。',
  },
]

function newStep(type) {
  const id = Math.random().toString(36).slice(2, 10)
  switch (type) {
    case 'keyboard_tap':
    case 'keyboard_down':
    case 'keyboard_up':
      return { id, type, key: 'KEY_ENTER', repeat: 1 }
    case 'text':
      return { id, type, content: 'Hello World', interval_ms: 5 }
    case 'mouse_move':
      return { id, type, x: 10, y: 0 }
    case 'mouse_scroll':
      return { id, type, x: 0, y: -2 }
    case 'mouse_click':
      return { id, type, button: 1 }
    case 'delay':
      return { id, type, delay_ms: 200 }
    default:
      return { id, type }
  }
}

function defaultMacroConfig() {
  return {
    execution_mode: 'ble_targeted',
    require_connected: true,
    action_timeout_ms: 10000,
    switch_timeout_s: 5,
  }
}

function normalizeMacroConfig(config = {}) {
  const merged = { ...defaultMacroConfig(), ...(config || {}) }
  if (merged.execution_mode === 'network_debug') {
    merged.require_connected = false
  } else if (merged.require_connected == null) {
    merged.require_connected = true
  }
  return merged
}

function modeLabel(mode) {
  return EXECUTION_MODES.find((item) => item.value === mode)?.label || mode
}

function flattenForSave(steps) {
  const out = []
  for (const step of steps) {
    const repeat = Math.max(1, parseInt(step.repeat, 10) || 1)
    const { id, repeat: _repeat, ...clean } = step
    for (let i = 0; i < repeat; i += 1) out.push(clean)
  }
  return out
}

const rows = ref([])
const loading = ref(false)

async function load() {
  loading.value = true
  try {
    rows.value = await macrosApi.list()
  } finally {
    loading.value = false
  }
}

onMounted(load)

// KPI Computations
const totalMacrosCount = computed(() => rows.value.length)
const bleMacrosCount = computed(() => rows.value.filter(r => normalizeMacroConfig(r.content_json?.config).execution_mode === 'ble_targeted').length)
const totalStepsCount = computed(() => rows.value.reduce((acc, cur) => acc + (cur.content_json?.steps?.length || 0), 0))

const editor = reactive({
  visible: false,
  id: null,
  name: '',
  steps: [],
  config: defaultMacroConfig(),
})

const editorConfig = computed(() => normalizeMacroConfig(editor.config))
const editorMode = computed(() => editorConfig.value.execution_mode)

function openCreate() {
  editor.visible = true
  editor.id = null
  editor.name = ''
  editor.steps = [newStep('keyboard_tap')]
  editor.config = defaultMacroConfig()
}

function openEdit(row) {
  editor.visible = true
  editor.id = row.id
  editor.name = row.name
  editor.config = normalizeMacroConfig(row.content_json?.config)
  const src = row.content_json?.steps || []
  editor.steps = src.map((step) => ({
    id: Math.random().toString(36).slice(2, 10),
    repeat: 1,
    ...step,
  }))
}

function onEditorModeChange(mode) {
  if (mode === 'network_debug') {
    editor.config.require_connected = false
  } else if (editor.config.require_connected === false) {
    editor.config.require_connected = true
  }
}

function addStep(type) {
  editor.steps.push(newStep(type))
}

function removeStep(index) {
  editor.steps.splice(index, 1)
}

function duplicateStep(index) {
  const step = editor.steps[index]
  editor.steps.splice(index + 1, 0, {
    ...step,
    id: Math.random().toString(36).slice(2, 10),
  })
}

async function save() {
  if (!editor.name.trim()) return ElMessage.warning('请输入宏流程名称')
  if (!editor.steps.length) return ElMessage.warning('宏流程至少需包含一个执行动作')

  const payload = {
    name: editor.name.trim(),
    steps: flattenForSave(editor.steps),
    config: normalizeMacroConfig(editor.config),
  }

  if (editor.id == null) {
    await macrosApi.create(payload)
    ElMessage.success('宏编排流程创建成功')
  } else {
    await macrosApi.update(editor.id, payload)
    ElMessage.success('宏编排流程保存成功')
  }

  editor.visible = false
  await load()
}

async function remove(row) {
  await ElMessageBox.confirm(`确认删除宏流程「${row.name}」？`, '删除确认', { type: 'warning' })
  await macrosApi.remove(row.id)
  ElMessage.success('宏流程已删除')
  await load()
}

const runner = reactive({
  visible: false,
  macro: null,
  config: defaultMacroConfig(),
  deviceUuid: '',
  selectedSlots: [],
  devices: [],
  online: [],
  slots: [],
  results: null,
  busy: false,
})

const runnerConfig = computed(() => normalizeMacroConfig(runner.config))
const runnerMode = computed(() => runnerConfig.value.execution_mode)

function formatRunnerResults(results) {
  const failed = results.filter((item) => item.status !== 'acked')
  if (!failed.length) ElMessage.success(`所有 ${results.length} 个目标已成功执行！`)
  else ElMessage.warning(`${failed.length}/${results.length} 个目标执行未确认`)
}

async function loadSlotsForRunner() {
  if (!runner.deviceUuid || runnerMode.value !== 'ble_targeted') {
    runner.slots = []
    runner.selectedSlots = []
    return
  }
  try {
    runner.slots = (await commandsApi.listSlots(runner.deviceUuid)) || []
  } catch {
    runner.slots = []
  }
  runner.selectedSlots = runner.slots.length ? [runner.slots[0].slot_index] : []
}

async function openRun(row) {
  runner.visible = true
  runner.macro = row
  runner.config = normalizeMacroConfig(row.content_json?.config)
  runner.results = null
  runner.selectedSlots = []
  runner.deviceUuid = ''
  runner.slots = []
  runner.devices = await devicesApi.list()
  runner.online = (await devicesApi.online()).online
  const firstOnline = runner.devices.find((device) => runner.online.includes(device.device_uuid))
  if (firstOnline) {
    runner.deviceUuid = firstOnline.device_uuid
    await loadSlotsForRunner()
  }
}

async function doRun() {
  if (!runner.deviceUuid) return ElMessage.warning('请先选择在线的受控硬件设备')

  const action = {
    type: 'macro',
    steps: runner.macro?.content_json?.steps || [],
  }

  runner.busy = true
  try {
    if (runnerMode.value === 'network_debug') {
      action.debug_no_hid = true
      action.skip_link_check = true
      action.need_ack = true

      const resp = await actionsApi.send({
        device_id: runner.deviceUuid,
        payload: action,
        timeout_ms: runnerConfig.value.action_timeout_ms,
      })

      runner.results = (resp.results || []).map((item) => ({
        slot_index: null,
        addr: item.device_id,
        switched: false,
        connected: null,
        status: item.status,
        exec_time_ms: item.exec_time_ms || 0,
        error_code: item.error_code,
        error_message: item.error_message,
        msg_id: item.msg_id,
      }))
      formatRunnerResults(runner.results)
      return
    }

    if (!runner.selectedSlots.length) {
      return ElMessage.warning('请至少勾选一个目标主机 Slot 槽位')
    }

    const resp = await commandsApi.send(runner.deviceUuid, {
      slots: [...runner.selectedSlots],
      action,
      require_connected: runnerConfig.value.require_connected,
      switch_timeout_s: runnerConfig.value.switch_timeout_s,
      action_timeout_ms: runnerConfig.value.action_timeout_ms,
    })
    runner.results = resp.results || []
    formatRunnerResults(runner.results)
  } finally {
    runner.busy = false
  }
}

const previewJson = computed(() => JSON.stringify({
  steps: flattenForSave(editor.steps),
  config: normalizeMacroConfig(editor.config),
}, null, 2))
</script>

<template>
  <div class="macros-container">
    <!-- Header -->
    <div class="macros-header glass-panel">
      <div class="header-left">
        <div class="header-icon-badge">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <polygon points="12 2 2 7 12 12 22 7 12 2"/>
            <polyline points="2 17 12 22 22 17"/>
            <polyline points="2 12 12 17 22 12"/>
          </svg>
        </div>
        <div>
          <h1 class="header-title">外设自动化宏编排</h1>
          <p class="header-subtitle">Visual HID Macro Pipeline, Replay Engine & Targeted Dispatch</p>
        </div>
      </div>

      <div class="header-actions">
        <el-button class="refresh-btn" :loading="loading" @click="load">
          <svg class="btn-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/>
          </svg>
          刷新
        </el-button>
        <el-button type="primary" class="create-btn" @click="openCreate">
          <svg class="btn-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <line x1="12" y1="5" x2="12" y2="19"/>
            <line x1="5" y1="12" x2="19" y2="12"/>
          </svg>
          新建宏流程
        </el-button>
      </div>
    </div>

    <!-- KPI Metrics -->
    <div class="kpi-grid">
      <div class="kpi-card glass-panel">
        <div class="kpi-label">已编排宏脚本</div>
        <div class="kpi-value text-blue">{{ totalMacrosCount }}</div>
        <div class="kpi-sub">预置键盘/鼠标组合指令流</div>
      </div>
      <div class="kpi-card glass-panel">
        <div class="kpi-label">物理 BLE 模式</div>
        <div class="kpi-value text-purple">{{ bleMacrosCount }}</div>
        <div class="kpi-sub">定向下发真实 HID 外设脉冲</div>
      </div>
      <div class="kpi-card glass-panel">
        <div class="kpi-label">编排动作步数</div>
        <div class="kpi-value text-green">{{ totalStepsCount }}</div>
        <div class="kpi-sub">平均每宏约 {{ totalMacrosCount ? Math.round(totalStepsCount / totalMacrosCount) : 0 }} 步</div>
      </div>
    </div>

    <!-- Macros Table Card -->
    <div class="table-card glass-panel" v-loading="loading">
      <el-empty v-if="!rows.length && !loading" description="暂无宏编排流程，点击上方新建宏流程开启自动化" :image-size="80" />
      <el-table v-else :data="rows" size="default" class="modern-macro-table">
        <el-table-column prop="id" label="ID" width="70" />

        <el-table-column prop="name" label="宏流程名称" min-width="180">
          <template #default="{ row }">
            <span class="macro-name-text">{{ row.name }}</span>
          </template>
        </el-table-column>

        <el-table-column label="运行模式" width="160">
          <template #default="{ row }">
            <span
              class="mode-badge"
              :class="normalizeMacroConfig(row.content_json?.config).execution_mode === 'ble_targeted' ? 'mode-ble' : 'mode-debug'"
            >
              {{ normalizeMacroConfig(row.content_json?.config).execution_mode === 'ble_targeted' ? '⚡ BLE 定向物理' : '🔍 网络调试' }}
            </span>
          </template>
        </el-table-column>

        <el-table-column label="步骤数" width="100">
          <template #default="{ row }">
            <span class="step-count-pill">
              {{ row.content_json?.steps?.length ?? 0 }} 步
            </span>
          </template>
        </el-table-column>

        <el-table-column label="动作摘要" min-width="220">
          <template #default="{ row }">
            <div class="steps-preview-tags">
              <span
                v-for="(step, i) in (row.content_json?.steps || []).slice(0, 4)"
                :key="i"
                class="step-micro-tag"
                :class="'cat-' + (STEP_TYPES.find(s => s.value === step.type)?.category || 'key')"
              >
                {{ step.type.replace('keyboard_', '').replace('mouse_', '') }}
              </span>
              <span v-if="(row.content_json?.steps?.length || 0) > 4" class="more-steps-tag">
                +{{ row.content_json.steps.length - 4 }}
              </span>
            </div>
          </template>
        </el-table-column>

        <el-table-column prop="created_at" label="创建时间" width="170">
          <template #default="{ row }">
            <span class="timestamp-text">{{ row.created_at ? new Date(row.created_at).toLocaleDateString() : '-' }}</span>
          </template>
        </el-table-column>

        <el-table-column label="操作" width="220" fixed="right">
          <template #default="{ row }">
            <div class="action-btn-row">
              <button class="macro-run-btn" @click="openRun(row)">
                ▶ 执行
              </button>
              <el-button size="small" link type="primary" @click="openEdit(row)">编辑</el-button>
              <el-button size="small" link type="danger" @click="remove(row)">删除</el-button>
            </div>
          </template>
        </el-table-column>
      </el-table>
    </div>

    <!-- Dialog: Macro Pipeline Editor -->
    <el-dialog
      v-model="editor.visible"
      :title="editor.id == null ? '新建可视化宏编排' : `编辑宏流程 #${editor.id} · ${editor.name}`"
      width="1040px"
      top="4vh"
      class="modern-dialog macro-editor-dialog"
    >
      <div class="editor-body-grid">
        <!-- Settings Bar -->
        <div class="editor-settings-strip">
          <div class="es-item name-field">
            <div class="es-label">宏名称</div>
            <el-input v-model="editor.name" placeholder="例如：自动化表格填充、复制并切换" size="default" />
          </div>

          <div class="es-item mode-field">
            <div class="es-label">执行模式</div>
            <el-radio-group v-model="editor.config.execution_mode" size="default" @change="onEditorModeChange">
              <el-radio-button label="ble_targeted">BLE 定向物理</el-radio-button>
              <el-radio-button label="network_debug">网络调试模式</el-radio-button>
            </el-radio-group>
          </div>

          <div class="es-item">
            <div class="es-label">动作超时 (ms)</div>
            <el-input-number v-model="editor.config.action_timeout_ms" :min="500" :max="60000" :step="1000" size="default" controls-position="right" />
          </div>

          <div class="es-item" v-if="editorMode === 'ble_targeted'">
            <div class="es-label">切槽超时 (s)</div>
            <el-input-number v-model="editor.config.switch_timeout_s" :min="1" :max="60" size="default" controls-position="right" />
          </div>
        </div>

        <!-- Main Workspace: Left Add Buttons, Center Step Pipeline, Right JSON -->
        <div class="pipeline-workspace">
          <!-- Step Type Palette -->
          <div class="step-palette glass-panel">
            <div class="palette-title">添加动作节点</div>
            <div class="palette-buttons">
              <button
                v-for="type in STEP_TYPES"
                :key="type.value"
                class="palette-btn"
                :class="'btn-cat-' + type.category"
                @click="addStep(type.value)"
              >
                <span class="btn-icon">{{ type.icon }}</span>
                <span class="btn-label">{{ type.label }}</span>
              </button>
            </div>
          </div>

          <!-- Step Pipeline Droppable -->
          <div class="pipeline-stream">
            <div class="stream-header">
              <span class="stream-title">动作执行流水线 ({{ editor.steps.length }} 步)</span>
              <span class="stream-hint">可直接拖拽左侧句柄上下重排步骤</span>
            </div>

            <el-empty v-if="!editor.steps.length" description="流水线为空，点击左侧动作按钮添加节点" :image-size="70" />
            <draggable
              v-else
              v-model="editor.steps"
              item-key="id"
              handle=".drag-handle"
              animation="200"
              class="draggable-container"
            >
              <template #item="{ element: step, index }">
                <div class="step-card" :class="'card-cat-' + (STEP_TYPES.find(t => t.value === step.type)?.category || 'key')">
                  <div class="drag-handle" title="按住拖动调序">⠿</div>
                  <div class="step-index-pill">#{{ index + 1 }}</div>

                  <div class="step-type-pill">
                    {{ STEP_TYPES.find(t => t.value === step.type)?.label || step.type }}
                  </div>

                  <!-- Dynamic Field Controls -->
                  <div class="step-controls">
                    <!-- Key tap / down / up -->
                    <template v-if="['keyboard_tap', 'keyboard_down', 'keyboard_up'].includes(step.type)">
                      <el-select v-model="step.key" filterable size="small" style="width: 170px">
                        <el-option v-for="key in KEYS" :key="key" :label="key" :value="key" />
                      </el-select>
                      <div class="ctrl-group">
                        <span class="ctrl-label">重复:</span>
                        <el-input-number v-model="step.repeat" :min="1" :max="999" size="small" controls-position="right" style="width: 80px" />
                      </div>
                    </template>

                    <!-- Text -->
                    <template v-else-if="step.type === 'text'">
                      <el-input v-model="step.content" placeholder="输入要下发的字符串内容" size="small" style="flex: 1; min-width: 160px" />
                      <div class="ctrl-group">
                        <span class="ctrl-label">打字延时(ms):</span>
                        <el-input-number v-model="step.interval_ms" :min="0" :max="500" size="small" controls-position="right" style="width: 80px" />
                      </div>
                    </template>

                    <!-- Mouse Move -->
                    <template v-else-if="step.type === 'mouse_move'">
                      <div class="ctrl-group">
                        <span class="ctrl-label">dx:</span>
                        <el-input-number v-model="step.x" :min="-127" :max="127" size="small" controls-position="right" style="width: 80px" />
                      </div>
                      <div class="ctrl-group">
                        <span class="ctrl-label">dy:</span>
                        <el-input-number v-model="step.y" :min="-127" :max="127" size="small" controls-position="right" style="width: 80px" />
                      </div>
                    </template>

                    <!-- Mouse Scroll -->
                    <template v-else-if="step.type === 'mouse_scroll'">
                      <div class="ctrl-group">
                        <span class="ctrl-label">水平 hx:</span>
                        <el-input-number v-model="step.x" :min="-127" :max="127" size="small" controls-position="right" style="width: 80px" />
                      </div>
                      <div class="ctrl-group">
                        <span class="ctrl-label">垂直 vy:</span>
                        <el-input-number v-model="step.y" :min="-127" :max="127" size="small" controls-position="right" style="width: 80px" />
                      </div>
                    </template>

                    <!-- Mouse Click -->
                    <template v-else-if="step.type === 'mouse_click'">
                      <el-radio-group v-model="step.button" size="small">
                        <el-radio-button :label="1">左键 (Left)</el-radio-button>
                        <el-radio-button :label="2">右键 (Right)</el-radio-button>
                        <el-radio-button :label="4">中键 (Middle)</el-radio-button>
                      </el-radio-group>
                    </template>

                    <!-- Delay -->
                    <template v-else-if="step.type === 'delay'">
                      <div class="ctrl-group">
                        <span class="ctrl-label">等待时长:</span>
                        <el-input-number v-model="step.delay_ms" :min="0" :max="60000" :step="100" size="small" controls-position="right" style="width: 120px" />
                        <span class="ctrl-label">ms</span>
                      </div>
                    </template>
                  </div>

                  <!-- Step Card Actions -->
                  <div class="step-card-actions">
                    <button class="step-action-btn" title="克隆节点" @click="duplicateStep(index)">⧉</button>
                    <button class="step-action-btn danger" title="删除节点" @click="removeStep(index)">✕</button>
                  </div>
                </div>
              </template>
            </draggable>
          </div>

          <!-- JSON Live Preview -->
          <div class="json-preview-sidebar glass-panel">
            <div class="json-header">
              <span>JSON 实时结构</span>
            </div>
            <pre class="json-code-box">{{ previewJson }}</pre>
          </div>
        </div>
      </div>

      <template #footer>
        <el-button @click="editor.visible = false">取消</el-button>
        <el-button type="primary" @click="save">保存宏编排</el-button>
      </template>
    </el-dialog>

    <!-- Dialog: Macro Runner -->
    <el-dialog
      v-model="runner.visible"
      :title="`执行宏流程 · ${runner.macro?.name || ''}`"
      width="720px"
      class="modern-dialog"
    >
      <div class="runner-form-body">
        <div class="runner-mode-banner">
          <span class="banner-title">执行模式:</span>
          <span
            class="mode-badge"
            :class="runnerMode === 'ble_targeted' ? 'mode-ble' : 'mode-debug'"
          >
            {{ modeLabel(runnerMode) }}
          </span>
        </div>

        <div class="field-row">
          <div class="field-label">下发硬件终端 (必须在线):</div>
          <el-select v-model="runner.deviceUuid" @change="loadSlotsForRunner" style="width: 100%">
            <el-option
              v-for="device in runner.devices"
              :key="device.id"
              :label="`${device.name || device.device_uuid} ${runner.online.includes(device.device_uuid) ? '● 在线' : '○ 离线'}`"
              :value="device.device_uuid"
              :disabled="!runner.online.includes(device.device_uuid)"
            />
          </el-select>
        </div>

        <div class="field-row" v-if="runnerMode === 'ble_targeted'">
          <div class="field-label">目标主机 Slot 槽位:</div>
          <div v-if="!runner.slots.length" class="text-muted" style="font-size: 12px">
            该设备尚未在“设备控制”中分配任何 BLE Slot 槽位，请先配置槽位。
          </div>
          <el-checkbox-group v-else v-model="runner.selectedSlots" class="slots-checkbox-group">
            <el-checkbox
              v-for="slot in runner.slots"
              :key="slot.slot_index"
              :label="slot.slot_index"
              class="slot-check-card"
            >
              Slot #{{ slot.slot_index }} ({{ slot.name || slot.addr }})
            </el-checkbox>
          </el-checkbox-group>
        </div>

        <!-- Execution Results Table -->
        <div v-if="runner.results" class="runner-results-card glass-panel">
          <div class="results-header">执行响应列表</div>
          <el-table :data="runner.results" size="small" class="custom-tab-table">
            <el-table-column label="Slot" width="70">
              <template #default="{ row }">#{{ row.slot_index ?? '-' }}</template>
            </el-table-column>
            <el-table-column label="目标主机">
              <template #default="{ row }">{{ row.name || row.addr || '-' }}</template>
            </el-table-column>
            <el-table-column label="状态" width="100">
              <template #default="{ row }">
                <span
                  class="status-pill"
                  :class="row.status === 'acked' ? 'pill-acked' : 'pill-failed'"
                >
                  {{ row.status }}
                </span>
              </template>
            </el-table-column>
            <el-table-column label="耗时">
              <template #default="{ row }">
                <span v-if="row.status === 'acked'">{{ row.exec_time_ms }} ms</span>
                <span v-else class="text-danger">{{ row.error_code }} {{ row.error_message }}</span>
              </template>
            </el-table-column>
          </el-table>
        </div>
      </div>

      <template #footer>
        <el-button @click="runner.visible = false">关闭</el-button>
        <el-button type="primary" :loading="runner.busy" @click="doRun">立即执行宏</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<style scoped>
.macros-container {
  display: flex;
  flex-direction: column;
  gap: 20px;
  width: 100%;
}

/* Header */
.macros-header {
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

.text-blue { color: #2563eb; }
.text-purple { color: #7c3aed; }
.text-green { color: #16a34a; }
.text-danger { color: #dc2626; }
.text-muted { color: #64748b; }

/* Table */
.table-card {
  padding: 8px;
  border-radius: var(--radius-lg);
  overflow: hidden;
}

.modern-macro-table {
  background: transparent !important;
}

.modern-macro-table :deep(.el-table__header-wrapper th) {
  background: var(--color-bg-base) !important;
  color: var(--color-text-muted);
  font-weight: 600;
  font-size: 12px;
}

.macro-name-text {
  font-weight: 700;
  color: var(--color-text-primary);
  font-size: 14px;
}

.mode-badge {
  font-size: 11px;
  padding: 2px 8px;
  border-radius: var(--radius-pill);
  font-weight: 600;
}

.mode-ble {
  background: #f3e8ff;
  color: #7e22ce;
  border: 1px solid #e9d5ff;
}

.mode-debug {
  background: #e0f2fe;
  color: #0369a1;
  border: 1px solid #bae6fd;
}

.step-count-pill {
  font-size: 11px;
  font-weight: 700;
  color: var(--color-text-primary);
  background: #f1f5f9;
  border: 1px solid #e2e8f0;
  padding: 2px 8px;
  border-radius: 4px;
}

.steps-preview-tags {
  display: flex;
  gap: 4px;
  flex-wrap: wrap;
}

.step-micro-tag {
  font-size: 10px;
  padding: 1px 6px;
  border-radius: 3px;
  font-weight: 600;
}

.cat-key {
  background: #e0f2fe;
  color: #0369a1;
  border: 1px solid #bae6fd;
}

.cat-text {
  background: #dcfce7;
  color: #15803d;
  border: 1px solid #bbf7d0;
}

.cat-mouse {
  background: #f3e8ff;
  color: #7e22ce;
  border: 1px solid #e9d5ff;
}

.cat-delay {
  background: #fef3c7;
  color: #b45309;
  border: 1px solid #fde68a;
}

.more-steps-tag {
  font-size: 10px;
  color: var(--color-text-muted);
}

.timestamp-text {
  font-size: 11px;
  color: var(--color-text-muted);
}

.action-btn-row {
  display: flex;
  align-items: center;
  gap: 8px;
}

.macro-run-btn {
  padding: 3px 10px;
  border-radius: var(--radius-pill);
  background: #dcfce7;
  border: 1px solid #86efac;
  color: #15803d;
  font-size: 11px;
  font-weight: 700;
  cursor: pointer;
  transition: all 0.2s;
}

.macro-run-btn:hover {
  background: #bbf7d0;
  transform: translateY(-1px);
}

/* Editor Dialog Layout */
.editor-body-grid {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.editor-settings-strip {
  display: flex;
  gap: 14px;
  align-items: flex-end;
  padding: 14px 16px;
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-md);
  flex-wrap: wrap;
}

.es-item {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.name-field { flex: 2; min-width: 220px; }
.mode-field { flex: 1; min-width: 200px; }

.es-label {
  font-size: 11px;
  font-weight: 600;
  color: var(--color-text-muted);
  text-transform: uppercase;
}

.pipeline-workspace {
  display: grid;
  grid-template-columns: 180px 1fr 280px;
  gap: 14px;
  align-items: start;
}

@media (max-width: 900px) {
  .pipeline-workspace {
    grid-template-columns: 1fr;
  }
}

/* Step Palette */
.step-palette {
  padding: 14px;
  border-radius: var(--radius-md);
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.palette-title {
  font-size: 12px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.palette-buttons {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.palette-btn {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 7px 10px;
  border-radius: var(--radius-sm);
  border: 1px solid var(--color-border);
  background: var(--color-bg-base);
  color: var(--color-text-secondary);
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
  text-align: left;
  transition: all 0.15s;
}

.palette-btn:hover {
  border-color: var(--color-primary);
  color: var(--color-primary);
  transform: translateX(2px);
}

.btn-icon {
  font-size: 13px;
}

/* Pipeline Stream */
.pipeline-stream {
  display: flex;
  flex-direction: column;
  gap: 10px;
  min-height: 280px;
  max-height: 520px;
  overflow-y: auto;
  padding-right: 4px;
}

.stream-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.stream-title {
  font-size: 13px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.stream-hint {
  font-size: 11px;
  color: var(--color-text-muted);
}

.draggable-container {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.step-card {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 12px;
  border-radius: var(--radius-md);
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  box-shadow: 0 2px 6px rgba(0, 0, 0, 0.1);
  transition: all 0.2s;
}

.card-cat-key { border-left: 3px solid #3b82f6; }
.card-cat-text { border-left: 3px solid #10b981; }
.card-cat-mouse { border-left: 3px solid #a855f7; }
.card-cat-delay { border-left: 3px solid #f59e0b; }

.drag-handle {
  cursor: grab;
  color: var(--color-text-muted);
  font-size: 16px;
  user-select: none;
}
.drag-handle:active { cursor: grabbing; }

.step-index-pill {
  font-size: 10px;
  font-weight: 700;
  color: var(--color-text-muted);
}

.step-type-pill {
  font-size: 11px;
  font-weight: 600;
  padding: 2px 6px;
  border-radius: 4px;
  background: rgba(148, 163, 184, 0.1);
  color: var(--color-text-primary);
  min-width: 72px;
  text-align: center;
}

.step-controls {
  display: flex;
  align-items: center;
  gap: 8px;
  flex: 1;
  flex-wrap: wrap;
}

.ctrl-group {
  display: flex;
  align-items: center;
  gap: 4px;
}

.ctrl-label {
  font-size: 11px;
  color: var(--color-text-muted);
}

.step-card-actions {
  display: flex;
  align-items: center;
  gap: 4px;
  margin-left: auto;
}

.step-action-btn {
  background: transparent;
  border: none;
  color: var(--color-text-muted);
  cursor: pointer;
  padding: 4px;
  font-size: 14px;
}
.step-action-btn:hover { color: var(--color-primary); }
.step-action-btn.danger:hover { color: #dc2626; }

/* JSON Preview Sidebar */
.json-preview-sidebar {
  padding: 12px;
  border-radius: var(--radius-md);
  display: flex;
  flex-direction: column;
  gap: 8px;
  max-height: 520px;
}

.json-header {
  font-size: 12px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.json-code-box {
  margin: 0;
  max-height: 480px;
  overflow-y: auto;
  font-family: 'JetBrains Mono', Consolas, Monaco, monospace;
  font-size: 10.5px;
  background: #0f172a;
  color: #38bdf8;
  border: 1px solid #1e293b;
  padding: 10px;
  border-radius: var(--radius-sm);
  white-space: pre-wrap;
}

/* Runner */
.runner-form-body {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.runner-mode-banner {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
}

.field-row {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.field-label {
  font-size: 12px;
  font-weight: 600;
  color: var(--color-text-secondary);
}

.slots-checkbox-group {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.slot-check-card {
  margin-right: 0;
}

.runner-results-card {
  padding: 12px;
  border-radius: var(--radius-md);
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.results-header {
  font-size: 12px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.status-pill {
  font-size: 11px;
  font-weight: 600;
  padding: 2px 6px;
  border-radius: var(--radius-pill);
}

.pill-acked {
  color: #15803d;
  background: #dcfce7;
  border: 1px solid #bbf7d0;
}

.pill-failed {
  color: #b91c1c;
  background: #fee2e2;
  border: 1px solid #fecaca;
}
</style>
