<script setup>
import { computed, reactive, watch } from 'vue'
import {
  DISPLAY_MODE_OPTIONS,
  DISPLAY_THEME_OPTIONS,
  buildDisplayPayload,
  clipScreenText,
  normalizeDisplayConfig,
} from '@/utils/displayPresets'

const props = defineProps({
  status: { type: Object, default: null },
  deviceName: { type: String, default: '' },
  loading: { type: Boolean, default: false },
  pcMonitorDefault: { type: Boolean, default: true },
  errorMessage: { type: String, default: '' },
  compact: { type: Boolean, default: false },
})

const emit = defineEmits(['refresh', 'save'])

const form = reactive(normalizeDisplayConfig(props.status, props.pcMonitorDefault))

watch(
  () => props.status,
  (val) => {
    if (val && typeof val === 'object') {
      Object.assign(form, normalizeDisplayConfig(val, props.pcMonitorDefault))
    }
  },
  { immediate: true, deep: true },
)

const activeTheme = computed(
  () => DISPLAY_THEME_OPTIONS.find((t) => t.value === form.theme) || DISPLAY_THEME_OPTIONS[0],
)

const previewOpacity = computed(() => {
  if (form.mode === 'off' || form.backlight <= 0) return 0.16
  return 0.35 + (form.backlight / 100) * 0.65
})

const previewHeader = computed(() => {
  if (form.header_text) return clipScreenText(form.header_text, 7)
  if (form.mode === 'monitor' && form.show_clock) {
    return props.status?.metrics?.time && props.status.metrics.time !== '00:00:00'
      ? clipScreenText(props.status.metrics.time, 8)
      : 'C3-DISP'
  }
  return clipScreenText(props.deviceName || 'ESP-DISP', 10)
})

const previewFooter = computed(() => {
  if (form.footer_text) return clipScreenText(form.footer_text, 10)
  if (form.mode === 'text') return 'CUSTOM TXT'
  if (form.mode === 'status') return '[ONLINE]'
  return props.status?.telemetry_active ? '[LIVE OK]' : '[WAIT PC]'
})

const previewMetrics = computed(() => ({
  cpu: Math.round(Number(props.status?.metrics?.cpu ?? 24)),
  ram: Math.round(Number(props.status?.metrics?.ram ?? 58)),
  disk: Math.round(Number(props.status?.metrics?.disk ?? 42)),
  down: props.status?.metrics?.down || '1.2M/s',
  up: props.status?.metrics?.up || '240K/s',
  uptime: props.status?.metrics?.uptime || '2h15m',
}))

function clearLines() {
  form.lines = ['', '', '', '', '', '']
}

function fillDemoLines() {
  form.mode = 'text'
  form.lines = [
    clipScreenText(props.deviceName || 'DESK-C3', 10),
    'STATUS: OK',
    'MODE: SIGN',
    'WIFI LINK',
    'HUB READY',
    'EDIT ME :)',
  ]
}

function applyPreset(preset) {
  if (preset === 'monitor') {
    Object.assign(form, {
      mode: 'monitor',
      backlight: Math.max(form.backlight, 80),
      show_clock: true,
      show_cpu: true,
      show_ram: true,
      show_disk: true,
      show_net: true,
      show_uptime: true,
    })
  } else if (preset === 'compact_net') {
    Object.assign(form, {
      mode: 'monitor',
      backlight: Math.max(form.backlight, 80),
      show_clock: true,
      show_cpu: true,
      show_ram: true,
      show_disk: false,
      show_net: true,
      show_uptime: false,
    })
  } else if (preset === 'status') {
    form.mode = 'status'
    form.backlight = Math.max(form.backlight, 75)
  } else if (preset === 'off') {
    form.mode = 'off'
  }
}

function submit() {
  emit('save', buildDisplayPayload(form))
}
</script>

<template>
  <div class="display-config-wrap" :class="{ 'is-compact': compact }">
    <div v-if="errorMessage" class="legacy-fw-alert">
      <strong>提示：</strong>{{ errorMessage }}
      <span v-if="errorMessage.includes('E404') || errorMessage.includes('disabled')">
        （当前设备运行的固件尚未开启可配置屏幕协议，请在「统一芯片 &amp; 云编译中心」对 `esp32c3_pc_monitor.bin` 执行一次无线 Pull OTA 后即可生效）
      </span>
    </div>

    <div class="display-grid">
      <!-- Live 80x160 ST7735 Simulator -->
      <div class="screen-preview-col">
        <div class="preview-caption">80×160 竖屏效果预览</div>
        <div class="st7735-bezel">
          <div class="st7735-screen" :style="{ opacity: previewOpacity }">
            <template v-if="form.mode === 'off'">
              <div class="screen-off-state">BACKLIGHT OFF</div>
            </template>
            <template v-else>
              <div class="scr-hdr" :style="{ color: activeTheme.header }">
                <span>{{ previewHeader }}</span>
                <span v-if="form.mode === 'monitor'" :style="{ color: status?.telemetry_active ? activeTheme.ok : '#ef4444' }">
                  {{ status?.telemetry_active ? 'OK' : '--' }}
                </span>
              </div>
              <div class="scr-divider" />

              <div v-if="form.mode === 'text'" class="scr-body scr-lines">
                <div
                  v-for="(ln, idx) in form.lines"
                  :key="idx"
                  class="scr-line"
                  :style="{ color: idx === 0 ? activeTheme.a1 : idx === 5 ? activeTheme.a2 : '#f8fafc' }"
                >
                  {{ clipScreenText(ln, 10) || '\u00a0' }}
                </div>
              </div>

              <div v-else-if="form.mode === 'status'" class="scr-body scr-lines">
                <div class="scr-line" :style="{ color: activeTheme.ok }">WF:CONNECTED</div>
                <div class="scr-line" style="color: #fff">IP:192.168</div>
                <div class="scr-line" :style="{ color: activeTheme.a1 }">.68.210</div>
                <div class="scr-line" :style="{ color: activeTheme.a2 }">MEM:112K</div>
                <div class="scr-line" :style="{ color: activeTheme.a3 }">UP:{{ previewMetrics.uptime }}</div>
                <div class="scr-line" style="color: #cbd5e1">{{ clipScreenText(form.lines[0], 10) }}</div>
              </div>

              <div v-else class="scr-body scr-mon">
                <div v-if="form.show_cpu" class="mon-blk">
                  <div class="mon-row">
                    <span :style="{ color: activeTheme.a1 }">CPU:</span>
                    <span>{{ previewMetrics.cpu }}%</span>
                  </div>
                  <div class="mon-bar"><div class="mon-fill" :style="{ width: `${previewMetrics.cpu}%`, background: activeTheme.ok }" /></div>
                </div>
                <div v-if="form.show_ram" class="mon-blk">
                  <div class="mon-row">
                    <span :style="{ color: activeTheme.a2 }">RAM:</span>
                    <span>{{ previewMetrics.ram }}%</span>
                  </div>
                  <div class="mon-bar"><div class="mon-fill" :style="{ width: `${previewMetrics.ram}%`, background: activeTheme.a1 }" /></div>
                </div>
                <div v-if="form.show_disk" class="mon-blk">
                  <div class="mon-row">
                    <span :style="{ color: activeTheme.a3 }">DISK:</span>
                    <span>{{ previewMetrics.disk }}%</span>
                  </div>
                  <div class="mon-bar"><div class="mon-fill" :style="{ width: `${previewMetrics.disk}%`, background: activeTheme.a3 }" /></div>
                </div>
                <div v-if="form.show_net" class="mon-net">
                  <div><span :style="{ color: activeTheme.ok }">D:</span> {{ previewMetrics.down }}</div>
                  <div><span :style="{ color: activeTheme.a1 }">U:</span> {{ previewMetrics.up }}</div>
                </div>
                <div v-if="form.show_uptime" class="mon-up">
                  <span style="color: #facc15">UP:</span> {{ previewMetrics.uptime }}
                </div>
                <div v-if="form.lines[0]" class="mon-note" :style="{ color: activeTheme.a1 }">
                  {{ clipScreenText(form.lines[0], 10) }}
                </div>
              </div>

              <div class="scr-divider" />
              <div class="scr-ftr" :style="{ color: form.footer_text ? activeTheme.header : '#94a3b8' }">
                {{ previewFooter }}
              </div>
            </template>
          </div>
        </div>
        <div class="preview-meta">
          <div>驱动: <code>{{ status?.driver || 'ST7735_80x160' }}</code></div>
          <div v-if="status?.last_key">摇杆: <code>{{ status.last_key }}</code> (#{{ status.joystick_seq || 0 }})</div>
        </div>
      </div>

      <!-- Configuration Form -->
      <div class="screen-form-col">
        <div class="toolbar-row">
          <div class="quick-presets">
            <el-button size="small" @click="applyPreset('monitor')">完整监控</el-button>
            <el-button size="small" @click="applyPreset('compact_net')">精简监控</el-button>
            <el-button size="small" @click="applyPreset('status')">独立状态卡</el-button>
            <el-button size="small" @click="fillDemoLines">自定义看板示例</el-button>
            <el-button size="small" type="warning" plain @click="applyPreset('off')">息屏</el-button>
          </div>
          <el-button size="small" :loading="loading" @click="emit('refresh')">读取当前</el-button>
        </div>

        <div class="cfg-block">
          <div class="cfg-label">显示模式</div>
          <div class="mode-pills">
            <button
              v-for="m in DISPLAY_MODE_OPTIONS"
              :key="m.value"
              type="button"
              class="mode-pill"
              :class="{ active: form.mode === m.value }"
              :title="m.desc"
              @click="form.mode = m.value"
            >
              {{ m.label }}
            </button>
          </div>
        </div>

        <div class="cfg-two-col">
          <div class="cfg-block">
            <div class="cfg-label">背光亮度 ({{ form.backlight }}%)</div>
            <el-slider v-model="form.backlight" :min="0" :max="100" :step="5" />
          </div>

          <div class="cfg-block">
            <div class="cfg-label">色彩主题</div>
            <el-select v-model="form.theme" size="small" style="width: 100%">
              <el-option v-for="t in DISPLAY_THEME_OPTIONS" :key="t.value" :label="t.label" :value="t.value" />
            </el-select>
          </div>
        </div>

        <div class="cfg-two-col">
          <div class="cfg-block">
            <div class="cfg-label">顶部标题 (留空自动, 建议≤10 ASCII)</div>
            <el-input v-model="form.header_text" size="small" maxlength="20" clearable placeholder="如 DESK-C3 / NAS" />
          </div>
          <div class="cfg-block">
            <div class="cfg-label">底部角标 (留空自动)</div>
            <el-input v-model="form.footer_text" size="small" maxlength="20" clearable placeholder="如 [MONITOR]" />
          </div>
        </div>

        <div v-if="form.mode === 'monitor'" class="cfg-block">
          <div class="cfg-label">监控显示内容开关</div>
          <div class="toggle-grid">
            <el-checkbox v-model="form.show_clock">顶部时钟</el-checkbox>
            <el-checkbox v-model="form.show_cpu">CPU 占用</el-checkbox>
            <el-checkbox v-model="form.show_ram">内存 RAM</el-checkbox>
            <el-checkbox v-model="form.show_disk">磁盘空间</el-checkbox>
            <el-checkbox v-model="form.show_net">上下行网速</el-checkbox>
            <el-checkbox v-model="form.show_uptime">运行时长</el-checkbox>
          </div>
        </div>

        <div class="cfg-block">
          <div class="cfg-label-row">
            <span class="cfg-label">
              {{ form.mode === 'text' ? '自定义文本内容（第 0~5 行，每行约 10 英文/数字字符）' : '自定义备注行（自定义看板全显，监控/状态底部展示第 0 行）' }}
            </span>
            <el-button size="small" link @click="clearLines">清空行内容</el-button>
          </div>
          <div class="lines-grid">
            <el-input
              v-for="(_, idx) in form.lines"
              :key="idx"
              v-model="form.lines[idx]"
              size="small"
              maxlength="36"
              :placeholder="`第 ${idx} 行`"
            >
              <template #prepend>L{{ idx }}</template>
            </el-input>
          </div>
        </div>

        <div class="submit-row">
          <el-button type="primary" :loading="loading" @click="submit">保存并应用到 C3 屏幕</el-button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.display-config-wrap {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.legacy-fw-alert {
  padding: 10px 12px;
  border-radius: 8px;
  background: #fef3c7;
  border: 1px solid #f59e0b;
  color: #92400e;
  font-size: 12px;
  line-height: 1.5;
}

.display-grid {
  display: grid;
  grid-template-columns: 164px 1fr;
  gap: 16px;
  align-items: start;
}

.is-compact .display-grid {
  grid-template-columns: 1fr;
}

.screen-preview-col {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 8px;
}

.preview-caption {
  font-size: 11px;
  font-weight: 600;
  color: var(--color-text-muted, #64748b);
}

.st7735-bezel {
  padding: 8px;
  border-radius: 12px;
  background: #0f172a;
  border: 2px solid #334155;
  box-shadow: 0 6px 18px rgba(15, 23, 42, 0.25);
}

.st7735-screen {
  width: 120px;
  height: 240px;
  background: #000000;
  color: #f8fafc;
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 11px;
  line-height: 1.3;
  padding: 4px;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  transition: opacity 0.15s ease;
}

.screen-off-state {
  margin: auto;
  color: #64748b;
  font-size: 10px;
  letter-spacing: 0.5px;
}

.scr-hdr, .scr-ftr {
  display: flex;
  justify-content: space-between;
  font-weight: 700;
  white-space: nowrap;
  overflow: hidden;
}

.scr-divider {
  height: 1px;
  background: #475569;
  margin: 4px 0;
}

.scr-body {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.scr-lines {
  justify-content: space-around;
}

.scr-line {
  white-space: nowrap;
  overflow: hidden;
}

.scr-mon {
  gap: 6px;
}

.mon-row {
  display: flex;
  justify-content: space-between;
}

.mon-bar {
  height: 6px;
  border: 1px solid #475569;
  padding: 1px;
  margin-top: 2px;
}

.mon-fill {
  height: 100%;
}

.mon-net, .mon-up, .mon-note {
  font-size: 10.5px;
  white-space: nowrap;
  overflow: hidden;
}

.preview-meta {
  font-size: 11px;
  color: var(--color-text-muted, #64748b);
  text-align: center;
  line-height: 1.5;
}

.screen-form-col {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.toolbar-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.quick-presets {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
}

.cfg-block {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.cfg-label-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.cfg-label {
  font-size: 12px;
  font-weight: 600;
  color: var(--color-text-secondary, #334155);
}

.cfg-two-col {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
}

.mode-pills {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 6px;
}

.mode-pill {
  padding: 6px 8px;
  border-radius: 6px;
  border: 1px solid var(--color-border, #cbd5e1);
  background: var(--color-bg-base, #f8fafc);
  color: var(--color-text-primary, #1e293b);
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.15s ease;
}

.mode-pill.active {
  border-color: var(--color-primary, #2563eb);
  background: #eff6ff;
  color: var(--color-primary, #2563eb);
}

.toggle-grid {
  display: flex;
  flex-wrap: wrap;
  gap: 6px 14px;
}

.lines-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 8px;
}

.submit-row {
  display: flex;
  justify-content: flex-end;
  padding-top: 4px;
}

@media (max-width: 640px) {
  .display-grid, .cfg-two-col, .lines-grid, .mode-pills {
    grid-template-columns: 1fr;
  }
}
</style>
