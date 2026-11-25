# ESP Fleet Hub

A unified multi-chip IoT device hub, cloud firmware compiler, OTA orchestrator, and wireless HID (Human Interface Device) control platform (`ESP32` / `ESP32-S3` / `ESP32-C3` / `ESP8266`).

```text
Client (Web UI / REST / WS) ──> ESP Fleet Hub (FastAPI) ──> Multi-Chip Fleet (ESP32-S3 / C3 / CAM / 8266)
                                                              ├── USB/BLE HID Control (/ws/device V2)
                                                              └── Fleet Provisioning, Shadow Config & Pull OTA (/api/fleet/*)
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
    - **Real-Time Hardware Console**: Tactile mechanical keycaps matrix (Navigation, Modifiers, Shortcuts, Alpha keys), text stream injection with presets, unified USB/BLE output-target switcher, multi-device concurrent dispatch, live ACK terminal, and a Pointer Lock mouse & keyboard takeover mode that streams real mouse movement, clicks, drags, scrolling and keystrokes to the target host (drag & arbitrary key combos require the updated firmware).
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
py_web/ (ESP Fleet Hub)
├── .github/workflows/    # CI pipeline (Alembic, pytest, unified framework, frontend build/test)
├── alembic/              # SQLAlchemy schema migrations (SQLite & MySQL)
├── alembic.ini
├── app/                  # FastAPI backend application
│   ├── api/              # REST (v1, v2, fleet_compat) & WebSocket (/ws/device, /ws/client)
│   ├── core/             # Configuration, logging, JWT/HMAC security & field crypto
│   ├── database/         # SQLAlchemy models, schemas, and sessions
│   ├── services/         # Dispatcher, ACK/OTA tracker, queue, firmware_utils, unified_store & compiler bridge
│   └── main.py           # FastAPI entrypoint and SPA mount
├── vue-project/          # Vue 3 SPA frontend (built -> vue-project/dist)
│   ├── scripts/          # Node native unit tests (check_frontend.test.mjs)
│   ├── src/              # Components, views (/unified, /devices, /terminal, ...), stores, router
│   └── vite.config.js
├── esp_unified_framework/ # Unified multi-chip firmware (ESP32-S3/C3/ESP32/ESP8266) + LuatOS compiler tools
│   ├── main/             # Modular HAL, web UI, fleet_agent, ws_gateway (TLS + chunked RX), rollback OTA, USB-OTG HID
│   ├── profiles/         # YAML hardware capability & Kconfig strip profiles (incl. esp32_cam_full)
│   └── tools/            # build_flavor.py, web_compiler.py (multi-task + per-port locks), nvs_gen.py, fleet_router.py, hub_db.py
├── ble_hid/              # Dedicated ESP32-S3 BLE + USB HID firmware project
├── scripts/              # Runner & ops scripts
│   ├── py/               # Cross-platform Python scripts (start, stop, logs, clean, build)
│   ├── sh/               # Shell equivalents (Git Bash / WSL / Linux / macOS)
│   └── mysql/            # MySQL backup / restore / schema dump helpers
├── tools/                # Host-side utilities (PC passthrough, macro recorder, WS tester)
├── tests/                # Pytest suite (REST/WS APIs, security hardening, RBAC, WS tunnel, unified fleet, smoke)
├── docs/                 # System architecture, protocol specs, API reference
├── deploy/               # Legacy SQL reference configs
├── backups/              # Database backups (gitignored)
├── uploads/              # Firmware upload storage (gitignored)
├── logs/                 # Runtime logs (gitignored)
├── docker-entrypoint.sh  # Container entrypoint running `alembic upgrade head` automatically
├── Dockerfile            # Production image (multi-stage: frontend build + backend runtime + openssh-client)
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

# 3. Apply database migrations (SQLite or MySQL per .env)
alembic upgrade head

# 4. Start FastAPI server
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
# Full pytest suite (REST/WS APIs, security hardening, schemas, unified framework, ESP32 dispatch E2E)
python -m pytest -q

# Frontend unit tests & production build check
cd vue-project && npm test && npm run build

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

Detailed specifications and architectural guides are organized in the **[docs/](docs/README.md)** directory:

- 📑 **[Documentation Center Index](docs/README.md)**: Full navigation map and architecture blueprint.
- 📋 **Requirements & Scenarios**:
  - [Product Requirements Document (PRD)](docs/01-requirements/product_requirements.md)
  - [Core User Scenarios & Sequence Flows](docs/01-requirements/user_scenarios.md)
- 🏗️ **System Architecture**:
  - [System Architecture (V2.1 Industrial)](docs/02-architecture/system_architecture.md)
  - [Security & Authorization Architecture](docs/02-architecture/security_architecture.md)
- 🔌 **Protocols & APIs**:
  - [Protocol V2 Specification](docs/03-protocols/protocol_v2_spec.md)
  - [API & WebSocket Reference](docs/03-protocols/api_reference.md)
- 🧩 **Subsystem Designs**:
  - [Backend Gateway Subsystem](docs/04-subsystems/backend_gateway.md)
  - [ESP32-S3 Firmware Subsystem](docs/04-subsystems/esp32_firmware.md)
  - [Frontend Web Subsystem](docs/04-subsystems/frontend_web.md)
- 🚀 **Operations & Deployment**:
  - [Deployment & Operations Guide](docs/05-operations/deployment_guide.md)
  - [Hardware & Network Devices List](docs/05-operations/devices.md)
  - [Firmware Build, Flashing & OTA Guide](docs/05-operations/firmware_flashing.md)
- 📜 **Handover & History**:
  - [Project Handover & Troubleshooting](docs/06-history-and-handover/project_handover.md)
  - [Changelog & Milestones](docs/06-history-and-handover/changelog.md)
