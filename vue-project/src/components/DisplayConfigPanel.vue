<script setup>
import { computed, nextTick, reactive, ref, watch } from 'vue'
import {
  DISPLAY_MODE_OPTIONS,
  DISPLAY_PANEL_OPTIONS,
  DISPLAY_ROTATION_OPTIONS,
  DISPLAY_THEME_OPTIONS,
  EPD_DEFAULT_PINS,
  EPD_WIRING_GUIDE,
  buildDisplayPayload,
  clipScreenText,
  getPanelDimensions,
  getPanelSpec,
  hasRedInk,
  isEpdPanel,
  isLineRed,
  normalizeDisplayConfig,
  stripRedPrefix,
} from '@/utils/displayPresets'
import {
  buildEpdImagePayload,
  generateTestPatternRgba,
  quantizeRgbaToEpd,
} from '@/utils/epdImage'

const props = defineProps({
  status: { type: Object, default: null },
  deviceName: { type: String, default: '' },
  loading: { type: Boolean, default: false },
  pcMonitorDefault: { type: Boolean, default: true },
  errorMessage: { type: String, default: '' },
  compact: { type: Boolean, default: false },
})

const emit = defineEmits(['refresh', 'save', 'sendImage'])

const form = reactive(normalizeDisplayConfig(props.status, props.pcMonitorDefault))
const showWiring = ref(false)

// E-Paper Image Studio state
const fileInputRef = ref(null)
const epdCanvasRef = ref(null)
const sourceImageEl = ref(null)
const imgState = reactive({
  name: '',
  colorMode: 'bwr', // 'bwr' (tri-color) | 'bw' (monochrome)
  algorithm: 'dither', // 'dither' | 'threshold'
  fit: 'contain', // 'contain' | 'cover'
  brightness: 0,
  redSensitivity: 1.0,
  prepared: false,
  stats: null,
  lastBuilt: null,
})

watch(
  () => props.status,
  (val) => {
    if (val && typeof val === 'object') {
      Object.assign(form, normalizeDisplayConfig(val, props.pcMonitorDefault))
    }
  },
  { immediate: true, deep: true },
)

const activePanelSpec = computed(() => getPanelSpec(form.panel))
const isEpd = computed(() => isEpdPanel(form.panel))
const isThreeColor = computed(() => hasRedInk(form.panel))
const dims = computed(() => getPanelDimensions(form.panel, form.rotation))
const maxCharsPerLine = computed(() => (dims.value.w >= 300 ? 22 : dims.value.w >= 200 ? 28 : 14))

const availablePanels = computed(() => {
  if (props.pcMonitorDefault) return DISPLAY_PANEL_OPTIONS
  return DISPLAY_PANEL_OPTIONS.filter((p) => p.value !== 'st7735_160')
})
const availableModes = computed(() =>
  isEpd.value ? DISPLAY_MODE_OPTIONS : DISPLAY_MODE_OPTIONS.filter((m) => m.value !== 'image'),
)

watch(
  isThreeColor,
  (hasRed) => {
    if (!hasRed && imgState.colorMode === 'bwr') {
      imgState.colorMode = 'bw'
    } else if (hasRed && !imgState.prepared) {
      imgState.colorMode = 'bwr'
    }
  },
  { immediate: true },
)

const activeTheme = computed(
  () => DISPLAY_THEME_OPTIONS.find((t) => t.value === form.theme) || DISPLAY_THEME_OPTIONS[0],
)

const previewOpacity = computed(() => {
  if (isEpd.value) return 1
  if (form.mode === 'off' || form.backlight <= 0) return 0.16
  return 0.35 + (form.backlight / 100) * 0.65
})

const previewHeader = computed(() => {
  if (form.header_text) return clipScreenText(form.header_text, maxCharsPerLine.value)
  if (form.mode === 'monitor' && form.show_clock && props.pcMonitorDefault) {
    return props.status?.metrics?.time && props.status.metrics.time !== '00:00:00'
      ? clipScreenText(props.status.metrics.time, 8)
      : 'C3-DISP'
  }
  return clipScreenText(props.deviceName || 'ESP-DISP', maxCharsPerLine.value)
})

const previewFooter = computed(() => {
  if (form.footer_text) return clipScreenText(form.footer_text, maxCharsPerLine.value)
  if (isEpd.value) {
    const cnt = props.status?.refresh_count ? ` #${props.status.refresh_count}` : ''
    return `${form.panel}${cnt}`
  }
  if (form.mode === 'text') return 'CUSTOM TXT'
  if (form.mode === 'status') return '[ONLINE]'
  return props.status?.telemetry_active ? '[LIVE OK]' : '[WAIT PC]'
})

const epdPreviewStyle = computed(() => {
  const { w, h } = dims.value
  const targetMaxPx = props.compact ? 220 : 250
  const scale = Math.min(targetMaxPx / Math.max(w, h), w >= 300 ? 0.62 : 0.96)
  return {
    width: `${Math.max(118, Math.round(w * scale))}px`,
    height: `${Math.max(118, Math.round(h * scale))}px`,
  }
})

const epdHeaderBg = computed(() => {
  if (!isEpd.value) return 'transparent'
  const wantRedHdr = isThreeColor.value && ((form.red_mask & 0x40) !== 0 || form.theme === 'red_bar') && form.theme !== 'mono'
  if (wantRedHdr) return '#b91c1c'
  return form.invert ? '#f6f4ec' : '#18181b'
})

const previewMetrics = computed(() => ({
  cpu: Math.round(Number(props.status?.metrics?.cpu ?? 24)),
  ram: Math.round(Number(props.status?.metrics?.ram ?? 58)),
  disk: Math.round(Number(props.status?.metrics?.disk ?? 42)),
  down: props.status?.metrics?.down || '1.2M/s',
  up: props.status?.metrics?.up || '240K/s',
  uptime: props.status?.metrics?.uptime || '2h15m',
}))

function toggleLineRed(idx) {
  form.red_mask = Number(form.red_mask || 0) ^ (1 << idx)
}

function toggleHeaderRed() {
  form.red_mask = Number(form.red_mask || 0) ^ 0x40
}

function isRedBit(idx) {
  return (Number(form.red_mask || 0) & (1 << idx)) !== 0
}

function previewLineText(raw) {
  return clipScreenText(stripRedPrefix(raw).text, maxCharsPerLine.value)
}

function clearLines() {
  form.lines = ['', '', '', '', '', '']
}

function fillDemoLines() {
  form.mode = 'text'
  if (isEpd.value) {
    form.lines = [
      clipScreenText(props.deviceName || 'ESP32-S3 DESK', 20),
      '[R]STATUS: ONLINE',
      'PANEL: ' + activePanelSpec.value.shortLabel.slice(0, 14),
      'WIFI: CONNECTED',
      'SSD1683 E-INK OK',
      'HUB AUTO SYNC',
    ]
  } else {
    form.lines = [
      clipScreenText(props.deviceName || 'DESK-C3', 10),
      'STATUS: OK',
      'MODE: SIGN',
      'WIFI LINK',
      'HUB READY',
      'EDIT ME :)',
    ]
  }
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

function submit(extras = null) {
  if (isEpd.value && form.mode === 'image' && imgState.lastBuilt) {
    submitImage(extras?.full_refresh !== undefined ? Boolean(extras.full_refresh) : true)
    return
  }
  emit('save', buildDisplayPayload(form, extras))
}

function renderImageCanvas() {
  if (!isEpd.value || form.mode !== 'image') return
  const canvas = epdCanvasRef.value
  if (!canvas) return
  const { w, h } = dims.value
  canvas.width = w
  canvas.height = h
  const ctx = canvas.getContext('2d', { willReadFrequently: true })
  if (!ctx) return

  let srcRgba = null
  if (sourceImageEl.value) {
    ctx.fillStyle = '#ffffff'
    ctx.fillRect(0, 0, w, h)
    const img = sourceImageEl.value
    const sw = img.naturalWidth || img.width || w
    const sh = img.naturalHeight || img.height || h
    const scale =
      imgState.fit === 'cover' ? Math.max(w / sw, h / sh) : Math.min(w / sw, h / sh)
    const dw = sw * scale
    const dh = sh * scale
    const dx = (w - dw) / 2
    const dy = (h - dh) / 2
    ctx.drawImage(img, dx, dy, dw, dh)
    srcRgba = ctx.getImageData(0, 0, w, h).data
  } else if (imgState.prepared) {
    srcRgba = generateTestPatternRgba(w, h, imgState.colorMode)
  } else {
    imgState.lastBuilt = null
    imgState.stats = null
    return
  }

  const q = quantizeRgbaToEpd(srcRgba, w, h, {
    colorMode: imgState.colorMode,
    algorithm: imgState.algorithm,
    invert: form.invert,
    brightness: imgState.brightness,
    redSensitivity: imgState.redSensitivity,
  })
  const outImgData = new ImageData(q.previewRgba, w, h)
  ctx.putImageData(outImgData, 0, 0)
  imgState.stats = q.stats
  imgState.lastBuilt = {
    bw: q.bw,
    red: q.red,
    width: w,
    height: h,
    colorMode: imgState.colorMode,
  }
}

watch(
  [
    () => form.mode,
    () => form.panel,
    () => form.rotation,
    () => form.invert,
    () => imgState.colorMode,
    () => imgState.algorithm,
    () => imgState.fit,
    () => imgState.brightness,
    () => imgState.redSensitivity,
    () => imgState.prepared,
  ],
  async () => {
    if (isEpd.value && form.mode === 'image' && imgState.prepared) {
      await nextTick()
      renderImageCanvas()
    }
  },
)

function triggerImagePicker() {
  fileInputRef.value?.click()
}

function onImageFileSelected(ev) {
  const file = ev?.target?.files?.[0]
  if (!file) return
  const reader = new FileReader()
  reader.onload = () => {
    const img = new Image()
    img.onload = async () => {
      sourceImageEl.value = img
      imgState.name = file.name
      imgState.prepared = true
      form.mode = 'image'
      await nextTick()
      renderImageCanvas()
    }
    img.src = String(reader.result || '')
  }
  reader.readAsDataURL(file)
  if (ev?.target) ev.target.value = ''
}

async function loadTestPattern(mode) {
  sourceImageEl.value = null
  imgState.colorMode = mode
  imgState.name = mode === 'bwr' ? '三色测试样张 (B/W/R)' : '黑白测试样张 (B/W)'
  imgState.prepared = true
  form.mode = 'image'
  await nextTick()
  renderImageCanvas()
}

function submitImage(fullRefresh = true) {
  if (!imgState.lastBuilt) {
    emit('save', buildDisplayPayload(form, { mode: 'image', refresh: true, full_refresh: fullRefresh }))
    return
  }
  const payload = buildEpdImagePayload({
    ...imgState.lastBuilt,
    panel: form.panel,
    rotation: form.rotation,
    invert: form.invert,
    fullRefresh,
  })
  emit('sendImage', payload)
}

function resetPinsDefault() {
  form.pins = { mosi: -1, clk: -1, cs: -1, dc: -1, rst: -1, busy: -1 }
}

const effectivePins = computed(() => {
  const rp = props.status?.resolved_pins || {}
  return {
    mosi: form.pins.mosi >= 0 ? form.pins.mosi : (rp.mosi ?? EPD_DEFAULT_PINS.mosi),
    clk: form.pins.clk >= 0 ? form.pins.clk : (rp.clk ?? EPD_DEFAULT_PINS.clk),
    cs: form.pins.cs >= 0 ? form.pins.cs : (rp.cs ?? EPD_DEFAULT_PINS.cs),
    dc: form.pins.dc >= 0 ? form.pins.dc : (rp.dc ?? EPD_DEFAULT_PINS.dc),
    rst: form.pins.rst >= 0 ? form.pins.rst : (rp.rst ?? EPD_DEFAULT_PINS.rst),
    busy: form.pins.busy >= 0 ? form.pins.busy : (rp.busy ?? EPD_DEFAULT_PINS.busy),
  }
})
</script>

<template>
  <div class="display-config-wrap" :class="{ 'is-compact': compact, 'is-epd': isEpd }">
    <div v-if="errorMessage" class="legacy-fw-alert">
      <strong>提示：</strong>{{ errorMessage }}
      <span v-if="errorMessage.includes('E404') || errorMessage.includes('disabled')">
        （当前设备运行的固件尚未开启可配置屏幕协议，请在「统一芯片中心」对最新固件执行一次无线 Pull OTA 后即可生效）
      </span>
    </div>

    <div v-if="status?.busy_timeout" class="epd-warn-alert">
      <strong>⚠️ 墨水屏 BUSY 信号超时：</strong>
      请检查驱动板 v1.4 与 S3 接线（特别是 <code>BUSY -> GPIO{{ effectivePins.busy }}</code>、<code>3V3</code> 供电、FPC 金手指插紧，并将驱动板底部采样电阻开关拨向右侧 <b>0.47Ω</b>）。
    </div>

    <div class="display-grid">
      <!-- Live LCD / E-Paper Preview Column -->
      <div class="screen-preview-col">
        <div class="preview-caption">
          {{ activePanelSpec.shortLabel }} · {{ dims.w }}×{{ dims.h }}
          <span v-if="isThreeColor" class="bwr-pill">红黑白三色</span>
          <span v-else-if="isEpd" class="bw-pill">黑白墨水</span>
        </div>

        <div class="screen-bezel" :class="{ 'epd-bezel': isEpd }">
          <div
            class="screen-surface"
            :class="{
              'epd-paper': isEpd,
              'epd-inverted': isEpd && form.invert,
            }"
            :style="{ ...(isEpd ? epdPreviewStyle : {}), opacity: previewOpacity }"
          >
            <template v-if="form.mode === 'off'">
              <div class="screen-off-state">
                {{ isEpd ? 'E-INK CLEARED / SLEEP' : 'BACKLIGHT OFF' }}
              </div>
            </template>
            <template v-else-if="form.mode === 'image' && isEpd">
              <canvas v-show="imgState.prepared" ref="epdCanvasRef" class="epd-img-canvas" />
              <div v-if="!imgState.prepared" class="screen-off-state" style="text-align: center; padding: 10px">
                {{
                  status?.has_image
                    ? `设备已存图片 (${status.image_width}×${status.image_height} · ${status.image_color_mode === 'bwr' ? '三色' : '黑白'})`
                    : '请选择图片或点击测试图'
                }}
              </div>
            </template>
            <template v-else>
              <div
                class="scr-hdr"
                :class="{ 'epd-hdr': isEpd }"
                :style="{
                  background: epdHeaderBg,
                  color: isEpd ? (form.invert && epdHeaderBg === '#f6f4ec' ? '#18181b' : '#ffffff') : activeTheme.header,
                }"
              >
                <span>{{ previewHeader }}</span>
                <span
                  v-if="isEpd"
                  style="font-size: 9px; opacity: 0.9"
                >
                  {{ form.mode.toUpperCase() }}
                </span>
                <span
                  v-else-if="form.mode === 'monitor'"
                  :style="{ color: status?.telemetry_active ? activeTheme.ok : '#ef4444' }"
                >
                  {{ status?.telemetry_active ? 'OK' : '--' }}
                </span>
              </div>
              <div v-if="!isEpd" class="scr-divider" />

              <div v-if="form.mode === 'text'" class="scr-body scr-lines" :style="isEpd ? { padding: '4px 5px' } : {}">
                <div
                  v-for="(ln, idx) in form.lines"
                  :key="idx"
                  class="scr-line"
                  :style="{
                    color: isEpd
                      ? (isLineRed(form, idx) ? '#b91c1c' : (form.invert ? '#f6f4ec' : '#18181b'))
                      : (idx === 0 ? activeTheme.a1 : idx === 5 ? activeTheme.a2 : '#f8fafc'),
                    fontSize: isEpd && dims.w >= 300 ? '12px' : undefined,
                  }"
                >
                  {{ previewLineText(ln) || '\u00a0' }}
                </div>
              </div>

              <div v-else-if="form.mode === 'status'" class="scr-body scr-lines" :style="isEpd ? { padding: '4px 5px' } : {}">
                <div class="scr-line" :style="{ color: isEpd ? (isThreeColor ? '#b91c1c' : '#18181b') : activeTheme.ok }">
                  IP:192.168.68.143
                </div>
                <div class="scr-line" :style="{ color: isEpd ? '#18181b' : activeTheme.a1 }">
                  WF:Connected (-22dBm)
                </div>
                <div class="scr-line" :style="{ color: isEpd ? '#18181b' : activeTheme.a2 }">
                  HEAP:320K PSRAM:8M
                </div>
                <div class="scr-line" :style="{ color: isEpd ? '#18181b' : activeTheme.a3 }">
                  UP:{{ previewMetrics.uptime }}
                </div>
                <div class="scr-line" :style="{ color: isEpd && isLineRed(form, 0) ? '#b91c1c' : undefined }">
                  {{ previewLineText(form.lines[0]) || 'Ready' }}
                </div>
              </div>

              <div v-else class="scr-body scr-mon" :style="isEpd ? { padding: '4px 5px' } : {}">
                <div v-if="form.show_cpu" class="mon-blk">
                  <div class="mon-row">
                    <span :style="{ color: isEpd ? '#18181b' : activeTheme.a1 }">CPU:</span>
                    <span>{{ previewMetrics.cpu }}%</span>
                  </div>
                  <div class="mon-bar" :style="{ borderColor: isEpd ? '#18181b' : '#475569' }">
                    <div
                      class="mon-fill"
                      :style="{
                        width: `${previewMetrics.cpu}%`,
                        background: isEpd ? (isThreeColor ? '#b91c1c' : '#18181b') : activeTheme.ok,
                      }"
                    />
                  </div>
                </div>
                <div v-if="form.show_ram" class="mon-blk">
                  <div class="mon-row">
                    <span :style="{ color: isEpd ? '#18181b' : activeTheme.a2 }">RAM:</span>
                    <span>{{ previewMetrics.ram }}%</span>
                  </div>
                  <div class="mon-bar" :style="{ borderColor: isEpd ? '#18181b' : '#475569' }">
                    <div
                      class="mon-fill"
                      :style="{
                        width: `${previewMetrics.ram}%`,
                        background: isEpd ? '#18181b' : activeTheme.a1,
                      }"
                    />
                  </div>
                </div>
                <div v-if="form.show_disk" class="mon-blk">
                  <div class="mon-row">
                    <span :style="{ color: isEpd ? '#18181b' : activeTheme.a3 }">DISK:</span>
                    <span>{{ previewMetrics.disk }}%</span>
                  </div>
                  <div class="mon-bar" :style="{ borderColor: isEpd ? '#18181b' : '#475569' }">
                    <div
                      class="mon-fill"
                      :style="{
                        width: `${previewMetrics.disk}%`,
                        background: isEpd ? '#18181b' : activeTheme.a3,
                      }"
                    />
                  </div>
                </div>
                <div v-if="form.show_net" class="mon-net">
                  <span>D:{{ previewMetrics.down }} U:{{ previewMetrics.up }}</span>
                </div>
              </div>

              <div class="scr-divider" :style="{ background: isEpd ? '#18181b' : '#475569' }" />
              <div
                class="scr-ftr"
                :style="{
                  color: isEpd
                    ? ((isThreeColor && (form.red_mask & 0x80)) ? '#b91c1c' : '#3f3f46')
                    : (form.footer_text ? activeTheme.header : '#94a3b8'),
                  padding: isEpd ? '0 4px 2px' : undefined,
                }"
              >
                {{ previewFooter }}
              </div>
            </template>
          </div>
        </div>

        <div class="preview-meta">
          <div>驱动: <code>{{ status?.driver || activePanelSpec.chip }}</code></div>
          <div v-if="isEpd && status">
            刷新: <b>{{ status.refresh_count || 0 }}</b> 次
            <span v-if="status.last_refresh_ms">· 耗时 {{ status.last_refresh_ms }}ms</span>
            <el-tag v-if="status.refreshing" size="small" type="warning" style="margin-left: 4px">刷屏中...</el-tag>
          </div>
          <div v-if="status?.last_key">摇杆: <code>{{ status.last_key }}</code> (#{{ status.joystick_seq || 0 }})</div>
        </div>
      </div>

      <!-- Configuration Form -->
      <div class="screen-form-col">
        <div class="toolbar-row">
          <div class="quick-presets">
            <el-button size="small" @click="applyPreset('status')">节点状态卡</el-button>
            <el-button size="small" @click="fillDemoLines">自定义看板</el-button>
            <el-button size="small" @click="applyPreset('monitor')">监控仪表</el-button>
            <el-button size="small" type="warning" plain @click="applyPreset('off')">
              {{ isEpd ? '清屏休眠' : '息屏' }}
            </el-button>
          </div>
          <el-button size="small" :loading="loading" @click="emit('refresh')">读取当前</el-button>
        </div>

        <!-- Panel Selector & Rotation (for E-Paper / S3 Display) -->
        <div v-if="!pcMonitorDefault || isEpd" class="cfg-two-col">
          <div class="cfg-block">
            <div class="cfg-label">硬件屏幕型号</div>
            <el-select v-model="form.panel" size="small" style="width: 100%">
              <el-option
                v-for="p in availablePanels"
                :key="p.value"
                :label="p.label"
                :value="p.value"
              />
            </el-select>
          </div>
          <div class="cfg-block">
            <div class="cfg-label">显示方向</div>
            <el-select v-model="form.rotation" size="small" style="width: 100%">
              <el-option
                v-for="r in DISPLAY_ROTATION_OPTIONS"
                :key="r.value"
                :label="r.label"
                :value="r.value"
              />
            </el-select>
          </div>
        </div>

        <div class="cfg-block">
          <div class="cfg-label">显示模式</div>
          <div class="mode-pills">
            <button
              v-for="m in availableModes"
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
          <div v-if="!isEpd" class="cfg-block">
            <div class="cfg-label">背光亮度 ({{ form.backlight }}%)</div>
            <el-slider v-model="form.backlight" :min="0" :max="100" :step="5" />
          </div>
          <div v-else class="cfg-block">
            <div class="cfg-label">墨水屏选项</div>
            <div class="epd-toggles">
              <el-checkbox v-model="form.invert">黑白反色</el-checkbox>
              <el-checkbox v-if="activePanelSpec.partial" v-model="form.partial_refresh">
                局部快刷 (仅2.13黑白)
              </el-checkbox>
              <el-checkbox v-if="isThreeColor" :model-value="isRedBit(6)" @change="toggleHeaderRed">
                顶部红底高亮
              </el-checkbox>
            </div>
          </div>

          <div class="cfg-block">
            <div class="cfg-label">{{ isEpd ? '自动周期刷新间隔 (秒, 0=仅更改时刷)' : '色彩主题' }}</div>
            <el-input-number
              v-if="isEpd"
              v-model="form.refresh_sec"
              size="small"
              :min="0"
              :max="86400"
              :step="30"
              style="width: 100%"
            />
            <el-select v-else v-model="form.theme" size="small" style="width: 100%">
              <el-option v-for="t in DISPLAY_THEME_OPTIONS" :key="t.value" :label="t.label" :value="t.value" />
            </el-select>
          </div>
        </div>

        <!-- E-Paper Image Converter Studio (B/W & Tri-Color B/W/R) -->
        <div v-if="isEpd && form.mode === 'image'" class="img-studio-box">
          <input
            ref="fileInputRef"
            type="file"
            accept="image/*"
            style="display: none"
            @change="onImageFileSelected"
          />
          <div class="img-toolbar">
            <el-button size="small" type="primary" @click="triggerImagePicker">选择本地图片</el-button>
            <el-button
              v-if="isThreeColor"
              size="small"
              type="danger"
              plain
              @click="loadTestPattern('bwr')"
            >
              三色测试图 (B/W/R)
            </el-button>
            <el-button size="small" plain @click="loadTestPattern('bw')">黑白测试图 (B/W)</el-button>
          </div>

          <div class="cfg-two-col">
            <div class="cfg-block">
              <div class="cfg-label">墨水色彩输出</div>
              <el-radio-group v-model="imgState.colorMode" size="small">
                <el-radio-button value="bwr" :disabled="!isThreeColor">黑白红三色</el-radio-button>
                <el-radio-button value="bw">纯黑白</el-radio-button>
              </el-radio-group>
            </div>
            <div class="cfg-block">
              <div class="cfg-label">点阵处理算法</div>
              <el-select v-model="imgState.algorithm" size="small" style="width: 100%">
                <el-option value="dither" label="照片抖动 (Floyd-Steinberg)" />
                <el-option value="threshold" label="锐利二值 (Logo / 二维码)" />
              </el-select>
            </div>
          </div>

          <div class="cfg-two-col">
            <div class="cfg-block">
              <div class="cfg-label">缩放与构图</div>
              <el-select v-model="imgState.fit" size="small" style="width: 100%">
                <el-option value="contain" label="完整适配 (留白不裁剪)" />
                <el-option value="cover" label="铺满屏幕 (居中裁切)" />
              </el-select>
            </div>
            <div class="cfg-block">
              <div class="cfg-label">亮度补偿 ({{ imgState.brightness > 0 ? `+${imgState.brightness}` : imgState.brightness }})</div>
              <el-slider v-model="imgState.brightness" :min="-60" :max="60" :step="5" />
            </div>
          </div>

          <div v-if="imgState.colorMode === 'bwr' && isThreeColor" class="cfg-block">
            <div class="cfg-label">红色提取灵敏度 ({{ imgState.redSensitivity.toFixed(1) }}×)</div>
            <el-slider v-model="imgState.redSensitivity" :min="0.4" :max="1.8" :step="0.1" />
          </div>

          <div v-if="imgState.stats" class="img-stats-bar">
            <span>来源: <b>{{ imgState.name }}</b></span>
            <span>
              {{ imgState.stats.width }}×{{ imgState.stats.height }} ·
              白 {{ imgState.stats.whiteCount }} / 黑 {{ imgState.stats.blackCount }}
              <template v-if="imgState.colorMode === 'bwr'"> / <b style="color:#b91c1c">红 {{ imgState.stats.redCount }}</b></template>
            </span>
          </div>
        </div>

        <template v-if="form.mode !== 'image'">
        <div class="cfg-two-col">
          <div class="cfg-block">
            <div class="cfg-label">顶部标题 (留空显示设备 ID)</div>
            <el-input v-model="form.header_text" size="small" maxlength="20" clearable placeholder="如 DESK-EINK / NOTICE" />
          </div>
          <div class="cfg-block">
            <div class="cfg-label">底部角标 (留空自动状态)</div>
            <el-input v-model="form.footer_text" size="small" maxlength="20" clearable placeholder="如 [S3 EPD]" />
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
              {{ form.mode === 'text' ? '自定义文本内容（第 0~5 行，三色屏可点「红」字或加 [R] 前缀）' : '自定义备注行（自定义看板全显，状态/监控展示第 0 行）' }}
            </span>
            <el-button size="small" link @click="clearLines">清空行内容</el-button>
          </div>
          <div class="lines-grid">
            <el-input
              v-for="(_, idx) in form.lines"
              :key="idx"
              v-model="form.lines[idx]"
              size="small"
              maxlength="38"
              :placeholder="`第 ${idx} 行`"
            >
              <template #prepend>L{{ idx }}</template>
              <template v-if="isThreeColor" #append>
                <button
                  type="button"
                  class="red-chip-btn"
                  :class="{ active: isRedBit(idx) }"
                  title="切换该行红色墨水高亮"
                  @click="toggleLineRed(idx)"
                >
                  红
                </button>
              </template>
            </el-input>
          </div>
        </div>
        </template>

        <!-- E-Paper Driver Board Wiring & Custom Pins Collapsible -->
        <div v-if="isEpd" class="wiring-box">
          <div class="wiring-head" @click="showWiring = !showWiring">
            <span>驱动板 v1.4 接线: <b>SCK..BUSY 直插 S3 GPIO 9~14 (6P 不交叉连排)</b> · 开关 <b>0.47Ω</b></span>
            <el-button size="small" link>{{ showWiring ? '收起' : '展开接线/改引脚' }}</el-button>
          </div>
          <div v-if="showWiring" class="wiring-body">
            <div class="wiring-note">
              ✨ <b>6P 连续免交叉：</b>驱动板第 3~8 脚（<code>SCK, SDA, RST, DC, CS1, BUSY</code>）与 S3 左侧 <code>GPIO 9, 10, 11, 12, 13, 14</code> 顺序完全一一对应，可直接用一排 6 芯连体杜邦线直插！底部「采样电阻」开关请务必拨向右侧 <b>0.47Ω</b>。
            </div>
            <div class="wiring-grid">
              <div v-for="w in EPD_WIRING_GUIDE" :key="w.boardPin" class="wiring-item">
                <b>{{ w.boardPin }}</b> -> <code>{{ w.s3Pin }}</code>
                <span class="wiring-desc">{{ w.note }}</span>
              </div>
            </div>
            <div class="pin-override-row">
              <span class="cfg-label" style="grid-column: span 3">自定义引脚映射 (-1 = S3 默认引脚):</span>
              <div class="pin-cell"><span class="pin-lbl">SCK (默认9)</span><el-input-number v-model="form.pins.clk" size="small" :min="-1" :max="48" controls-position="right" /></div>
              <div class="pin-cell"><span class="pin-lbl">SDA (默认10)</span><el-input-number v-model="form.pins.mosi" size="small" :min="-1" :max="48" controls-position="right" /></div>
              <div class="pin-cell"><span class="pin-lbl">RST (默认11)</span><el-input-number v-model="form.pins.rst" size="small" :min="-1" :max="48" controls-position="right" /></div>
              <div class="pin-cell"><span class="pin-lbl">DC (默认12)</span><el-input-number v-model="form.pins.dc" size="small" :min="-1" :max="48" controls-position="right" /></div>
              <div class="pin-cell"><span class="pin-lbl">CS1 (默认13)</span><el-input-number v-model="form.pins.cs" size="small" :min="-1" :max="48" controls-position="right" /></div>
              <div class="pin-cell"><span class="pin-lbl">BUSY (默认14)</span><el-input-number v-model="form.pins.busy" size="small" :min="-1" :max="48" controls-position="right" /></div>
              <el-button size="small" link @click="resetPinsDefault">恢复默认(-1)</el-button>
            </div>
          </div>
        </div>

        <div class="submit-row">
          <template v-if="isEpd && form.mode === 'image' && imgState.lastBuilt">
            <el-button
              v-if="activePanelSpec.partial"
              size="default"
              plain
              :loading="loading"
              @click="submitImage(false)"
            >
              局部快刷推图
            </el-button>
            <el-button type="primary" :loading="loading" @click="submitImage(true)">
              推送图片到墨水屏 ({{ imgState.colorMode === 'bwr' ? '三色' : '黑白' }})
            </el-button>
          </template>
          <template v-else>
            <el-button
              v-if="isEpd"
              size="default"
              plain
              :loading="loading"
              @click="submit({ full_refresh: true, refresh: true })"
            >
              强制全刷消残影
            </el-button>
            <el-button type="primary" :loading="loading" @click="submit({ refresh: true })">
              {{ isEpd ? '保存并刷新墨水屏' : '保存并应用到设备屏幕' }}
            </el-button>
          </template>
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

.epd-warn-alert {
  padding: 10px 12px;
  border-radius: 8px;
  background: #fef2f2;
  border: 1px solid #fca5a5;
  color: #991b1b;
  font-size: 12px;
  line-height: 1.5;
}

.display-grid {
  display: grid;
  grid-template-columns: 245px 1fr;
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
  display: flex;
  align-items: center;
  gap: 6px;
  flex-wrap: wrap;
}

.bwr-pill {
  background: #fee2e2;
  color: #991b1b;
  padding: 1px 6px;
  border-radius: 999px;
  font-size: 10px;
}

.bw-pill {
  background: #e2e8f0;
  color: #334155;
  padding: 1px 6px;
  border-radius: 999px;
  font-size: 10px;
}

.screen-bezel {
  padding: 8px;
  border-radius: 12px;
  background: #0f172a;
  border: 2px solid #334155;
  box-shadow: 0 6px 18px rgba(15, 23, 42, 0.25);
}

.screen-bezel.epd-bezel {
  padding: 9px;
  background: #e5e7eb;
  border: 2px solid #cbd5e1;
  box-shadow: 0 4px 14px rgba(15, 23, 42, 0.12);
}

.screen-surface {
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

.screen-surface.epd-paper {
  background: #f6f4ec;
  color: #18181b;
  padding: 0;
  border: 1px solid #d4d4d8;
}

.screen-surface.epd-paper.epd-inverted {
  background: #18181b;
  color: #f6f4ec;
}

.screen-off-state {
  margin: auto;
  color: #64748b;
  font-size: 10px;
  letter-spacing: 0.5px;
}

.epd-img-canvas {
  width: 100%;
  height: 100%;
  display: block;
  image-rendering: pixelated;
}

.scr-hdr, .scr-ftr {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-weight: 700;
  white-space: nowrap;
  overflow: hidden;
}

.epd-hdr {
  padding: 3px 6px;
  font-size: 11px;
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

.mon-net {
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
  grid-template-columns: repeat(auto-fit, minmax(72px, 1fr));
  gap: 6px;
}

.img-studio-box {
  background: var(--color-bg-base, #f8fafc);
  border: 1px solid var(--color-border, #e2e8f0);
  border-radius: 8px;
  padding: 10px 12px;
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.img-toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

.img-stats-bar {
  display: flex;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 6px;
  font-size: 11px;
  color: #475569;
  padding-top: 4px;
  border-top: 1px dashed #cbd5e1;
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

.epd-toggles, .toggle-grid {
  display: flex;
  flex-wrap: wrap;
  gap: 4px 12px;
}

.lines-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 8px;
}

.red-chip-btn {
  border: none;
  background: transparent;
  padding: 0 8px;
  cursor: pointer;
  font-size: 11px;
  font-weight: 700;
  color: #64748b;
}

.red-chip-btn.active {
  color: #b91c1c;
  background: #fee2e2;
  border-radius: 3px;
}

.wiring-box {
  background: var(--color-bg-base, #f8fafc);
  border: 1px solid var(--color-border, #e2e8f0);
  border-radius: 8px;
  padding: 8px 12px;
}

.wiring-head {
  display: flex;
  justify-content: space-between;
  align-items: center;
  cursor: pointer;
  font-size: 12px;
  color: var(--color-text-primary, #1e293b);
}

.wiring-body {
  margin-top: 8px;
  padding-top: 8px;
  border-top: 1px dashed #cbd5e1;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.wiring-note {
  font-size: 11.5px;
  color: #9a3412;
  background: #ffedd5;
  padding: 6px 8px;
  border-radius: 6px;
}

.wiring-grid {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: 4px 10px;
  font-size: 11.5px;
}

.wiring-item {
  display: flex;
  align-items: center;
  gap: 6px;
}

.wiring-desc {
  color: #64748b;
  font-size: 11px;
}

.pin-override-row {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 8px;
  padding-top: 6px;
  border-top: 1px dashed #cbd5e1;
}

.pin-cell {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.pin-cell :deep(.el-input-number) {
  width: 100%;
}

.pin-lbl {
  font-size: 11px;
  color: #475569;
  font-weight: 600;
}

.submit-row {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  padding-top: 4px;
}

@media (max-width: 640px) {
  .display-grid, .cfg-two-col, .lines-grid, .mode-pills, .wiring-grid, .pin-override-row {
    grid-template-columns: 1fr;
  }
}
</style>
