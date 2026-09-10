<script>
export const meta = { title: 'Pure Touchpad', layout: 'blank' }
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

const isSelectedOnline = computed(() => !!selected.value && online.value.includes(selected.value))

async function load() {
  try {
    devices.value = await devicesApi.list()
    online.value = (await devicesApi.online()).online
    if (!selected.value && online.value.length) selected.value = online.value[0]
  } catch {
    devices.value = []
    online.value = []
  }
}

async function silentRefresh() {
  try {
    const [list, on] = await Promise.all([devicesApi.list(), devicesApi.online()])
    devices.value = list
    online.value = on.online
    if (!selected.value && online.value.length) selected.value = online.value[0]
  } catch {
    // ignore
  }
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
  router.replace({ path: '/trackpad-pure', query: next ? { device: next } : {} })
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
  <div class="pure-trackpad-page">
    <TrackpadPanel
      fullscreen
      minimal
      :device-uuid="selected"
      :device-online="isSelectedOnline"
    />
  </div>
</template>

<style scoped>
.pure-trackpad-page {
  width: 100vw;
  height: 100dvh;
  min-height: 100vh;
  padding: 0;
  background:
    radial-gradient(circle at top, rgba(64, 158, 255, 0.14), transparent 34%),
    linear-gradient(180deg, #eff6ff 0%, #eaf2ff 46%, #eef2f8 100%);
}

.pure-trackpad-page :deep(.trackpad-card) {
  height: 100%;
}

.pure-trackpad-page :deep(.el-card__body) {
  height: 100%;
}
</style>
