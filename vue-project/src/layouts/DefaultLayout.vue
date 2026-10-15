<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { RouterView, useRoute, useRouter } from 'vue-router'
import {
  House, Cpu, VideoCamera, MagicStick, DocumentCopy, SwitchButton, Grid,
  Aim, Fold, Expand, UserFilled, Lock, Check,
} from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useAuthStore } from '@/stores/auth'
import { authApi } from '@/api/auth'
import { devicesApi } from '@/api/devices'

const auth = useAuthStore()
const route = useRoute()
const router = useRouter()

const isCollapse = ref(false)
const onlineCount = ref(0)
let pollTimer = null

const menus = [
  { path: '/dashboard', title: '总览仪表盘', icon: House },
  { path: '/devices', title: '设备管理', icon: Cpu },
  { path: '/device-groups', title: '设备分组', icon: Grid },
  { path: '/trackpad', title: '虚拟触控板', icon: Aim },
  { path: '/console', title: '动作控制台', icon: VideoCamera },
  { path: '/macros', title: '宏编排库', icon: MagicStick },
  { path: '/logs', title: '动作审计日志', icon: DocumentCopy },
]

const activePath = computed(() => {
  if (route.path.startsWith('/devices/')) return '/devices'
  return route.path
})

// 修改密码弹窗状态
const pwdDialog = reactive({
  visible: false,
  loading: false,
  oldPassword: '',
  newPassword: '',
  confirmPassword: '',
})

async function fetchOnlineCount() {
  try {
    const res = await devicesApi.online()
    onlineCount.value = Array.isArray(res?.online) ? res.online.length : 0
  } catch {
    // ignore
  }
}

function openChangePassword() {
  pwdDialog.oldPassword = ''
  pwdDialog.newPassword = ''
  pwdDialog.confirmPassword = ''
  pwdDialog.visible = true
}

async function submitChangePassword() {
  if (!pwdDialog.oldPassword || !pwdDialog.newPassword) {
    ElMessage.warning('请填写完整密码信息')
    return
  }
  if (pwdDialog.newPassword.length < 6) {
    ElMessage.warning('新密码长度不能少于 6 位')
    return
  }
  if (pwdDialog.newPassword !== pwdDialog.confirmPassword) {
    ElMessage.warning('两次输入的新密码不一致')
    return
  }

  pwdDialog.loading = true
  try {
    await authApi.changePassword(pwdDialog.oldPassword, pwdDialog.newPassword)
    ElMessage.success('密码修改成功，请使用新密码重新登录')
    pwdDialog.visible = false
    auth.logout()
    router.push('/login')
  } catch (err) {
    const msg = err.response?.data?.detail || err.message || '修改密码失败'
    ElMessage.error(msg)
  } finally {
    pwdDialog.loading = false
  }
}

function handleLogout() {
  ElMessageBox.confirm('确定要退出当前登录账号吗？', '退出确认', {
    confirmButtonText: '退出',
    cancelButtonText: '取消',
    type: 'warning',
  }).then(() => {
    auth.logout()
    ElMessage.info('已安全退出')
    router.push('/login')
  }).catch(() => {})
}

onMounted(() => {
  fetchOnlineCount()
  pollTimer = setInterval(fetchOnlineCount, 5000)
})

onBeforeUnmount(() => {
  if (pollTimer) clearInterval(pollTimer)
})
</script>

<template>
  <el-container class="app-layout">
    <!-- Modern Sidebar -->
    <el-aside :width="isCollapse ? '68px' : '230px'" class="app-aside">
      <div class="aside-brand">
        <div class="brand-logo-wrap">
          <span class="status-dot online"></span>
          <span v-if="!isCollapse" class="brand-text">HID Gateway</span>
        </div>
      </div>

      <el-menu
        :default-active="activePath"
        :collapse="isCollapse"
        :collapse-transition="false"
        class="aside-menu"
        router
      >
        <el-menu-item v-for="m in menus" :key="m.path" :index="m.path" class="menu-item-custom">
          <el-icon class="menu-icon"><component :is="m.icon" /></el-icon>
          <template #title>
            <span class="menu-title">{{ m.title }}</span>
          </template>
        </el-menu-item>
      </el-menu>

      <!-- Sidebar Footer -->
      <div class="aside-footer">
        <button
          type="button"
          class="collapse-btn"
          :title="isCollapse ? '展开菜单' : '折叠菜单'"
          @click="isCollapse = !isCollapse"
        >
          <el-icon><component :is="isCollapse ? Expand : Fold" /></el-icon>
          <span v-if="!isCollapse" style="margin-left: 8px">折叠菜单</span>
        </button>
      </div>
    </el-aside>

    <!-- Main Container -->
    <el-container class="main-wrapper">
      <!-- Top Header -->
      <el-header height="64px" class="app-header">
        <div class="header-left">
          <h2 class="page-title">{{ route.meta?.title || '控制台' }}</h2>
        </div>

        <div class="header-right">
          <!-- Online Device Pill -->
          <div class="header-badge" title="当前连接至网关的在线设备数">
            <span class="status-dot" :class="onlineCount > 0 ? 'online' : 'offline'"></span>
            <span class="badge-text">{{ onlineCount }} 设备在线</span>
          </div>

          <!-- Quick Trackpad Link -->
          <el-button
            size="small"
            type="primary"
            plain
            class="header-action-btn"
            @click="router.push('/trackpad')"
          >
            <el-icon style="margin-right: 4px"><Aim /></el-icon>
            触控板
          </el-button>

          <!-- User Profile Dropdown -->
          <el-dropdown trigger="click">
            <div class="user-avatar-badge">
              <el-avatar :size="32" class="user-avatar">
                <el-icon><UserFilled /></el-icon>
              </el-avatar>
              <div class="user-info">
                <span class="user-name">{{ auth.user?.username || '用户' }}</span>
                <span class="user-role">{{ auth.user?.role === 'admin' ? '管理员' : '控制员' }}</span>
              </div>
            </div>
            <template #dropdown>
              <el-dropdown-menu class="user-menu-dropdown">
                <el-dropdown-item @click="openChangePassword">
                  <el-icon><Lock /></el-icon>
                  修改密码
                </el-dropdown-item>
                <el-dropdown-item divided @click="handleLogout">
                  <el-icon><SwitchButton /></el-icon>
                  安全退出
                </el-dropdown-item>
              </el-dropdown-menu>
            </template>
          </el-dropdown>
        </div>
      </el-header>

      <!-- Main Body View with Transition -->
      <el-main class="app-main">
        <RouterView v-slot="{ Component }">
          <transition name="fade-slide" mode="out-in">
            <component :is="Component" />
          </transition>
        </RouterView>
      </el-main>
    </el-container>
  </el-container>

  <!-- 修改密码弹窗 -->
  <el-dialog
    v-model="pwdDialog.visible"
    title="修改账号密码"
    width="420px"
    destroy-on-close
  >
    <el-form label-position="top" @submit.prevent="submitChangePassword">
      <el-form-item label="当前原密码">
        <el-input
          v-model="pwdDialog.oldPassword"
          type="password"
          show-password
          placeholder="请输入原密码"
        />
      </el-form-item>
      <el-form-item label="设置新密码">
        <el-input
          v-model="pwdDialog.newPassword"
          type="password"
          show-password
          placeholder="请输入新密码 (不少于6位)"
        />
      </el-form-item>
      <el-form-item label="确认新密码">
        <el-input
          v-model="pwdDialog.confirmPassword"
          type="password"
          show-password
          placeholder="请再次输入新密码"
        />
      </el-form-item>
    </el-form>
    <template #footer>
      <el-button @click="pwdDialog.visible = false">取消</el-button>
      <el-button type="primary" :loading="pwdDialog.loading" @click="submitChangePassword">
        确认修改
      </el-button>
    </template>
  </el-dialog>
</template>

<style scoped>
.app-layout {
  height: 100vh;
  width: 100%;
  overflow: hidden;
  background-color: #f1f5f9;
}

/* Aside Styling */
.app-aside {
  background: linear-gradient(180deg, #0f172a 0%, #090d16 100%);
  display: flex;
  flex-direction: column;
  border-right: 1px solid rgba(255, 255, 255, 0.08);
  transition: width 0.25s cubic-bezier(0.4, 0, 0.2, 1);
  overflow-x: hidden;
  z-index: 20;
}

.aside-brand {
  height: 64px;
  display: flex;
  align-items: center;
  padding: 0 20px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.06);
}

.brand-logo-wrap {
  display: flex;
  align-items: center;
  gap: 12px;
}

.brand-text {
  font-size: 16px;
  font-weight: 700;
  letter-spacing: -0.3px;
  background: linear-gradient(135deg, #ffffff 40%, #60a5fa 100%);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  white-space: nowrap;
}

.aside-menu {
  flex: 1;
  background: transparent;
  border-right: none;
  padding: 12px 8px;
}

.aside-menu :deep(.el-menu-item) {
  height: 46px;
  line-height: 46px;
  border-radius: 10px;
  margin-bottom: 4px;
  color: #94a3b8;
  transition: all 0.2s ease;
}

.aside-menu :deep(.el-menu-item:hover) {
  background: rgba(255, 255, 255, 0.05);
  color: #f8fafc;
}

.aside-menu :deep(.el-menu-item.is-active) {
  background: linear-gradient(90deg, rgba(59, 130, 246, 0.22) 0%, rgba(59, 130, 246, 0.05) 100%);
  color: #38bdf8;
  font-weight: 600;
  box-shadow: inset 3px 0 0 #38bdf8;
}

.menu-icon {
  font-size: 18px;
  color: inherit;
}

.aside-footer {
  padding: 14px;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}

.collapse-btn {
  width: 100%;
  height: 38px;
  background: rgba(255, 255, 255, 0.04);
  border: 1px solid rgba(255, 255, 255, 0.08);
  border-radius: 8px;
  color: #94a3b8;
  display: flex;
  align-items: center;
  justify-content: center;
  cursor: pointer;
  font-size: 13px;
  transition: all 0.2s ease;
}

.collapse-btn:hover {
  background: rgba(255, 255, 255, 0.08);
  color: #ffffff;
}

/* Header Styling */
.main-wrapper {
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.app-header {
  background: rgba(255, 255, 255, 0.85);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
  border-bottom: 1px solid #e2e8f0;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 24px;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.02);
}

.page-title {
  margin: 0;
  font-size: 17px;
  font-weight: 600;
  color: #0f172a;
  letter-spacing: -0.2px;
}

.header-right {
  display: flex;
  align-items: center;
  gap: 16px;
}

.header-badge {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 12px;
  background: #f1f5f9;
  border-radius: 9999px;
  border: 1px solid #e2e8f0;
  font-size: 12px;
  font-weight: 500;
  color: #475569;
}

.header-action-btn {
  border-radius: 8px;
}

.user-avatar-badge {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 4px 8px;
  border-radius: 10px;
  cursor: pointer;
  transition: background 0.2s ease;
}

.user-avatar-badge:hover {
  background: rgba(0, 0, 0, 0.04);
}

.user-avatar {
  background: linear-gradient(135deg, #3b82f6 0%, #8b5cf6 100%);
  color: #ffffff;
}

.user-info {
  display: flex;
  flex-direction: column;
  line-height: 1.2;
}

.user-name {
  font-size: 13px;
  font-weight: 600;
  color: #1e293b;
}

.user-role {
  font-size: 11px;
  color: #94a3b8;
}

/* Main Content Area */
.app-main {
  padding: 20px 24px;
  overflow-y: auto;
  background-color: #f8fafc;
}

/* Page Transition Animation */
.fade-slide-enter-active,
.fade-slide-leave-active {
  transition: all 0.2s cubic-bezier(0.4, 0, 0.2, 1);
}

.fade-slide-enter-from {
  opacity: 0;
  transform: translateY(6px);
}

.fade-slide-leave-to {
  opacity: 0;
  transform: translateY(-6px);
}
</style>
