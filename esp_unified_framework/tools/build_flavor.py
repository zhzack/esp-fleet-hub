#!/usr/bin/env python3
"""
ESP Unified Framework - LuatOS-Style Profile Builder
Builds bespoke, stripped-down firmwares for different chips and requirements,
with strict board-pinout isolation, partition sizing, and meta-bundle archiving.
"""

import sys
import os
import json
import time
import hashlib
import argparse
import subprocess
import shutil
import yaml

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DIST_DIR = os.path.join(PROJECT_ROOT, "dist")
BUNDLES_DIR = os.path.join(DIST_DIR, "bundles")
PROFILES_DIR = os.path.join(PROJECT_ROOT, "profiles")

BOARD_CONFIG_MAP = {
    "auto": "CONFIG_BOARD_AUTO_DEFAULT=y",
    "esp32_cam_aithinker": "CONFIG_BOARD_ESP32_CAM_AITHINKER=y",
    "esp32s3_cam": "CONFIG_BOARD_ESP32S3_CAM_DEV=y",
    "esp32s3_devkit": "CONFIG_BOARD_ESP32S3_DEVKIT=y",
    "esp32c3_air101": "CONFIG_BOARD_ESP32C3_AIR101=y",
    "esp32c3_supermini": "CONFIG_BOARD_ESP32C3_SUPERMINI=y",
    "esp32_generic": "CONFIG_BOARD_ESP32_GENERIC=y",
    "esp32c2_generic": "CONFIG_BOARD_ESP32C2_GENERIC=y",
}

PARTITION_CSV_MAP = {
    "2MB": ("CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y", "2MB", "partitions_2mb.csv"),
    "4MB": ("CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y", "4MB", "partitions.csv"),
    "8MB": ("CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y", "8MB", "partitions_8mb.csv"),
    "16MB": ("CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y", "16MB", "partitions_16mb.csv"),
}

SOC_CAPABILITIES = {
    "esp8266": {
        "unsupported": {
            "camera": "ESP8266 芯片无 8-bit 并行 DVP 摄像头控制器硬件接口",
            "usb_otg": "ESP8266 芯片无片上原生 USB-OTG 控制器 (仅支持外部串口)",
            "spiram": "ESP8266 芯片无片外 PSRAM 内存总线接口",
            "display": "ESP8266 内部内存受限 (仅约 80KB RAM)，不适宜挂载大型彩色液晶屏",
            "pc_monitor": "ESP8266 引脚不足且内存受限，不支持 Air101 监控屏方案",
            "joystick": "ESP8266 仅少数 GPIO，不支持 5 向摇杆矩阵",
        }
    },
    "esp32": {
        "unsupported": {
            "usb_otg": "经典 ESP32 芯片无片上原生 USB-OTG 控制器 (仅支持外部串口芯片)",
        }
    },
    "esp32s3": {
        "unsupported": {}
    },
    "esp32c3": {
        "unsupported": {
            "camera": "ESP32-C3 芯片无 8-bit 并行 DVP 摄像头控制器硬件接口",
            "usb_otg": "ESP32-C3 仅支持 USB-Serial-JTAG，不支持原生 USB-OTG 协议栈",
            "spiram": "ESP32-C3 芯片无片外 PSRAM 内存总线接口",
        }
    },
    "esp32c2": {
        "unsupported": {
            "camera": "ESP32-C2 芯片无摄像头硬件接口",
            "usb_otg": "ESP32-C2 芯片无原生 USB-OTG 控制器",
            "spiram": "ESP32-C2 芯片无片外 PSRAM 支持",
            "display": "ESP32-C2 内部内存受限 (仅~272KB)，不适宜挂载大型显示帧缓冲",
            "pc_monitor": "ESP32-C2 引脚受限，不支持 Air101 监控屏方案",
            "joystick": "ESP32-C2 引脚受限，不支持 5 向摇杆矩阵",
        }
    },
}


def validate_soc_compatibility(target, features, hardware):
    spec = SOC_CAPABILITIES.get(target)
    if spec:
        unsupported = spec.get("unsupported", {})
        for feat, is_enabled in features.items():
            if is_enabled and feat in unsupported:
                raise ValueError(f"【芯片硬件不兼容】目标芯片 '{target}' 无法开启组件 '{feat}': {unsupported[feat]}")
        if hardware.get("spiram", False) and "spiram" in unsupported:
            raise ValueError(f"【芯片硬件不兼容】目标芯片 '{target}' 不支持使能 PSRAM/SPIRAM: {unsupported['spiram']}")

    flash_size = str(hardware.get("flash_size", "4MB")).upper()
    if flash_size == "2MB":
        if features.get("storage", False):
            raise ValueError("【Flash 分区容量不足】2MB 双 OTA 分区表不含 SPIFFS storage 分区，请选用 4MB 及以上 Flash 或关闭文件系统。")
        if features.get("camera", False) or features.get("pc_monitor", False):
            raise ValueError("【Flash 分区容量不足】启用摄像头或图形屏幕后的固件 (~1MB) 超过 2MB 双 OTA 单槽上限 (960KB)，请切换至 4MB 及以上 Flash。")


def load_profile(profile_path_or_name):
    if not os.path.isfile(profile_path_or_name):
        candidate = os.path.join(PROFILES_DIR, profile_path_or_name)
        if not candidate.endswith(".yaml"):
            candidate += ".yaml"
        if os.path.isfile(candidate):
            profile_path_or_name = candidate
        else:
            raise FileNotFoundError(f"Profile not found: {profile_path_or_name}")

    with open(profile_path_or_name, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    return data, profile_path_or_name


def generate_sdkconfig_defaults(profile_data):
    target = profile_data.get("target", "esp32")
    features = profile_data.get("features", {})
    hardware = profile_data.get("hardware", {})
    defaults = profile_data.get("defaults", {})

    validate_soc_compatibility(target, features, hardware)

    flash_size = str(hardware.get("flash_size", "4MB")).upper()
    flash_cfg_macro, flash_cfg_val, part_csv = PARTITION_CSV_MAP.get(
        flash_size, PARTITION_CSV_MAP["4MB"]
    )
    if not os.path.isfile(os.path.join(PROJECT_ROOT, part_csv)):
        part_csv = "partitions.csv"

    board = str(hardware.get("board", "auto")).lower()
    board_macro = BOARD_CONFIG_MAP.get(board, BOARD_CONFIG_MAP["auto"])

    cpu_freq = int(hardware.get("cpu_freq_mhz", 240 if target == "esp32s3" else 160))
    if target == "esp32c3" and cpu_freq > 160:
        cpu_freq = 160
    elif target == "esp32c2" and cpu_freq > 120:
        cpu_freq = 120

    lines = [
        f'# Generated by LuatOS-Style Profile Builder: {profile_data.get("name", "custom")}',
        f'CONFIG_IDF_TARGET="{target}"',
        f'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_{cpu_freq}=y',
        f'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ={cpu_freq}',
        'CONFIG_ESP_BROWNOUT_DET=n',
        'CONFIG_PARTITION_TABLE_CUSTOM=y',
        f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{part_csv}"',
        f'CONFIG_PARTITION_TABLE_FILENAME="{part_csv}"',
        'CONFIG_FREERTOS_HZ=1000',
        'CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096',
        'CONFIG_ESP_TIMER_TASK_STACK_SIZE=4096',
        'CONFIG_ESP_MAIN_TASK_STACK_SIZE=6144',
        flash_cfg_macro,
        f'CONFIG_ESPTOOLPY_FLASHSIZE="{flash_cfg_val}"',
        board_macro,
    ]

    if hardware.get("spiram", False):
        lines.extend([
            'CONFIG_SPIRAM=y',
            'CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y',
            'CONFIG_SPIRAM_USE_MALLOC=y',
            'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384',
            'CONFIG_SPIRAM_IGNORE_NOTFOUND=y',
        ])
        if target == "esp32s3":
            lines.extend([
                'CONFIG_SPIRAM_MODE_OCT=y',
                'CONFIG_SPIRAM_SPEED_80M=y',
            ])
    else:
        lines.append('CONFIG_SPIRAM=n')

    if features.get("usb_otg", False) and target in ("esp32s2", "esp32s3", "esp32p4"):
        lines.extend([
            'CONFIG_TINYUSB_HID_COUNT=1',
            'CONFIG_TINYUSB_HID_BUFSIZE=64',
            'CONFIG_TINYUSB_CDC_ENABLED=n',
        ])

    if features.get("http_server", True):
        lines.extend([
            'CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024',
            'CONFIG_HTTPD_MAX_URI_LEN=512',
            'CONFIG_HTTPD_WS_SUPPORT=y',
            'CONFIG_FEATURE_HTTP_SERVER=y',
        ])
    else:
        lines.append('CONFIG_FEATURE_HTTP_SERVER=n')

    if features.get("ota", True):
        lines.append('CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y')

    feat_map = {
        "web_ui": "CONFIG_FEATURE_WEB_UI",
        "rest_api": "CONFIG_FEATURE_REST_API",
        "ota": "CONFIG_FEATURE_OTA",
        "serial_cli": "CONFIG_FEATURE_SERIAL_CLI",
        "fleet_agent": "CONFIG_FEATURE_FLEET_AGENT",
        "camera": "CONFIG_FEATURE_CAMERA",
        "gpio_ctrl": "CONFIG_FEATURE_GPIO_CTRL",
        "storage": "CONFIG_FEATURE_STORAGE",
        "display": "CONFIG_FEATURE_DISPLAY",
        "usb_otg": "CONFIG_FEATURE_USB_OTG",
        "pc_monitor": "CONFIG_FEATURE_PC_MONITOR",
        "joystick": "CONFIG_FEATURE_JOYSTICK",
        "ws_gateway": "CONFIG_FEATURE_WS_GATEWAY",
    }
    default_on = {"serial_cli", "fleet_agent"}
    for feat_key, config_macro in feat_map.items():
        val = features.get(feat_key, feat_key in default_on)
        lines.append(f"{config_macro}={'y' if val else 'n'}")

    fw_version = defaults.get("fw_version", "1.1.0-unified")
    lines.append(f'CONFIG_APP_FW_VERSION="{fw_version}"')

    if "wifi_ssid" in defaults:
        lines.append(f'CONFIG_APP_DEFAULT_WIFI_SSID="{defaults["wifi_ssid"]}"')
    if "wifi_pass" in defaults:
        lines.append(f'CONFIG_APP_DEFAULT_WIFI_PASS="{defaults["wifi_pass"]}"')
    if "device_id" in defaults:
        lines.append(f'CONFIG_APP_DEFAULT_DEVICE_ID="{defaults["device_id"]}"')
    if "mdns_host" in defaults:
        lines.append(f'CONFIG_APP_MDNS_HOST="{defaults["mdns_host"]}"')
    if "hub_url" in defaults:
        lines.append(f'CONFIG_APP_DEFAULT_HUB_URL="{defaults["hub_url"]}"')
    if "ws_gateway_url" in defaults:
        lines.append(f'CONFIG_APP_DEFAULT_WS_GATEWAY_URL="{defaults["ws_gateway_url"]}"')

    return "\n".join(lines) + "\n"


def clean_stale_sdkconfig():
    """Remove local sdkconfig and remote NAS sdkconfig so new sdkconfig.defaults is always honored."""
    local_sdkcfg = os.path.join(PROJECT_ROOT, "sdkconfig")
    if os.path.isfile(local_sdkcfg):
        try:
            os.remove(local_sdkcfg)
        except OSError:
            pass

    # Also clear stale sdkconfig on Feiniu NAS build node before stage_sync runs
    ssh_key = os.path.expanduser("~/.ssh/id_ed25519")
    cmd = ["ssh", "-o", "StrictHostKeyChecking=no", "-o", "ConnectTimeout=4"]
    if os.path.isfile(ssh_key):
        cmd.extend(["-i", ssh_key])
    cmd.extend([
        "b1ue@192.168.68.146",
        "rm -f /vol1/esp/projects/esp_unified_framework/sdkconfig",
    ])
    try:
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=6)
    except Exception:
        pass


def compute_sha256(file_path):
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def archive_build_artifacts(profile_name, payload, built_bin_path):
    """Save binary, .meta.json, and target-matched bootloader/partition bundle to dist/."""
    os.makedirs(DIST_DIR, exist_ok=True)
    bundle_dir = os.path.join(BUNDLES_DIR, profile_name)
    os.makedirs(bundle_dir, exist_ok=True)

    dist_bin = os.path.join(DIST_DIR, f"{profile_name}.bin")
    shutil.copyfile(built_bin_path, dist_bin)
    shutil.copyfile(built_bin_path, os.path.join(bundle_dir, "app.bin"))

    build_dir = os.path.join(PROJECT_ROOT, "build")
    bl_candidates = [
        os.path.join(build_dir, "bootloader", "bootloader.bin"),
        os.path.join(build_dir, "bootloader.bin"),
    ]
    pt_candidates = [
        os.path.join(build_dir, "partition_table", "partition-table.bin"),
        os.path.join(build_dir, "partition-table.bin"),
    ]
    ota_data = os.path.join(build_dir, "ota_data_initial.bin")

    has_bootloader = False
    for cand in bl_candidates:
        if os.path.isfile(cand):
            shutil.copyfile(cand, os.path.join(bundle_dir, "bootloader.bin"))
            has_bootloader = True
            break

    has_pt = False
    for cand in pt_candidates:
        if os.path.isfile(cand):
            shutil.copyfile(cand, os.path.join(bundle_dir, "partition-table.bin"))
            has_pt = True
            break

    has_ota_data = False
    if os.path.isfile(ota_data):
        shutil.copyfile(ota_data, os.path.join(bundle_dir, "ota_data_initial.bin"))
        has_ota_data = True

    target = payload.get("target", "esp32")
    hardware = payload.get("hardware", {})
    defaults = payload.get("defaults", {})
    size_bytes = os.path.getsize(dist_bin)
    sha256_hex = compute_sha256(dist_bin)

    meta = {
        "name": profile_name,
        "artifact": f"{profile_name}.bin",
        "target": target,
        "board": hardware.get("board", "auto"),
        "flash_size": hardware.get("flash_size", "4MB"),
        "spiram": bool(hardware.get("spiram", False)),
        "fw_version": defaults.get("fw_version", "1.1.0-unified"),
        "features": payload.get("features", {}),
        "size": size_bytes,
        "sha256": sha256_hex,
        "bl_offset": "0x0" if target in ["esp32c3", "esp32s3", "esp32c2", "esp8266"] else "0x1000",
        "bundle": {
            "dir": f"bundles/{profile_name}",
            "bootloader": has_bootloader,
            "partition_table": has_pt,
            "ota_data": has_ota_data,
        },
        "build_time": int(time.time()),
    }

    with open(os.path.join(DIST_DIR, f"{profile_name}.meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)
    with open(os.path.join(bundle_dir, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)

    return meta


def load_artifact_meta(file_name):
    base = file_name[:-4] if file_name.endswith(".bin") else file_name
    meta_path = os.path.join(DIST_DIR, f"{base}.meta.json")
    if os.path.isfile(meta_path):
        with open(meta_path, "r", encoding="utf-8") as f:
            return json.load(f)
    return None


def build_profile(profile_path_or_name, ota_ip=None, clean=False):
    data, full_path = load_profile(profile_path_or_name)
    name = data.get("name", os.path.basename(full_path).replace(".yaml", ""))
    target = data.get("target", "esp32")

    print("===========================================================================")
    print(f"🚀 LuatOS-Style Builder: Profile [{name}] (Target: {target})")
    print(f"   Desc: {data.get('description', 'Custom build')}")
    print("===========================================================================")

    if target == "esp8266":
        pio_dir = os.path.join(PROJECT_ROOT, "esp8266_port")
        pio_exe = os.path.join(os.path.expanduser("~"), ".platformio", "penv", "Scripts", "pio.exe")
        if not os.path.isfile(pio_exe):
            pio_exe = "pio"
        features = data.get("features", {})
        defaults = data.get("defaults", {})
        flags = [
            f'-DFEATURE_WEB_UI={1 if features.get("web_ui", True) else 0}',
            f'-DFEATURE_REST_API={1 if features.get("rest_api", True) else 0}',
            f'-DFEATURE_GPIO_CTRL={1 if features.get("gpio_ctrl", True) else 0}',
            f'-DFEATURE_OTA={1 if features.get("ota", True) else 0}',
            f'-DFEATURE_FLEET_AGENT={1 if features.get("fleet_agent", True) else 0}',
            f'-DDEFAULT_WIFI_SSID=\\"{defaults.get("wifi_ssid", "1")}\\"',
            f'-DDEFAULT_WIFI_PASS=\\"{defaults.get("wifi_pass", "11111111")}\\"',
        ]
        if defaults.get("mdns_host"):
            flags.append(f'-DDEFAULT_MDNS_HOST=\\"{defaults["mdns_host"]}\\"')
        hub_url = defaults.get("hub_url")
        if not hub_url:
            try:
                import mdns_advertiser
                hub_url = f"http://{mdns_advertiser.get_lan_ip()}:8088"
            except Exception:
                hub_url = ""
        if hub_url:
            flags.append(f'-DDEFAULT_HUB_URL=\\"{hub_url}\\"')
        env = os.environ.copy()
        env["PLATFORMIO_BUILD_FLAGS"] = " ".join(flags)
        res = subprocess.run([pio_exe, "run", "-d", pio_dir], env=env)
        if res.returncode != 0:
            print("❌ ESP8266 PlatformIO build failed!")
            return False
        built_bin = os.path.join(pio_dir, ".pio", "build", "esp8266_unified", "firmware.bin")
        if not os.path.isfile(built_bin):
            print(f"❌ Missing built firmware: {built_bin}")
            return False
        meta = archive_build_artifacts(name, data, built_bin)
        print(f"\n🎉 Bespoke ESP8266 firmware saved: {name}.bin ({meta['size']/1024:.1f} KB, sha256={meta['sha256'][:12]}...)")
        return True

    cfg_content = generate_sdkconfig_defaults(data)
    sdkconfig_defaults_path = os.path.join(PROJECT_ROOT, "sdkconfig.defaults")
    with open(sdkconfig_defaults_path, "w", encoding="utf-8") as f:
        f.write(cfg_content)
    print("✅ Generated sdkconfig.defaults with custom component selection.")

    clean_stale_sdkconfig()

    pipeline_path = os.path.join(PROJECT_ROOT, "pipeline.yaml")
    pipeline_data = {
        "name": f"esp_unified_{name}",
        "target": target,
        "flash": {
            "port": "COM11",
            "baud": 921600,
            "auto_flash": False,
        },
    }
    with open(pipeline_path, "w", encoding="utf-8") as f:
        yaml.dump(pipeline_data, f)

    cmd = ["idf-build", "-t", target]
    if clean:
        cmd.append("--clean")
    print(f"🔨 Invoking build pipeline: {' '.join(cmd)}...")
    res = subprocess.run(cmd, cwd=PROJECT_ROOT, shell=True)
    if res.returncode != 0:
        print("❌ Build failed!")
        return False

    built_bin = os.path.join(PROJECT_ROOT, "build", "esp_unified_framework.bin")
    if not os.path.isfile(built_bin):
        built_bin = os.path.join(PROJECT_ROOT, "build", f"esp_unified_{name}.bin")

    if os.path.isfile(built_bin):
        meta = archive_build_artifacts(name, data, built_bin)
        print(f"\n🎉 Bespoke firmware saved: {name}.bin ({meta['size']/1024:.1f} KB, sha256={meta['sha256'][:12]}...)")
    else:
        print(f"⚠️ Built binary not found at {built_bin}")
        return False

    if ota_ip:
        dist_bin = os.path.join(DIST_DIR, f"{name}.bin")
        print(f"\n📡 Pushing firmware via wireless OTA to http://{ota_ip}/api/ota/upload...")
        import urllib.request
        try:
            with open(dist_bin, "rb") as f:
                payload_bytes = f.read()
            req = urllib.request.Request(
                f"http://{ota_ip}/api/ota/upload",
                data=payload_bytes,
                headers={
                    "Content-Type": "application/octet-stream",
                    "Content-Length": str(len(payload_bytes)),
                    "X-Firmware-SHA256": meta["sha256"],
                },
            )
            with urllib.request.urlopen(req, timeout=60) as resp:
                print(f"🎉 OTA Success! Device is rebooting into profile [{name}]! ({resp.read().decode('utf-8', 'replace')})")
        except Exception as e:
            print(f"❌ OTA push failed: {e}")

    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="LuatOS-Style Profile Builder for ESP Unified Framework")
    parser.add_argument("-p", "--profile", required=True, help="Profile YAML file or name in profiles/")
    parser.add_argument("--ota", help="Target IP address for immediate wireless OTA upgrade")
    parser.add_argument("--clean", action="store_true", help="Clean remote build before compiling")
    args = parser.parse_args()

    success = build_profile(args.profile, ota_ip=args.ota, clean=args.clean)
    sys.exit(0 if success else 1)
