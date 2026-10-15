<script>
export const meta = { layout: 'blank', requiresAuth: false, title: '登录 & 注册' }
</script>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import { User, Lock, Key, ArrowRight, Connection } from '@element-plus/icons-vue'
import { useAuthStore } from '@/stores/auth'
import { authApi } from '@/api/auth'

const auth = useAuthStore()
const router = useRouter()
const route = useRoute()

const mode = ref('login') // 'login' | 'register'
const loading = ref(false)
const formRef = ref(null)
const isShaking = ref(false)
const rememberMe = ref(true)

const form = reactive({
  username: '',
  password: '',
  confirmPassword: '',
  role: 'user',
})

// 密码强度计算 (0 ~ 4)
const passwordStrength = computed(() => {
  const p = form.password
  if (!p) return 0
  let score = 0
  if (p.length >= 6) score += 1
  if (p.length >= 10) score += 1
  if (/[0-9]/.test(p) && /[a-zA-Z]/.test(p)) score += 1
  if (/[^a-zA-Z0-9]/.test(p)) score += 1
  return score
})

const strengthText = computed(() => {
  switch (passwordStrength.value) {
    case 1: return { text: '弱', color: '#ef4444', percent: 25 }
    case 2: return { text: '中', color: '#f59e0b', percent: 50 }
    case 3: return { text: '较强', color: '#3b82f6', percent: 75 }
    case 4: return { text: '极佳', color: '#10b981', percent: 100 }
    default: return { text: '', color: '#94a3b8', percent: 0 }
  }
})

function triggerShake() {
  isShaking.value = true
  setTimeout(() => { isShaking.value = false }, 500)
}

function quickFillDemo() {
  form.username = 'alice'
  form.password = 'secret123'
  ElMessage.info('已快速填入测试账号 alice')
}

async function submit() {
  const u = form.username.trim()
  const p = form.password

  if (!u || !p) {
    ElMessage.warning('请输入用户名和密码')
    triggerShake()
    return
  }

  if (u.length < 3) {
    ElMessage.warning('用户名至少需要 3 个字符')
    triggerShake()
    return
  }

  if (p.length < 6) {
    ElMessage.warning('密码长度至少需要 6 个字符')
    triggerShake()
    return
  }

  if (mode.value === 'register') {
    if (form.password !== form.confirmPassword) {
      ElMessage.warning('两次输入的密码不一致')
      triggerShake()
      return
    }
  }

  loading.value = true
  try {
    if (mode.value === 'register') {
      await authApi.register(u, p, form.role)
      ElMessage.success('注册成功！正在为您自动登录...')
      // 注册后自动登录
      await auth.login(u, p)
      if (rememberMe.value) {
        localStorage.setItem('saved_username', u)
      }
      router.push(route.query.redirect || '/dashboard')
    } else {
      await auth.login(u, p)
      ElMessage.success('欢迎回来！')
      if (rememberMe.value) {
        localStorage.setItem('saved_username', u)
      } else {
        localStorage.removeItem('saved_username')
      }
      router.push(route.query.redirect || '/dashboard')
    }
  } catch (err) {
    triggerShake()
    const msg = err.response?.data?.detail || err.message || '操作失败，请重试'
    ElMessage.error(typeof msg === 'string' ? msg : JSON.stringify(msg))
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  const saved = localStorage.getItem('saved_username')
  if (saved) {
    form.username = saved
  }
})
</script>

<template>
  <div class="auth-card" :class="{ 'animate-shake': isShaking }">
    <!-- Brand Header -->
    <div class="auth-brand">
      <div class="brand-badge">
        <el-icon :size="24" class="brand-icon"><Connection /></el-icon>
      </div>
      <h1 class="brand-title">HID Gateway</h1>
      <p class="brand-subtitle">硬件级网络外设控制与中继中枢</p>
    </div>

    <!-- Mode Switch Tabs -->
    <div class="mode-tabs">
      <button
        type="button"
        class="tab-btn"
        :class="{ active: mode === 'login' }"
        @click="mode = 'login'"
      >
        账号登录
      </button>
      <button
        type="button"
        class="tab-btn"
        :class="{ active: mode === 'register' }"
        @click="mode = 'register'"
      >
        新用户注册
      </button>
    </div>

    <!-- Form Body -->
    <el-form
      ref="formRef"
      @submit.prevent="submit"
      label-position="top"
      class="auth-form"
    >
      <el-form-item label="用户名">
        <el-input
          v-model="form.username"
          :prefix-icon="User"
          placeholder="请输入用户名 (≥3位)"
          autocomplete="username"
          clearable
          size="large"
          @keyup.enter="submit"
        />
      </el-form-item>

      <el-form-item label="密码">
        <el-input
          v-model="form.password"
          :prefix-icon="Lock"
          type="password"
          placeholder="请输入密码 (≥6位)"
          show-password
          autocomplete="current-password"
          size="large"
          @keyup.enter="submit"
        />
        <!-- 密码强度条 (注册模式显示) -->
        <div v-if="mode === 'register' && form.password" class="password-meter">
          <div class="meter-bar">
            <div
              class="meter-fill"
              :style="{ width: strengthText.percent + '%', backgroundColor: strengthText.color }"
            ></div>
          </div>
          <span class="meter-label" :style="{ color: strengthText.color }">
            {{ strengthText.text }}
          </span>
        </div>
      </el-form-item>

      <!-- 确认密码 (注册模式) -->
      <el-form-item v-if="mode === 'register'" label="确认密码">
        <el-input
          v-model="form.confirmPassword"
          :prefix-icon="Key"
          type="password"
          placeholder="请再次输入密码"
          show-password
          autocomplete="new-password"
          size="large"
          @keyup.enter="submit"
        />
      </el-form-item>

      <!-- 角色选择 (注册模式) -->
      <el-form-item v-if="mode === 'register'" label="用户角色">
        <el-radio-group v-model="form.role" class="role-group">
          <el-radio-button label="user">普通控制员 (User)</el-radio-button>
          <el-radio-button label="admin">系统管理员 (Admin)</el-radio-button>
        </el-radio-group>
      </el-form-item>

      <!-- 记住账号 & 找回密码 (登录模式) -->
      <div v-if="mode === 'login'" class="form-auxiliary">
        <el-checkbox v-model="rememberMe">记住账号</el-checkbox>
        <span class="demo-quick" @click="quickFillDemo">填入测试账号</span>
      </div>

      <!-- 提交按钮 -->
      <el-button
        type="primary"
        :loading="loading"
        class="submit-button"
        size="large"
        @click="submit"
      >
        <span>{{ mode === 'login' ? '立即登录' : '创建账号' }}</span>
        <el-icon class="el-icon--right"><ArrowRight /></el-icon>
      </el-button>
    </el-form>

    <!-- Footer Switch -->
    <div class="auth-footer">
      <span v-if="mode === 'login'">还没有账号？</span>
      <span v-else>已经拥有账号？</span>
      <button
        type="button"
        class="switch-link"
        @click="mode = mode === 'login' ? 'register' : 'login'"
      >
        {{ mode === 'login' ? '免费注册' : '返回登录' }}
      </button>
    </div>
  </div>
</template>

<style scoped>
.auth-card {
  width: 100%;
  max-width: 440px;
  background: rgba(15, 23, 42, 0.72);
  backdrop-filter: blur(24px);
  -webkit-backdrop-filter: blur(24px);
  border: 1px solid rgba(255, 255, 255, 0.12);
  border-radius: 24px;
  padding: 38px 36px;
  box-shadow: 
    0 20px 40px -15px rgba(0, 0, 0, 0.5),
    0 0 0 1px rgba(255, 255, 255, 0.05),
    inset 0 1px 1px rgba(255, 255, 255, 0.15);
  transition: all 0.3s ease;
}

/* Brand Header */
.auth-brand {
  text-align: center;
  margin-bottom: 26px;
}

.brand-badge {
  width: 52px;
  height: 52px;
  margin: 0 auto 12px;
  background: linear-gradient(135deg, rgba(59, 130, 246, 0.25) 0%, rgba(168, 85, 247, 0.25) 100%);
  border: 1px solid rgba(96, 165, 250, 0.4);
  border-radius: 16px;
  display: flex;
  align-items: center;
  justify-content: center;
  box-shadow: 0 0 20px rgba(59, 130, 246, 0.35);
}

.brand-icon {
  color: #60a5fa;
  animation: pulse-icon 3s ease-in-out infinite;
}

@keyframes pulse-icon {
  0%, 100% { transform: scale(1); filter: drop-shadow(0 0 4px #60a5fa); }
  50% { transform: scale(1.08); filter: drop-shadow(0 0 10px #93c5fd); }
}

.brand-title {
  margin: 0;
  font-size: 24px;
  font-weight: 700;
  letter-spacing: -0.5px;
  background: linear-gradient(135deg, #ffffff 30%, #93c5fd 100%);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
}

.brand-subtitle {
  margin: 6px 0 0;
  font-size: 13px;
  color: #94a3b8;
  letter-spacing: 0.2px;
}

/* Mode Switch Tabs */
.mode-tabs {
  display: flex;
  background: rgba(30, 41, 59, 0.7);
  border-radius: 12px;
  padding: 4px;
  margin-bottom: 24px;
  border: 1px solid rgba(255, 255, 255, 0.06);
}

.tab-btn {
  flex: 1;
  padding: 9px 0;
  border: none;
  background: transparent;
  color: #94a3b8;
  font-size: 13px;
  font-weight: 600;
  border-radius: 8px;
  cursor: pointer;
  transition: all 0.2s ease;
}

.tab-btn.active {
  background: rgba(59, 130, 246, 0.25);
  color: #ffffff;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.2), inset 0 0 0 1px rgba(96, 165, 250, 0.3);
}

.tab-btn:hover:not(.active) {
  color: #cbd5e1;
}

/* Form Styles */
.auth-form :deep(.el-form-item__label) {
  color: #cbd5e1;
  font-size: 13px;
  font-weight: 500;
  margin-bottom: 6px;
}

.auth-form :deep(.el-input__wrapper) {
  background: rgba(30, 41, 59, 0.85);
  box-shadow: 0 0 0 1px rgba(255, 255, 255, 0.1) inset;
  border-radius: 10px;
  padding: 4px 14px;
}

.auth-form :deep(.el-input__wrapper:hover) {
  box-shadow: 0 0 0 1px rgba(96, 165, 250, 0.4) inset;
}

.auth-form :deep(.el-input__wrapper.is-focus) {
  box-shadow: 0 0 0 1.5px #3b82f6 inset, 0 0 16px rgba(59, 130, 246, 0.25) !important;
}

.auth-form :deep(.el-input__inner) {
  color: #f8fafc;
  font-size: 14px;
}

.auth-form :deep(.el-input__prefix-inner) {
  color: #64748b;
  margin-right: 8px;
}

/* Password strength meter */
.password-meter {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 6px;
  width: 100%;
}

.meter-bar {
  flex: 1;
  height: 4px;
  background: rgba(255, 255, 255, 0.1);
  border-radius: 2px;
  overflow: hidden;
}

.meter-fill {
  height: 100%;
  transition: all 0.3s ease;
  border-radius: 2px;
}

.meter-label {
  font-size: 11px;
  font-weight: 600;
}

/* Role Selector Group */
.role-group {
  width: 100%;
  display: flex;
}

.role-group :deep(.el-radio-button) {
  flex: 1;
}

.role-group :deep(.el-radio-button__inner) {
  width: 100%;
  background: rgba(30, 41, 59, 0.8);
  border-color: rgba(255, 255, 255, 0.1);
  color: #94a3b8;
  font-size: 12px;
}

.role-group :deep(.el-radio-button__original-radio:checked + .el-radio-button__inner) {
  background: #3b82f6;
  border-color: #3b82f6;
  color: #ffffff;
  box-shadow: -1px 0 0 0 #3b82f6;
}

/* Form Auxiliary Links */
.form-auxiliary {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin: -6px 0 20px;
}

.form-auxiliary :deep(.el-checkbox__label) {
  color: #94a3b8;
  font-size: 13px;
}

.demo-quick {
  font-size: 12px;
  color: #60a5fa;
  cursor: pointer;
  transition: color 0.2s ease;
}

.demo-quick:hover {
  color: #93c5fd;
  text-decoration: underline;
}

/* Submit Button */
.submit-button {
  width: 100%;
  height: 44px;
  border-radius: 10px;
  font-size: 15px;
  font-weight: 600;
  letter-spacing: 0.5px;
  background: linear-gradient(135deg, #3b82f6 0%, #6366f1 100%);
  border: none;
  box-shadow: 0 4px 16px rgba(59, 130, 246, 0.35);
  margin-top: 4px;
}

.submit-button:hover {
  background: linear-gradient(135deg, #60a5fa 0%, #4f46e5 100%);
  box-shadow: 0 6px 20px rgba(59, 130, 246, 0.5);
}

/* Footer Link */
.auth-footer {
  margin-top: 24px;
  text-align: center;
  font-size: 13px;
  color: #94a3b8;
}

.switch-link {
  background: none;
  border: none;
  color: #60a5fa;
  font-size: 13px;
  font-weight: 600;
  cursor: pointer;
  margin-left: 4px;
  padding: 0;
  transition: color 0.2s ease;
}

.switch-link:hover {
  color: #93c5fd;
  text-decoration: underline;
}
</style>
