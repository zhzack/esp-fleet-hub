<script>
export const meta = { title: 'Kiosk Touchpad', layout: 'blank' }
</script>

<script setup>
import { onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import TrackpadPanel from '@/components/TrackpadPanel.vue'
import { devicesApi } from '@/api/devices'

const route = useRoute()
const router = useRouter()

const rootRef = ref(null)
const devices = ref([])
const online = ref([])
const selected = ref(typeof route.query.device === 'string' ? route.query.device : '')
const isSelectedOnline = ref(false)

let wakeLock = null
let pollTimer = null

async function load() {
  try {
    devices.value = await devicesApi.list()
    online.value = (await devicesApi.online()).online
    if (!selected.value && online.value.length) selected.value = online.value[0]
    isSelectedOnline.value = !!selected.value && online.value.includes(selected.value)
  } catch {
    devices.value = []
    online.value = []
    isSelectedOnline.value = false
  }
}

async function silentRefresh() {
  try {
    const [list, on] = await Promise.all([devicesApi.list(), devicesApi.online()])
    devices.value = list
    online.value = on.online
    if (!selected.value && online.value.length) selected.value = online.value[0]
    isSelectedOnline.value = !!selected.value && online.value.includes(selected.value)
  } catch {
    // ignore
  }
}

watch(
  () => route.query.device,
  (value) => {
    const next = typeof value === 'string' ? value : ''
    selected.value = next || selected.value
    isSelectedOnline.value = !!selected.value && online.value.includes(selected.value)
  },
)

watch(selected, (value) => {
  isSelectedOnline.value = !!value && online.value.includes(value)
  const current = typeof route.query.device === 'string' ? route.query.device : ''
  const next = value || ''
  if (current === next) return
  router.replace({ path: '/trackpad-kiosk', query: next ? { device: next } : {} })
})

async function requestWakeLock() {
  if (!('wakeLock' in navigator)) return
  try {
    wakeLock = await navigator.wakeLock.request('screen')
  } catch {
    wakeLock = null
  }
}

async function releaseWakeLock() {
  if (!wakeLock) return
  try {
    await wakeLock.release()
  } catch {
    // ignore
  } finally {
    wakeLock = null
  }
}

async function lockLandscape() {
  if (!screen.orientation?.lock) return
  try {
    await screen.orientation.lock('landscape')
  } catch {
    // ignore
  }
}

async function requestFullscreen() {
  if (!rootRef.value || document.fullscreenElement) return
  try {
    await rootRef.value.requestFullscreen({ navigationUI: 'hide' })
  } catch {
    // ignore
  }
}

async function requestImmersiveMode() {
  await requestFullscreen()
  await lockLandscape()
  await requestWakeLock()
}

async function handleVisibilityChange() {
  if (document.visibilityState === 'visible') {
    await requestWakeLock()
  } else {
    await releaseWakeLock()
  }
}

onMounted(() => {
  load()
  pollTimer = window.setInterval(silentRefresh, 5000)
  document.addEventListener('visibilitychange', handleVisibilityChange)
  document.addEventListener('fullscreenchange', requestImmersiveMode)
  requestImmersiveMode()
})

onBeforeUnmount(() => {
  if (pollTimer) window.clearInterval(pollTimer)
  document.removeEventListener('visibilitychange', handleVisibilityChange)
  document.removeEventListener('fullscreenchange', requestImmersiveMode)
  releaseWakeLock()
})
</script>

<template>
  <div ref="rootRef" class="kiosk-trackpad-page" @pointerdown.capture="requestImmersiveMode">
    <TrackpadPanel
      fullscreen
      minimal
      immersive
      :device-uuid="selected"
      :device-online="isSelectedOnline"
    />
  </div>
</template>

<style scoped>
.kiosk-trackpad-page {
  width: 100vw;
  height: 100dvh;
  min-height: 100vh;
  overflow: hidden;
  background:
    radial-gradient(circle at top left, rgba(64, 158, 255, 0.2), transparent 32%),
    linear-gradient(180deg, #edf5ff 0%, #e5efff 50%, #eaf1ff 100%);
  overscroll-behavior: none;
  touch-action: none;
}

.kiosk-trackpad-page :deep(.trackpad-card),
.kiosk-trackpad-page :deep(.el-card__body) {
  height: 100%;
}
</style>
