<script>
export const meta = { title: '固件与 OTA 管理' }
</script>

<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import {
  UploadFilled,
  Cpu,
  Download,
  Delete,
  Promotion,
  Refresh,
  DocumentCopy,
  Check,
  Close,
  Warning,
  CircleCheck,
  Loading,
} from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { firmwareApi } from '@/api/firmware'
import { devicesApi } from '@/api/devices'

const firmwares = ref([])
const devices = ref([])
const onlineDeviceUuids = ref([])
const loading = ref(false)

// 上传表单
const uploadForm = reactive({
  version: '',
  description: '',
  targetChip: 'esp32s3',
  file: null,
  fileName: '',
  uploading: false,
  uploadPercent: 0,
})

// 推送 OTA 弹窗
const pushDialog = reactive({
  visible: false,
  firmware: null,
  selectedDeviceUuids: [],
  customUrl: '',
  submitting: false,
})

// 实时进度弹窗
const progressDialog = reactive({
  visible: false,
  deviceUuid: '',
  firmwareVersion: '',
  status: 'idle', // idle, starting, connecting, downloading, success, failed
  progress: 0,
  bytesRead: 0,
  totalBytes: 0,
  runningPartition: '',
  targetPartition: '',
  error: '',
  logs: [],
})

let pollTimer = null

// 格式化文件大小
function formatBytes(bytes) {
  if (!bytes || bytes === 0) return '0 B'
  const k = 1024
  const sizes = ['B', 'KB', 'MB', 'GB']
  const i = Math.floor(Math.log(bytes) / Math.log(k))
  return (bytes / Math.pow(k, i)).toFixed(2) + ' ' + sizes[i]
}

// 格式化时间
function formatTime(isoStr) {
  if (!isoStr) return '-'
  try {
    const d = new Date(isoStr)
    return d.toLocaleString('zh-CN', { hour12: false })
  } catch {
    return isoStr
  }
}

// 复制文本
async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text)
    ElMessage.success('已复制到剪贴板')
  } catch {
    ElMessage.info('复制失败，请手动选取')
  }
}

// 加载固件列表与设备列表
async function loadData() {
  loading.value = true
  try {
    const [fwList, devList, onRes] = await Promise.all([
      firmwareApi.list(),
      devicesApi.list(),
      devicesApi.online(),
    ])
    firmwares.value = fwList || []
    devices.value = devList || []
    onlineDeviceUuids.value = onRes.online || []
  } catch (err) {
    ElMessage.error('加载数据失败: ' + (err.message || err))
  } finally {
    loading.value = false
  }
}

// 文件选取
function handleFileChange(uploadFile) {
  const file = uploadFile.raw
  if (!file) return
  if (!file.name.endsWith('.bin')) {
    ElMessage.error('仅支持上传 .bin 格式的固件文件')
    return
  }
  uploadForm.file = file
  uploadForm.fileName = file.name
  if (!uploadForm.version) {
    // 自动尝试从文件名中提取版本号，如 ble_hid_v1.2.0.bin
    const match = file.name.match(/v?(\d+\.\d+(\.\d+)?)/i)
    if (match) {
      uploadForm.version = 'v' + match[1]
    } else {
      uploadForm.version = 'v1.0.0'
    }
  }
}

// 提交上传
async function submitUpload() {
  if (!uploadForm.file) {
    ElMessage.warning('请选择固件 .bin 文件')
    return
  }
  if (!uploadForm.version.trim()) {
    ElMessage.warning('请输入固件版本号')
    return
  }

  const formData = new FormData()
  formData.append('file', uploadForm.file)
  formData.append('version', uploadForm.version.trim())
  formData.append('description', uploadForm.description.trim())
  formData.append('target_chip', uploadForm.targetChip)

  uploadForm.uploading = true
  uploadForm.uploadPercent = 0

  try {
    await firmwareApi.upload(formData, (progressEvent) => {
      if (progressEvent.total > 0) {
        uploadForm.uploadPercent = Math.round((progressEvent.loaded * 100) / progressEvent.total)
      }
    })
    ElMessage.success('固件上传成功')
    uploadForm.file = null
    uploadForm.fileName = ''
    uploadForm.version = ''
    uploadForm.description = ''
    await loadData()
  } catch (err) {
    ElMessage.error('上传失败: ' + (err.response?.data?.detail || err.message))
  } finally {
    uploadForm.uploading = false
  }
}

// 删除固件
async function handleDelete(fw) {
  try {
    await ElMessageBox.confirm(
      `确定要删除固件版本 ${fw.version} (${fw.filename}) 吗？此操作不可撤销。`,
      '确认删除',
      {
        confirmButtonText: '确定删除',
        cancelButtonText: '取消',
        type: 'warning',
      }
    )
    await firmwareApi.delete(fw.id)
    ElMessage.success('固件已删除')
    await loadData()
  } catch (e) {
    if (e !== 'cancel') {
      ElMessage.error('删除失败: ' + (e.message || e))
    }
  }
}

// 打开推送升级弹窗
function openPushDialog(fw) {
  pushDialog.firmware = fw
  pushDialog.selectedDeviceUuids = onlineDevices.value.map((d) => d.device_uuid)
  pushDialog.customUrl = ''
  pushDialog.visible = true
}

// 在线设备列表计算
const onlineDevices = computed(() => {
  return devices.value.filter((d) => onlineDeviceUuids.value.includes(d.device_uuid))
})

// 启动轮询检查 OTA 进度
function startProgressPolling(deviceUuid) {
  stopProgressPolling()
  pollTimer = setInterval(async () => {
    try {
      const res = await firmwareApi.getProgress(deviceUuid)
      if (res) {
        progressDialog.status = res.status || 'downloading'
        progressDialog.progress = res.progress || 0
        progressDialog.bytesRead = res.bytes_read || 0
        progressDialog.totalBytes = res.total_bytes || 0
        progressDialog.runningPartition = res.running_partition || ''
        progressDialog.targetPartition = res.target_partition || ''
        progressDialog.error = res.error || ''

        // 记录日志
        const logLine = `[${new Date().toLocaleTimeString()}] 状态: ${res.status}, 进度: ${res.progress}%, 已写入: ${formatBytes(res.bytes_read)}`
        if (!progressDialog.logs.includes(logLine)) {
          progressDialog.logs.unshift(logLine)
        }

        if (res.status === 'success' || res.status === 'failed') {
          stopProgressPolling()
          if (res.status === 'success') {
            ElMessage.success(`设备 ${deviceUuid} OTA 固件更新成功，已重启切换至新分区！`)
          } else {
            ElMessage.error(`设备 ${deviceUuid} OTA 升级失败: ${res.error || '未知错误'}`)
          }
        }
      }
    } catch {
      // 忽略单次网络闪断
    }
  }, 1000)
}

function stopProgressPolling() {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = null
  }
}

// 确认下发升级
async function confirmPushOta() {
  if (!pushDialog.selectedDeviceUuids.length) {
    ElMessage.warning('请选择至少一台要升级的在线设备')
    return
  }

  pushDialog.submitting = true
  const targetUuid = pushDialog.selectedDeviceUuids[0]

  try {
    const res = await firmwareApi.pushOta(
      pushDialog.firmware.id,
      pushDialog.selectedDeviceUuids,
      pushDialog.customUrl ? pushDialog.customUrl.trim() : null
    )

    ElMessage.success('OTA 升级指令已下发，设备开始拉取固件')
    pushDialog.visible = false

    // 打开实时监控弹窗
    progressDialog.deviceUuid = targetUuid
    progressDialog.firmwareVersion = pushDialog.firmware.version
    progressDialog.status = 'starting'
    progressDialog.progress = 0
    progressDialog.bytesRead = 0
    progressDialog.totalBytes = pushDialog.firmware.file_size
    progressDialog.error = ''
    progressDialog.logs = [`[${new Date().toLocaleTimeString()}] 成功下发 OTA 升级指令至 ${targetUuid}`]
    progressDialog.visible = true

    startProgressPolling(targetUuid)
  } catch (err) {
    ElMessage.error('下发失败: ' + (err.response?.data?.detail || err.message))
  } finally {
    pushDialog.submitting = false
  }
}

function closeProgressDialog() {
  stopProgressPolling()
  progressDialog.visible = false
}

onMounted(() => {
  loadData()
})

onBeforeUnmount(() => {
  stopProgressPolling()
})
</script>

<template>
  <div class="p-6 max-w-7xl mx-auto space-y-6">
    <!-- 头部看板与统计 -->
    <div class="relative overflow-hidden rounded-2xl bg-gradient-to-r from-blue-700 via-indigo-700 to-purple-800 p-8 text-white shadow-xl">
      <div class="relative z-10 flex flex-col md:flex-row md:items-center justify-between gap-6">
        <div>
          <div class="flex items-center gap-3">
            <span class="p-2.5 rounded-xl bg-white/10 backdrop-blur border border-white/20">
              <el-icon :size="28"><Cpu /></el-icon>
            </span>
            <h1 class="text-3xl font-bold tracking-tight">固件与 OTA 升级中心</h1>
          </div>
          <p class="mt-2 text-blue-100 text-sm max-w-2xl leading-relaxed">
            管理 ESP32-S3 双分区 OTA 升级固件包。支持固件上传校验、局域网安全分发、一键静默推送升级与实时进度可视化追踪。
          </p>
        </div>
        <div class="flex items-center gap-4">
          <el-button
            type="primary"
            class="!bg-white !text-blue-900 !border-none font-semibold hover:!bg-blue-50 shadow-md"
            :icon="Refresh"
            @click="loadData"
          >
            刷新数据
          </el-button>
        </div>
      </div>

      <!-- KPI 徽标统计 -->
      <div class="mt-6 pt-6 border-t border-white/10 grid grid-cols-2 sm:grid-cols-4 gap-4">
        <div>
          <div class="text-xs uppercase tracking-wider text-blue-200">固件版本总数</div>
          <div class="text-2xl font-bold mt-1">{{ firmwares.length }}</div>
        </div>
        <div>
          <div class="text-xs uppercase tracking-wider text-blue-200">最新版本</div>
          <div class="text-2xl font-bold mt-1 text-emerald-300">
            {{ firmwares[0]?.version || '暂无' }}
          </div>
        </div>
        <div>
          <div class="text-xs uppercase tracking-wider text-blue-200">在册设备总数</div>
          <div class="text-2xl font-bold mt-1">{{ devices.length }}</div>
        </div>
        <div>
          <div class="text-xs uppercase tracking-wider text-blue-200">在线支持 OTA 设备</div>
          <div class="text-2xl font-bold mt-1 text-amber-300">{{ onlineDevices.length }}</div>
        </div>
      </div>
    </div>

    <!-- 上传卡片与固件列表 -->
    <div class="grid grid-cols-1 lg:grid-cols-3 gap-6">
      <!-- 左侧：固件上传面板 -->
      <div class="lg:col-span-1 bg-white dark:bg-gray-800 rounded-xl p-6 border border-gray-200 dark:border-gray-700 shadow-sm flex flex-col justify-between">
        <div>
          <div class="flex items-center gap-2 pb-4 border-b border-gray-100 dark:border-gray-700 mb-5">
            <el-icon class="text-blue-600 dark:text-blue-400" :size="20"><UploadFilled /></el-icon>
            <h2 class="text-base font-bold text-gray-900 dark:text-white">上传新固件包 (.bin)</h2>
          </div>

          <div class="space-y-4">
            <!-- 拖拽/点击文件上传组件 -->
            <el-upload
              drag
              :auto-upload="false"
              :show-file-list="false"
              accept=".bin"
              @change="handleFileChange"
            >
              <div class="py-4">
                <el-icon class="text-blue-500 text-4xl mb-2"><UploadFilled /></el-icon>
                <div class="text-sm font-medium text-gray-700 dark:text-gray-300">
                  {{ uploadForm.fileName ? uploadForm.fileName : '点击或将 .bin 文件拖到此处' }}
                </div>
                <div class="text-xs text-gray-400 mt-1">仅支持 ESP-IDF 编译产物 (.bin)</div>
              </div>
            </el-upload>

            <div v-if="uploadForm.fileName" class="p-3 bg-blue-50 dark:bg-blue-900/20 rounded-lg flex items-center justify-between text-xs text-blue-700 dark:text-blue-300">
              <span class="truncate max-w-[200px]">{{ uploadForm.fileName }}</span>
              <span>{{ formatBytes(uploadForm.file?.size) }}</span>
            </div>

            <!-- 版本号输入 -->
            <div>
              <label class="block text-xs font-semibold text-gray-700 dark:text-gray-300 mb-1">
                固件版本号 <span class="text-red-500">*</span>
              </label>
              <el-input
                v-model="uploadForm.version"
                placeholder="例如: v1.2.0 或 v1.3-beta"
                clearable
              />
            </div>

            <!-- 适用芯片 -->
            <div>
              <label class="block text-xs font-semibold text-gray-700 dark:text-gray-300 mb-1">适用硬件</label>
              <el-select v-model="uploadForm.targetChip" class="w-full">
                <el-option label="ESP32-S3 (双分区 OTA)" value="esp32s3" />
                <el-option label="ESP32 通用" value="esp32" />
              </el-select>
            </div>

            <!-- 描述 / 更新日志 -->
            <div>
              <label class="block text-xs font-semibold text-gray-700 dark:text-gray-300 mb-1">发布说明 / 变更日志</label>
              <el-input
                v-model="uploadForm.description"
                type="textarea"
                :rows="3"
                placeholder="简述本次固件版本的功能改动与修复..."
              />
            </div>
          </div>
        </div>

        <div class="mt-6 pt-4 border-t border-gray-100 dark:border-gray-700">
          <div v-if="uploadForm.uploading" class="mb-3">
            <el-progress :percentage="uploadForm.uploadPercent" :status="uploadForm.uploadPercent === 100 ? 'success' : ''" />
          </div>
          <el-button
            type="primary"
            class="w-full !py-2.5 font-medium shadow-sm"
            :loading="uploadForm.uploading"
            @click="submitUpload"
          >
            确认发布固件
          </el-button>
        </div>
      </div>

      <!-- 右侧：固件版本库列表 -->
      <div class="lg:col-span-2 bg-white dark:bg-gray-800 rounded-xl p-6 border border-gray-200 dark:border-gray-700 shadow-sm flex flex-col justify-between">
        <div>
          <div class="flex items-center justify-between pb-4 border-b border-gray-100 dark:border-gray-700 mb-4">
            <div class="flex items-center gap-2">
              <el-icon class="text-indigo-600 dark:text-indigo-400" :size="20"><DocumentCopy /></el-icon>
              <h2 class="text-base font-bold text-gray-900 dark:text-white">固件版本仓库</h2>
            </div>
            <span class="text-xs text-gray-500">共 {{ firmwares.length }} 个版本</span>
          </div>

          <el-table
            v-loading="loading"
            :data="firmwares"
            stripe
            class="w-full text-sm"
            empty-text="暂无固件记录，请在左侧上传发布"
          >
            <el-table-column prop="version" label="版本号" width="120">
              <template #default="{ row }">
                <el-tag effect="light" class="font-mono font-semibold" type="success">
                  {{ row.version }}
                </el-tag>
              </template>
            </el-table-column>

            <el-table-column prop="filename" label="文件名 / 大小" min-width="160">
              <template #default="{ row }">
                <div class="font-medium text-gray-900 dark:text-white truncate" :title="row.filename">
                  {{ row.filename }}
                </div>
                <div class="text-xs text-gray-400 mt-0.5">
                  {{ formatBytes(row.file_size) }} · {{ row.target_chip }}
                </div>
              </template>
            </el-table-column>

            <el-table-column prop="sha256" label="SHA-256 校验和" min-width="160">
              <template #default="{ row }">
                <div
                  class="font-mono text-xs text-gray-600 dark:text-gray-300 truncate cursor-pointer hover:text-blue-600 flex items-center gap-1"
                  :title="row.sha256"
                  @click="copyText(row.sha256)"
                >
                  <span>{{ row.sha256.substring(0, 16) }}...</span>
                  <el-icon><DocumentCopy /></el-icon>
                </div>
              </template>
            </el-table-column>

            <el-table-column prop="created_at" label="发布时间" width="160">
              <template #default="{ row }">
                <span class="text-xs text-gray-500">{{ formatTime(row.created_at) }}</span>
              </template>
            </el-table-column>

            <el-table-column label="操作" width="220" fixed="right">
              <template #default="{ row }">
                <div class="flex items-center gap-1.5">
                  <el-button
                    size="small"
                    type="primary"
                    :icon="Promotion"
                    @click="openPushDialog(row)"
                  >
                    一键推送
                  </el-button>

                  <el-button
                    size="small"
                    tag="a"
                    :href="row.download_url"
                    target="_blank"
                    :icon="Download"
                    title="下载到本地"
                  />

                  <el-button
                    size="small"
                    type="danger"
                    plain
                    :icon="Delete"
                    title="删除版本"
                    @click="handleDelete(row)"
                  />
                </div>
              </template>
            </el-table-column>
          </el-table>
        </div>
      </div>
    </div>

    <!-- 弹窗 1：推送 OTA 升级弹窗 -->
    <el-dialog
      v-model="pushDialog.visible"
      title="一键下发 OTA 固件升级"
      width="540px"
      destroy-on-close
    >
      <div class="space-y-5">
        <!-- 固件摘要信息卡 -->
        <div class="p-4 rounded-xl bg-blue-50 dark:bg-blue-900/20 border border-blue-100 dark:border-blue-800 space-y-2 text-sm">
          <div class="flex items-center justify-between">
            <span class="text-gray-500">目标版本:</span>
            <el-tag type="success" class="font-mono font-bold">{{ pushDialog.firmware?.version }}</el-tag>
          </div>
          <div class="flex items-center justify-between">
            <span class="text-gray-500">固件文件:</span>
            <span class="font-medium text-gray-900 dark:text-white">{{ pushDialog.firmware?.filename }}</span>
          </div>
          <div class="flex items-center justify-between">
            <span class="text-gray-500">文件大小:</span>
            <span class="text-gray-700 dark:text-gray-300">{{ formatBytes(pushDialog.firmware?.file_size) }}</span>
          </div>
          <div class="flex items-center justify-between">
            <span class="text-gray-500">默认下载链接:</span>
            <span class="text-xs font-mono text-gray-500 truncate max-w-[280px]" :title="pushDialog.firmware?.download_url">
              {{ pushDialog.firmware?.download_url }}
            </span>
          </div>
        </div>

        <!-- 在线设备选择 -->
        <div>
          <div class="flex items-center justify-between mb-2">
            <label class="text-sm font-semibold text-gray-900 dark:text-white">选择要推送的在线设备</label>
            <span class="text-xs text-gray-500">当前在线: {{ onlineDevices.length }} 台</span>
          </div>

          <div v-if="!onlineDevices.length" class="p-4 rounded-lg bg-amber-50 dark:bg-amber-900/20 text-amber-700 dark:text-amber-300 text-xs flex items-center gap-2">
            <el-icon :size="16"><Warning /></el-icon>
            <span>当前没有在线设备连入。请先确保 ESP32 已接通电源并连上 Wi-Fi。</span>
          </div>

          <el-checkbox-group v-else v-model="pushDialog.selectedDeviceUuids" class="space-y-2">
            <div
              v-for="d in onlineDevices"
              :key="d.device_uuid"
              class="flex items-center justify-between p-3 rounded-lg border border-gray-200 dark:border-gray-700 hover:bg-gray-50 dark:hover:bg-gray-700/50"
            >
              <el-checkbox :value="d.device_uuid">
                <span class="font-medium text-gray-900 dark:text-white">{{ d.name || d.device_uuid }}</span>
                <span class="ml-2 text-xs font-mono text-gray-400">({{ d.device_uuid }})</span>
              </el-checkbox>
              <el-tag size="small" type="success">在线</el-tag>
            </div>
          </el-checkbox-group>
        </div>

        <!-- 自定义 URL -->
        <div>
          <label class="block text-xs font-semibold text-gray-700 dark:text-gray-300 mb-1">
            自定义下载地址 (留空使用服务端自动解析的局域网 IP)
          </label>
          <el-input
            v-model="pushDialog.customUrl"
            placeholder="http://192.168.xx.xx:8000/api/v2/firmwares/1/download"
            clearable
          />
        </div>
      </div>

      <template #footer>
        <div class="flex justify-end gap-3">
          <el-button @click="pushDialog.visible = false">取消</el-button>
          <el-button
            type="primary"
            :loading="pushDialog.submitting"
            :disabled="!onlineDevices.length"
            @click="confirmPushOta"
          >
            确认下发升级
          </el-button>
        </div>
      </template>
    </el-dialog>

    <!-- 弹窗 2：实时 OTA 升级进度监视面板 -->
    <el-dialog
      v-model="progressDialog.visible"
      title="OTA 升级实时进度监控"
      width="600px"
      :close-on-click-modal="false"
      :before-close="closeProgressDialog"
    >
      <div class="space-y-6">
        <!-- 核心进度展示 -->
        <div class="text-center py-4 space-y-4">
          <div class="inline-flex items-center justify-center">
            <el-progress
              type="dashboard"
              :percentage="progressDialog.progress"
              :status="progressDialog.status === 'success' ? 'success' : progressDialog.status === 'failed' ? 'exception' : ''"
              :width="150"
            >
              <template #default="{ percentage }">
                <div class="space-y-1">
                  <div class="text-2xl font-bold font-mono">{{ percentage }}%</div>
                  <div class="text-xs text-gray-400 uppercase tracking-wider">
                    {{ progressDialog.status }}
                  </div>
                </div>
              </template>
            </el-progress>
          </div>

          <div class="space-y-1">
            <h3 class="text-base font-semibold text-gray-900 dark:text-white">
              正在升级设备: <span class="font-mono text-blue-600">{{ progressDialog.deviceUuid }}</span>
            </h3>
            <p class="text-xs text-gray-500">
              目标版本: {{ progressDialog.firmwareVersion }} · 已传输: {{ formatBytes(progressDialog.bytesRead) }} / {{ formatBytes(progressDialog.totalBytes) }}
            </p>
          </div>
        </div>

        <!-- 详细状态卡 -->
        <div class="grid grid-cols-2 gap-3 text-xs">
          <div class="p-3 bg-gray-50 dark:bg-gray-800 rounded-lg border border-gray-100 dark:border-gray-700">
            <span class="text-gray-400 block">当前运行分区</span>
            <span class="font-mono font-semibold text-gray-800 dark:text-gray-200 mt-1 block">
              {{ progressDialog.runningPartition || 'ota_0' }}
            </span>
          </div>
          <div class="p-3 bg-gray-50 dark:bg-gray-800 rounded-lg border border-gray-100 dark:border-gray-700">
            <span class="text-gray-400 block">写入目标分区</span>
            <span class="font-mono font-semibold text-indigo-600 dark:text-indigo-400 mt-1 block">
              {{ progressDialog.targetPartition || 'ota_1' }}
            </span>
          </div>
        </div>

        <div v-if="progressDialog.error" class="p-3 rounded-lg bg-red-50 text-red-700 text-xs flex items-center gap-2">
          <el-icon><Close /></el-icon>
          <span>错误信息: {{ progressDialog.error }}</span>
        </div>

        <!-- 实时日志流水 -->
        <div>
          <div class="text-xs font-semibold text-gray-700 dark:text-gray-300 mb-2">执行流水日志</div>
          <div class="h-32 bg-gray-900 text-emerald-400 font-mono text-xs p-3 rounded-lg overflow-y-auto space-y-1">
            <div v-for="(log, idx) in progressDialog.logs" :key="idx">
              {{ log }}
            </div>
          </div>
        </div>
      </div>

      <template #footer>
        <div class="flex justify-end gap-3">
          <el-button @click="closeProgressDialog">关闭</el-button>
        </div>
      </template>
    </el-dialog>
  </div>
</template>
