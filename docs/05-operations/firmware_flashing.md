# 固件编译、烧录与 OTA 在线升级实操指南 (Firmware Build, Flashing & OTA Guide)

> **文档版本**：V2.1 工业级标准  
> **编写日期**：2026-10-26  
> **适用硬件**：ESP32-S3 (8MB PSRAM + 16MB Flash)  
> **固件目录**：`ble_hid/`  

---

## 1. 固件编译环境准备

固件开发支持两种编译方式：
- **方式 A（免安装环境，推荐）**：使用 Espressif 官方 Docker 镜像容器内编译，宿主机仅需 Docker，无需配置庞大的 ESP-IDF 工具链。
- **方式 B（本地极速编译）**：在宿主机本地安装 ESP-IDF v5.4.1 环境。

### 1.1 方式 A：Docker 容器一键编译
在项目根目录下执行：
```bash
# Linux / macOS
docker run --rm -v "${PWD}/ble_hid:/project" -w /project espressif/idf:release-v5.4 idf.py build

# Windows PowerShell
docker run --rm -v "${PWD}/ble_hid:/project" -w /project espressif/idf:release-v5.4 idf.py build
```
编译产物将直接输出在本地 `ble_hid/build/` 目录下（包括 `ble_hid.bin`, `bootloader.bin`, `partition-table.bin`）。

### 1.2 方式 B：本地 ESP-IDF v5.4.1 编译
若本地已安装 ESP-IDF：
```powershell
# 1. 导出 ESP-IDF 环境变量 (Windows 示例)
$env:IDF_TOOLS_PATH="C:\Users\zz101\esp"
$env:IDF_PYTHON_ENV_PATH="C:\Users\zz101\esp\python_env\idf5.4_py3.11_env"
. C:\Users\zz101\esp\v5.4.1\esp-idf\export.ps1

# 2. 避免 Git Bash / MSYS 环境变量污染 (关键避坑)
Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue

# 3. 编译
cd ble_hid
idf.py build
```

---

## 2. 物理串口线刷与串口监视 (`flash.py`)

在设备初次出厂烧录或固件严重损坏变砖时，必须通过 USB 线缆直连 CH343 串口（如 `COM5` 或 `/dev/ttyUSB0`）进行线刷。

### 2.1 依赖安装
线刷脚本基于 Python 运行：
```bash
pip install esptool pyserial esp-idf-monitor
```

### 2.2 烧录助手常用指令速查
```bash
# 1. 列出当前可用串口设备
python ble_hid/flash.py --list-ports

# 2. 自动构建并烧录 (指定 COM5，烧录波特率 921600，烧录完成后自动进入 115200 监控)
python ble_hid/flash.py --port COM5

# 3. 仅烧录已有 bin 文件并跳过串口监视
python ble_hid/flash.py --port COM5 --no-monitor

# 4. 全盘擦除 Flash 并重新刷入初始固件
python ble_hid/flash.py --port COM5 --erase

# 5. Linux 下指定串口设备
python ble_hid/flash.py --port /dev/ttyUSB0 --no-monitor
```

> [!CAUTION]
> #### 串口打开陷阱 (DTR/RTS 电平保护)
> ESP32 自动下载电路通过 DTR 和 RTS 引脚控制芯片的 EN 和 GPIO0。使用自定义 Python 脚本（如 `pyserial`）连接设备串口读取日志时，**必须在 `serial.Serial()` 打开前或立即设置 `dtr=False, rts=False`**，否则打开串口瞬间的电平拉扯会将芯片误带入 BootROM 下载模式，导致设备直接停机挂死。

---

## 3. 在线 OTA 双槽升级实操

在硬件正常连入局域网并在服务端显示 `online` 时，**优先推荐走 OTA 在线推送升级**，完全免去插拔线缆操作。

```mermaid
flowchart LR
    A["开发者本地编译 ble_hid.bin"] --> B["上传至 Web 控制台 /firmwares"]
    B --> C["服务端计算 SHA-256 并生成版本记录"]
    C --> D["管理员点击「一键推送」选定设备"]
    D --> E["ESP32 接收 ota.start 并在后台拉取固件"]
    E --> F["mbedtls 校验 SHA-256 正确"]
    F --> G["重启切换应用程序槽位"]
    G --> H["90s 自检通过正式确认生效"]
```

### 3.1 Web 控制台一键推送（推荐）
1. 访问系统控制台，进入侧边栏的 **“固件与 OTA”** 页面 (`/firmware`)。
2. 将本地编译好的 `ble_hid/build/ble_hid.bin` 拖拽至上传区域。
3. 填入版本号（例如 `1.2.1-fix`）与更新说明，点击上传入库。
4. 在固件列表中找到刚上传的版本，点击 **“推送升级”**，勾选在线设备（例如 `esp32_001`）确认下发。
5. 在界面上观察环形进度条（0% $\rightarrow$ 100%），等待设备自动重启重连。

### 3.2 命令行 / API 直接推送
```bash
# 获取管理员 JWT Token
TOKEN=$(curl -s -X POST http://127.0.0.1:8000/api/v1/auth/login \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "username=admin&password=admin123" | jq -r .access_token)

# 向 esp32_001 推送 ID 为 1 的固件
curl -X POST http://127.0.0.1:8000/api/v2/firmwares/1/push \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"device_uuids": ["esp32_001"]}'
```

---

## 4. 固件健康自检与自动回滚验证

系统内置三重防变砖机制：
1. **下载完整性校验**：固件拉取完毕后校验 SHA-256，不匹配立刻丢弃不重启。
2. **看门狗与崩溃拦截**：处于 `PENDING_VERIFY` 状态的新固件发生任何复位（Panic / WDT），Bootloader 立即回滚。
3. **90s 网络在线健康自检**：新固件启动后必须在 90 秒内完成注册，超时未通过自动切回旧分区。

### 4.1 自动回滚验证测试流程
可通过 Kconfig 注入测试开关验证回滚机制：
1. 在 `ble_hid/main/Kconfig.projbuild` 中临时打开 `APP_OTA_TEST_UNHEALTHY`（故意拒绝上报健康状态）。
2. 编译并推送到目标设备。
3. 观察设备重启进入新分区，串口打出 `PENDING_VERIFY — health check started`。
4. 90 秒后观察到日志输出 `health check TIMEOUT — rolling back and rebooting`。
5. 设备自动重启并重新引导旧分区（例如 `ota_0`），设备再次正常上线，验证回滚闭环。

