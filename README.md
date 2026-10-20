# HID Gateway Platform

A scalable, multi-client, multi-device network HID (Human Interface Device) forwarding platform.

```text
Client (Web UI / REST / WS) ──> Gateway Server (FastAPI) ──> ESP32-S3 ──> HID Output (USB / BLE)
```

---

## 📌 Features

- **Multi-Device & Multi-Client**: Supports simultaneous client connections (Web UI, REST API, WebSocket) to control multiple ESP32 HID peripheral devices.
- **Hardware Integration**: Runs on ESP32-S3 (ESP-IDF v5.4) supporting both **BLE HID** (Bluetooth Low Energy) and **USB HID** mouse/keyboard emulation.
- **Protocol V2 & REST Control**: Supports flexible JSON-based command packets, batch macros, trackpad/mouse controls, keyboard events, and bidirectional state synchronization.
- **Modern Full-Stack Architecture**:
  - **Frontend Console (Modernized)**:
    - **Design System**: Rich dark/light theme, glassmorphism (`glass-panel`), HSL design tokens, and smooth micro-animations.
    - **Login & Register**: Modern frosted cards, smooth tab switching, password strength meter, remember-me.
    - **Dashboard**: Live KPI metrics (total devices, active hardware, success rates, latency gauge) and recent log stream.
    - **Device Management**: Dual grid/table view toggle, search & status filter tabs, hardware specs, one-click slide-out control drawer (WiFi scanning/connect, BLE host pairing & slot binding, USB/BLE transport switching).
    - **Real-Time Hardware Console**: Tactile mechanical keycaps matrix (Navigation, Modifiers, Shortcuts, Alpha keys), text stream injection with presets, BLE target host switcher, and live ACK terminal.
    - **Device Grouping & Batch Dispatch**: Cluster devices into groups with predefined action presets and latency comparison reports.
    - **Macro Pipeline Builder**: Visual drag-and-drop workflow builder with color-coded nodes (keyboard, mouse, delay, text) and live JSON schema preview.
    - **Action Log Center**: Multi-parameter filter toolbar (status, action type, limit), color-coded latency speed indicators, 3s auto-refresh, and audit inspection drawer.
    - **Touchpad Suites**: Full-screen gesture touchpad (`/trackpad`), distraction-free pure trackpad (`/trackpad-pure`), and kiosk showcase (`/trackpad-kiosk`) with pinch-to-zoom and continuous spring-back scrolling.
  - **Backend**: Python 3.11+, FastAPI, SQLAlchemy, MySQL, Redis / In-memory queue.
  - **Firmware**: ESP-IDF C/C++ (`ble_hid`).
- **Tools**:
  - `tools/pc_passthrough.py`: Windows Raw Input hardware-relative mouse passthrough under cursor lock, global `Ctrl+Shift+Q` hotkey, and anti-echo filter.
  - `tools/macro_recorder.py`: Low-level mouse & keyboard macro recorder with jitter filtering and one-click cloud upload.


---

## 📂 Directory Structure

```text
py_web/
├── app/                  # FastAPI backend application
│   ├── api/              # REST (v1, v2) & WebSocket routers
│   ├── core/             # Configuration, logging, security
│   ├── database/         # SQLAlchemy models and database sessions
│   ├── modules/          # Business logic modules (auth, device, macro, queue)
│   ├── services/         # Device manager, queue worker, protocol handlers
│   └── main.py           # FastAPI entrypoint and static mount
├── vue-project/          # Vue 3 SPA frontend
│   ├── src/              # Components, views, stores, router, API client
│   └── vite.config.js
├── ble_hid/              # ESP32-S3 ESP-IDF firmware project
│   ├── main/             # BLE HID, USB HID, Wi-Fi / WebSocket logic, OTA
│   └── flash.py          # Firmware build / flash / monitor helper
├── scripts/              # Runner & ops scripts
│   ├── py/               # Cross-platform Python scripts (start, stop, logs, clean, build)
│   ├── sh/               # Shell equivalents (Git Bash / WSL / Linux / macOS)
│   └── mysql/            # MySQL backup / restore / schema dump helpers
├── tools/                # Host-side utilities (PC passthrough, macro recorder, WS tester)
├── tests/                # Pytest suite (REST & WS APIs, schemas, services, ESP32 dispatch E2E, smoke)
├── docs/                 # System architecture, protocol specs, API reference
├── deploy/               # Deployment configs (MySQL init SQL)
├── backups/              # Database backups (gitignored)
├── uploads/              # Firmware upload storage (gitignored)
├── logs/                 # Runtime logs (gitignored)
├── Dockerfile            # Production image (multi-stage: frontend build + backend runtime)
├── Dockerfile.backend    # Backend-only image (used by compose.dev.yml)
├── Dockerfile.frontend.dev # Vite dev-server image (used by compose.dev.yml)
├── compose.dev.yml       # Docker compose configuration (Development)
├── compose.prod.yml      # Docker compose configuration (Production)
├── run.py                # Local FastAPI runner with local IP display
└── requirements.txt      # Python dependencies
```

---

## 🚀 Quick Start

### 1. Prerequisites

- Python 3.10+
- Node.js 18+ and npm
- Docker & Docker Compose (optional for containerized run)
- ESP-IDF v5.4 (or Docker Espressif image for firmware compilation)

### 2. Local Backend Run

```bash
# 1. Activate virtual environment
.venv\Scripts\activate   # Windows
# source .venv/bin/activate # Linux / macOS

# 2. Install dependencies
pip install -r requirements.txt

# 3. Start FastAPI server
python run.py
# Or using script:
python scripts/py/start_backend.py
```

Backend will be running at:
- **Local**: `http://127.0.0.1:8000`
- **Swagger Docs**: `http://127.0.0.1:8000/docs`
- **Health Check**: `http://127.0.0.1:8000/api/v1/health`

### 3. Local Frontend Run

```bash
cd vue-project
npm install
npm run dev
```

Frontend development server will be running at `http://127.0.0.1:5173`.

### 4. Run Tests

```bash
# Full pytest suite (REST/WS APIs, schemas, ESP32 dispatch E2E; uses in-memory SQLite)
.venv\Scripts\pytest

# E2E smoke test against a running backend (simulates an ESP32 over WebSocket)
python tests/smoke_test.py
```

### 5. Docker Stack (Recommended for Integrated Testing)

Using the cross-platform Python scripts:

```bash
# Start development stack (FastAPI + Vite frontend + MySQL + Redis)
python scripts/py/start.py --dev

# Check logs
python scripts/py/logs.py --dev

# Stop development stack
python scripts/py/stop.py --dev --app
```

---

## 🛠️ ESP32-S3 Firmware Compilation

Firmware can be built locally or via Docker without installing ESP-IDF locally:

```bash
# Using Docker (from repo root)
docker run --rm -v "%cd%/ble_hid:/project" -w /project espressif/idf:release-v5.4 idf.py set-target esp32s3
docker run --rm -v "%cd%/ble_hid:/project" -w /project espressif/idf:release-v5.4 idf.py build
```

---

## 📖 Documentation Reference

Detailed architectural and design specifications are available in the [docs/](docs/) directory:

- [API Reference](docs/api_reference.md)
- [System Architecture (V1.1)](docs/hid_gateway_detailed_architecture_v_1.md)
- [Protocol V2 Design](docs/hid_gateway_protocol_v_2_design.md)
- [Backend Design](docs/hid_gateway_backend_design_v_1.md)
- [ESP32 Firmware Design](docs/hid_gateway_esp_32_firmware_design_v_1.md)
- [Docker Usage Guide](docs/docker_usage.md)
