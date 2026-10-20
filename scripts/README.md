# scripts

Prefer the Python entrypoints on Windows:

- `python scripts/py/start.py`
- `python scripts/py/stop.py`
- `python scripts/py/logs.py`
- `python scripts/py/clean.py`
- `python scripts/py/build_image.py`

The shell scripts under `scripts/sh/` are still available for Git Bash, WSL, Linux, and macOS. They target the **prod** stack by default (`compose.prod.yml`); override with the `COMPOSE_FILE` environment variable if needed.

## Web stack

The Python Docker helpers support both stacks explicitly:

- `--dev` uses `compose.dev.yml`
- `--prod` uses `compose.prod.yml`
- `COMPOSE_STACK=dev` or `COMPOSE_STACK=prod` can set the default

Common commands:

```bash
python scripts/py/start.py --dev
python scripts/py/start.py --dev --build
python scripts/py/start.py --prod --build

python scripts/py/stop.py --dev --app
python scripts/py/stop.py --prod --rm

python scripts/py/logs.py --dev
python scripts/py/logs.py --prod --all

python scripts/py/clean.py --dev --all
python scripts/py/clean.py --prod --image

python scripts/py/build_image.py --dev
python scripts/py/build_image.py --prod --tag v1.0
```

Notes:

- `start.py --dev` waits for both `http://127.0.0.1:5173` and `http://127.0.0.1:8000/api/v1/health`
- `start.py --prod` waits for `http://127.0.0.1:8000/api/v1/health`
- `build_image.py --tag` is only valid for `--prod`

## Backend-only local run

```bash
python scripts/py/start_backend.py --kill
python scripts/py/start_backend.py --port 8001
python scripts/py/start_backend.py --no-reload
```

## Frontend build

```bash
python scripts/py/build_frontend.py
```

## Firmware workflow

`ble_hid/flash.py` is optimized for the practical Windows flow:

1. Build in Docker with ESP-IDF.
2. Flash and monitor from the host with `esptool` and `esp-idf-monitor`.
3. Avoid Docker serial passthrough unless you explicitly want it.

Examples:

```bash
python ble_hid/flash.py
python ble_hid/flash.py --build
python ble_hid/flash.py --build-only
python ble_hid/flash.py --port COM5
python ble_hid/flash.py --list-ports
python ble_hid/flash.py --erase --no-monitor
python ble_hid/flash.py --idf-image espressif/idf:release-v5.4
```

Behavior:

- If flash artifacts are missing, `flash.py` auto-builds them in Docker by default.
- `--no-auto-build` turns that off and fails fast instead.
- `--docker-no-cache` forces a clean Docker image build before the container compile step.
- `--idf-image` or `ESP_IDF_IMAGE` lets you switch the ESP-IDF base image without editing files.

Host requirements for flashing and monitoring:

```bash
pip install esptool pyserial esp-idf-monitor
```

## Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `APP_PORT` | `8000` | Backend port for local scripts |
| `COMPOSE_STACK` | `prod` | Default Docker stack for `scripts/py/*.py` |
| `IMAGE_NAME` | `hid-gateway:latest` | Production app image tag |
| `ESP_IDF_IMAGE` | `espressif/idf:release-v5.4` | Base image used by `ble_hid/docker-compose.yml` |
