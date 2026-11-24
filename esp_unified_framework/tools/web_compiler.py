#!/usr/bin/env python3
"""
LuatOS-Style Visual Web Compiler for ESP Unified Framework
Provides a browser-based UI to customize features, select chips, and trigger remote NAS builds
with full hardware capability interlocking and compatibility validation.
"""

import sys, os, json, time, subprocess, threading
from http.server import HTTPServer, BaseHTTPRequestHandler
import urllib.parse, urllib.request

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DIST_DIR = os.path.join(PROJECT_ROOT, "dist")
PROFILES_DIR = os.path.join(PROJECT_ROOT, "profiles")
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tools"))
import build_flavor

s_current_task = {
    "status": "idle", # "idle", "building", "success", "failed"
    "log": "",
    "artifact_name": "",
    "artifact_size": 0,
    "percent": 0
}
s_lock = threading.Lock()

HTML_PAGE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>ESP 固件定制编译平台 (LuatOS Cloud Compilation Style)</title>
<style>
:root{--bg:#0b1120;--surface:#1e293b;--border:#334155;--primary:#3b82f6;--primary-hover:#2563eb;--text:#f8fafc;--text-dim:#94a3b8;--success:#10b981;--warn:#f59e0b;--danger:#ef4444;--radius:10px}
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
body{background:var(--bg);color:var(--text);padding:20px;font-size:14px;line-height:1.5}
.container{max-width:1050px;margin:0 auto;display:flex;flex-direction:column;gap:18px}
header{display:flex;justify-content:space-between;align-items:center;background:var(--surface);padding:16px 24px;border-radius:var(--radius);border:1px solid var(--border)}
.logo{display:flex;align-items:center;gap:12px}
.logo-badge{width:36px;height:36px;background:linear-gradient(135deg,#3b82f6,#8b5cf6);border-radius:8px;display:flex;align-items:center;justify-content:center;font-size:20px}
.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:20px;display:flex;flex-direction:column;gap:16px;box-shadow:0 4px 6px -1px rgba(0,0,0,0.2)}
.card-title{font-size:16px;font-weight:600;display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--border);padding-bottom:12px}
.chip-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}
.chip-opt{border:2px solid var(--border);border-radius:8px;padding:14px;cursor:pointer;transition:all .15s;background:#0f172a66;text-align:center}
.chip-opt:hover{border-color:var(--primary)}
.chip-opt.active{border-color:var(--primary);background:#1e3a8a44;box-shadow:0 0 0 1px var(--primary)}
.chip-name{font-weight:bold;font-size:15px;color:#fff}
.chip-sub{font-size:11px;color:var(--text-dim);margin-top:4px}
.feat-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(290px,1fr));gap:12px}
.feat-box{background:#0f172a80;border:1px solid var(--border);border-radius:8px;padding:12px 14px;display:flex;align-items:flex-start;gap:12px;cursor:pointer;transition:all .15s;position:relative}
.feat-box:hover{border-color:#475569}
.feat-box.disabled{opacity:0.42;cursor:not-allowed;background:#090d16;border-style:dashed;border-color:#33415566}
.feat-box input[type=checkbox]{margin-top:3px;transform:scale(1.2);accent-color:var(--primary)}
.feat-info strong{display:flex;align-items:center;gap:6px;color:#fff;font-size:13px}
.feat-info p{font-size:11px;color:var(--text-dim);margin-top:2px}
.feat-badge{font-size:10px;padding:1px 6px;border-radius:4px;font-weight:600}
.feat-badge.supported{background:#065f4688;color:#6ee7b7}
.feat-badge.unsupported{background:#7f1d1d88;color:#fca5a5}
.input-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}
label{font-size:12px;color:var(--text-dim);display:flex;flex-direction:column;gap:6px}
input[type=text]{background:#0f172a;border:1px solid var(--border);color:#fff;padding:8px 12px;border-radius:6px;font-size:13px}
input[type=text]:focus{outline:none;border-color:var(--primary)}
button{cursor:pointer;background:var(--primary);color:#fff;border:none;border-radius:6px;padding:10px 18px;font-size:14px;font-weight:600;transition:all .15s;display:inline-flex;align-items:center;justify-content:center;gap:8px}
button:hover{background:var(--primary-hover)}
button.secondary{background:#334155;color:#cbd5e1}
button.secondary:hover{background:#475569}
button:disabled{opacity:0.5;cursor:not-allowed}
pre.console{background:#050b14;color:#38bdf8;padding:14px;border-radius:8px;font-family:monospace;font-size:11px;height:220px;overflow-y:auto;white-space:pre-wrap;border:1px solid #1e293b}
.status-pill{font-size:12px;padding:3px 10px;border-radius:12px;background:#334155;color:#fff}
.status-pill.success{background:#065f46;color:#6ee7b7}
.status-pill.building{background:#1e3a8a;color:#93c5fd}
.status-pill.failed{background:#7f1d1d;color:#fca5a5}
.preset-bar{display:flex;gap:8px;flex-wrap:wrap}
</style>
</head>
<body>
<div class="container">
  <header>
    <div class="logo">
      <div class="logo-badge">&#9881;</div>
      <div>
        <h1 style="font-size:18px;font-weight:bold">ESP 固件定制云编译平台</h1>
        <p style="font-size:11px;color:var(--text-dim)">LuatOS-Style Component Matrix & Fast NAS Pipeline</p>
      </div>
    </div>
    <div style="display:flex;gap:8px;align-items:center">
      <span class="status-pill" id="compile-status">空闲</span>
    </div>
  </header>

  <!-- Step 1: Chip Selection -->
  <div class="card">
    <div class="card-title">
      <span>1. 选择目标芯片架构 (Target SoC)</span>
      <span style="font-size:12px;color:var(--text-dim)">选择芯片后，下方功能组件将根据芯片物理硅片能力自动互锁！</span>
    </div>
    <div class="chip-grid">
      <div class="chip-opt active" onclick="selectChip('esp32', this)">
        <div class="chip-name">ESP32 (经典双核)</div>
        <div class="chip-sub">支持 DVP 摄像头 / 外部 PSRAM</div>
      </div>
      <div class="chip-opt" onclick="selectChip('esp32s3', this)">
        <div class="chip-name">ESP32-S3 (AI与多媒体)</div>
        <div class="chip-sub">支持 原生 USB-OTG / 摄像头 / LCD</div>
      </div>
      <div class="chip-opt" onclick="selectChip('esp32c3', this)">
        <div class="chip-name">ESP32-C3 (RISC-V)</div>
        <div class="chip-sub">极简单核 / 低功耗开关 / 无摄像头与OTG</div>
      </div>
      <div class="chip-opt" onclick="selectChip('esp32c2', this)">
        <div class="chip-name">ESP32-C2 (超轻量)</div>
        <div class="chip-sub">成本优化型小内存 / 仅基础 GPIO 与 API</div>
      </div>
    </div>
  </div>

  <!-- Step 2: Component Customization -->
  <div class="card">
    <div class="card-title">
      <span>2. 定制功能组件矩阵 (自动根据芯片物理硬件动态适配)</span>
      <div class="preset-bar">
        <button class="secondary" style="font-size:11px;padding:4px 8px" onclick="loadPreset('esp32_cam')">ESP32-CAM 预设</button>
        <button class="secondary" style="font-size:11px;padding:4px 8px" onclick="loadPreset('c3_relay')">C3 继电器预设</button>
        <button class="secondary" style="font-size:11px;padding:4px 8px" onclick="loadPreset('s3_dock')">S3 控制坞预设</button>
      </div>
    </div>
    <div class="feat-grid">
      <label class="feat-box" id="box_web_ui">
        <input type="checkbox" id="f_web_ui" checked/>
        <div class="feat-info">
          <strong>Web 桌面控制台 (Web UI) <span class="feat-badge supported" id="badge_web_ui">全芯片支持</span></strong>
          <p>嵌入自适应 HTML5 控制面板（取消勾选则为无头纯 API 固件，省 Flash）</p>
        </div>
      </label>

      <label class="feat-box" id="box_rest_api">
        <input type="checkbox" id="f_rest_api" checked/>
        <div class="feat-info">
          <strong>HTTP REST 配置接口 (REST API) <span class="feat-badge supported" id="badge_rest_api">全芯片支持</span></strong>
          <p>提供 /api/system/*、/api/gpio/*、/api/config 供网络调用</p>
        </div>
      </label>

      <label class="feat-box" id="box_ota">
        <input type="checkbox" id="f_ota" checked/>
        <div class="feat-info">
          <strong>双分区无线 OTA (Wireless OTA) <span class="feat-badge supported" id="badge_ota">全芯片支持</span></strong>
          <p>双 1.75MB A/B 分区空中升级，支持坏固件自动回滚</p>
        </div>
      </label>

      <label class="feat-box" id="box_camera">
        <input type="checkbox" id="f_camera" checked/>
        <div class="feat-info">
          <strong>摄像头模块 (OV2640 / OV5640) <span class="feat-badge supported" id="badge_camera">仅 ESP32 / S3</span></strong>
          <p>含底层 SCCB 驱动、双 PSRAM 帧缓冲与实时 MJPEG 串流</p>
        </div>
      </label>

      <label class="feat-box" id="box_gpio_ctrl">
        <input type="checkbox" id="f_gpio_ctrl" checked/>
        <div class="feat-info">
          <strong>GPIO 开关与 PWM 调光 (GPIO Control) <span class="feat-badge supported" id="badge_gpio_ctrl">全芯片支持</span></strong>
          <p>动态引脚电平控制、PWM 渐变调光与状态查询矩阵</p>
        </div>
      </label>

      <label class="feat-box" id="box_spiram">
        <input type="checkbox" id="f_spiram" checked/>
        <div class="feat-info">
          <strong>外部伪静态内存 (PSRAM / SPIRAM) <span class="feat-badge supported" id="badge_spiram">仅 ESP32 / S3</span></strong>
          <p>使能外部 2MB~8MB RAM，摄像头与大型任务必备</p>
        </div>
      </label>

      <label class="feat-box" id="box_usb_otg">
        <input type="checkbox" id="f_usb_otg"/>
        <div class="feat-info">
          <strong>原生 USB-OTG (虚拟键鼠注入) <span class="feat-badge unsupported" id="badge_usb_otg">仅 ESP32-S2/S3</span></strong>
          <p>TinyUSB 免驱 HID 注入（硬件需原生 USB 控制器）</p>
        </div>
      </label>

      <label class="feat-box" id="box_display">
        <input type="checkbox" id="f_display"/>
        <div class="feat-info">
          <strong>屏幕图形引擎 (LCD / E-Paper) <span class="feat-badge supported" id="badge_display">支持</span></strong>
          <p>ST7789 IPS 屏或 SSD1680 水墨屏驱动</p>
        </div>
      </label>

      <label class="feat-box" id="box_storage">
        <input type="checkbox" id="f_storage"/>
        <div class="feat-info">
          <strong>文件系统 (SPIFFS / SD Card) <span class="feat-badge supported" id="badge_storage">支持</span></strong>
          <p>片上 Flash 存储挂载或 MicroSD 卡文件读写</p>
        </div>
      </label>
    </div>
  </div>

  <!-- Step 3: Default Network Identity -->
  <div class="card">
    <div class="card-title">
      <span>3. 预设网络与设备身份 (固化在 Flash 中，亦可在运行时在线配置修改)</span>
    </div>
    <div class="input-grid">
      <label>默认 WiFi SSID:
        <input type="text" id="wifi_ssid" value="1"/>
      </label>
      <label>默认 WiFi 密码:
        <input type="text" id="wifi_pass" value="11111111"/>
      </label>
      <label>设备名称 (Device ID):
        <input type="text" id="device_id" value="esp32_node"/>
      </label>
      <label>mDNS 本地域名:
        <input type="text" id="mdns_host" value="esp-node"/>
      </label>
    </div>
  </div>

  <!-- Step 4: Build & Deployment -->
  <div class="card">
    <div class="card-title">
      <span>4. 执行构建与部署 (飞牛 NAS 多核极速编译器)</span>
      <div style="display:flex;gap:8px">
        <button id="btn-build" onclick="triggerBuild()">⚡ 立即生成定制固件</button>
      </div>
    </div>
    <pre class="console" id="log-box">[System Ready] 等待配置构建请求...</pre>
    <div id="result-box" style="display:none;background:#0f172a;padding:12px;border-radius:8px;border:1px solid #10b981;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px">
      <div>
        <strong style="color:#10b981">🎉 固件构建完成: </strong>
        <span id="res-filename" style="color:#fff;font-family:monospace">firmware.bin</span>
        <span id="res-filesize" style="font-size:12px;color:var(--text-dim)">(0 KB)</span>
      </div>
      <div style="display:flex;gap:8px">
        <button class="secondary" onclick="downloadFirmware()">⬇️ 下载固件文件</button>
        <button style="background:#059669" onclick="promptOtaPush()">📡 无线 OTA 推送</button>
      </div>
    </div>
  </div>
</div>

<script>
let selectedChip = "esp32";
let pollTimer = null;
let currentArtifact = "";

// 核心硬件互锁规则表
const SOC_SPECS = {
  esp32: {
    name: "ESP32 (经典双核)",
    unsupported: {
      usb_otg: "芯片无原生 USB-OTG 控制器 (仅支持外部串口芯片)"
    },
    default_freq: 160
  },
  esp32s3: {
    name: "ESP32-S3 (AI与矢量)",
    unsupported: {},
    default_freq: 240
  },
  esp32c3: {
    name: "ESP32-C3 (RISC-V)",
    unsupported: {
      camera: "C3 芯片无 8-bit 并行 DVP 摄像头硬件控制器",
      spiram: "C3 芯片无片外 PSRAM 内存总线接口",
      usb_otg: "C3 仅支持基础 USB-Serial-JTAG，不支持原生 USB-OTG 协议栈"
    },
    default_freq: 160
  },
  esp32c2: {
    name: "ESP32-C2 (超轻量)",
    unsupported: {
      camera: "C2 芯片无摄像头硬件接口",
      spiram: "C2 芯片无片外 PSRAM 支持",
      storage: "C2 Flash 容量有限，不适宜挂载 SPIFFS 分区",
      display: "C2 内存受限 (~272KB)，不适宜挂载图形渲染驱动",
      usb_otg: "C2 芯片无原生 USB-OTG 控制器"
    },
    default_freq: 120
  }
};

function selectChip(chip, el) {
  selectedChip = chip;
  document.querySelectorAll(".chip-opt").forEach(c => c.classList.remove("active"));
  el.classList.add("active");

  const spec = SOC_SPECS[chip] || SOC_SPECS.esp32;
  const allFeats = ["web_ui", "rest_api", "ota", "camera", "gpio_ctrl", "spiram", "storage", "display", "usb_otg"];

  allFeats.forEach(f => {
    const box = document.getElementById("box_" + f);
    const input = document.getElementById("f_" + f);
    const badge = document.getElementById("badge_" + f);

    if (spec.unsupported && spec.unsupported[f]) {
      // 硬件不支持：强制取消勾选并禁用，提示原因
      input.checked = false;
      input.disabled = true;
      box.classList.add("disabled");
      badge.textContent = "硬件不支持: " + spec.unsupported[f];
      badge.className = "feat-badge unsupported";
    } else {
      // 硬件支持：解开禁用状态
      input.disabled = false;
      box.classList.remove("disabled");
      badge.textContent = "硬件支持";
      badge.className = "feat-badge supported";
    }
  });
}

function loadPreset(type) {
  if (type === "esp32_cam") {
    selectChip("esp32", document.querySelectorAll(".chip-opt")[0]);
    document.getElementById("f_web_ui").checked = true;
    document.getElementById("f_rest_api").checked = true;
    document.getElementById("f_ota").checked = true;
    document.getElementById("f_camera").checked = true;
    document.getElementById("f_gpio_ctrl").checked = true;
    document.getElementById("f_spiram").checked = true;
    document.getElementById("f_storage").checked = false;
    document.getElementById("f_display").checked = false;
    document.getElementById("device_id").value = "esp32_cam_node";
    document.getElementById("mdns_host").value = "esp-cam";
  } else if (type === "c3_relay") {
    selectChip("esp32c3", document.querySelectorAll(".chip-opt")[2]);
    document.getElementById("f_web_ui").checked = true;
    document.getElementById("f_rest_api").checked = true;
    document.getElementById("f_ota").checked = true;
    document.getElementById("f_gpio_ctrl").checked = true;
    document.getElementById("f_storage").checked = false;
    document.getElementById("f_display").checked = false;
    document.getElementById("device_id").value = "esp32c3_relay";
    document.getElementById("mdns_host").value = "c3-relay";
  } else if (type === "s3_dock") {
    selectChip("esp32s3", document.querySelectorAll(".chip-opt")[1]);
    document.getElementById("f_web_ui").checked = true;
    document.getElementById("f_rest_api").checked = true;
    document.getElementById("f_ota").checked = true;
    document.getElementById("f_gpio_ctrl").checked = true;
    document.getElementById("f_display").checked = true;
    document.getElementById("f_usb_otg").checked = true;
    document.getElementById("f_spiram").checked = true;
    document.getElementById("device_id").value = "esp32s3_dock";
    document.getElementById("mdns_host").value = "s3-dock";
  }
}

async function triggerBuild() {
  const btn = document.getElementById("btn-build");
  btn.disabled = true;
  document.getElementById("result-box").style.display = "none";
  document.getElementById("compile-status").textContent = "正在构建...";
  document.getElementById("compile-status").className = "status-pill building";

  const payload = {
    name: document.getElementById("device_id").value || "custom_node",
    target: selectedChip,
    features: {
      http_server: true,
      web_ui: document.getElementById("f_web_ui").checked,
      rest_api: document.getElementById("f_rest_api").checked,
      ota: document.getElementById("f_ota").checked,
      camera: document.getElementById("f_camera").checked,
      gpio_ctrl: document.getElementById("f_gpio_ctrl").checked,
      storage: document.getElementById("f_storage").checked,
      display: document.getElementById("f_display").checked,
      usb_otg: document.getElementById("f_usb_otg").checked
    },
    hardware: {
      cpu_freq_mhz: (selectedChip === "esp32s3") ? 240 : 160,
      spiram: document.getElementById("f_spiram").checked,
      flash_size: "4MB"
    },
    defaults: {
      wifi_ssid: document.getElementById("wifi_ssid").value,
      wifi_pass: document.getElementById("wifi_pass").value,
      device_id: document.getElementById("device_id").value,
      mdns_host: document.getElementById("mdns_host").value
    }
  };

  try {
    const res = await fetch("/api/build", {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify(payload)
    });
    const data = await res.json();
    if (!data.ok) {
      alert("❌ 参数互锁校验未通过: " + data.error);
      btn.disabled = false;
      document.getElementById("compile-status").textContent = "校验拒绝";
      document.getElementById("compile-status").className = "status-pill failed";
      return;
    }
    pollStatus();
  } catch(e) {
    alert("请求失败: " + e);
    btn.disabled = false;
  }
}

function pollStatus() {
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(async () => {
    try {
      const res = await fetch("/api/status");
      const st = await res.json();
      const box = document.getElementById("log-box");
      box.textContent = st.log || "编译中...";
      box.scrollTop = box.scrollHeight;

      if (st.status === "success") {
        clearInterval(pollTimer);
        document.getElementById("btn-build").disabled = false;
        document.getElementById("compile-status").textContent = "构建成功";
        document.getElementById("compile-status").className = "status-pill success";
        currentArtifact = st.artifact_name;
        document.getElementById("res-filename").textContent = st.artifact_name;
        document.getElementById("res-filesize").textContent = "(" + (st.artifact_size/1024).toFixed(1) + " KB)";
        document.getElementById("result-box").style.display = "flex";
      } else if (st.status === "failed") {
        clearInterval(pollTimer);
        document.getElementById("btn-build").disabled = false;
        document.getElementById("compile-status").textContent = "构建失败";
        document.getElementById("compile-status").className = "status-pill failed";
      }
    } catch(e) {}
  }, 1000);
}

function downloadFirmware() {
  if (currentArtifact) {
    window.location.href = "/api/download?file=" + encodeURIComponent(currentArtifact);
  }
}

async function promptOtaPush() {
  const ip = prompt("请输入局域网中已运行设备的 IP 地址进行无线升级 (例如 192.168.68.178):", "192.168.68.178");
  if (!ip) return;
  alert("正在推送固件至 http://" + ip + "/api/ota/upload，请稍候...");
  try {
    const res = await fetch("/api/push_ota?ip=" + encodeURIComponent(ip) + "&file=" + encodeURIComponent(currentArtifact));
    const data = await res.json();
    if (data.ok) {
      alert("🎉 无线 OTA 推送成功！设备正在重启加载定制固件！");
    } else {
      alert("❌ OTA 推送失败: " + data.error);
    }
  } catch(e) {
    alert("请求异常: " + e);
  }
}

// 页面加载默认初始化
window.onload = function() {
  selectChip("esp32", document.querySelectorAll(".chip-opt")[0]);
};
</script>
</body>
</html>
"""

def run_build_task(payload):
    global s_current_task
    with s_lock:
        s_current_task["status"] = "building"
        s_current_task["log"] = ">>> [Cloud Compiler] Validating SoC capabilities and initializing job...\n"

    try:
        target = payload.get("target", "esp32")
        features = payload.get("features", {})
        hardware = payload.get("hardware", {})

        # Validation
        build_flavor.validate_soc_compatibility(target, features, hardware)

        # Save temp profile
        profile_name = payload.get("name", "custom_build")
        profile_path = os.path.join(PROFILES_DIR, f"{profile_name}.yaml")
        os.makedirs(PROFILES_DIR, exist_ok=True)
        with open(profile_path, "w", encoding="utf-8") as f:
            import yaml
            yaml.dump(payload, f)

        with s_lock:
            s_current_task["log"] += f"✅ SoC compatibility validated for [{target}]\n"
            s_current_task["log"] += f"Saved profile to {profile_path}\n"
            s_current_task["log"] += "Invoking build_flavor on Feiniu NAS pipeline...\n"

        # Generate sdkconfig.defaults
        cfg = build_flavor.generate_sdkconfig_defaults(payload)
        with open(os.path.join(PROJECT_ROOT, "sdkconfig.defaults"), "w", encoding="utf-8") as f:
            f.write(cfg)

        # Trigger idf-build
        proc = subprocess.Popen(
            ["idf-build"],
            cwd=PROJECT_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            shell=True,
            text=True,
            bufsize=1
        )

        for line in iter(proc.stdout.readline, ''):
            with s_lock:
                s_current_task["log"] += line

        proc.wait()

        if proc.returncode == 0:
            os.makedirs(DIST_DIR, exist_ok=True)
            built_bin = os.path.join(PROJECT_ROOT, "build", "esp_unified_framework.bin")
            dist_bin = os.path.join(DIST_DIR, f"{profile_name}.bin")
            if os.path.isfile(built_bin):
                import shutil
                shutil.copyfile(built_bin, dist_bin)
                size = os.path.getsize(dist_bin)
                with s_lock:
                    s_current_task["status"] = "success"
                    s_current_task["artifact_name"] = f"{profile_name}.bin"
                    s_current_task["artifact_size"] = size
                    s_current_task["log"] += f"\n🎉 Build Finished! Bespoke firmware ready: {profile_name}.bin ({size/1024:.1f} KB)\n"
            else:
                with s_lock:
                    s_current_task["status"] = "failed"
                    s_current_task["log"] += "\n❌ Binary output not found!\n"
        else:
            with s_lock:
                s_current_task["status"] = "failed"
                s_current_task["log"] += f"\n❌ Pipeline failed with exit code {proc.returncode}\n"

    except Exception as e:
        with s_lock:
            s_current_task["status"] = "failed"
            s_current_task["log"] += f"\n❌ Exception: {e}\n"

class CompilerHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        return

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path in ["/", "/index.html"]:
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(HTML_PAGE.encode("utf-8"))
        elif parsed.path == "/api/status":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            with s_lock:
                self.wfile.write(json.dumps(s_current_task).encode("utf-8"))
        elif parsed.path == "/api/download":
            qs = urllib.parse.parse_qs(parsed.query)
            fn = qs.get("file", ["custom.bin"])[0]
            fp = os.path.join(DIST_DIR, fn)
            if os.path.isfile(fp):
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Disposition", f'attachment; filename="{fn}"')
                self.send_header("Content-Length", str(os.path.getsize(fp)))
                self.end_headers()
                with open(fp, "rb") as f:
                    self.wfile.write(f.read())
            else:
                self.send_error(404, "File not found")
        elif parsed.path == "/api/push_ota":
            qs = urllib.parse.parse_qs(parsed.query)
            ip = qs.get("ip", [""])[0]
            fn = qs.get("file", [""])[0]
            fp = os.path.join(DIST_DIR, fn)
            if not ip or not os.path.isfile(fp):
                self.send_response(400)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"ok":false,"error":"Invalid IP or firmware"}')
                return
            try:
                with open(fp, "rb") as f:
                    bin_data = f.read()
                req = urllib.request.Request(
                    f"http://{ip}/api/ota/upload",
                    data=bin_data,
                    headers={"Content-Type": "application/octet-stream", "Content-Length": str(len(bin_data))}
                )
                with urllib.request.urlopen(req, timeout=60) as r:
                    res_body = r.read().decode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"ok":true}')
            except Exception as e:
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"ok": False, "error": str(e)}).encode("utf-8"))
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path == "/api/build":
            content_len = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(content_len).decode("utf-8")
            payload = json.loads(body)

            target = payload.get("target", "esp32")
            features = payload.get("features", {})
            hardware = payload.get("hardware", {})

            try:
                build_flavor.validate_soc_compatibility(target, features, hardware)
            except ValueError as val_err:
                self.send_response(400)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"ok": False, "error": str(val_err)}).encode("utf-8"))
                return

            t = threading.Thread(target=run_build_task, args=(payload,))
            t.daemon = True
            t.start()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"ok":true,"message":"Build task launched"}')

def start_server(port=8088):
    server = HTTPServer(("0.0.0.0", port), CompilerHandler)
    print("===========================================================================")
    print(f"🚀 LuatOS-Style Visual Web Compiler Running at:")
    print(f"   Local:   http://localhost:{port}")
    print(f"   LAN:     http://0.0.0.0:{port}")
    print("===========================================================================")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server.")

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8088
    start_server(port)
