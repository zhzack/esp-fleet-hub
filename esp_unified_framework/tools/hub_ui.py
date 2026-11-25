"""
HTML5 / CSS3 / ES6 Frontend for the Unified ESP Fleet Hub & Cloud Compiler Platform.
"""

HTML_PAGE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>ESP Unified Hub - 设备注册管理与云编译平台</title>
<style>
:root{--bg:#0b1120;--surface:#1e293b;--border:#334155;--primary:#3b82f6;--primary-hover:#2563eb;--text:#f8fafc;--text-dim:#94a3b8;--success:#10b981;--warn:#f59e0b;--danger:#ef4444;--radius:10px}
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
body{background:var(--bg);color:var(--text);padding:16px;font-size:14px;line-height:1.5}
.container{max-width:1200px;margin:0 auto;display:flex;flex-direction:column;gap:16px}
header{display:flex;justify-content:space-between;align-items:center;background:var(--surface);padding:14px 20px;border-radius:var(--radius);border:1px solid var(--border);flex-wrap:wrap;gap:12px}
.logo{display:flex;align-items:center;gap:12px}
.logo-badge{width:36px;height:36px;background:linear-gradient(135deg,#3b82f6,#8b5cf6);border-radius:8px;display:flex;align-items:center;justify-content:center;font-size:20px}
.nav-tabs{display:flex;gap:8px;flex-wrap:wrap}
.nav-tab{background:#0f172a;color:var(--text-dim);border:1px solid var(--border);border-radius:8px;padding:8px 14px;cursor:pointer;font-weight:600;font-size:13px;transition:all .15s}
.nav-tab:hover{border-color:var(--primary);color:#fff}
.nav-tab.active{background:var(--primary);border-color:var(--primary);color:#fff}
.tab-pane{display:none;flex-direction:column;gap:16px}
.tab-pane.active{display:flex}
.kpi-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}
.kpi-card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:14px 16px}
.kpi-label{font-size:11px;color:var(--text-dim)}
.kpi-val{font-size:22px;font-weight:700;margin-top:4px;color:#fff}
.dev-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(340px,1fr));gap:14px}
.dev-card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:16px;display:flex;flex-direction:column;gap:10px;position:relative}
.dev-card.online{border-color:#10b98166}
.pill{font-size:11px;padding:2px 8px;border-radius:12px;font-weight:600;display:inline-flex;align-items:center;gap:4px}
.pill.online{background:#065f46;color:#6ee7b7}
.pill.offline{background:#334155;color:#cbd5e1}
.pill.provisioned{background:#1e3a8a;color:#93c5fd}
.pill.warn{background:#78350f;color:#fde68a}
.cap-tags{display:flex;flex-wrap:wrap;gap:6px}
.cap-tag{font-size:10px;padding:2px 7px;border-radius:4px;background:#0f172a;border:1px solid #334155;color:#cbd5e1}
.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:18px;display:flex;flex-direction:column;gap:14px;box-shadow:0 4px 6px -1px rgba(0,0,0,0.2)}
.card-title{font-size:15px;font-weight:600;display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--border);padding-bottom:10px;flex-wrap:wrap;gap:8px}
.chip-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px}
.chip-opt{border:2px solid var(--border);border-radius:8px;padding:12px;cursor:pointer;transition:all .15s;background:#0f172a66;text-align:center}
.chip-opt:hover{border-color:var(--primary)}
.chip-opt.active{border-color:var(--primary);background:#1e3a8a44;box-shadow:0 0 0 1px var(--primary)}
.chip-name{font-weight:bold;font-size:14px;color:#fff}
.chip-sub{font-size:11px;color:var(--text-dim);margin-top:3px}
.feat-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:10px}
.feat-box{background:#0f172a80;border:1px solid var(--border);border-radius:8px;padding:11px 13px;display:flex;align-items:flex-start;gap:10px;cursor:pointer;transition:all .15s}
.feat-box:hover{border-color:#475569}
.feat-box.disabled{opacity:0.42;cursor:not-allowed;background:#090d16;border-style:dashed;border-color:#33415566}
.feat-box input[type=checkbox]{margin-top:3px;transform:scale(1.15);accent-color:var(--primary)}
.feat-info strong{display:flex;align-items:center;gap:6px;color:#fff;font-size:13px}
.feat-info p{font-size:11px;color:var(--text-dim);margin-top:2px}
.feat-badge{font-size:10px;padding:1px 6px;border-radius:4px;font-weight:600}
.feat-badge.supported{background:#065f4688;color:#6ee7b7}
.feat-badge.unsupported{background:#7f1d1d88;color:#fca5a5}
.input-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:10px}
label{font-size:12px;color:var(--text-dim);display:flex;flex-direction:column;gap:5px}
input[type=text],input[type=password],select{background:#0f172a;border:1px solid var(--border);color:#fff;padding:8px 10px;border-radius:6px;font-size:12px;width:100%}
input:focus,select:focus{outline:none;border-color:var(--primary)}
button{cursor:pointer;background:var(--primary);color:#fff;border:none;border-radius:6px;padding:8px 14px;font-size:12px;font-weight:600;transition:all .15s;display:inline-flex;align-items:center;justify-content:center;gap:6px}
button:hover{background:var(--primary-hover)}
button.secondary{background:#334155;color:#cbd5e1}
button.secondary:hover{background:#475569}
button.danger{background:var(--danger)}
button:disabled{opacity:0.5;cursor:not-allowed}
pre.console{background:#050b14;color:#38bdf8;padding:12px;border-radius:8px;font-family:monospace;font-size:11px;height:210px;overflow-y:auto;white-space:pre-wrap;border:1px solid #1e293b}
.status-pill{font-size:12px;padding:3px 10px;border-radius:12px;background:#334155;color:#fff}
.status-pill.success{background:#065f46;color:#6ee7b7}
.status-pill.building{background:#1e3a8a;color:#93c5fd}
.status-pill.failed{background:#7f1d1d;color:#fca5a5}
.preset-bar{display:flex;gap:6px;flex-wrap:wrap}
.endpoint-item{background:#0f172a99;border:1px solid var(--border);border-radius:6px;padding:7px 10px;display:flex;justify-content:space-between;align-items:center;font-size:12px}
.endpoint-item.disabled{opacity:0.35;background:#060a12;border-style:dashed}
.method-badge{font-size:10px;font-weight:700;padding:2px 6px;border-radius:4px;font-family:monospace}
.method-get{background:#0369a1;color:#bae6fd}
.method-post{background:#15803d;color:#bbf7d0}
.ep-path{font-family:monospace;font-size:12px;color:#f1f5f9}
.ep-status{font-size:10px;padding:2px 8px;border-radius:10px}
.ep-status.active{background:#065f46;color:#6ee7b7}
.ep-status.inactive{background:#334155;color:#94a3b8}
.modal-bg{position:fixed;inset:0;background:rgba(0,0,0,0.72);display:none;align-items:center;justify-content:center;z-index:50;padding:16px}
.modal-bg.open{display:flex}
.modal{background:var(--surface);border:1px solid var(--border);border-radius:12px;max-width:720px;width:100%;max-height:90vh;overflow-y:auto;padding:20px;display:flex;flex-direction:column;gap:14px}
table{width:100%;border-collapse:collapse;font-size:12px}
th,td{padding:8px 10px;text-align:left;border-bottom:1px solid #33415566}
th{color:var(--text-dim);font-weight:600}
</style>
</head>
<body>
<div class="container">
  <header>
    <div class="logo">
      <div class="logo-badge">&#9881;</div>
      <div>
        <h1 style="font-size:17px;font-weight:bold">ESP Unified Hub &amp; Cloud Compiler</h1>
        <p style="font-size:11px;color:var(--text-dim)">mDNS 零配置自动发现 · HMAC 防伪注册 · 影子配置同步 · 跨芯片云编译与产线预制</p>
      </div>
    </div>
    <div class="nav-tabs">
      <button class="nav-tab active" onclick="switchTab('fleet', this)">&#127760; 设备注册与控制中心</button>
      <button class="nav-tab" onclick="switchTab('ota', this)">&#128230; 固件库与批量 OTA</button>
      <button class="nav-tab" onclick="switchTab('compiler', this)">&#128736; 固件云编译与串口烧录</button>
    </div>
    <span class="status-pill" id="compile-status">空闲</span>
  </header>

  <!-- ==================== TAB 1: FLEET HUB DASHBOARD ==================== -->
  <div id="tab-fleet" class="tab-pane active">
    <div class="kpi-grid">
      <div class="kpi-card"><div class="kpi-label">已纳管设备 (Registered)</div><div class="kpi-val" id="kpi-total">0</div></div>
      <div class="kpi-card"><div class="kpi-label">实时在线 (Online)</div><div class="kpi-val" style="color:var(--success)" id="kpi-online">0</div></div>
      <div class="kpi-card"><div class="kpi-label">出厂预置/未认领</div><div class="kpi-val" style="color:#93c5fd" id="kpi-unclaimed">0</div></div>
      <div class="kpi-card"><div class="kpi-label">配置待同步 (Shadow Pending)</div><div class="kpi-val" style="color:var(--warn)" id="kpi-pending">0</div></div>
    </div>

    <div class="card">
      <div class="card-title">
        <span>实时接入设备列表 (自动响应 _esp-hub._tcp mDNS 广播)</span>
        <div style="display:flex;gap:8px">
          <button class="secondary" onclick="refreshFleet()">&#8635; 刷新设备</button>
          <button onclick="openProvisionManualModal()">+ 手动预注册设备</button>
        </div>
      </div>
      <div class="dev-grid" id="fleet-grid">
        <div style="color:var(--text-dim);font-size:13px">暂无注册设备。用下方编译工具烧录开启 Pre-Provision，或在设备端填写 Hub 地址后设备上电即自动出现在此处。</div>
      </div>
    </div>

    <div class="card">
      <div class="card-title">
        <span>平台事件与注册审计流 (Audit Log)</span>
      </div>
      <div style="max-height:190px;overflow-y:auto">
        <table>
          <thead><tr><th>时间</th><th>设备 ID</th><th>事件</th><th>详情</th></tr></thead>
          <tbody id="audit-tbody"></tbody>
        </table>
      </div>
    </div>
  </div>

  <!-- ==================== TAB 2: FIRMWARE REPO & BATCH OTA ==================== -->
  <div id="tab-ota" class="tab-pane">
    <div class="card">
      <div class="card-title">
        <span>固件版本仓库与跨芯片批量无线 OTA 推送</span>
        <button class="secondary" onclick="loadFirmwares()">&#8635; 刷新制品库</button>
      </div>
      <table>
        <thead><tr><th>固件文件</th><th>目标芯片</th><th>板型 / Flash</th><th>大小</th><th>SHA-256 摘要</th><th>操作</th></tr></thead>
        <tbody id="fw-tbody"></tbody>
      </table>
    </div>
  </div>

  <!-- ==================== TAB 3: CLOUD COMPILER & FLASHER ==================== -->
  <div id="tab-compiler" class="tab-pane">
    <div class="card">
      <div class="card-title">
        <span>1. 选择目标芯片架构 (Target SoC)</span>
        <span style="font-size:12px;color:var(--text-dim)">选择芯片后，下方功能组件将根据芯片物理能力自动互锁</span>
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
          <div class="chip-sub">极简单核 / Air101 监控屏 / 低功耗</div>
        </div>
        <div class="chip-opt" onclick="selectChip('esp32c2', this)">
          <div class="chip-name">ESP32-C2 (超轻量)</div>
          <div class="chip-sub">成本优化 / 基础 GPIO 与 API</div>
        </div>
        <div class="chip-opt" onclick="selectChip('esp8266', this)">
          <div class="chip-name">ESP8266 (经典 WiFi)</div>
          <div class="chip-sub">80/160MHz 单核 / 1MB Flash 开关节点</div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-title">
        <span>2. 定制功能组件矩阵</span>
        <div class="preset-bar">
          <button class="secondary" style="font-size:11px;padding:4px 8px" onclick="loadPreset('esp32_cam')">ESP32-CAM 预设</button>
          <button class="secondary" style="font-size:11px;padding:4px 8px" onclick="loadPreset('c3_relay')">C3 继电器预设</button>
          <button class="secondary" style="font-size:11px;padding:4px 8px;background:#1e3a8a;color:#93c5fd" onclick="loadPreset('c3_pc_monitor')">&#128187; C3 副屏监控预设</button>
          <button class="secondary" style="font-size:11px;padding:4px 8px" onclick="loadPreset('s3_dock')">S3 控制坞预设</button>
          <button class="secondary" style="font-size:11px;padding:4px 8px" onclick="loadPreset('esp8266_switch')">ESP8266 开关预设</button>
        </div>
      </div>
      <div class="feat-grid">
        <label class="feat-box" id="box_web_ui"><input type="checkbox" id="f_web_ui" checked/><div class="feat-info"><strong>Web 控制台 (Web UI) <span class="feat-badge supported" id="badge_web_ui">全芯片支持</span></strong><p>内置单板管理与配置网页（取消则变无头纯 API 版）</p></div></label>
        <label class="feat-box" id="box_rest_api"><input type="checkbox" id="f_rest_api" checked/><div class="feat-info"><strong>HTTP REST 配置接口 <span class="feat-badge supported" id="badge_rest_api">全芯片支持</span></strong><p>提供 /api/system/*、/api/wifi/*、/api/gpio/* 供程序调用</p></div></label>
        <label class="feat-box" id="box_ota"><input type="checkbox" id="f_ota" checked/><div class="feat-info"><strong>双分区无线 OTA <span class="feat-badge supported" id="badge_ota">全芯片支持</span></strong><p>双分区 A/B 升级与坏固件 90s 自动回滚安全守护</p></div></label>
        <label class="feat-box" id="box_fleet_agent"><input type="checkbox" id="f_fleet_agent" checked/><div class="feat-info"><strong>Fleet Hub 主动注册代理 <span class="feat-badge supported" id="badge_fleet_agent">全芯片支持</span></strong><p>连网自动发现并 HMAC 认证注册到上方设备大盘</p></div></label>
        <label class="feat-box" id="box_serial_cli"><input type="checkbox" id="f_serial_cli" checked/><div class="feat-info"><strong>交互式串口 CLI 终端 <span class="feat-badge supported" id="badge_serial_cli">全芯片支持</span></strong><p>UART0/USB-Serial 命令行应急改网、看状态与恢复出厂</p></div></label>
        <label class="feat-box" id="box_gpio_ctrl"><input type="checkbox" id="f_gpio_ctrl" checked/><div class="feat-info"><strong>GPIO 开关与 PWM 调光 <span class="feat-badge supported" id="badge_gpio_ctrl">全芯片支持</span></strong><p>按芯片与板型自动过滤安全引脚、支持 NVS 自定义名称</p></div></label>
        <label class="feat-box" id="box_camera"><input type="checkbox" id="f_camera" checked/><div class="feat-info"><strong>摄像头模块 (OV2640/5640) <span class="feat-badge supported" id="badge_camera">仅 ESP32/S3</span></strong><p>含 SCCB 驱动、双 PSRAM 帧缓冲与实时 MJPEG 推流</p></div></label>
        <label class="feat-box" id="box_spiram"><input type="checkbox" id="f_spiram" checked/><div class="feat-info"><strong>外部 PSRAM 内存 <span class="feat-badge supported" id="badge_spiram">仅 ESP32/S3</span></strong><p>启用外部 2MB~8MB RAM，摄像头与大内存任务必备</p></div></label>
        <label class="feat-box" id="box_pc_monitor"><input type="checkbox" id="f_pc_monitor"/><div class="feat-info"><strong>Air101 ST7735 竖屏监控 <span class="feat-badge supported" id="badge_pc_monitor">ESP32-C3 特色</span></strong><p>80x160 竖屏硬件看板与 /api/monitor/push REST 推流</p></div></label>
        <label class="feat-box" id="box_joystick"><input type="checkbox" id="f_joystick"/><div class="feat-info"><strong>5 向导航摇杆矩阵 <span class="feat-badge supported" id="badge_joystick">ESP32-C3 特色</span></strong><p>GPIO 4/5/8/9/13 摇杆驱动，支持事件 seq 计数与媒体控制</p></div></label>
        <label class="feat-box" id="box_usb_otg"><input type="checkbox" id="f_usb_otg"/><div class="feat-info"><strong>原生 USB-OTG 键鼠 <span class="feat-badge unsupported" id="badge_usb_otg">仅 ESP32-S2/S3</span></strong><p>TinyUSB 免驱 HID 键盘鼠标注入</p></div></label>
        <label class="feat-box" id="box_display"><input type="checkbox" id="f_display"/><div class="feat-info"><strong>通用图形屏引擎 <span class="feat-badge supported" id="badge_display">支持</span></strong><p>ST7789 IPS / SPI LCD 文本图形框架</p></div></label>
        <label class="feat-box" id="box_storage"><input type="checkbox" id="f_storage"/><div class="feat-info"><strong>Flash 文件系统 (SPIFFS) <span class="feat-badge supported" id="badge_storage">支持</span></strong><p>挂载 storage 分区，支持 Web 上传/下载配置文件</p></div></label>
      </div>
    </div>

    <div class="card">
      <div class="card-title"><span>3. 动态 REST API 清单预览</span></div>
      <div id="endpoint-grid" style="display:grid;grid-template-columns:repeat(auto-fit,minmax(330px,1fr));gap:8px"></div>
    </div>

    <div class="card">
      <div class="card-title"><span>4. 硬件规格与默认出厂参数</span></div>
      <div class="input-grid">
        <label>默认 WiFi SSID:<input type="text" id="wifi_ssid" value="1"/></label>
        <label>默认 WiFi 密码:<input type="text" id="wifi_pass" value="11111111"/></label>
        <label>设备名称 (留空则按 MAC 生成):<input type="text" id="device_id" value="esp32_node"/></label>
        <label>mDNS 本地域名:<input type="text" id="mdns_host" value="esp-node"/></label>
        <label>Flash 容量规格:<select id="flash_size"><option value="4MB" selected>4MB (标准双OTA+SPIFFS)</option><option value="8MB">8MB (3MB双OTA+2MB存储)</option><option value="16MB">16MB (旗舰 4MB双OTA)</option><option value="2MB">2MB (紧凑双OTA无SPIFFS)</option><option value="1MB">1MB (ESP8266 极简版)</option></select></label>
        <label>硬件板级引脚配置:<select id="board_profile"><option value="auto" selected>自动按芯片安全匹配引脚</option><option value="esp32_cam_aithinker">AI-Thinker ESP32-CAM</option><option value="esp32c3_air101">LuatOS ESP32-C3 + Air101</option><option value="esp32c3_supermini">ESP32-C3 SuperMini</option><option value="esp32s3_cam">ESP32-S3 CAM</option><option value="esp32s3_devkit">ESP32-S3 DevKitC 16MB</option><option value="esp32_generic">ESP32 经典 DevKit</option></select></label>
      </div>
    </div>

    <div class="card">
      <div class="card-title">
        <span>5. 执行构建与产线烧录 (支持自动生成 NVS 密钥预注册)</span>
        <button id="btn-build" onclick="triggerBuild()">&#9889; 立即生成定制固件</button>
      </div>
      <pre class="console" id="log-box">[System Ready] 等待配置构建请求...</pre>

      <div id="result-box" style="display:none;background:#0d1527;padding:16px;border-radius:8px;border:1px solid #10b981;flex-direction:column;gap:14px">
        <div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px;border-bottom:1px solid #1e293b;padding-bottom:10px">
          <div>
            <strong style="color:#10b981;font-size:15px">&#127881; 当前待部署固件: </strong>
            <span id="res-filename" style="color:#fff;font-family:monospace;font-size:14px">firmware.bin</span>
            <span id="res-filesize" style="font-size:12px;color:var(--text-dim)"></span>
          </div>
          <button class="secondary" onclick="downloadFirmware()">&#11015; 下载 .bin 产物</button>
        </div>
        <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(340px,1fr));gap:14px">
          <div style="background:#131e33;border:1px solid #334155;border-radius:8px;padding:14px;display:flex;flex-direction:column;gap:10px">
            <div style="display:flex;justify-content:space-between;align-items:center">
              <strong style="color:#38bdf8;font-size:13px">&#128268; 方式 A：USB 串口线烧录与出厂建档</strong>
              <button class="secondary" style="font-size:11px;padding:2px 8px" onclick="refreshComPorts()">&#8635; 刷新串口</button>
            </div>
            <div style="display:grid;grid-template-columns:1fr 110px;gap:8px">
              <label>目标串口:<select id="flash-port-select"></select></label>
              <label>波特率:<select id="flash-baud-select"><option value="921600" selected>921600</option><option value="460800">460800</option><option value="115200">115200</option></select></label>
            </div>
            <label style="flex-direction:row;align-items:center;gap:6px;cursor:pointer;color:#cbd5e1"><input type="checkbox" id="flash-full-image" checked style="width:auto"/> 全量完整刷写 (Bootloader + 分区表 + App)</label>
            <label style="flex-direction:row;align-items:center;gap:6px;cursor:pointer;color:#6ee7b7"><input type="checkbox" id="flash-auto-prov" checked style="width:auto"/> 烧录时自动写唯一 Secret 入 NVS 并预注册到 Hub！</label>
            <button id="btn-flash-serial" style="background:#0284c7" onclick="triggerSerialFlash()">&#9889; 一键串口烧录与预置</button>
          </div>
          <div style="background:#131e33;border:1px solid #334155;border-radius:8px;padding:14px;display:flex;flex-direction:column;gap:10px">
            <strong style="color:#34d399;font-size:13px">&#128225; 方式 B：局域网无线 OTA 直推</strong>
            <label>设备局域网 IP 地址:<input type="text" id="ota-target-ip" placeholder="例如 192.168.68.210"/></label>
            <button id="btn-push-ota" style="background:#059669;margin-top:auto" onclick="triggerOtaPush()">&#128640; 推送无线升级</button>
          </div>
        </div>
      </div>
    </div>
  </div>
</div>

<!-- ==================== MODALS ==================== -->
<div class="modal-bg" id="cfg-modal">
  <div class="modal">
    <div class="card-title"><span id="cfg-modal-title">设备配置与影子同步</span><button class="secondary" onclick="closeModal('cfg-modal')">关闭</button></div>
    <div class="input-grid">
      <label>设备友好别名 (Name):<input type="text" id="m-dev-name"/></label>
      <label>绑定本地 API Token (若设备已加密):<input type="password" id="m-dev-tok"/></label>
      <label>WiFi SSID (下发修改):<input type="text" id="m-cfg-ssid"/></label>
      <label>WiFi 密码 (留空不改):<input type="password" id="m-cfg-pass"/></label>
      <label>mDNS 域名:<input type="text" id="m-cfg-mdns"/></label>
      <label>新 Hub URL:<input type="text" id="m-cfg-hub"/></label>
    </div>
    <div style="display:flex;justify-content:flex-end;gap:8px">
      <button class="secondary" onclick="saveDeviceClaim()">仅认领与改名</button>
      <button onclick="pushShadowConfig()">保存影子配置并下发</button>
    </div>
  </div>
</div>

<div class="modal-bg" id="ctrl-modal">
  <div class="modal">
    <div class="card-title"><span id="ctrl-modal-title">远程设备实时控制</span><button class="secondary" onclick="closeModal('ctrl-modal')">关闭</button></div>
    <div id="ctrl-body" style="display:flex;flex-direction:column;gap:12px"></div>
  </div>
</div>

<script>
let selectedChip = "esp32";
let pollTimer = null;
let currentArtifact = "";
let activeModalDevId = "";
let fleetCache = [];

function switchTab(tab, el) {
  document.querySelectorAll(".tab-pane").forEach(p => p.classList.remove("active"));
  document.querySelectorAll(".nav-tab").forEach(b => b.classList.remove("active"));
  document.getElementById("tab-" + tab).classList.add("active");
  if (el) el.classList.add("active");
  if (tab === "fleet") refreshFleet();
  if (tab === "ota") loadFirmwares();
  if (tab === "compiler") refreshComPorts();
}

function closeModal(id) { document.getElementById(id).classList.remove("open"); }

/* -------- TAB 1: FLEET HUB -------- */
async function refreshFleet() {
  try {
    const res = await fetch("/api/fleet/devices");
    const data = await res.json();
    if (!data.ok) return;
    fleetCache = data.devices || [];

    const total = fleetCache.length;
    const online = fleetCache.filter(d => d.status === "online").length;
    const unclaimed = fleetCache.filter(d => !d.claimed).length;
    const pending = fleetCache.filter(d => d.shadow && d.shadow.sync_status === "pending").length;

    document.getElementById("kpi-total").textContent = total;
    document.getElementById("kpi-online").textContent = online;
    document.getElementById("kpi-unclaimed").textContent = unclaimed;
    document.getElementById("kpi-pending").textContent = pending;

    const grid = document.getElementById("fleet-grid");
    if (total === 0) {
      grid.innerHTML = "<div style='color:var(--text-dim);font-size:13px'>暂无纳管设备。通过串口烧录 (启用自动预注册) 或在设备端配置 Hub URL 连网后即可自动发现上线！</div>";
    } else {
      grid.innerHTML = "";
      fleetCache.forEach(d => {
        const caps = d.capabilities || {};
        const capTags = [];
        if (caps.camera?.enabled) capTags.push("&#128247; Camera");
        if (caps.gpio?.enabled) capTags.push("&#128268; GPIO");
        if (caps.pc_monitor?.enabled) capTags.push("&#128187; C3 Monitor");
        if (caps.joystick?.enabled) capTags.push("&#127918; Joystick");
        if (caps.usb_otg?.enabled) capTags.push("&#9000; USB-OTG");
        if (caps.storage?.enabled) capTags.push("&#128193; Storage");
        if (caps.ota?.enabled) capTags.push("&#9889; OTA");
        if (d.has_secret) capTags.push("&#128274; HMAC");

        const stCls = d.status === "online" ? "online" : (d.status === "provisioned" ? "provisioned" : "offline");
        const syncBadge = (d.shadow?.sync_status === "pending") ? "<span class='pill warn'>待同步配置</span>" : "";
        const claimBadge = d.claimed ? "" : "<span class='pill warn'>待认领</span>";
        const card = document.createElement("div");
        card.className = "dev-card " + (d.status === "online" ? "online" : "");
        card.innerHTML = `
          <div style="display:flex;justify-content:space-between;align-items:flex-start;gap:8px">
            <div>
              <strong style="font-size:15px">${d.name || d.device_id}</strong>
              <div style="font-size:11px;color:var(--text-dim);font-family:monospace">${d.device_id} · ${d.mac || 'No MAC'}</div>
            </div>
            <div style="display:flex;gap:4px;flex-wrap:wrap">
              ${claimBadge}${syncBadge}
              <span class="pill ${stCls}">${d.status.toUpperCase()}</span>
            </div>
          </div>
          <div style="font-size:12px;color:#cbd5e1;display:grid;grid-template-columns:1fr 1fr;gap:4px">
            <div>芯片: <b>${d.chip || '-'}</b></div>
            <div>IP: <a href="http://${d.ip}:${d.web_port||80}/" target="_blank" style="color:#60a5fa">${d.ip || '-'}</a></div>
            <div>固件: v${d.fw_version || '-'}</div>
            <div>信号: ${d.rssi ? d.rssi + ' dBm' : '-'} | Heap: ${d.free_heap ? Math.round(d.free_heap/1024) + 'KB' : '-'}</div>
          </div>
          <div class="cap-tags">${capTags.map(t => `<span class="cap-tag">${t}</span>`).join("")}</div>
          <div style="display:flex;gap:6px;flex-wrap:wrap;margin-top:auto;border-top:1px solid #33415555;padding-top:8px">
            <button style="flex:1" onclick="openDeviceCtrl('${d.device_id}')">&#127899; 控制</button>
            <button class="secondary" style="flex:1" onclick="openDeviceCfg('${d.device_id}')">&#9881; 配置/认领</button>
            <button class="danger" onclick="deleteFleetDev('${d.device_id}')">&#128465;</button>
          </div>
        `;
        grid.appendChild(card);
      });
    }

    const tbody = document.getElementById("audit-tbody");
    tbody.innerHTML = (data.audit || []).map(a => `
      <tr>
        <td style="color:var(--text-dim)">${new Date(a.created_at * 1000).toLocaleTimeString()}</td>
        <td style="font-family:monospace">${a.device_id || '-'}</td>
        <td><span class="cap-tag">${a.event}</span></td>
        <td style="color:var(--text-dim)">${a.detail}</td>
      </tr>
    `).join("");
  } catch(e) {}
}

function openDeviceCfg(devId) {
  const d = fleetCache.find(x => x.device_id === devId);
  if (!d) return;
  activeModalDevId = devId;
  let desired = {};
  try { desired = JSON.parse(d.shadow?.desired_json || "{}"); } catch {}
  document.getElementById("cfg-modal-title").textContent = "配置 & 认领: " + devId;
  document.getElementById("m-dev-name").value = d.name || devId;
  document.getElementById("m-dev-tok").value = "";
  document.getElementById("m-cfg-ssid").value = desired.wifi_ssid || "";
  document.getElementById("m-cfg-pass").value = "";
  document.getElementById("m-cfg-mdns").value = desired.mdns_host || d.mdns_host || "";
  document.getElementById("m-cfg-hub").value = desired.hub_url || "";
  document.getElementById("cfg-modal").classList.add("open");
}

async function saveDeviceClaim() {
  const body = { device_id: activeModalDevId, name: document.getElementById("m-dev-name").value.trim(), claimed: true };
  const tok = document.getElementById("m-dev-tok").value;
  if (tok !== "") body.api_token = tok;
  await fetch("/api/fleet/device/claim", { method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify(body) });
  closeModal("cfg-modal");
  refreshFleet();
}

async function pushShadowConfig() {
  await saveDeviceClaim();
  const patch = {};
  const ssid = document.getElementById("m-cfg-ssid").value.trim();
  const pass = document.getElementById("m-cfg-pass").value;
  const mdns = document.getElementById("m-cfg-mdns").value.trim();
  const hub  = document.getElementById("m-cfg-hub").value.trim();
  if (ssid) patch.wifi_ssid = ssid;
  if (pass) patch.wifi_pass = pass;
  if (mdns) patch.mdns_host = mdns;
  if (hub !== "") patch.hub_url = hub;

  const r = await (await fetch("/api/fleet/device/config", {
    method:"POST", headers:{"Content-Type":"application/json"},
    body: JSON.stringify({ device_id: activeModalDevId, config: patch })
  })).json();
  alert(r.pushed_live ? "✅ 已保存影子配置并直接推送设备即时生效！" : "✅ 已保存影子配置，设备下次心跳自动同步生效！");
  refreshFleet();
}

async function deleteFleetDev(devId) {
  if (!confirm("确定移除设备 " + devId + "？")) return;
  await fetch("/api/fleet/device/delete", { method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify({device_id: devId}) });
  refreshFleet();
}

async function openDeviceCtrl(devId) {
  const d = fleetCache.find(x => x.device_id === devId);
  if (!d) return;
  document.getElementById("ctrl-modal-title").textContent = `控制中心: ${d.name} (${d.ip || '离线'})`;
  const body = document.getElementById("ctrl-body");
  body.innerHTML = "<div style='color:var(--text-dim)'>正在探测远端节点能力接口...</div>";
  document.getElementById("ctrl-modal").classList.add("open");

  const caps = d.capabilities || {};
  let html = "";
  if (caps.camera?.enabled && d.ip) {
    html += `<div class="card" style="padding:12px"><strong>&#128247; 实时抓拍</strong><img src="/api/fleet/proxy/${devId}?path=/capture&t=${Date.now()}" style="width:100%;max-height:260px;object-fit:contain;background:#000;border-radius:6px"/><div><a href="http://${d.ip}:${d.web_port||80}/" target="_blank" style="color:#60a5fa;font-size:12px">打开设备端完整视频控制台 &rarr;</a></div></div>`;
  }
  if (caps.gpio?.enabled) {
    try {
      const g = await (await fetch(`/api/fleet/proxy/${devId}?path=/api/gpio/pins`)).json();
      html += `<div class="card" style="padding:12px"><strong>&#128268; GPIO 控制矩阵</strong>` +
        (g.pins || []).map(p => `<div style="display:flex;justify-content:space-between;align-items:center;padding:4px 0;border-bottom:1px solid #33415544"><span>${p.label} (GPIO ${p.pin})</span><div style="display:flex;gap:6px"><button onclick="sendFleetCmd('${devId}','gpio_set',{pin:${p.pin},val:1})">ON</button><button class="secondary" onclick="sendFleetCmd('${devId}','gpio_set',{pin:${p.pin},val:0})">OFF</button></div></div>`).join("") +
        `</div>`;
    } catch { html += "<div>GPIO 查询失败或设备离线</div>"; }
  }
  if (caps.pc_monitor?.enabled) {
    html += `<div class="card" style="padding:12px"><strong>&#128187; C3 副屏测试推送</strong><button onclick="testPushC3('${devId}')">推送演示 CPU/网速 指标上屏</button></div>`;
  }
  html += `<div style="display:flex;gap:8px"><button class="secondary" onclick="fetchRemoteLogs('${devId}')">查看远端串口环形日志</button><button class="danger" onclick="sendFleetCmd('${devId}','reboot',{})">远程重启设备</button></div><pre class="console" id="remote-log-box" style="height:140px">点击查看日志...</pre>`;
  body.innerHTML = html;
}

async function sendFleetCmd(devId, type, payload) {
  const r = await (await fetch("/api/fleet/device/command", {
    method:"POST", headers:{"Content-Type":"application/json"},
    body: JSON.stringify({ device_id: devId, cmd_type: type, payload: payload })
  })).json();
  if (type === "gpio_set" && r.delivered_direct) openDeviceCtrl(devId);
  else alert(r.delivered_direct ? "✅ 指令已即时下发" : "⏳ 设备离线，已入队待心跳执行");
}

async function fetchRemoteLogs(devId) {
  const txt = await (await fetch(`/api/fleet/proxy/${devId}?path=/api/system/logs`)).text();
  document.getElementById("remote-log-box").textContent = txt;
}

async function testPushC3(devId) {
  const d = fleetCache.find(x => x.device_id === devId);
  if (!d || !d.ip) return alert("设备离线");
  await fetch(`http://${d.ip}:${d.web_port||80}/api/monitor/push`, {
    method:"POST", headers:{"Content-Type":"application/json"},
    body: JSON.stringify({cpu:55, ram:62, disk:71, down:"8.4MB/s", up:"1.2MB/s", uptime:"99h 01m", time:"HUB OK"})
  });
  alert("已刷新屏幕！");
}

function openProvisionManualModal() {
  const did = prompt("输入待预置 Device ID (例如 esp32_node_02):");
  if (!did) return;
  const chip = prompt("芯片型号 (esp32 / esp32c3 / esp32s3 / esp8266):", "esp32c3") || "esp32c3";
  fetch("/api/fleet/provision", {
    method:"POST", headers:{"Content-Type":"application/json"},
    body: JSON.stringify({ device_id: did.trim(), chip: chip.trim() })
  }).then(() => refreshFleet());
}

/* -------- TAB 2: FIRMWARES & BATCH OTA -------- */
async function loadFirmwares() {
  const data = await (await fetch("/api/fleet/firmwares")).json();
  const tbody = document.getElementById("fw-tbody");
  tbody.innerHTML = (data.firmwares || []).map(f => `
    <tr>
      <td style="font-family:monospace;font-weight:bold">${f.file}</td>
      <td><span class="pill online">${f.target.toUpperCase()}</span></td>
      <td>${f.board} / ${f.flash_size}</td>
      <td>${(f.size/1024).toFixed(1)} KB</td>
      <td style="font-family:monospace;font-size:11px">${(f.sha256||'').slice(0,14)}...</td>
      <td style="display:flex;gap:6px">
        <button onclick="batchOtaPrompt('${f.file}','${f.target}')">&#128640; 批量 OTA</button>
        <button class="secondary" onclick="window.location.href='/api/download?file=${encodeURIComponent(f.file)}'">下载</button>
      </td>
    </tr>
  `).join("");
}

async function batchOtaPrompt(fwFile, fwTarget) {
  await refreshFleet();
  const candidates = fleetCache.filter(d => d.status === "online");
  if (!candidates.length) return alert("当前无在线设备！");
  const listStr = candidates.map(d => `${d.device_id} (${d.chip})`).join("\\n");
  const picked = prompt(`向兼容 ${fwTarget.toUpperCase()} 的在线设备推送 ${fwFile}。请输入 Device ID（逗号分隔）：\\n\\n当前在线:\\n${listStr}`,
    candidates.filter(d => (d.chip||'').toLowerCase().replace('-','').includes(fwTarget.replace('-',''))).map(d => d.device_id).join(",")
  );
  if (!picked) return;
  const ids = picked.split(",").map(s => s.trim()).filter(Boolean);
  const res = await (await fetch("/api/fleet/ota/batch", {
    method:"POST", headers:{"Content-Type":"application/json"},
    body: JSON.stringify({ file: fwFile, device_ids: ids })
  })).json();
  alert("批量 OTA 下发结果:\\n" + JSON.stringify(res.results, null, 2));
}

/* -------- TAB 3: COMPILER & PROVISIONING -------- */
const SOC_SPECS = {
  esp32:   { unsupported: { usb_otg: "经典 ESP32 无原生 USB-OTG 控制器" } },
  esp32s3: { unsupported: {} },
  esp32c3: { unsupported: { camera: "无并行 DVP 接口", spiram: "无 PSRAM 总线", usb_otg: "仅支持 USB-Serial-JTAG" } },
  esp32c2: { unsupported: { camera: "无摄像头接口", spiram: "无 PSRAM", storage: "Flash 较小", display: "RAM 受限", usb_otg: "无 USB-OTG", pc_monitor: "引脚不足", joystick: "引脚不足" } },
  esp8266: { unsupported: { camera: "无 DVP 接口", spiram: "无 PSRAM", usb_otg: "无 USB-OTG", display: "RAM (~80KB) 受限", pc_monitor: "引脚不足", joystick: "引脚不足" } }
};

function selectChip(chip, el) {
  selectedChip = chip;
  document.querySelectorAll(".chip-opt").forEach(c => c.classList.remove("active"));
  if (el) el.classList.add("active");
  const devInput = document.getElementById("device_id");
  const hostInput = document.getElementById("mdns_host");
  if (chip === "esp8266") {
    if (!devInput.value || devInput.value.startsWith("esp32")) devInput.value = "esp8266_node";
    if (!hostInput.value || hostInput.value.startsWith("esp-")) hostInput.value = "esp8266-node";
  } else if (devInput.value.startsWith("esp8266")) {
    devInput.value = chip + "_node";
  }

  const spec = SOC_SPECS[chip] || SOC_SPECS.esp32;
  const allFeats = ["web_ui","rest_api","ota","fleet_agent","serial_cli","camera","gpio_ctrl","spiram","storage","display","usb_otg","pc_monitor","joystick"];
  allFeats.forEach(f => {
    const box = document.getElementById("box_" + f);
    const input = document.getElementById("f_" + f);
    const badge = document.getElementById("badge_" + f);
    if (!box || !input) return;
    if (spec.unsupported && spec.unsupported[f]) {
      input.checked = false; input.disabled = true; box.classList.add("disabled");
      badge.textContent = "不支持: " + spec.unsupported[f]; badge.className = "feat-badge unsupported";
    } else {
      input.disabled = false; box.classList.remove("disabled");
      badge.textContent = "支持"; badge.className = "feat-badge supported";
    }
  });
  updateEndpointsPreview();
}

const API_ENDPOINTS = [
  { method: "GET",  path: "/",                        feat: "web_ui",    desc: "Web 控制台" },
  { method: "GET",  path: "/api/system/capabilities", feat: "rest_api",  desc: "芯片能力自描述" },
  { method: "GET",  path: "/api/system/config",       feat: "rest_api",  desc: "读 NVS 配置" },
  { method: "POST", path: "/api/system/config",       feat: "rest_api",  desc: "改配置热生效(15s回滚)" },
  { method: "GET",  path: "/api/wifi/scan",           feat: "rest_api",  desc: "周围 WiFi 扫描" },
  { method: "POST", path: "/api/ota/upload",          feat: "ota",       desc: "无线 OTA 上传" },
  { method: "GET",  path: "/api/ota/status",          feat: "ota",       desc: "当前分区槽位" },
  { method: "GET",  path: "/api/gpio/pins",           feat: "gpio_ctrl", desc: "枚举安全 GPIO" },
  { method: "POST", path: "/api/gpio/set",            feat: "gpio_ctrl", desc: "数字输出与 PWM" },
  { method: "GET",  path: "/stream",                  feat: "camera",    desc: "MJPEG 实时流" },
  { method: "POST", path: "/api/camera/config",       feat: "camera",    desc: "分辨率与翻转配置" },
  { method: "POST", path: "/api/monitor/push",        feat: "pc_monitor",desc: "C3 副屏推流" },
  { method: "GET",  path: "/api/joystick/state",      feat: "joystick",  desc: "摇杆事件 seq" }
];

function updateEndpointsPreview() {
  const c = document.getElementById("endpoint-grid");
  if (!c) return;
  c.innerHTML = API_ENDPOINTS.map(ep => {
    const el = document.getElementById("f_" + ep.feat);
    const on = el && el.checked && !el.disabled;
    return `<div class="endpoint-item ${on?'':'disabled'}">
      <div style="display:flex;align-items:center;gap:6px"><span class="method-badge method-${ep.method.toLowerCase()}">${ep.method}</span><span class="ep-path">${ep.path}</span></div>
      <span class="ep-status ${on?'active':'inactive'}">${on?'200 OK':'404'}</span>
    </div>`;
  }).join("");
}

function loadPreset(type) {
  if (type === "esp32_cam") {
    selectChip("esp32", document.querySelectorAll(".chip-opt")[0]);
    ["f_web_ui","f_rest_api","f_ota","f_fleet_agent","f_camera","f_gpio_ctrl","f_spiram"].forEach(id => document.getElementById(id).checked = true);
    document.getElementById("device_id").value = "esp32_cam_full";
    document.getElementById("mdns_host").value = "esp32-cam";
    document.getElementById("flash_size").value = "4MB";
    document.getElementById("board_profile").value = "esp32_cam_aithinker";
    currentArtifact = "esp32_cam_full.bin";
    showPresetReady("esp32_cam_full.bin", "ESP32-CAM 摄像头版");
  } else if (type === "c3_pc_monitor") {
    selectChip("esp32c3", document.querySelectorAll(".chip-opt")[2]);
    ["f_web_ui","f_rest_api","f_ota","f_fleet_agent","f_pc_monitor","f_joystick","f_gpio_ctrl"].forEach(id => document.getElementById(id).checked = true);
    document.getElementById("device_id").value = "esp32c3_pc_monitor";
    document.getElementById("mdns_host").value = "c3-monitor";
    document.getElementById("flash_size").value = "4MB";
    document.getElementById("board_profile").value = "esp32c3_air101";
    currentArtifact = "esp32c3_pc_monitor.bin";
    showPresetReady("esp32c3_pc_monitor.bin", "C3 ST7735 副屏+摇杆版");
  } else if (type === "c3_relay") {
    selectChip("esp32c3", document.querySelectorAll(".chip-opt")[2]);
    ["f_web_ui","f_rest_api","f_ota","f_fleet_agent","f_gpio_ctrl"].forEach(id => document.getElementById(id).checked = true);
    ["f_pc_monitor","f_joystick"].forEach(id => document.getElementById(id).checked = false);
    document.getElementById("device_id").value = "esp32c3_relay";
    document.getElementById("mdns_host").value = "c3-relay";
    document.getElementById("flash_size").value = "4MB";
    document.getElementById("board_profile").value = "esp32c3_supermini";
  } else if (type === "s3_dock") {
    selectChip("esp32s3", document.querySelectorAll(".chip-opt")[1]);
    ["f_web_ui","f_rest_api","f_ota","f_fleet_agent","f_gpio_ctrl","f_display","f_usb_otg","f_spiram"].forEach(id => document.getElementById(id).checked = true);
    document.getElementById("device_id").value = "esp32s3_dock";
    document.getElementById("mdns_host").value = "s3-dock";
    document.getElementById("flash_size").value = "16MB";
    document.getElementById("board_profile").value = "esp32s3_devkit";
  } else if (type === "esp8266_switch") {
    selectChip("esp8266", document.querySelectorAll(".chip-opt")[4]);
    ["f_web_ui","f_rest_api","f_ota","f_fleet_agent","f_gpio_ctrl"].forEach(id => document.getElementById(id).checked = true);
    document.getElementById("device_id").value = "esp8266_switch";
    document.getElementById("mdns_host").value = "esp8266-node";
    document.getElementById("flash_size").value = "1MB";
    document.getElementById("board_profile").value = "auto";
    currentArtifact = "esp8266_switch.bin";
    showPresetReady("esp8266_switch.bin", "ESP8266 1MB Flash 轻量版");
  }
  updateEndpointsPreview();
}

function showPresetReady(fn, desc) {
  document.getElementById("res-filename").textContent = fn;
  document.getElementById("res-filesize").textContent = "(" + desc + ")";
  document.getElementById("result-box").style.display = "flex";
  refreshComPorts();
}

async function triggerBuild() {
  const btn = document.getElementById("btn-build");
  btn.disabled = true;
  document.getElementById("compile-status").textContent = "正在构建...";
  document.getElementById("compile-status").className = "status-pill building";
  const payload = {
    name: document.getElementById("device_id").value.trim() || (selectedChip + "_custom"),
    target: selectedChip,
    features: {
      http_server: true,
      web_ui: document.getElementById("f_web_ui").checked,
      rest_api: document.getElementById("f_rest_api").checked,
      ota: document.getElementById("f_ota").checked,
      fleet_agent: document.getElementById("f_fleet_agent").checked,
      serial_cli: document.getElementById("f_serial_cli").checked,
      camera: document.getElementById("f_camera").checked,
      gpio_ctrl: document.getElementById("f_gpio_ctrl").checked,
      storage: document.getElementById("f_storage").checked,
      display: document.getElementById("f_display").checked,
      usb_otg: document.getElementById("f_usb_otg").checked,
      pc_monitor: document.getElementById("f_pc_monitor").checked,
      joystick: document.getElementById("f_joystick").checked
    },
    hardware: {
      cpu_freq_mhz: (selectedChip === "esp32s3") ? 240 : (selectedChip === "esp32c2" ? 120 : 160),
      spiram: document.getElementById("f_spiram").checked,
      flash_size: document.getElementById("flash_size").value,
      board: document.getElementById("board_profile").value
    },
    defaults: {
      wifi_ssid: document.getElementById("wifi_ssid").value,
      wifi_pass: document.getElementById("wifi_pass").value,
      device_id: document.getElementById("device_id").value,
      mdns_host: document.getElementById("mdns_host").value
    }
  };
  const res = await fetch("/api/build", { method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify(payload) });
  const data = await res.json();
  if (!data.ok) { alert("校验拒绝: " + data.error); btn.disabled = false; return; }
  pollStatus();
}

function pollStatus() {
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(async () => {
    try {
      const st = await (await fetch("/api/status")).json();
      const box = document.getElementById("log-box");
      box.textContent = st.log || "";
      box.scrollTop = box.scrollHeight;
      if (st.status === "success") {
        clearInterval(pollTimer);
        document.getElementById("btn-build").disabled = false;
        document.getElementById("btn-flash-serial").disabled = false;
        document.getElementById("compile-status").textContent = "完成";
        document.getElementById("compile-status").className = "status-pill success";
        currentArtifact = st.artifact_name;
        document.getElementById("res-filename").textContent = st.artifact_name;
        document.getElementById("res-filesize").textContent = "(" + (st.artifact_size/1024).toFixed(1) + " KB)";
        document.getElementById("result-box").style.display = "flex";
        refreshComPorts();
      } else if (st.status === "failed") {
        clearInterval(pollTimer);
        document.getElementById("btn-build").disabled = false;
        document.getElementById("btn-flash-serial").disabled = false;
        document.getElementById("compile-status").textContent = "失败";
        document.getElementById("compile-status").className = "status-pill failed";
      }
    } catch {}
  }, 1000);
}

function downloadFirmware() {
  if (currentArtifact) window.location.href = "/api/download?file=" + encodeURIComponent(currentArtifact);
}

async function refreshComPorts() {
  const data = await (await fetch("/api/ports")).json();
  const sel = document.getElementById("flash-port-select");
  sel.innerHTML = "";
  (data.ports || []).forEach(p => {
    const o = document.createElement("option");
    o.value = p.port; o.textContent = `${p.port} (${p.desc})`;
    sel.appendChild(o);
  });
}

async function triggerSerialFlash() {
  const port = document.getElementById("flash-port-select").value;
  if (!port) return alert("请选择目标串口");
  document.getElementById("btn-flash-serial").disabled = true;
  document.getElementById("compile-status").textContent = "正在烧录...";
  document.getElementById("compile-status").className = "status-pill building";
  const res = await (await fetch("/api/flash_serial", {
    method:"POST", headers:{"Content-Type":"application/json"},
    body: JSON.stringify({
      port: port,
      baud: document.getElementById("flash-baud-select").value,
      file: currentArtifact,
      target: selectedChip,
      full: document.getElementById("flash-full-image").checked,
      auto_provision: document.getElementById("flash-auto-prov").checked,
      wifi_ssid: document.getElementById("wifi_ssid").value,
      wifi_pass: document.getElementById("wifi_pass").value,
      mdns_host: document.getElementById("mdns_host").value
    })
  })).json();
  if (!res.ok) { alert(res.error); document.getElementById("btn-flash-serial").disabled = false; return; }
  pollStatus();
}

async function triggerOtaPush() {
  const ip = document.getElementById("ota-target-ip").value.trim();
  if (!ip) return alert("请输入目标设备 IP");
  const r = await (await fetch(`/api/push_ota?ip=${encodeURIComponent(ip)}&file=${encodeURIComponent(currentArtifact)}`)).json();
  alert(r.ok ? "✅ 无线 OTA 推送成功！" : "❌ OTA 失败: " + r.error);
}

window.onload = function() {
  selectChip("esp32", document.querySelectorAll(".chip-opt")[0]);
  document.querySelectorAll(".feat-box input[type=checkbox]").forEach(cb => cb.addEventListener("change", updateEndpointsPreview));
  refreshFleet();
  refreshComPorts();
  setInterval(() => {
    if (document.getElementById("tab-fleet").classList.contains("active")) refreshFleet();
  }, 5000);
};
</script>
</body>
</html>
"""
