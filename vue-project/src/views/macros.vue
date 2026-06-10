<script>
export const meta = { title: 'Macro Manager' }
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
  { value: 'keyboard_tap', label: 'Key Tap' },
  { value: 'keyboard_down', label: 'Key Down' },
  { value: 'keyboard_up', label: 'Key Up' },
  { value: 'text', label: 'Text' },
  { value: 'mouse_move', label: 'Mouse Move' },
  { value: 'mouse_scroll', label: 'Mouse Scroll' },
  { value: 'mouse_click', label: 'Mouse Click' },
  { value: 'delay', label: 'Delay' },
]

const EXECUTION_MODES = [
  {
    value: 'network_debug',
    label: 'Network Debug',
    description: 'Validate WS protocol and ACK flow only. No real HID output.',
  },
  {
    value: 'ble_targeted',
    label: 'BLE Targeted',
    description: 'Switch to configured BLE slots and execute against real targets.',
  },
]

function newStep(type) {
  const id = Math.random().toString(36).slice(2, 10)
  switch (type) {
    case 'keyboard_tap':
    case 'keyboard_down':
    case 'keyboard_up':
      return { id, type, key: 'KEY_A', repeat: 1 }
    case 'text':
      return { id, type, content: '', interval_ms: 5 }
    case 'mouse_move':
      return { id, type, x: 0, y: 0 }
    case 'mouse_scroll':
      return { id, type, x: 0, y: -1 }
    case 'mouse_click':
      return { id, type, button: 1 }
    case 'delay':
      return { id, type, delay_ms: 100 }
    default:
      return { id, type }
  }
}

function defaultMacroConfig() {
  return {
    execution_mode: 'network_debug',
    require_connected: false,
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
  editor.steps = []
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
  if (!editor.name.trim()) return ElMessage.warning('Please enter a macro name.')
  if (!editor.steps.length) return ElMessage.warning('Add at least one step.')

  const payload = {
    name: editor.name.trim(),
    steps: flattenForSave(editor.steps),
    config: normalizeMacroConfig(editor.config),
  }

  if (editor.id == null) {
    await macrosApi.create(payload)
    ElMessage.success('Macro created.')
  } else {
    await macrosApi.update(editor.id, payload)
    ElMessage.success('Macro saved.')
  }

  editor.visible = false
  await load()
}

async function remove(row) {
  await ElMessageBox.confirm(`Delete macro "${row.name}"?`, 'Confirm', { type: 'warning' })
  await macrosApi.remove(row.id)
  ElMessage.success('Macro deleted.')
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
  if (!failed.length) ElMessage.success(`Completed ${results.length} target(s).`)
  else ElMessage.warning(`${failed.length}/${results.length} target(s) failed.`)
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
  runner.selectedSlots = []
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
  if (!runner.deviceUuid) return ElMessage.warning('Select an online device first.')

  const action = {
    type: 'macro',
    steps: runner.macro?.content_json?.steps || [],
  }

  runner.busy = true
  try {
    if (runnerMode.value === 'network_debug') {
      action.debug_no_hid = true
      action.skip_link_check = true

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
      return ElMessage.warning('Select at least one slot.')
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
  <el-card>
    <div style="display:flex; justify-content:space-between; margin-bottom:12px">
      <el-button @click="load" :loading="loading">Refresh</el-button>
      <el-button type="primary" @click="openCreate">New Macro</el-button>
    </div>

    <el-table :data="rows" v-loading="loading">
      <el-table-column prop="id" label="ID" width="60" />
      <el-table-column prop="name" label="Name" width="220" />
      <el-table-column label="Mode" width="150">
        <template #default="{ row }">
          <el-tag size="small" :type="normalizeMacroConfig(row.content_json?.config).execution_mode === 'network_debug' ? 'info' : 'success'">
            {{ modeLabel(normalizeMacroConfig(row.content_json?.config).execution_mode) }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="Steps" width="80">
        <template #default="{ row }">{{ row.content_json?.steps?.length ?? 0 }}</template>
      </el-table-column>
      <el-table-column label="Preview">
        <template #default="{ row }">
          <code style="font-size:12px">
            {{ JSON.stringify(row.content_json?.steps?.slice(0, 3) || []) }}{{ (row.content_json?.steps?.length || 0) > 3 ? ' ...' : '' }}
          </code>
        </template>
      </el-table-column>
      <el-table-column prop="created_at" label="Created" width="180" />
      <el-table-column label="Actions" width="240">
        <template #default="{ row }">
          <el-button size="small" type="primary" @click="openRun(row)">Run</el-button>
          <el-button size="small" @click="openEdit(row)">Edit</el-button>
          <el-button size="small" type="danger" @click="remove(row)">Delete</el-button>
        </template>
      </el-table-column>
    </el-table>
  </el-card>

  <el-dialog v-model="editor.visible" :title="editor.id == null ? 'New Macro' : `Edit Macro #${editor.id}`" width="980px" top="5vh">
    <el-form label-width="120px">
      <el-form-item label="Macro Name">
        <el-input v-model="editor.name" style="width:320px" placeholder="For example: Copy and Paste" />
      </el-form-item>

      <el-form-item label="Execution Mode">
        <div style="display:flex; flex-direction:column; gap:8px; width:100%">
          <el-radio-group v-model="editor.config.execution_mode" @change="onEditorModeChange">
            <el-radio-button v-for="mode in EXECUTION_MODES" :key="mode.value" :label="mode.value">
              {{ mode.label }}
            </el-radio-button>
          </el-radio-group>
          <div style="font-size:12px; color:#909399">
            {{ EXECUTION_MODES.find((item) => item.value === editorMode)?.description }}
          </div>
        </div>
      </el-form-item>

      <el-form-item label="Action Timeout">
        <el-input-number v-model="editor.config.action_timeout_ms" :min="500" :max="60000" controls-position="right" />
        <span style="margin-left:8px; font-size:12px; color:#909399">ms</span>
      </el-form-item>

      <el-form-item v-if="editorMode === 'ble_targeted'" label="Switch Timeout">
        <el-input-number v-model="editor.config.switch_timeout_s" :min="1" :max="60" controls-position="right" />
        <span style="margin-left:8px; font-size:12px; color:#909399">seconds</span>
      </el-form-item>

      <el-form-item v-if="editorMode === 'ble_targeted'" label="Require Connected">
        <el-switch v-model="editor.config.require_connected" />
      </el-form-item>

      <el-alert
        v-if="editorMode === 'network_debug'"
        type="info"
        :closable="false"
        title="Network Debug mode only validates the action envelope, queueing, ACKs, and timing. It will not emit real HID events."
      />
    </el-form>

    <div style="display:flex; gap:12px; margin-top:16px">
      <el-card style="width:170px; flex-shrink:0" header="Add Step">
        <el-button
          v-for="type in STEP_TYPES"
          :key="type.value"
          size="small"
          @click="addStep(type.value)"
          style="width:100%; margin-bottom:6px"
        >
          + {{ type.label }}
        </el-button>
      </el-card>

      <div style="flex:1; min-width:0">
        <el-empty v-if="!editor.steps.length" description="Add steps from the left panel." :image-size="80" />
        <draggable
          v-else
          v-model="editor.steps"
          item-key="id"
          handle=".drag-handle"
          animation="150"
          style="display:flex; flex-direction:column; gap:8px"
        >
          <template #item="{ element: step, index }">
            <div class="step-row">
              <div class="drag-handle" title="Drag to reorder">::</div>
              <el-tag size="small" style="width:120px; text-align:center">
                {{ index + 1 }} / {{ STEP_TYPES.find((type) => type.value === step.type)?.label || step.type }}
              </el-tag>

              <template v-if="['keyboard_tap', 'keyboard_down', 'keyboard_up'].includes(step.type)">
                <el-select v-model="step.key" filterable size="small" style="width:170px">
                  <el-option v-for="key in KEYS" :key="key" :label="key" :value="key" />
                </el-select>
                <el-input-number v-model="step.repeat" :min="1" :max="999" size="small" controls-position="right" style="width:110px" />
                <span style="font-size:12px; color:#909399">repeat</span>
              </template>

              <template v-else-if="step.type === 'text'">
                <el-input v-model="step.content" placeholder="Text to type" size="small" style="flex:1; min-width:0" />
                <el-input-number v-model="step.interval_ms" :min="0" :max="500" size="small" controls-position="right" style="width:110px" />
                <span style="font-size:12px; color:#909399">ms/char</span>
              </template>

              <template v-else-if="step.type === 'mouse_move'">
                <span style="font-size:12px">dx</span>
                <el-input-number v-model="step.x" :min="-127" :max="127" size="small" controls-position="right" style="width:100px" />
                <span style="font-size:12px">dy</span>
                <el-input-number v-model="step.y" :min="-127" :max="127" size="small" controls-position="right" style="width:100px" />
              </template>

              <template v-else-if="step.type === 'mouse_scroll'">
                <span style="font-size:12px">hx</span>
                <el-input-number v-model="step.x" :min="-127" :max="127" size="small" controls-position="right" style="width:100px" />
                <span style="font-size:12px">vy</span>
                <el-input-number v-model="step.y" :min="-127" :max="127" size="small" controls-position="right" style="width:100px" />
              </template>

              <template v-else-if="step.type === 'mouse_click'">
                <el-radio-group v-model="step.button" size="small">
                  <el-radio-button :label="1">Left</el-radio-button>
                  <el-radio-button :label="2">Right</el-radio-button>
                  <el-radio-button :label="4">Middle</el-radio-button>
                </el-radio-group>
              </template>

              <template v-else-if="step.type === 'delay'">
                <el-input-number v-model="step.delay_ms" :min="0" :max="60000" size="small" controls-position="right" style="width:140px" />
                <span style="font-size:12px; color:#909399">ms</span>
              </template>

              <div style="margin-left:auto; display:flex; gap:4px">
                <el-button size="small" link @click="duplicateStep(index)">Duplicate</el-button>
                <el-button size="small" link type="danger" @click="removeStep(index)">Delete</el-button>
              </div>
            </div>
          </template>
        </draggable>
      </div>

      <el-card style="width:300px; flex-shrink:0" header="JSON Preview">
        <pre style="max-height:560px; overflow:auto; margin:0; font-size:11px">{{ previewJson }}</pre>
      </el-card>
    </div>

    <template #footer>
      <el-button @click="editor.visible = false">Cancel</el-button>
      <el-button type="primary" @click="save">Save</el-button>
    </template>
  </el-dialog>

  <el-dialog v-model="runner.visible" :title="`Run Macro: ${runner.macro?.name || ''}`" width="700px">
    <el-form label-width="120px">
      <el-form-item label="Execution Mode">
        <el-tag :type="runnerMode === 'network_debug' ? 'info' : 'success'">
          {{ modeLabel(runnerMode) }}
        </el-tag>
      </el-form-item>

      <el-form-item label="Device">
        <el-select v-model="runner.deviceUuid" @change="loadSlotsForRunner" style="width:100%">
          <el-option
            v-for="device in runner.devices"
            :key="device.id"
            :label="`${device.name || device.device_uuid} ${runner.online.includes(device.device_uuid) ? '[online]' : '[offline]'}`"
            :value="device.device_uuid"
            :disabled="!runner.online.includes(device.device_uuid)"
          />
        </el-select>
      </el-form-item>

      <el-form-item v-if="runnerMode === 'ble_targeted'" label="Target Slots">
        <div v-if="!runner.slots.length" style="font-size:12px; color:#909399">
          No slots are configured on this device. Assign BLE slots from Device Control first.
        </div>
        <el-checkbox-group v-else v-model="runner.selectedSlots">
          <el-checkbox v-for="slot in runner.slots" :key="slot.slot_index" :label="slot.slot_index">
            slot {{ slot.slot_index }} / {{ slot.name || slot.addr }}
          </el-checkbox>
        </el-checkbox-group>
      </el-form-item>

      <el-form-item v-else label="Debug Note">
        <el-alert
          type="info"
          :closable="false"
          title="This run uses debug_no_hid + skip_link_check. It exercises only network delivery and ACK handling."
        />
      </el-form-item>
    </el-form>

    <el-card v-if="runner.results" header="Execution Results" style="margin-top:8px">
      <el-table :data="runner.results" size="small">
        <el-table-column label="Slot" width="70">
          <template #default="{ row }">{{ row.slot_index ?? '-' }}</template>
        </el-table-column>
        <el-table-column label="Target">
          <template #default="{ row }">{{ row.name || row.addr || '-' }}</template>
        </el-table-column>
        <el-table-column label="Switched" width="80">
          <template #default="{ row }">
            <el-tag v-if="row.switched" size="small">yes</el-tag>
            <span v-else style="font-size:12px; color:#909399">-</span>
          </template>
        </el-table-column>
        <el-table-column label="Status" width="110">
          <template #default="{ row }">
            <el-tag size="small" :type="row.status === 'acked' ? 'success' : 'danger'">
              {{ row.status }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="Detail">
          <template #default="{ row }">
            <span v-if="row.status === 'acked'">{{ row.exec_time_ms }} ms</span>
            <span v-else style="color:#f56c6c; font-size:12px">{{ row.error_code }} {{ row.error_message }}</span>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <template #footer>
      <el-button @click="runner.visible = false">Close</el-button>
      <el-button type="primary" :loading="runner.busy" @click="doRun">Send</el-button>
    </template>
  </el-dialog>
</template>

<style scoped>
.step-row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  background: #fafafa;
  border: 1px solid #ebeef5;
  border-radius: 6px;
}

.drag-handle {
  cursor: grab;
  user-select: none;
  color: #c0c4cc;
  padding: 0 6px;
  font-size: 18px;
  line-height: 1;
}

.drag-handle:active {
  cursor: grabbing;
}
</style>
