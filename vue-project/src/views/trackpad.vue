<script>
export const meta = { title: 'Touchpad', layout: 'blank' }
</script>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import TrackpadPanel from '@/components/TrackpadPanel.vue'
import { devicesApi } from '@/api/devices'

const route = useRoute()
const router = useRouter()

const devices = ref([])
const online = ref([])
const selected = ref(typeof route.query.device === 'string' ? route.query.device : '')
const loading = ref(false)

const selectedDevice = computed(() => (
  devices.value.find((item) => item.device_uuid === selected.value) || null
))
const isSelectedOnline = computed(() => !!selected.value && online.value.includes(selected.value))

async function load() {
  loading.value = true
  try {
    devices.value = await devicesApi.list()
    online.value = (await devicesApi.online()).online
    if (!selected.value && online.value.length) selected.value = online.value[0]
  } finally {
    loading.value = false
  }
}

async function silentRefresh() {
  try {
    const [list, on] = await Promise.all([devicesApi.list(), devicesApi.online()])
    devices.value = list
    online.value = on.online
  } catch {
    // ignore
  }
}

function backToConsole() {
  router.push('/console')
}

function openPureMode() {
  const target = router.resolve({
    path: '/trackpad-pure',
    query: selected.value ? { device: selected.value } : {},
  })
  window.open(target.href, '_blank', 'noopener')
}

function openKioskMode() {
  const target = router.resolve({
    path: '/trackpad-kiosk',
    query: selected.value ? { device: selected.value } : {},
  })
  window.open(target.href, '_blank', 'noopener')
}

watch(
  () => route.query.device,
  (value) => {
    const next = typeof value === 'string' ? value : ''
    if (next !== selected.value) selected.value = next
  },
)

watch(selected, (value) => {
  const current = typeof route.query.device === 'string' ? route.query.device : ''
  const next = value || ''
  if (current === next) return
  router.replace({ path: '/trackpad', query: next ? { device: next } : {} })
})

let pollTimer = null

onMounted(() => {
  load()
  pollTimer = window.setInterval(silentRefresh, 5000)
})

onBeforeUnmount(() => {
  if (pollTimer) window.clearInterval(pollTimer)
})
</script>

<template>
  <div class="trackpad-page">
    <div class="trackpad-topbar">
      <div class="topbar-copy">
        <div class="topbar-title">Touchpad</div>
        <div class="topbar-subtitle">
          Dedicated full-page control surface for phones, tablets, and secondary screens.
        </div>
      </div>

      <div class="topbar-actions">
        <el-select
          v-model="selected"
          class="device-select"
          filterable
          placeholder="Select device"
          :loading="loading"
        >
          <el-option
            v-for="device in devices"
            :key="device.id"
            :label="`${device.name || device.device_uuid} ${online.includes(device.device_uuid) ? '[online]' : '[offline]'}`"
            :value="device.device_uuid"
          />
        </el-select>
        <el-tag size="small" :type="isSelectedOnline ? 'success' : 'info'">
          {{ isSelectedOnline ? 'online' : 'offline' }}
        </el-tag>
        <el-button size="small" type="primary" :disabled="!selected" @click="openKioskMode">Kiosk</el-button>
        <el-button size="small" type="primary" :disabled="!selected" @click="openPureMode">Pure</el-button>
        <el-button size="small" @click="load" :loading="loading">Refresh</el-button>
        <el-button size="small" @click="backToConsole">Back</el-button>
      </div>
    </div>

    <div class="trackpad-meta">
      <span>{{ selectedDevice?.name || selected || 'No device selected' }}</span>
      <span v-if="selectedDevice?.device_uuid && selectedDevice?.name && selectedDevice.device_uuid !== selectedDevice.name">
        {{ selectedDevice.device_uuid }}
      </span>
    </div>

    <div class="trackpad-panel-wrap">
      <TrackpadPanel
        fullscreen
        :device-uuid="selected"
        :device-online="isSelectedOnline"
      />
    </div>
  </div>
</template>

<style scoped>
.trackpad-page {
  width: 100vw;
  height: 100dvh;
  min-height: 100vh;
  padding: 14px;
  display: flex;
  flex-direction: column;
  gap: 12px;
  background:
    radial-gradient(circle at top, rgba(64, 158, 255, 0.12), transparent 28%),
    linear-gradient(180deg, #f5f9ff 0%, #edf4ff 42%, #eef2f8 100%);
  box-sizing: border-box;
}

.trackpad-topbar {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  align-items: flex-start;
  padding: 14px 16px;
  border-radius: 18px;
  background: rgba(255, 255, 255, 0.82);
  backdrop-filter: blur(14px);
  box-shadow: 0 10px 40px rgba(15, 23, 42, 0.08);
}

.topbar-copy {
  min-width: 0;
}

.topbar-title {
  font-size: 18px;
  font-weight: 700;
  line-height: 1.2;
}

.topbar-subtitle {
  margin-top: 4px;
  font-size: 12px;
  color: #667085;
  line-height: 1.45;
}

.topbar-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  justify-content: flex-end;
  align-items: center;
}

.device-select {
  width: min(360px, 72vw);
}

.trackpad-meta {
  display: flex;
  gap: 12px;
  flex-wrap: wrap;
  padding: 0 4px;
  font-size: 12px;
  color: #667085;
}

.trackpad-panel-wrap {
  flex: 1 1 auto;
  min-height: 0;
}

.trackpad-panel-wrap :deep(.trackpad-card) {
  height: 100%;
}

@media (max-width: 900px) {
  .trackpad-page {
    padding: 10px;
  }

  .trackpad-topbar {
    flex-direction: column;
    align-items: stretch;
  }

  .topbar-actions {
    justify-content: flex-start;
  }

  .device-select {
    width: 100%;
  }
}
</style>
