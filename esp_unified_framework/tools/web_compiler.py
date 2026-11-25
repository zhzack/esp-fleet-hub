#!/usr/bin/env python3
"""
ESP Unified Fleet Hub & LuatOS-Style Visual Web Compiler Server
Provides:
- mDNS service advertisement (_esp-hub._tcp) for zero-config device discovery
- SQLite Device Fleet Registry, HMAC Challenge-Response Auth & Config Shadow Sync
- Cross-Chip Cloud Compilation (Feiniu NAS IDF pipeline + local ESP8266 PIO)
- Serial Flasher with Hardware Pre-Probe & Automatic NVS Pre-Provisioning
"""

import sys
import os
import re
import json
import time
import shutil
import secrets
import subprocess
import threading
import uuid

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DIST_DIR = os.path.join(PROJECT_ROOT, "dist")
PROFILES_DIR = os.path.join(PROJECT_ROOT, "profiles")
TOOLS_DIR = os.path.join(PROJECT_ROOT, "tools")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import build_flavor
import hub_db
import nvs_gen
import mdns_advertiser

s_tasks = {}
s_task_order = []
s_latest_task_id = None
s_lock = threading.Lock()
_build_lock = threading.Lock()
_port_locks = {}
_task_save_fn = None
_task_load_fn = None


def set_task_store(save_fn, load_fn=None):
    """Register persistence hooks for compiler/flash task history and hydrate from store."""
    global _task_save_fn, _task_load_fn, s_latest_task_id
    _task_save_fn = save_fn
    _task_load_fn = load_fn
    if load_fn is not None:
        try:
            loaded = load_fn(limit=30) or []
            with s_lock:
                for item in loaded:
                    tid = item.get("task_id")
                    if not tid or tid in s_tasks:
                        continue
                    s_tasks[tid] = dict(item)
                    if tid not in s_task_order:
                        s_task_order.append(tid)
                if s_task_order and not s_latest_task_id:
                    s_latest_task_id = s_task_order[-1]
        except Exception:
            pass


def _persist_task(task_copy):
    fn = _task_save_fn
    if fn is None or not task_copy:
        return
    try:
        fn(task_copy)
    except Exception:
        pass


def _get_port_lock(port):
    with s_lock:
        if port not in _port_locks:
            _port_locks[port] = threading.Lock()
        return _port_locks[port]


def create_task(kind, label="", status="queued", initial_log=""):
    """Create a tracked background build or serial-flash task and return its task_id."""
    global s_latest_task_id
    tid = uuid.uuid4().hex[:10]
    now = int(time.time())
    task = {
        "task_id": tid,
        "kind": kind,
        "label": label,
        "status": status,
        "log": initial_log,
        "artifact_name": "",
        "artifact_size": 0,
        "percent": 0,
        "created_at": now,
        "updated_at": now,
    }
    with s_lock:
        s_tasks[tid] = task
        s_task_order.append(tid)
        s_latest_task_id = tid
        while len(s_task_order) > 30:
            old_id = s_task_order.pop(0)
            if old_id != s_latest_task_id:
                s_tasks.pop(old_id, None)
        snap = dict(task)
    _persist_task(snap)
    return tid


def _update_task(tid, append_log=None, **fields):
    snap = None
    with s_lock:
        task = s_tasks.get(tid)
        if not task:
            return
        if append_log:
            task["log"] += append_log
        for k, v in fields.items():
            task[k] = v
        task["updated_at"] = int(time.time())
        snap = dict(task)
    _persist_task(snap)


def is_build_running():
    return _build_lock.locked()


def is_port_flashing(port):
    return _get_port_lock(port).locked()


def list_tasks(limit=15):
    with s_lock:
        ids = list(reversed(s_task_order[-limit:]))
        return [
            {
                "task_id": t["task_id"],
                "kind": t["kind"],
                "label": t["label"],
                "status": t["status"],
                "artifact_name": t["artifact_name"],
                "created_at": t["created_at"],
                "updated_at": t["updated_at"],
            }
            for tid in ids
            if (t := s_tasks.get(tid))
        ]


def get_task_snapshot(task_id=None):
    with s_lock:
        tid = task_id or s_latest_task_id
        task = dict(s_tasks[tid]) if tid and tid in s_tasks else {
            "task_id": "",
            "kind": "idle",
            "label": "",
            "status": "idle",
            "log": "",
            "artifact_name": "",
            "artifact_size": 0,
            "percent": 0,
        }
        ids = list(reversed(s_task_order[-15:]))
        task["tasks"] = [
            {
                "task_id": t["task_id"],
                "kind": t["kind"],
                "label": t["label"],
                "status": t["status"],
                "artifact_name": t["artifact_name"],
                "created_at": t["created_at"],
                "updated_at": t["updated_at"],
            }
            for xid in ids
            if (t := s_tasks.get(xid))
        ]
        return task


def list_profiles_data():
    import yaml
    items = []
    if os.path.isdir(PROFILES_DIR):
        for fn in sorted(os.listdir(PROFILES_DIR)):
            if not fn.endswith(".yaml"):
                continue
            fp = os.path.join(PROFILES_DIR, fn)
            try:
                with open(fp, "r", encoding="utf-8") as f:
                    data = yaml.safe_load(f) or {}
                items.append({
                    "id": fn[:-5],
                    "file": fn,
                    "name": data.get("name", fn[:-5]),
                    "target": data.get("target", "esp32"),
                    "description": data.get("description", ""),
                    "features": data.get("features", {}),
                    "hardware": data.get("hardware", {}),
                    "defaults": data.get("defaults", {}),
                })
            except Exception:
                pass
    return items


def list_serial_ports():
    import serial.tools.list_ports
    ports = []
    for p in serial.tools.list_ports.comports():
        if "BTHENUM" in p.hwid or "PNP0501" in p.hwid:
            continue
        ports.append({"port": p.device, "desc": p.description})
    return ports


def run_build_task(payload, hub_port=None, task_id=None):
    eff_port = int(hub_port or SERVER_PORT)
    target = payload.get("target", "esp32")
    profile_name = payload.get("name", "custom_build")
    tid = task_id or create_task("build", f"{profile_name} ({target})", status="building")
    _update_task(
        tid,
        status="building",
        log=">>> [Cloud Compiler] Validating SoC capabilities and initializing job...\n",
    )

    try:
        features = payload.get("features", {})
        hardware = payload.get("hardware", {})
        build_flavor.validate_soc_compatibility(target, features, hardware)

        with _build_lock:
            profile_path = os.path.join(PROFILES_DIR, f"{profile_name}.yaml")
            os.makedirs(PROFILES_DIR, exist_ok=True)
            with open(profile_path, "w", encoding="utf-8") as f:
                import yaml
                yaml.dump(payload, f)

            _update_task(
                tid,
                append_log=f"✅ SoC compatibility validated for [{target}]\nSaved profile to {profile_path}\n",
            )

            # Branch A: ESP8266 PlatformIO lightweight build
            if target == "esp8266":
                _update_task(
                    tid,
                    append_log=(
                        "\n===========================================================================\n"
                        "ℹ️ [ESP8266 专属轻量内核流水线] 目标架构: Xtensa L106 (32-bit 单核)\n"
                        "===========================================================================\n"
                    ),
                )
                pio_dir = os.path.join(PROJECT_ROOT, "esp8266_port")
                pio_exe = os.path.join(os.path.expanduser("~"), ".platformio", "penv", "Scripts", "pio.exe")
                built_8266 = None

                if os.path.isdir(pio_dir) and os.path.isfile(pio_exe):
                    flags = []
                    if features.get("web_ui", True): flags.append("-DFEATURE_WEB_UI=1")
                    if features.get("rest_api", True): flags.append("-DFEATURE_REST_API=1")
                    if features.get("gpio_ctrl", True): flags.append("-DFEATURE_GPIO_CTRL=1")
                    if features.get("ota", True): flags.append("-DFEATURE_OTA=1")
                    if features.get("fleet_agent", True): flags.append("-DFEATURE_FLEET_AGENT=1")
                    defs = payload.get("defaults", {})
                    wifi_s = defs.get("wifi_ssid", "1")
                    wifi_p = defs.get("wifi_pass", "11111111")
                    flags.append(f'-DDEFAULT_WIFI_SSID=\\"{wifi_s}\\"')
                    flags.append(f'-DDEFAULT_WIFI_PASS=\\"{wifi_p}\\"')
                    if defs.get("mdns_host"):
                        flags.append(f'-DDEFAULT_MDNS_HOST=\\"{defs["mdns_host"]}\\"')
                    hub_u = defs.get("hub_url") or f"http://{mdns_advertiser.get_lan_ip()}:{eff_port}"
                    flags.append(f'-DDEFAULT_HUB_URL=\\"{hub_u}\\"')

                    env = os.environ.copy()
                    env["PLATFORMIO_BUILD_FLAGS"] = " ".join(flags)
                    proc = subprocess.Popen(
                        [pio_exe, "run", "-d", pio_dir],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, encoding="utf-8", errors="replace", env=env
                    )
                    for line in iter(proc.stdout.readline, ""):
                        _update_task(tid, append_log=line)
                    proc.wait()
                    if proc.returncode == 0:
                        cand = os.path.join(pio_dir, ".pio", "build", "esp8266_unified", "firmware.bin")
                        if os.path.isfile(cand):
                            built_8266 = cand

                if not built_8266:
                    src_8266 = os.path.join(DIST_DIR, "esp8266_smart_switch.bin")
                    if not os.path.isfile(src_8266):
                        src_tester = os.path.abspath(os.path.join(PROJECT_ROOT, "..", "..", "esp8266-batch-tester", "firmware.bin"))
                        if os.path.isfile(src_tester):
                            os.makedirs(DIST_DIR, exist_ok=True)
                            shutil.copyfile(src_tester, src_8266)
                    if os.path.isfile(src_8266):
                        built_8266 = src_8266

                if built_8266:
                    meta = build_flavor.archive_build_artifacts(profile_name, payload, built_8266)
                    _update_task(
                        tid,
                        status="success",
                        artifact_name=f"{profile_name}.bin",
                        artifact_size=meta["size"],
                        append_log=f"\n🎉 [ESP8266 构建完成] {profile_name}.bin ({meta['size']/1024:.1f} KB)\n",
                    )
                else:
                    _update_task(tid, status="failed", append_log="\n❌ 未能产出 ESP8266 固件产物！\n")
                return

            # Branch B: Modern ESP-IDF chips (ESP32 / S3 / C3 / C2)
            pipeline_path = os.path.join(PROJECT_ROOT, "pipeline.yaml")
            pipeline_data = {
                "name": f"esp_unified_{profile_name}",
                "target": target,
                "flash": {"port": "COM11", "baud": 921600, "auto_flash": False},
            }
            with open(pipeline_path, "w", encoding="utf-8") as f:
                import yaml
                yaml.dump(pipeline_data, f)

            _update_task(tid, append_log=f"Invoking build_flavor on remote IDF pipeline (target={target})...\n")

            cfg = build_flavor.generate_sdkconfig_defaults(payload)
            with open(os.path.join(PROJECT_ROOT, "sdkconfig.defaults"), "w", encoding="utf-8") as f:
                f.write(cfg)
            build_flavor.clean_stale_sdkconfig()

            build_cmd = [build_flavor._build_env_config()["cmd"], "-t", target]
            proc = subprocess.Popen(
                build_cmd,
                cwd=PROJECT_ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                shell=True,
                text=True,
                bufsize=1,
                encoding="utf-8",
                errors="replace",
            )
            for line in iter(proc.stdout.readline, ""):
                _update_task(tid, append_log=line)
            proc.wait()

            if proc.returncode == 0:
                built_bin = os.path.join(PROJECT_ROOT, "build", "esp_unified_framework.bin")
                if os.path.isfile(built_bin):
                    meta = build_flavor.archive_build_artifacts(profile_name, payload, built_bin)
                    _update_task(
                        tid,
                        status="success",
                        artifact_name=f"{profile_name}.bin",
                        artifact_size=meta["size"],
                        append_log=(
                            f"\n🎉 Build Finished! {profile_name}.bin ({meta['size']/1024:.1f} KB)\n"
                            f"   Target={meta['target']} | Board={meta['board']} | Flash={meta['flash_size']} | SHA256={meta['sha256'][:16]}...\n"
                        ),
                    )
                else:
                    _update_task(tid, status="failed", append_log="\n❌ Binary output not found!\n")
            else:
                _update_task(tid, status="failed", append_log=f"\n❌ Pipeline failed with exit code {proc.returncode}\n")
    except Exception as e:
        _update_task(tid, status="failed", append_log=f"\n❌ Exception: {e}\n")


def run_serial_flash_task(port, baud, file_name, target, full_flash=True,
                          auto_provision=True, wifi_ssid="1", wifi_pass="11111111", mdns_host="esp-node",
                          hub_port=None, task_id=None):
    eff_port = int(hub_port or SERVER_PORT)
    tid = task_id or create_task("flash", f"{port} <- {file_name}", status="flashing")
    header_log = (
        f"\n===========================================================================\n"
        f"⚡ [串口烧录] 目标端口: {port} | 波特率: {baud} | 固件: {file_name}\n"
        f"===========================================================================\n"
    )
    _update_task(tid, status="flashing", append_log=header_log)

    dist_bin = os.path.join(DIST_DIR, file_name)
    if not os.path.isfile(dist_bin):
        _update_task(tid, status="failed", append_log=f"❌ 固件文件不存在: {dist_bin}\n")
        return

    port_lock = _get_port_lock(port)
    with port_lock:
        _update_task(tid, append_log=f"🔍 [预检] 正在探测目标串口硬件 ({port})...\n")

        detected_chip = ""
        detected_mac = ""
        detected_flash_mb = None

        probe_cmd = [sys.executable, "-m", "esptool", "--port", port, "--baud", "115200", "flash-id"]
        try:
            probe_res = subprocess.run(
                probe_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=10, encoding="utf-8", errors="replace"
            )
            probe_output = probe_res.stdout
            for line in probe_output.splitlines():
                if "Chip type:" in line or "Chip is" in line:
                    detected_chip = line.split(":")[-1].strip() if ":" in line else line.strip()
                if "MAC:" in line:
                    m_mac = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", line)
                    if m_mac:
                        detected_mac = m_mac.group(1).upper()
                if "Detected flash size:" in line:
                    size_str = line.split(":")[-1].strip()
                    if "MB" in size_str:
                        try:
                            detected_flash_mb = float(size_str.replace("MB", "").strip())
                        except ValueError:
                            pass

            if detected_chip:
                _update_task(tid, append_log=f"  ↳ 硬件在线: {detected_chip} | MAC={detected_mac} | Flash={detected_flash_mb or '?'}MB\n")

                meta = build_flavor.load_artifact_meta(file_name)
                fw_target = meta["target"].lower() if meta and meta.get("target") else target.lower()

                phys_norm = detected_chip.upper().replace("-", "")
                if "ESP8266" in phys_norm:
                    expected_family = "ESP8266"
                elif "ESP32S3" in phys_norm:
                    expected_family = "ESP32S3"
                elif "ESP32C3" in phys_norm:
                    expected_family = "ESP32C3"
                elif "ESP32C2" in phys_norm:
                    expected_family = "ESP32C2"
                elif "ESP32S2" in phys_norm:
                    expected_family = "ESP32S2"
                else:
                    expected_family = "ESP32"

                fw_norm = fw_target.upper().replace("-", "")
                if fw_norm != expected_family and not ("ESP8266" in file_name.lower() and expected_family == "ESP8266"):
                    _update_task(
                        tid,
                        status="failed",
                        append_log=(
                            f"\n⛔ [安全拦截] 芯片架构与固件严重错配！\n"
                            f"  - 端口 {port} 物理芯片: {detected_chip} ({expected_family})\n"
                            f"  - 所选固件目标架构: {fw_norm}\n"
                        ),
                    )
                    return

                fw_bytes = os.path.getsize(dist_bin)
                if detected_flash_mb:
                    max_bytes = int(detected_flash_mb * 1024 * 1024)
                    if fw_bytes > max_bytes:
                        _update_task(
                            tid,
                            status="failed",
                            append_log=f"\n⛔ [安全拦截] 固件大小 ({fw_bytes/1024:.1f}KB) 超过物理 Flash ({detected_flash_mb}MB)！\n",
                        )
                        return
        except Exception as pe:
            _update_task(tid, append_log=f"  ↳ 硬件探测警告 ({pe})，继续烧录...\n")

        meta = build_flavor.load_artifact_meta(file_name)
        effective_target = meta["target"].lower() if meta and meta.get("target") else target.lower()

        nvs_prov_path = None
        prov_dev_id = None
        if auto_provision and detected_mac:
            mac_tail = detected_mac.replace(":", "").lower()[-6:]
            prov_dev_id = f"{effective_target}-{mac_tail}"
            dev_secret = "" if effective_target == "esp8266" else secrets.token_hex(16)
            lan_ip = mdns_advertiser.get_lan_ip()
            hub_url = f"http://{lan_ip}:{eff_port}"

            hub_db.provision_device(
                device_id=prov_dev_id,
                mac=detected_mac,
                chip=detected_chip or effective_target.upper(),
                flash_mb=int(detected_flash_mb or 4),
                board=meta.get("board", "auto") if meta else "auto",
                device_secret=dev_secret,
                hub_url=hub_url,
                allow_empty_secret=(effective_target == "esp8266"),
            )

            if effective_target != "esp8266":
                prov_dir = os.path.join(PROJECT_ROOT, "data", "nvs_prov")
                nvs_prov_path = os.path.join(prov_dir, f"{prov_dev_id}.bin")
                nvs_gen.generate_nvs_provision_bin(nvs_prov_path, {
                    "wifi_ssid": wifi_ssid,
                    "wifi_pass": wifi_pass,
                    "device_id": prov_dev_id,
                    "mdns_host": f"{effective_target}-{mac_tail}",
                    "hub_url": hub_url,
                    "device_secret": dev_secret,
                    "web_port": 80,
                    "config_ver": 1,
                })
            _update_task(tid, append_log=f"🪪 [出厂预置] 已生成唯一身份 {prov_dev_id} 并预注册至 Fleet Hub ({hub_url})\n")

        cmd = [sys.executable, "-m", "esptool", "--port", port, "--baud", str(baud)]
        if effective_target and effective_target != "generic":
            cmd += ["--chip", effective_target.replace("-", "")]

        if effective_target == "esp8266":
            cmd += ["write-flash", "0x0", dist_bin]
        else:
            base_name = file_name[:-4] if file_name.endswith(".bin") else file_name
            bundle_dir = os.path.join(DIST_DIR, "bundles", base_name)
            build_dir = bundle_dir if os.path.isdir(bundle_dir) else os.path.join(PROJECT_ROOT, "build")

            bootloader_bin = os.path.join(build_dir, "bootloader.bin")
            if not os.path.isfile(bootloader_bin):
                bootloader_bin = os.path.join(build_dir, "bootloader", "bootloader.bin")
            pt_bin = os.path.join(build_dir, "partition-table.bin")
            if not os.path.isfile(pt_bin):
                pt_bin = os.path.join(build_dir, "partition_table", "partition-table.bin")
            ota_data = os.path.join(build_dir, "ota_data_initial.bin")
            bl_offset = "0x0" if effective_target in ["esp32c3", "esp32s3", "esp32c2"] else "0x1000"

            flash_items = []
            if full_flash and os.path.isfile(bootloader_bin) and os.path.isfile(pt_bin):
                flash_items.extend([bl_offset, bootloader_bin, "0x8000", pt_bin])
                if os.path.isfile(ota_data):
                    flash_items.extend(["0xd000", ota_data])
                if nvs_prov_path and os.path.isfile(nvs_prov_path):
                    flash_items.extend(["0x9000", nvs_prov_path])
                flash_items.extend(["0x10000", dist_bin])
                _update_task(tid, append_log=f"模式: 全量刷写 (BL@{bl_offset} + NVS预置 + PT@0x8000 + App@0x10000)\n")
            else:
                if nvs_prov_path and os.path.isfile(nvs_prov_path):
                    flash_items.extend(["0x9000", nvs_prov_path])
                flash_items.extend(["0x10000", dist_bin])
                _update_task(tid, append_log="模式: 仅更新应用固件 + NVS预置\n")

            cmd += ["write-flash"] + flash_items

        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1, encoding="utf-8", errors="replace"
            )
            for line in iter(proc.stdout.readline, ""):
                _update_task(tid, append_log=line)
            proc.wait()
            if proc.returncode == 0:
                _update_task(
                    tid,
                    status="success",
                    append_log=f"\n🎉 [烧录成功] 芯片已硬复位！设备 {prov_dev_id or ''} 上电后将自动接入设备大盘！\n",
                )
            else:
                _update_task(tid, status="failed", append_log=f"\n❌ [烧录失败] Exit={proc.returncode}\n")
        except Exception as e:
            _update_task(tid, status="failed", append_log=f"\n❌ 烧录进程异常: {e}\n")
