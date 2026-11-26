<script setup>
import { computed, onMounted, ref } from 'vue'
import { devicesApi } from '@/api/devices'
import { macrosApi } from '@/api/macros'
import TimerConfigPanel from '@/components/TimerConfigPanel.vue'

const selectedUuid = ref('')
const devices = ref([])
const macros = ref([])
const loadingDevices = ref(false)

const selectedDeviceObj = computed(() =>
  devices.value.find((d) => d.device_uuid === selectedUuid.value) || null,
)

async function loadAll() {
  loadingDevices.value = true
  try {
    const [devs, macs] = await Promise.all([
      devicesApi.list().catch(() => []),
      macrosApi.list().catch(() => []),
    ])
    devices.value = devs || []
    macros.value = macs || []
  } finally {
    loadingDevices.value = false
  }
}

onMounted(loadAll)
</script>

<script>
export const meta = { title: '定时与触发' }
</script>

<template>
  <div class="timers-page">
    <div class="page-head">
      <div>
        <h2 class="page-title">模块定时与自动化调度</h2>
        <p class="page-sub">
          统一管理全网 ESP32-C3 / S3 / ESP8266 设备的 GPIO 开关脉冲、PWM 调光、屏幕模式切换、HID 传输切换与定时重启任务。
        </p>
      </div>
      <div class="page-filter">
        <el-select
          v-model="selectedUuid"
          clearable
          filterable
          placeholder="全部设备（筛选目标）"
          style="width: 250px"
          :loading="loadingDevices"
        >
          <el-option
            v-for="d in devices"
            :key="d.device_uuid"
            :label="`${d.name || d.device_uuid} · ${(d.online ?? d.status === 'online') ? '在线' : '离线'}`"
            :value="d.device_uuid"
          />
        </el-select>
        <el-button @click="loadAll" :loading="loadingDevices">刷新设备</el-button>
      </div>
    </div>

    <div class="panel-card">
      <TimerConfigPanel
        :device-uuid="selectedUuid"
        :device-caps="selectedDeviceObj?.capabilities || []"
        :devices="devices"
        :macros="macros"
      />
    </div>
  </div>
</template>

<style scoped>
.timers-page {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.page-head {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 12px;
}

.page-title {
  margin: 0 0 4px 0;
  font-size: 20px;
  font-weight: 700;
  color: var(--color-text-primary);
}

.page-sub {
  margin: 0;
  font-size: 13px;
  color: var(--color-text-secondary);
}

.page-filter {
  display: flex;
  align-items: center;
  gap: 8px;
}

.panel-card {
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-md);
  padding: 18px 20px;
}
</style>
