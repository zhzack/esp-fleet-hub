<script setup>
import { RouterView } from 'vue-router'
</script>

<template>
  <div class="blank-layout">
    <!-- Ambient glowing backgrounds -->
    <div class="glow-orb orb-1"></div>
    <div class="glow-orb orb-2"></div>
    <div class="glow-orb orb-3"></div>
    <div class="grid-overlay"></div>

    <div class="content-container">
      <RouterView />
    </div>
  </div>
</template>

<style scoped>
.blank-layout {
  min-height: 100vh;
  width: 100%;
  position: relative;
  background: radial-gradient(circle at 50% 10%, #1e293b 0%, #0f172a 60%, #020617 100%);
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
  padding: 24px;
}

.content-container {
  position: relative;
  z-index: 10;
  width: 100%;
  display: flex;
  justify-content: center;
}

/* Ambient glow orbs */
.glow-orb {
  position: absolute;
  border-radius: 50%;
  filter: blur(80px);
  pointer-events: none;
  opacity: 0.55;
  animation: floatOrb 16s ease-in-out infinite alternate;
}

.orb-1 {
  width: 460px;
  height: 460px;
  background: radial-gradient(circle, rgba(59, 130, 246, 0.45) 0%, rgba(37, 99, 235, 0.1) 70%);
  top: -100px;
  left: 15%;
}

.orb-2 {
  width: 420px;
  height: 420px;
  background: radial-gradient(circle, rgba(99, 102, 241, 0.4) 0%, rgba(79, 70, 229, 0.08) 70%);
  bottom: -60px;
  right: 18%;
  animation-delay: -6s;
}

.orb-3 {
  width: 320px;
  height: 320px;
  background: radial-gradient(circle, rgba(14, 165, 233, 0.3) 0%, rgba(2, 132, 199, 0.05) 70%);
  top: 45%;
  left: 65%;
  animation-delay: -11s;
}

.grid-overlay {
  position: absolute;
  inset: 0;
  background-image: 
    linear-gradient(rgba(255, 255, 255, 0.03) 1px, transparent 1px),
    linear-gradient(90deg, rgba(255, 255, 255, 0.03) 1px, transparent 1px);
  background-size: 40px 40px;
  pointer-events: none;
  mask-image: radial-gradient(ellipse at center, rgba(0, 0, 0, 1) 0%, rgba(0, 0, 0, 0) 75%);
}

@keyframes floatOrb {
  0% {
    transform: translate(0, 0) scale(1);
  }
  50% {
    transform: translate(40px, 30px) scale(1.08);
  }
  100% {
    transform: translate(-30px, -20px) scale(0.95);
  }
}
</style>
