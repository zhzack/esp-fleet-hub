import { fileURLToPath, URL } from 'node:url'

import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import vueDevTools from 'vite-plugin-vue-devtools'

// dev: 把 /api 和 /ws 代理到后端容器（compose.dev.yml）或本机
// 在容器内用 service name 指；本机直接跑 npm run dev 时回退到 127.0.0.1
const API_TARGET = process.env.VITE_API_PROXY_TARGET || 'http://127.0.0.1:8000'
const WS_TARGET  = process.env.VITE_WS_PROXY_TARGET  || 'ws://127.0.0.1:8000'

export default defineConfig({
  plugins: [
    vue(),
    vueDevTools(),
  ],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url))
    },
  },
  server: {
    host: true,           // 0.0.0.0，容器外可访问
    port: 5173,
    proxy: {
      '/api': {
        target: API_TARGET,
        changeOrigin: true,
      },
      '/ws': {
        target: WS_TARGET,
        ws: true,
        changeOrigin: true,
      },
    },
  },
})
