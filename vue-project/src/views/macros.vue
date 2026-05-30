<script>
export const meta = { title: '宏管理' }
</script>

<script setup>
import { onMounted, reactive, ref, computed } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import draggable from 'vuedraggable'
import { macrosApi } from '@/api/macros'
import { devicesApi } from '@/api/devices'
import { commandsApi } from '@/api/commands'

const KEYS = [
  'KEY_A','KEY_B','KEY_C','KEY_D','KEY_E','KEY_F','KEY_G','KEY_H','KEY_I','KEY_J',
  'KEY_K','KEY_L','KEY_M','KEY_N','KEY_O','KEY_P','KEY_Q','KEY_R','KEY_S','KEY_T',
  'KEY_U','KEY_V','KEY_W','KEY_X','KEY_Y','KEY_Z',
  'KEY_0','KEY_1','KEY_2','KEY_3','KEY_4','KEY_5','KEY_6','KEY_7','KEY_8','KEY_9',
  'KEY_ENTER','KEY_SPACE','KEY_TAB','KEY_ESC','KEY_BACKSPACE',
  'KEY_UP','KEY_DOWN','KEY_LEFT','KEY_RIGHT',
  'KEY_F1','KEY_F2','KEY_F3','KEY_F4','KEY_F5','KEY_F6','KEY_F7','KEY_F8','KEY_F9','KEY_F10','KEY_F11','KEY_F12',
  'KEY_LCTRL','KEY_LSHIFT','KEY_LALT','KEY_LMETA',
]

const STEP_TYPES = [
  { value: 'keyboard_tap',  label: '按一下', desc: '按下立即抬起' },
  { value: 'keyboard_down', label: '按下',   desc: '保持按下' },
  { value: 'keyboard_up',   label: '抬起',   desc: '释放按键' },
  { value: 'text',          label: '文本',   desc: '输入文字串' },
  { value: 'mouse_move',    label: '鼠标移动', desc: '相对位移 dx/dy' },
  { value: 'mouse_click',   label: '鼠标点击', desc: '左/中/右键' },
  { value: 'delay',         label: '延时',   desc: '等待若干毫秒' },
]

function newStep(type) {
  const id = Math.random().toString(36).slice(2, 10)
  switch (type) {
    case 'keyboard_tap':
    case 'keyboard_down':
    case 'keyboard_up':  return { id, type, key: 'KEY_A', repeat: 1 }
    case 'text':         return { id, type, content: '', interval_ms: 5 }
    case 'mouse_move':   return { id, type, x: 0, y: 0 }
    case 'mouse_click':  return { id, type, button: 1 }
    case 'delay':        return { id, type, delay_ms: 100 }
    default:             return { id, type }
  }
}

// ---------- list ----------
const rows = ref([])
const loading = ref(false)

async function load() {
  loading.value = true
  try { rows.value = await macrosApi.list() } finally { loading.value = false }
}
onMounted(load)

// ---------- editor ----------
const editor = reactive({
  visible: false,
  id: null,
  name: '',
  steps: [],            // 编辑用，会带 id 字段
})

function openCreate() {
  editor.visible = true
  editor.id = null
  editor.name = ''
  editor.steps = []
}

function openEdit(row) {
  editor.visible = true
  editor.id = row.id
  editor.name = row.name
  // 把已存的 steps 套上 id 方便拖拽
  const src = row.content_json?.steps || []
  editor.steps = src.map(s => ({ id: Math.random().toString(36).slice(2,10), repeat: 1, ...s }))
}

function addStep(type) {
  editor.steps.push(newStep(type))
}
function removeStep(idx) {
  editor.steps.splice(idx, 1)
}
function duplicateStep(idx) {
  const s = editor.steps[idx]
  editor.steps.splice(idx + 1, 0, { ...s, id: Math.random().toString(36).slice(2,10) })
}

// 把 step 摊平成执行格式：repeat>1 时复制 N 次
function flattenForSave(steps) {
  const out = []
  for (const s of steps) {
    const repeat = Math.max(1, parseInt(s.repeat) || 1)
    const { id, repeat: _, ...clean } = s
    for (let i = 0; i < repeat; i++) out.push(clean)
  }
  return out
}

async function save() {
  if (!editor.name.trim()) return ElMessage.warning('请填写宏名称')
  if (!editor.steps.length) return ElMessage.warning('至少添加一步')
  const steps = flattenForSave(editor.steps)
  if (editor.id == null) {
    await macrosApi.create({ name: editor.name, steps })
    ElMessage.success('已创建')
  } else {
    await macrosApi.update(editor.id, { name: editor.name, steps })
    ElMessage.success('已保存')
  }
  editor.visible = false
  load()
}

async function remove(row) {
  await ElMessageBox.confirm(`删除宏「${row.name}」？`, '确认', { type: 'warning' })
  await macrosApi.remove(row.id)
  ElMessage.success('已删除')
  load()
}

// ---------- run 触发 ----------
const runner = reactive({
  visible: false,
  macro: null,
  deviceUuid: '',
  selectedSlots: [],     // [1, 2]
  devices: [],
  online: [],
  slots: [],             // 当前选中设备的 slot 列表
  results: null,
  busy: false,
})

async function openRun(row) {
  runner.macro = row
  runner.visible = true
  runner.results = null
  runner.selectedSlots = []
  runner.deviceUuid = ''
  runner.slots = []
  // 拉设备 / 在线
  runner.devices = await devicesApi.list()
  runner.online = (await devicesApi.online()).online
  const firstOnline = runner.devices.find(d => runner.online.includes(d.device_uuid))
  if (firstOnline) {
    runner.deviceUuid = firstOnline.device_uuid
    await loadSlotsForRunner()
  }
}

async function loadSlotsForRunner() {
  if (!runner.deviceUuid) { runner.slots = []; return }
  try { runner.slots = await commandsApi.listSlots(runner.deviceUuid) || [] }
  catch { runner.slots = [] }
  runner.selectedSlots = []
}

async function doRun() {
  if (!runner.deviceUuid) return ElMessage.warning('请选择设备')
  if (!runner.selectedSlots.length) return ElMessage.warning('请至少选择一个目标主机')
  runner.busy = true
  try {
    // 把 macro 的 steps 包装成一个 type=macro 的 action payload，executor 就一口气跑完
    const action = { type: 'macro', steps: runner.macro.content_json?.steps || [] }
    const resp = await commandsApi.send(runner.deviceUuid, {
      slots: [...runner.selectedSlots],
      action,
      require_connected: true,
      switch_timeout_s: 5,
      action_timeout_ms: 10000,
    })
    runner.results = resp.results || []
    const fail = runner.results.filter(r => r.status !== 'acked')
    if (!fail.length) ElMessage.success(`全部 ${runner.results.length} 个目标完成`)
    else ElMessage.warning(`${fail.length}/${runner.results.length} 个目标失败`)
  } finally {
    runner.busy = false
  }
}

const previewJson = computed(() => JSON.stringify(flattenForSave(editor.steps), null, 2))
</script>

<template>
  <el-card>
    <div style="display:flex; justify-content:space-between; margin-bottom:12px">
      <el-button @click="load" :loading="loading">刷新</el-button>
      <el-button type="primary" @click="openCreate">新建宏</el-button>
    </div>
    <el-table :data="rows" v-loading="loading">
      <el-table-column prop="id" label="ID" width="60" />
      <el-table-column prop="name" label="名称" width="200" />
      <el-table-column label="步骤数" width="100">
        <template #default="{ row }">{{ row.content_json?.steps?.length ?? 0 }}</template>
      </el-table-column>
      <el-table-column label="内容预览">
        <template #default="{ row }">
          <code style="font-size:12px">{{ JSON.stringify(row.content_json?.steps?.slice(0,3) || []) }}{{ (row.content_json?.steps?.length || 0) > 3 ? '…' : '' }}</code>
        </template>
      </el-table-column>
      <el-table-column prop="created_at" label="创建时间" width="180" />
      <el-table-column label="操作" width="240">
        <template #default="{ row }">
          <el-button size="small" type="primary" @click="openRun(row)">运行</el-button>
          <el-button size="small" @click="openEdit(row)">编辑</el-button>
          <el-button size="small" type="danger" @click="remove(row)">删除</el-button>
        </template>
      </el-table-column>
    </el-table>
  </el-card>

  <el-dialog v-model="editor.visible" :title="editor.id == null ? '新建宏' : `编辑宏 #${editor.id}`" width="900px" top="5vh">
    <el-form label-width="60px" inline>
      <el-form-item label="名称">
        <el-input v-model="editor.name" style="width:280px" placeholder="例如：复制并粘贴" />
      </el-form-item>
    </el-form>

    <div style="display:flex; gap:12px">
      <!-- palette -->
      <el-card style="width:160px; flex-shrink:0" header="添加步骤">
        <el-button v-for="t in STEP_TYPES" :key="t.value" size="small"
                   @click="addStep(t.value)" style="width:100%; margin-bottom:6px">
          + {{ t.label }}
        </el-button>
      </el-card>

      <!-- step list -->
      <div style="flex:1; min-width:0">
        <el-empty v-if="!editor.steps.length" description="点击左侧按钮添加步骤" :image-size="80" />
        <draggable v-else v-model="editor.steps" item-key="id" handle=".drag-handle" animation="150"
                   style="display:flex; flex-direction:column; gap:8px">
          <template #item="{ element: step, index }">
            <div class="step-row">
              <div class="drag-handle" title="拖动排序">⋮⋮</div>
              <el-tag size="small" style="width:80px; text-align:center">{{ index + 1 }} · {{ STEP_TYPES.find(t => t.value === step.type)?.label || step.type }}</el-tag>

              <!-- 各类型不同字段 -->
              <template v-if="['keyboard_tap','keyboard_down','keyboard_up'].includes(step.type)">
                <el-select v-model="step.key" filterable size="small" style="width:160px">
                  <el-option v-for="k in KEYS" :key="k" :label="k" :value="k" />
                </el-select>
                <el-input-number v-model="step.repeat" :min="1" :max="999" size="small" controls-position="right" style="width:110px" />
                <span style="font-size:12px; color:#909399">次</span>
              </template>

              <template v-else-if="step.type === 'text'">
                <el-input v-model="step.content" placeholder="要输入的文字" size="small" style="flex:1; min-width:0" />
                <el-input-number v-model="step.interval_ms" :min="0" :max="500" size="small" controls-position="right" style="width:110px" />
                <span style="font-size:12px; color:#909399">ms/字符</span>
              </template>

              <template v-else-if="step.type === 'mouse_move'">
                <span style="font-size:12px">dx</span>
                <el-input-number v-model="step.x" :min="-127" :max="127" size="small" controls-position="right" style="width:100px" />
                <span style="font-size:12px">dy</span>
                <el-input-number v-model="step.y" :min="-127" :max="127" size="small" controls-position="right" style="width:100px" />
              </template>

              <template v-else-if="step.type === 'mouse_click'">
                <el-radio-group v-model="step.button" size="small">
                  <el-radio-button :label="1">左键</el-radio-button>
                  <el-radio-button :label="2">右键</el-radio-button>
                  <el-radio-button :label="4">中键</el-radio-button>
                </el-radio-group>
              </template>

              <template v-else-if="step.type === 'delay'">
                <el-input-number v-model="step.delay_ms" :min="0" :max="60000" size="small" controls-position="right" style="width:140px" />
                <span style="font-size:12px; color:#909399">毫秒</span>
              </template>

              <div style="margin-left:auto; display:flex; gap:4px">
                <el-button size="small" link @click="duplicateStep(index)">复制</el-button>
                <el-button size="small" link type="danger" @click="removeStep(index)">删除</el-button>
              </div>
            </div>
          </template>
        </draggable>
      </div>

      <!-- preview JSON -->
      <el-card style="width:280px; flex-shrink:0" header="JSON 预览">
        <pre style="max-height:560px; overflow:auto; margin:0; font-size:11px">{{ previewJson }}</pre>
      </el-card>
    </div>

    <template #footer>
      <el-button @click="editor.visible = false">取消</el-button>
      <el-button type="primary" @click="save">保存</el-button>
    </template>
  </el-dialog>

  <!-- 运行宏 -->
  <el-dialog v-model="runner.visible" :title="`运行宏：${runner.macro?.name || ''}`" width="640px">
    <el-form label-width="100px">
      <el-form-item label="目标设备">
        <el-select v-model="runner.deviceUuid" @change="loadSlotsForRunner" style="width:100%">
          <el-option v-for="d in runner.devices" :key="d.id"
            :label="`${d.name || d.device_uuid} ${runner.online.includes(d.device_uuid) ? '🟢' : '⚪'}`"
            :value="d.device_uuid" :disabled="!runner.online.includes(d.device_uuid)" />
        </el-select>
      </el-form-item>
      <el-form-item label="发送给主机">
        <div v-if="!runner.slots.length" style="font-size:12px; color:#909399">
          这台设备还没分配 slot，去"设备管理 → 控制 → BLE"先把已配对设备分到 slot
        </div>
        <el-checkbox-group v-else v-model="runner.selectedSlots">
          <el-checkbox v-for="s in runner.slots" :key="s.slot_index" :label="s.slot_index">
            slot {{ s.slot_index }} · {{ s.name || s.addr }}
          </el-checkbox>
        </el-checkbox-group>
      </el-form-item>
    </el-form>
    <el-card v-if="runner.results" header="执行结果" style="margin-top:8px">
      <el-table :data="runner.results" size="small">
        <el-table-column label="slot" width="60">
          <template #default="{ row }">{{ row.slot_index ?? '-' }}</template>
        </el-table-column>
        <el-table-column prop="addr" label="地址" />
        <el-table-column label="切换" width="60">
          <template #default="{ row }">
            <el-tag v-if="row.switched" size="small">是</el-tag>
            <span v-else style="font-size:12px; color:#909399">-</span>
          </template>
        </el-table-column>
        <el-table-column label="状态" width="100">
          <template #default="{ row }">
            <el-tag size="small" :type="row.status === 'acked' ? 'success' : 'danger'">{{ row.status }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="耗时 / 错误">
          <template #default="{ row }">
            <span v-if="row.status === 'acked'">{{ row.exec_time_ms }} ms</span>
            <span v-else style="color:#f56c6c; font-size:12px">{{ row.error_code }} {{ row.error_message }}</span>
          </template>
        </el-table-column>
      </el-table>
    </el-card>
    <template #footer>
      <el-button @click="runner.visible = false">关闭</el-button>
      <el-button type="primary" :loading="runner.busy" @click="doRun">发送</el-button>
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
.drag-handle:active { cursor: grabbing; }
</style>
