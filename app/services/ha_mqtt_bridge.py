"""Home Assistant MQTT Auto-Discovery bridge for unified multi-chip ESP devices & PC control."""

from __future__ import annotations

import asyncio
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
from typing import Any

import paho.mqtt.client as mqtt
from sqlalchemy.orm import Session

from app.core.config.config import settings
from app.core.logger.logger import logger
from app.core.wifi_crypto import decrypt_field, encrypt_field
from app.database.crud import device as crud_device
from app.database.crud import macro as crud_macro
from app.database.models.device import Device
from app.database.models.ha_integration import HaBrokerConfig, HaDeviceTarget
from app.database.session import SessionLocal
from app.services import unified_bridge as ub
from app.services.capabilities import DISPLAY_CAPS, GPIO_CAPS, HID_CAPS, normalize_caps
from app.services.device_executor import execute_device_command, execute_gpio_control
from app.services.device_registry import device_registry

_CN_NUM = {"零": 0, "一": 1, "二": 2, "两": 2, "三": 3, "四": 4, "五": 5, "六": 6, "七": 7, "八": 8, "九": 9}
_HOST_RE = re.compile(r"^[A-Za-z0-9.\-:]+$")


def sanitize_node_id(device_id: str) -> str:
    return re.sub(r"[^a-zA-Z0-9_-]", "_", str(device_id or "").strip()) or "esp_node"


def send_wol(mac: str, broadcast_ip: str = "255.255.255.255", port: int = 9) -> bool:
    """Broadcast a Wake-on-LAN magic packet for a 6-byte Ethernet MAC."""
    clean_mac = str(mac or "").replace(":", "").replace("-", "").replace(".", "").strip()
    if len(clean_mac) != 12:
        return False
    try:
        magic = bytes.fromhex("FF" * 6 + clean_mac * 16)
    except ValueError:
        return False

    targets = []
    bcast = (broadcast_ip or "").strip() or "255.255.255.255"
    targets.append((bcast, int(port or 9)))
    if bcast != "255.255.255.255":
        targets.append(("255.255.255.255", int(port or 9)))

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            for addr in targets:
                sock.sendto(magic, addr)
        return True
    except OSError as exc:
        logger.warning(f"[ha-mqtt] WOL send failed for {mac}: {exc}")
        return False


def _parse_cn_digits(token: str) -> int | None:
    if not token:
        return None
    if token.isdigit():
        return int(token)
    val, tmp, seen = 0, 0, False
    for ch in token:
        if ch in _CN_NUM:
            tmp = _CN_NUM[ch]
            seen = True
        elif ch == "十":
            seen = True
            val += (tmp or 1) * 10
            tmp = 0
        elif ch == "百":
            seen = True
            val += tmp * 100
            tmp = 0
    val += tmp
    return val if seen else None


def parse_cn_time_text(text: str) -> int | None:
    """Parse Chinese/English natural-language shutdown delay (in minutes, 0 = cancel)."""
    clean = str(text or "").strip()
    if not clean:
        return None
    if any(k in clean for k in ("取消", "别关", "不用关", "cancel")):
        return 0
    if "半小时" in clean:
        return 30

    mh = re.search(r"([0-9一两二三四五六七八九十]+)\s*(?:个)?(?:小时|hr|hour)", clean, re.IGNORECASE)
    if mh:
        h = _parse_cn_digits(mh.group(1))
        if h is not None and h > 0:
            return h * 60

    mm = re.search(r"([0-9一两二三四五六七八九十百]+)\s*(?:分钟|min)", clean, re.IGNORECASE)
    if mm:
        m = _parse_cn_digits(mm.group(1))
        if m is not None and m > 0:
            return m

    if clean.isdigit():
        return max(0, int(clean))
    if "定时" in clean and any(k in clean for k in ("关机", "关电脑")):
        return 30
    return None


def ping_host(ip: str, timeout_s: float = 1.0) -> bool:
    """Safely check if a target LAN IP responds to a single ICMP echo."""
    host = str(ip or "").strip()
    if not host or not _HOST_RE.match(host):
        return False
    if os.name == "nt":
        cmd = ["ping", "-n", "1", "-w", str(max(200, int(timeout_s * 1000))), host]
    else:
        cmd = ["ping", "-c", "1", "-W", str(max(1, int(round(timeout_s)))), host]
    try:
        res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout_s + 1.5)
        return res.returncode == 0
    except Exception:
        return False


def build_shutdown_action(os_kind: str = "windows") -> dict[str, Any]:
    """Build hardware HID shutdown action sequence for the target desktop OS."""
    kind = str(os_kind or "windows").strip().lower()
    if kind in ("mac", "macos", "darwin", "linux"):
        return {"type": "consumer", "key": "KEY_POWER", "need_ack": True}
    return {
        "type": "macro",
        "need_ack": True,
        "steps": [
            {"type": "keyboard_down", "key": "KEY_LEFTGUI"},
            {"type": "keyboard_tap", "key": "KEY_X"},
            {"type": "keyboard_up", "key": "KEY_LEFTGUI"},
            {"type": "delay", "delay_ms": 300},
            {"type": "keyboard_tap", "key": "KEY_U"},
            {"type": "delay", "delay_ms": 200},
            {"type": "keyboard_tap", "key": "KEY_U"},
        ],
    }


HID_SHORTCUT_ACTIONS: dict[str, tuple[str, str, list[dict[str, Any]]]] = {
    "lock": ("电脑锁屏", "mdi:lock", [{"type": "keyboard_tap", "key": "KEY_GUI_L"}]),
    "desktop": ("显示桌面", "mdi:monitor-dashboard", [{"type": "keyboard_tap", "key": "KEY_GUI_D"}]),
    "mute": ("电脑静音", "mdi:volume-off", [{"type": "consumer", "key": "KEY_MUTE"}]),
    "volup": ("音量加", "mdi:volume-high", [{"type": "consumer", "key": "KEY_VOL_UP"}] * 3),
    "voldown": ("音量减", "mdi:volume-medium", [{"type": "consumer", "key": "KEY_VOL_DOWN"}] * 3),
    "playpause": ("播放暂停", "mdi:play-pause", [{"type": "consumer", "key": "KEY_PLAY_PAUSE"}]),
    "next_track": ("下一曲", "mdi:skip-next", [{"type": "consumer", "key": "KEY_NEXT_TRACK"}]),
    "prev_track": ("上一曲", "mdi:skip-previous", [{"type": "consumer", "key": "KEY_PREV_TRACK"}]),
}


def load_broker_config(db: Session, *, include_secret: bool = False) -> dict[str, Any]:
    """Load effective MQTT Broker configuration (DB overrides initial env settings)."""
    row = db.query(HaBrokerConfig).filter(HaBrokerConfig.id == 1).first()
    if row is None:
        raw_pass = settings.HA_MQTT_PASS or ""
        return {
            "enabled": bool(settings.HA_MQTT_ENABLED),
            "broker_host": settings.HA_MQTT_HOST or "",
            "broker_port": int(settings.HA_MQTT_PORT or 1883),
            "mqtt_user": settings.HA_MQTT_USER or "",
            "has_mqtt_pass": bool(raw_pass),
            **({"mqtt_pass": raw_pass} if include_secret else {}),
            "discovery_prefix": (settings.HA_DISCOVERY_PREFIX or "homeassistant").strip("/"),
            "topic_prefix": (settings.HA_TOPIC_PREFIX or "esp_hub").strip("/"),
            "sync_interval_sec": max(3, min(300, int(settings.HA_SYNC_INTERVAL_SEC or 10))),
            "updated_at": 0,
        }
    plain_pass = decrypt_field(row.mqtt_pass) if row.mqtt_pass else ""
    return {
        "enabled": bool(row.enabled),
        "broker_host": row.broker_host or "",
        "broker_port": int(row.broker_port or 1883),
        "mqtt_user": row.mqtt_user or "",
        "has_mqtt_pass": bool(plain_pass),
        **({"mqtt_pass": plain_pass} if include_secret else {}),
        "discovery_prefix": (row.discovery_prefix or "homeassistant").strip("/"),
        "topic_prefix": (row.topic_prefix or "esp_hub").strip("/"),
        "sync_interval_sec": max(3, min(300, int(row.sync_interval_sec or 10))),
        "updated_at": int(row.updated_at or 0),
    }


def save_broker_config(db: Session, patch: dict[str, Any]) -> dict[str, Any]:
    row = db.query(HaBrokerConfig).filter(HaBrokerConfig.id == 1).first()
    if row is None:
        cur = load_broker_config(db, include_secret=True)
        row = HaBrokerConfig(
            id=1,
            enabled=cur["enabled"],
            broker_host=cur["broker_host"],
            broker_port=cur["broker_port"],
            mqtt_user=cur["mqtt_user"],
            mqtt_pass=encrypt_field(cur["mqtt_pass"]) if cur["mqtt_pass"] else "",
            discovery_prefix=cur["discovery_prefix"],
            topic_prefix=cur["topic_prefix"],
            sync_interval_sec=cur["sync_interval_sec"],
            updated_at=int(time.time()),
        )
        db.add(row)

    if "enabled" in patch and patch["enabled"] is not None:
        row.enabled = bool(patch["enabled"])
    if "broker_host" in patch and patch["broker_host"] is not None:
        row.broker_host = str(patch["broker_host"]).strip()
    if "broker_port" in patch and patch["broker_port"] is not None:
        row.broker_port = max(1, min(65535, int(patch["broker_port"])))
    if "mqtt_user" in patch and patch["mqtt_user"] is not None:
        row.mqtt_user = str(patch["mqtt_user"]).strip()
    if "mqtt_pass" in patch and patch["mqtt_pass"] is not None:
        p = str(patch["mqtt_pass"])
        row.mqtt_pass = encrypt_field(p) if p else ""
    if "discovery_prefix" in patch and patch["discovery_prefix"] is not None:
        row.discovery_prefix = str(patch["discovery_prefix"]).strip("/") or "homeassistant"
    if "topic_prefix" in patch and patch["topic_prefix"] is not None:
        row.topic_prefix = str(patch["topic_prefix"]).strip("/") or "esp_hub"
    if "sync_interval_sec" in patch and patch["sync_interval_sec"] is not None:
        row.sync_interval_sec = max(3, min(300, int(patch["sync_interval_sec"])))
    row.updated_at = int(time.time())
    db.commit()
    return load_broker_config(db, include_secret=False)


def serialize_target(row: HaDeviceTarget | None, device_uuid: str) -> dict[str, Any]:
    if row is None:
        return {
            "device_uuid": device_uuid,
            "enabled": True,
            "display_name": "",
            "area_name": "",
            "expose_hid": True,
            "expose_gpio": True,
            "expose_display": True,
            "expose_camera": True,
            "pc_enabled": False,
            "pc_name": "",
            "pc_ip": "",
            "pc_mac": "",
            "pc_broadcast_ip": "255.255.255.255",
            "pc_power_pin": None,
            "pc_power_pulse_ms": 500,
            "pc_os": "windows",
            "legacy_topic_prefix": "",
            "exposed_pins": [],
            "updated_at": 0,
        }
    return {
        "device_uuid": row.device_uuid,
        "enabled": bool(row.enabled),
        "display_name": row.display_name or "",
        "area_name": row.area_name or "",
        "expose_hid": bool(row.expose_hid),
        "expose_gpio": bool(row.expose_gpio),
        "expose_display": bool(row.expose_display),
        "expose_camera": bool(row.expose_camera),
        "pc_enabled": bool(row.pc_enabled),
        "pc_name": row.pc_name or "",
        "pc_ip": row.pc_ip or "",
        "pc_mac": row.pc_mac or "",
        "pc_broadcast_ip": row.pc_broadcast_ip or "255.255.255.255",
        "pc_power_pin": row.pc_power_pin,
        "pc_power_pulse_ms": int(row.pc_power_pulse_ms or 500),
        "pc_os": row.pc_os or "windows",
        "legacy_topic_prefix": (row.legacy_topic_prefix or "").strip("/"),
        "exposed_pins": list(row.exposed_pins or []),
        "updated_at": int(row.updated_at or 0),
    }


def upsert_device_target(db: Session, device_uuid: str, patch: dict[str, Any]) -> dict[str, Any]:
    row = db.query(HaDeviceTarget).filter(HaDeviceTarget.device_uuid == device_uuid).first()
    if row is None:
        row = HaDeviceTarget(device_uuid=device_uuid)
        db.add(row)
    for bool_key in ("enabled", "expose_hid", "expose_gpio", "expose_display", "expose_camera", "pc_enabled"):
        if bool_key in patch and patch[bool_key] is not None:
            setattr(row, bool_key, bool(patch[bool_key]))
    for str_key in ("display_name", "area_name", "pc_name", "pc_ip", "pc_mac", "pc_broadcast_ip", "pc_os", "legacy_topic_prefix"):
        if str_key in patch and patch[str_key] is not None:
            val = str(patch[str_key]).strip()
            if str_key == "legacy_topic_prefix":
                val = val.strip("/")
            setattr(row, str_key, val)
    if "pc_power_pin" in patch:
        row.pc_power_pin = patch["pc_power_pin"]
    if "pc_power_pulse_ms" in patch and patch["pc_power_pulse_ms"] is not None:
        row.pc_power_pulse_ms = max(100, min(5000, int(patch["pc_power_pulse_ms"])))
    if "exposed_pins" in patch and patch["exposed_pins"] is not None:
        row.exposed_pins = list(patch["exposed_pins"])
    row.updated_at = int(time.time())
    db.commit()
    return serialize_target(row, device_uuid)


def extract_device_caps_set(main_caps: list[str] | None, unified_caps: dict | list | None) -> set[str]:
    out = normalize_caps(main_caps)
    if isinstance(unified_caps, dict):
        for k, v in unified_caps.items():
            if isinstance(v, dict) and v.get("enabled"):
                out.add(str(k).strip().lower())
                if k == "usb_otg":
                    out.update(("usb_keyboard", "usb_mouse"))
            elif isinstance(v, bool) and v:
                out.add(str(k).strip().lower())
    elif isinstance(unified_caps, list):
        out.update(normalize_caps(unified_caps))
    return out


def build_device_discovery_specs(
    *,
    device_uuid: str,
    device_name: str,
    chip: str = "",
    fw_version: str = "",
    caps: set[str],
    target: dict[str, Any],
    topic_prefix: str = "esp_hub",
    discovery_prefix: str = "homeassistant",
    gpio_pins: list[dict[str, Any]] | None = None,
    macros: list[dict[str, Any]] | None = None,
) -> list[tuple[str, dict[str, Any]]]:
    """Generate Home Assistant MQTT Auto-Discovery (topic, config_dict) entries for one device."""
    if not target.get("enabled", True):
        return []

    nid = sanitize_node_id(device_uuid)
    base = f"{topic_prefix.strip('/')}/dev/{nid}"
    avail_topic = f"{base}/availability"
    dev_label = (target.get("display_name") or target.get("pc_name") or device_name or device_uuid).strip()

    device_block: dict[str, Any] = {
        "identifiers": [f"esp_hub_{nid}"],
        "name": dev_label,
        "model": chip or "ESP Unified Node",
        "manufacturer": "ESP Fleet Hub",
        "sw_version": fw_version or "unified",
    }
    if target.get("area_name"):
        device_block["suggested_area"] = target["area_name"]

    specs: list[tuple[str, dict[str, Any]]] = []

    def add_ent(component: str, obj_id: str, cfg: dict[str, Any]) -> None:
        payload = {
            **cfg,
            "unique_id": f"esphub_{nid}_{obj_id}",
            "availability_topic": avail_topic,
            "device": device_block,
        }
        specs.append((f"{discovery_prefix.strip('/')}/{component}/{nid}/{obj_id}/config", payload))

    # 1. Core diagnostics & reboot
    add_ent("sensor", "rssi", {
        "name": "Wi-Fi 信号",
        "state_topic": f"{base}/sensor/rssi",
        "device_class": "signal_strength",
        "unit_of_measurement": "dBm",
        "entity_category": "diagnostic",
        "icon": "mdi:wifi",
    })
    add_ent("sensor", "free_heap", {
        "name": "可用内存",
        "state_topic": f"{base}/sensor/free_heap",
        "unit_of_measurement": "KB",
        "entity_category": "diagnostic",
        "icon": "mdi:memory",
    })
    add_ent("sensor", "ip", {
        "name": "局域网 IP",
        "state_topic": f"{base}/sensor/ip",
        "entity_category": "diagnostic",
        "icon": "mdi:ip-network-outline",
    })
    add_ent("button", "reboot", {
        "name": "重启设备",
        "command_topic": f"{base}/button/reboot/set",
        "entity_category": "config",
        "icon": "mdi:restart",
    })

    # 2. GPIO entities (explicit configured pins or safe auto-discovered pins)
    if target.get("expose_gpio", True) and (caps & GPIO_CAPS):
        pins_src = target.get("exposed_pins") or gpio_pins or []
        for p in pins_src[:8]:
            if not isinstance(p, dict) or p.get("pin") is None:
                continue
            pin_num = int(p["pin"])
            pname = str(p.get("name") or p.get("label") or f"GPIO {pin_num}").strip()
            is_pwm = bool(p.get("pwm") or p.get("kind") == "light")
            if is_pwm:
                add_ent("light", f"gpio_{pin_num}", {
                    "name": pname,
                    "command_topic": f"{base}/gpio/{pin_num}/set",
                    "state_topic": f"{base}/gpio/{pin_num}/state",
                    "brightness_command_topic": f"{base}/gpio/{pin_num}/brightness/set",
                    "brightness_state_topic": f"{base}/gpio/{pin_num}/brightness/state",
                    "brightness_scale": 100,
                    "on_command_type": "brightness",
                })
            else:
                add_ent("switch", f"gpio_{pin_num}", {
                    "name": pname,
                    "command_topic": f"{base}/gpio/{pin_num}/set",
                    "state_topic": f"{base}/gpio/{pin_num}/state",
                    "payload_on": "ON",
                    "payload_off": "OFF",
                    "icon": "mdi:toggle-switch-outline",
                })

    # 3. Display / E-Ink / PC Monitor
    if target.get("expose_display", True) and (caps & DISPLAY_CAPS):
        add_ent("text", "display_text", {
            "name": "屏幕显示内容",
            "command_topic": f"{base}/display/text/set",
            "state_topic": f"{base}/display/text/state",
            "max": 120,
            "icon": "mdi:message-draw",
        })
        add_ent("select", "display_mode", {
            "name": "屏幕显示模式",
            "command_topic": f"{base}/display/mode/set",
            "state_topic": f"{base}/display/mode/state",
            "options": ["monitor", "text", "clock", "image"],
            "icon": "mdi:television-guide",
        })

    # 4. Camera / Flash
    if target.get("expose_camera", True) and "camera" in caps:
        add_ent("switch", "camera_flash", {
            "name": "摄像头补光灯",
            "command_topic": f"{base}/camera/flash/set",
            "state_topic": f"{base}/camera/flash/state",
            "payload_on": "ON",
            "payload_off": "OFF",
            "icon": "mdi:flashlight",
        })
        add_ent("button", "camera_capture", {
            "name": "立即抓拍画面",
            "command_topic": f"{base}/camera/capture/set",
            "icon": "mdi:camera",
        })
        add_ent("camera", "snapshot", {
            "name": "实时抓拍画面",
            "topic": f"{base}/camera/image",
        })

    # 5. HID Shortcuts & Desktop PC Control + Shutdown Countdown
    if target.get("expose_hid", True) and (caps & HID_CAPS):
        for btn_key, (btn_label, btn_icon, _steps) in HID_SHORTCUT_ACTIONS.items():
            add_ent("button", btn_key, {
                "name": btn_label,
                "command_topic": f"{base}/button/{btn_key}/set",
                "icon": btn_icon,
            })

    if target.get("pc_enabled"):
        add_ent("switch", "pc_power", {
            "name": target.get("pc_name") or "台式电脑电源",
            "command_topic": f"{base}/switch/pc_power/set",
            "state_topic": f"{base}/switch/pc_power/state",
            "payload_on": "ON",
            "payload_off": "OFF",
            "state_on": "ON",
            "state_off": "OFF",
            "icon": "mdi:desktop-classic",
        })
        if target.get("pc_mac"):
            add_ent("button", "wol", {
                "name": "网络唤醒",
                "command_topic": f"{base}/button/wol/set",
                "icon": "mdi:wake-on-lan",
            })
        if target.get("pc_power_pin") is not None:
            add_ent("button", "power_pulse", {
                "name": "主板电源按键脉冲",
                "command_topic": f"{base}/button/power_pulse/set",
                "icon": "mdi:power",
            })
        for tbtn, tname, ticon in (
            ("timer_15", "15分钟后关机", "mdi:timer-outline"),
            ("timer_30", "30分钟后关机", "mdi:timer-outline"),
            ("timer_60", "60分钟后关机", "mdi:timer-sand"),
            ("timer_cancel", "取消定时关机", "mdi:timer-off-outline"),
        ):
            add_ent("button", tbtn, {
                "name": tname,
                "command_topic": f"{base}/button/{tbtn}/set",
                "icon": ticon,
            })
        add_ent("number", "shutdown_timer", {
            "name": "定时关机倒计时",
            "command_topic": f"{base}/number/timer/set",
            "state_topic": f"{base}/number/timer/state",
            "min": 0,
            "max": 240,
            "step": 5,
            "unit_of_measurement": "min",
            "mode": "slider",
            "icon": "mdi:timer-cog-outline",
        })
        add_ent("sensor", "shutdown_timer_status", {
            "name": "定时关机状态",
            "state_topic": f"{base}/sensor/timer/state",
            "json_attributes_topic": f"{base}/sensor/timer/attributes",
            "icon": "mdi:clock-alert-outline",
        })
        add_ent("text", "shutdown_voice_parse", {
            "name": "语音定时指令",
            "command_topic": f"{base}/timer/parse_text",
            "state_topic": f"{base}/sensor/timer/state",
            "max": 100,
            "icon": "mdi:microphone-message",
        })

    # 6. User saved Macros
    for m in (macros or [])[:12]:
        mid = m.get("id")
        mname = str(m.get("name") or f"宏 #{mid}").strip()
        if mid is not None:
            add_ent("button", f"macro_{mid}", {
                "name": f"宏: {mname}",
                "command_topic": f"{base}/button/macro_{mid}/set",
                "icon": "mdi:script-text-play-outline",
            })

    return specs


def generate_xiaoai_automations_yaml(device_uuid: str, target: dict[str, Any], topic_prefix: str = "esp_hub") -> str:
    """Render ready-to-paste Home Assistant XiaoAI voice automation YAML for a configured node."""
    nid = sanitize_node_id(device_uuid)
    base = f"{topic_prefix.strip('/')}/dev/{nid}"
    slug = re.sub(r"[^a-z0-9_]+", "_", nid.lower())
    lines = [
        f"# Home Assistant 小爱音箱 / 语音自动化示例 (节点: {device_uuid})",
        f"# 请将 sensor.xiaomi_oh2p_681c_conversation 替换为你家小爱音箱的对话文本实体 ID",
        f"- id: xiaoai_{slug}_power_on",
        f"  alias: 小爱-{nid}-打开电脑",
        "  trigger:",
        "    - platform: state",
        "      entity_id: sensor.xiaomi_oh2p_681c_conversation",
        "  condition:",
        "    - condition: template",
        "      value_template: \"{{ '打开电脑' in trigger.to_state.state or '开机' in trigger.to_state.state }}\"",
        "  action:",
        "    - action: mqtt.publish",
        "      data:",
        f"        topic: \"{base}/switch/pc_power/set\"",
        "        payload: \"ON\"",
        "  mode: restart",
        "",
        f"- id: xiaoai_{slug}_timed_shutdown",
        f"  alias: 小爱-{nid}-定时或取消关机",
        "  trigger:",
        "    - platform: state",
        "      entity_id: sensor.xiaomi_oh2p_681c_conversation",
        "  condition:",
        "    - condition: template",
        "      value_template: \"{{ '关机' in trigger.to_state.state or '关电脑' in trigger.to_state.state or '取消定时' in trigger.to_state.state }}\"",
        "  action:",
        "    - action: mqtt.publish",
        "      data:",
        f"        topic: \"{base}/timer/parse_text\"",
        "        payload: \"{{ trigger.to_state.state }}\"",
        "  mode: restart",
        "",
        f"- id: xiaoai_{slug}_lock_mute",
        f"  alias: 小爱-{nid}-锁屏与静音",
        "  trigger:",
        "    - platform: state",
        "      entity_id: sensor.xiaomi_oh2p_681c_conversation",
        "  condition:",
        "    - condition: template",
        "      value_template: \"{{ '锁屏' in trigger.to_state.state or '静音' in trigger.to_state.state }}\"",
        "  action:",
        "    - action: mqtt.publish",
        "      data:",
        f"        topic: \"{base}/button/{{% if '锁屏' in trigger.to_state.state %}}lock{{% else %}}mute{{% endif %}}/set\"",
        "        payload: \"PRESS\"",
        "  mode: restart",
    ]
    return "\n".join(lines) + "\n"


def test_mqtt_connection(host: str, port: int = 1883, username: str = "", password: str = "", timeout_s: float = 4.0) -> tuple[bool, str]:
    """Verify live TCP + MQTT CONNACK against a Broker without keeping persistent state."""
    clean_host = str(host or "").strip()
    if not clean_host:
        return False, "Broker host is empty"
    ev = threading.Event()
    res: dict[str, Any] = {"ok": False, "msg": "connection timed out"}

    def _on_conn(_client, _ud, _flags, reason_code, *_args):
        rc_val = int(getattr(reason_code, "value", reason_code)) if not isinstance(reason_code, int) else reason_code
        if rc_val == 0:
            res["ok"] = True
            res["msg"] = f"Connected to {clean_host}:{port} (CONNACK rc=0)"
        else:
            res["msg"] = f"Broker rejected connection (rc={rc_val})"
        ev.set()

    try:
        client = mqtt.Client(
            client_id=f"esp_hub_probe_{int(time.time() * 1000) % 100000}",
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        )
    except AttributeError:
        client = mqtt.Client(client_id=f"esp_hub_probe_{int(time.time() * 1000) % 100000}")

    if username:
        client.username_pw_set(username, password or None)
    client.on_connect = _on_conn
    try:
        client.connect_async(clean_host, int(port), 15)
        client.loop_start()
        ev.wait(timeout=max(1.0, min(10.0, timeout_s)))
        client.disconnect()
        client.loop_stop()
    except Exception as exc:
        try:
            client.loop_stop()
        except Exception:
            pass
        return False, str(exc)
    return res["ok"], res["msg"]


class HaMqttBridge:
    """Async/thread-safe Home Assistant MQTT Auto-Discovery bridge embedded inside py_web."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._client: mqtt.Client | None = None
        self._connected = False
        self._running = False
        self._loop: asyncio.AbstractEventLoop | None = None
        self._task: asyncio.Task | None = None
        self._wake_event: asyncio.Event | None = None

        self._config: dict[str, Any] = {
            "enabled": False,
            "broker_host": "",
            "broker_port": 1883,
            "mqtt_user": "",
            "mqtt_pass": "",
            "discovery_prefix": "homeassistant",
            "topic_prefix": "esp_hub",
            "sync_interval_sec": 10,
        }
        self._last_error = ""
        self._last_sync_at = 0
        self._published_entities = 0
        self._published_devices = 0

        # Active topic dispatch maps rebuilt on sync
        self._nid_to_uuid: dict[str, str] = {}
        self._legacy_map: dict[str, str] = {}
        self._shutdown_deadlines: dict[str, float] = {}
        self._last_pc_power: dict[str, str] = {}
        self._gpio_cache: dict[str, list[dict[str, Any]]] = {}

    def get_status(self) -> dict[str, Any]:
        with self._lock:
            return {
                "enabled": bool(self._config.get("enabled")),
                "running": self._running,
                "connected": self._connected,
                "broker_host": self._config.get("broker_host", ""),
                "broker_port": int(self._config.get("broker_port", 1883)),
                "discovery_prefix": self._config.get("discovery_prefix", "homeassistant"),
                "topic_prefix": self._config.get("topic_prefix", "esp_hub"),
                "published_devices": self._published_devices,
                "published_entities": self._published_entities,
                "active_shutdown_timers": {
                    did: max(0, int(dl - time.time()))
                    for did, dl in self._shutdown_deadlines.items()
                    if dl > time.time()
                },
                "last_sync_at": self._last_sync_at,
                "last_error": self._last_error,
            }

    async def start(self) -> None:
        if self._task is not None and not self._task.done():
            return
        self._loop = asyncio.get_running_loop()
        self._wake_event = asyncio.Event()
        self._running = True
        self._reload_config_sync()
        self._task = asyncio.create_task(self._run_loop())
        logger.info("🏠 Home Assistant MQTT bridge manager initialized")

    async def stop(self) -> None:
        self._running = False
        if self._wake_event is not None:
            self._wake_event.set()
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None
        self._disconnect_client()

    def request_reload(self) -> None:
        self._reload_config_sync()
        self._disconnect_client()
        if self._loop and self._wake_event:
            self._loop.call_soon_threadsafe(self._wake_event.set)

    def request_sync(self) -> None:
        if self._loop and self._wake_event:
            self._loop.call_soon_threadsafe(self._wake_event.set)

    def _reload_config_sync(self) -> None:
        with SessionLocal() as db:
            cfg = load_broker_config(db, include_secret=True)
        with self._lock:
            self._config = cfg

    def _disconnect_client(self) -> None:
        with self._lock:
            cli = self._client
            self._client = None
            self._connected = False
        if cli is not None:
            try:
                tp = self._config.get("topic_prefix", "esp_hub")
                cli.publish(f"{tp}/bridge/status", "offline", retain=True)
                cli.disconnect()
            except Exception:
                pass
            try:
                cli.loop_stop()
            except Exception:
                pass

    def _connect_client(self) -> bool:
        with self._lock:
            cfg = dict(self._config)
        if not cfg.get("enabled") or not cfg.get("broker_host"):
            return False
        if self._client is not None:
            return self._connected

        tp = cfg.get("topic_prefix") or "esp_hub"
        cid = f"esp_fleet_hub_{os.getpid()}"
        try:
            client = mqtt.Client(client_id=cid, callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
        except AttributeError:
            client = mqtt.Client(client_id=cid)

        if cfg.get("mqtt_user"):
            client.username_pw_set(cfg["mqtt_user"], cfg.get("mqtt_pass") or None)
        client.will_set(f"{tp}/bridge/status", "offline", qos=1, retain=True)
        client.on_connect = self._on_connect
        client.on_disconnect = self._on_disconnect
        client.on_message = self._on_message

        try:
            client.connect_async(cfg["broker_host"], int(cfg["broker_port"]), 30)
            client.loop_start()
            with self._lock:
                self._client = client
                self._last_error = ""
            return True
        except Exception as exc:
            with self._lock:
                self._last_error = str(exc)
            logger.warning(f"[ha-mqtt] connect failed: {exc}")
            return False

    def _on_connect(self, client, _ud, _flags, reason_code, *_extra) -> None:
        rc_val = int(getattr(reason_code, "value", reason_code)) if not isinstance(reason_code, int) else reason_code
        if rc_val != 0:
            with self._lock:
                self._connected = False
                self._last_error = f"MQTT CONNACK rc={rc_val}"
            logger.warning(f"[ha-mqtt] broker rejected connect rc={rc_val}")
            return

        tp = self._config.get("topic_prefix") or "esp_hub"
        with self._lock:
            self._connected = True
            self._last_error = ""
        client.publish(f"{tp}/bridge/status", "online", qos=1, retain=True)
        client.subscribe(f"{tp}/dev/#", qos=1)
        for leg_prefix in set(self._legacy_map.keys()):
            client.subscribe(f"{leg_prefix}/#", qos=1)
        logger.info(f"🏠 [ha-mqtt] Connected to {self._config['broker_host']}:{self._config['broker_port']}")
        self.request_sync()

    def _on_disconnect(self, *_args) -> None:
        with self._lock:
            self._connected = False

    def _on_message(self, _client, _ud, msg) -> None:
        topic = str(msg.topic or "")
        payload = msg.payload.decode("utf-8", errors="ignore").strip()
        if self._loop and self._running:
            asyncio.run_coroutine_threadsafe(self.handle_mqtt_message(topic, payload), self._loop)

    def _publish(self, topic: str, payload: Any, *, retain: bool = False, raw: bool = False) -> None:
        with self._lock:
            cli = self._client
            ok = self._connected
        if cli is None or not ok:
            return
        if raw:
            out = payload
        elif isinstance(payload, (dict, list)):
            out = json.dumps(payload, ensure_ascii=False)
        else:
            out = str(payload)
        cli.publish(topic, out, qos=1, retain=retain)

    async def sync_all_devices(self, db: Session) -> dict[str, int]:
        """Publish HA discovery payloads, availability, telemetry, PC ping & timer states."""
        cfg = load_broker_config(db)
        tp = cfg["topic_prefix"]
        dp = cfg["discovery_prefix"]

        ub.hub_db.ensure_init()
        unified_map = {d["device_id"]: d for d in ub.hub_db.list_devices()}
        main_rows = {d.device_uuid: d for d in db.query(Device).all()}
        target_rows = {t.device_uuid: t for t in db.query(HaDeviceTarget).all()}
        all_ids = sorted(set(unified_map.keys()) | set(main_rows.keys()))

        nid_map: dict[str, str] = {}
        legacy_map: dict[str, str] = {}
        dev_count = 0
        ent_count = 0

        for did in all_ids:
            u_dev = unified_map.get(did) or {}
            m_dev = main_rows.get(did)
            tgt = serialize_target(target_rows.get(did), did)
            nid = sanitize_node_id(did)
            nid_map[nid] = did
            if tgt.get("legacy_topic_prefix") and tgt.get("enabled"):
                legacy_map[tgt["legacy_topic_prefix"]] = did

            base = f"{tp}/dev/{nid}"
            ws_on = device_registry.is_online(did)
            online = bool(ws_on or u_dev.get("status") == "online" or (m_dev and m_dev.status == "online"))
            self._publish(f"{base}/availability", "online" if online else "offline", retain=True)
            if not tgt.get("enabled", True):
                continue

            caps = extract_device_caps_set(
                getattr(m_dev, "capabilities", None) if m_dev else None,
                u_dev.get("capabilities"),
            )

            # Query live GPIO states when online so HA state stays accurate
            gpio_pins = self._gpio_cache.get(did, [])
            if (caps & GPIO_CAPS) and tgt.get("expose_gpio", True) and online:
                try:
                    g_res = await execute_device_command(
                        db, user_id=None, device_uuid=did, cmd_type="gpio.pins", payload={}, timeout_ms=2500
                    )
                    if g_res.get("status") == "acked" and isinstance((g_res.get("result") or {}).get("pins"), list):
                        gpio_pins = g_res["result"]["pins"]
                        self._gpio_cache[did] = gpio_pins
                except Exception:
                    pass

            user_macros = []
            if m_dev and m_dev.owner_user_id:
                user_macros = [
                    {"id": m.id, "name": m.name}
                    for m in crud_macro.list_by_user(db, m_dev.owner_user_id)
                ]

            specs = build_device_discovery_specs(
                device_uuid=did,
                device_name=(u_dev.get("name") or (m_dev.name if m_dev else "") or did),
                chip=u_dev.get("chip", ""),
                fw_version=u_dev.get("fw_version", ""),
                caps=caps,
                target=tgt,
                topic_prefix=tp,
                discovery_prefix=dp,
                gpio_pins=gpio_pins,
                macros=user_macros,
            )
            for disc_topic, disc_payload in specs:
                self._publish(disc_topic, disc_payload, retain=True)

            # Telemetry states
            self._publish(f"{base}/sensor/rssi", u_dev.get("rssi", 0), retain=True)
            self._publish(f"{base}/sensor/free_heap", round(int(u_dev.get("free_heap", 0) or 0) / 1024), retain=True)
            self._publish(f"{base}/sensor/ip", u_dev.get("ip", "") or "0.0.0.0", retain=True)

            live_pin_map = {
                int(p["pin"]): p for p in gpio_pins if isinstance(p, dict) and p.get("pin") is not None
            }
            for p in (tgt.get("exposed_pins") or gpio_pins):
                if isinstance(p, dict) and p.get("pin") is not None:
                    pnum = int(p["pin"])
                    live_p = live_pin_map.get(pnum, p)
                    if p.get("pwm") or p.get("kind") == "light" or live_p.get("pwm"):
                        duty = int(live_p.get("duty") or 0)
                        self._publish(f"{base}/gpio/{pnum}/state", "ON" if duty > 0 else "OFF", retain=True)
                        self._publish(f"{base}/gpio/{pnum}/brightness/state", duty, retain=True)
                    else:
                        self._publish(f"{base}/gpio/{pnum}/state", "ON" if live_p.get("val") else "OFF", retain=True)

            if tgt.get("pc_enabled"):
                await self._sync_pc_state(did, tgt, tp)

            dev_count += 1
            ent_count += len(specs)

        # Subscribe any newly added legacy prefixes cleanly
        with self._lock:
            self._nid_to_uuid = nid_map
            new_legs = set(legacy_map.keys()) - set(self._legacy_map.keys())
            self._legacy_map = legacy_map
            self._published_devices = dev_count
            self._published_entities = ent_count
            self._last_sync_at = int(time.time())
            cli = self._client
            conn = self._connected
        if cli and conn:
            for leg in new_legs:
                cli.subscribe(f"{leg}/#", qos=1)
        return {"devices": dev_count, "entities": ent_count}

    async def _sync_pc_state(self, device_uuid: str, tgt: dict[str, Any], tp: str) -> None:
        nid = sanitize_node_id(device_uuid)
        base = f"{tp}/dev/{nid}"
        pc_ip = tgt.get("pc_ip", "")
        if pc_ip:
            is_on = await asyncio.to_thread(ping_host, pc_ip, 1.0)
            state_str = "ON" if is_on else "OFF"
            prev = self._last_pc_power.get(device_uuid)
            self._last_pc_power[device_uuid] = state_str
            self._publish(f"{base}/switch/pc_power/state", state_str, retain=True)
            if tgt.get("legacy_topic_prefix"):
                self._publish(f"{tgt['legacy_topic_prefix']}/switch/power/state", state_str, retain=True)
            if prev == "ON" and state_str == "OFF":
                self._cancel_shutdown_timer(device_uuid, tgt, tp)
        self._publish_timer_state(device_uuid, tgt, tp)

    def _set_shutdown_timer(self, device_uuid: str, minutes: int, tgt: dict[str, Any], tp: str) -> None:
        if minutes <= 0:
            self._cancel_shutdown_timer(device_uuid, tgt, tp)
            return
        self._shutdown_deadlines[device_uuid] = time.time() + (minutes * 60)
        self._publish_timer_state(device_uuid, tgt, tp)

    def _cancel_shutdown_timer(self, device_uuid: str, tgt: dict[str, Any], tp: str) -> None:
        self._shutdown_deadlines.pop(device_uuid, None)
        self._publish_timer_state(device_uuid, tgt, tp)

    def _publish_timer_state(self, device_uuid: str, tgt: dict[str, Any], tp: str) -> None:
        nid = sanitize_node_id(device_uuid)
        base = f"{tp}/dev/{nid}"
        dl = self._shutdown_deadlines.get(device_uuid)
        if dl is None:
            state_txt = "未定时"
            rem_min, rem_sec = 0, 0
            attrs = {"active": False, "target_time": None, "remaining_minutes": 0, "remaining_seconds": 0}
        else:
            rem_sec = max(0, int(dl - time.time()))
            rem_min = (rem_sec + 59) // 60 if rem_sec > 0 else 0
            tstr = time.strftime("%H:%M", time.localtime(dl))
            state_txt = f"将在 {tstr} 关机 (剩余 {rem_min} 分钟)"
            attrs = {"active": True, "target_time": tstr, "remaining_minutes": rem_min, "remaining_seconds": rem_sec}

        self._publish(f"{base}/sensor/timer/state", state_txt, retain=True)
        self._publish(f"{base}/number/timer/state", str(rem_min), retain=True)
        self._publish(f"{base}/sensor/timer/attributes", attrs, retain=True)
        if tgt.get("legacy_topic_prefix"):
            leg = tgt["legacy_topic_prefix"]
            self._publish(f"{leg}/sensor/timer/state", state_txt, retain=True)
            self._publish(f"{leg}/number/timer/state", str(rem_min), retain=True)
            self._publish(f"{leg}/sensor/timer/attributes", attrs, retain=True)

    async def handle_mqtt_message(self, topic: str, payload: str) -> None:
        """Handle an inbound MQTT command from Home Assistant and route to ESP Hub execution."""
        with SessionLocal() as db:
            cfg = load_broker_config(db)
            tp = cfg["topic_prefix"]
            device_uuid = None
            subpath = ""

            prefix = f"{tp}/dev/"
            if topic.startswith(prefix):
                rest = topic[len(prefix):]
                if "/" not in rest:
                    return
                nid, subpath = rest.split("/", 1)
                device_uuid = self._nid_to_uuid.get(nid, nid)
            else:
                # Check legacy topic prefix compatibility (e.g. pc_ctrl/...)
                for leg_pfx, mapped_uuid in list(self._legacy_map.items()):
                    if topic.startswith(leg_pfx + "/"):
                        device_uuid = mapped_uuid
                        subpath = topic[len(leg_pfx) + 1 :]
                        break
            if not device_uuid or not subpath:
                return

            tgt_row = db.query(HaDeviceTarget).filter(HaDeviceTarget.device_uuid == device_uuid).first()
            tgt = serialize_target(tgt_row, device_uuid)
            nid = sanitize_node_id(device_uuid)
            base = f"{tp}/dev/{nid}"
            dev_row = crud_device.get_by_uuid(db, device_uuid)
            owner_uid = dev_row.owner_user_id if dev_row else None

            if subpath == "button/reboot/set":
                await execute_device_command(db, user_id=owner_uid, device_uuid=device_uuid, cmd_type="reboot", payload={}, timeout_ms=2500)
                return

            if subpath == "button/wol/set":
                send_wol(tgt.get("pc_mac", ""), tgt.get("pc_broadcast_ip", "255.255.255.255"))
                return

            if subpath in ("button/power_pulse/set", "button/pulse/set"):
                pin = tgt.get("pc_power_pin")
                if pin is not None:
                    dur_s = max(1, round(int(tgt.get("pc_power_pulse_ms", 500)) / 1000))
                    await execute_gpio_control(db, user_id=owner_uid, device_uuid=device_uuid, pin=int(pin), op="on", duration_sec=dur_s)
                return

            if subpath in ("switch/pc_power/set", "switch/power/set"):
                up = payload.upper()
                if up in ("ON", "1", "TRUE"):
                    if tgt.get("pc_mac"):
                        send_wol(tgt["pc_mac"], tgt.get("pc_broadcast_ip", "255.255.255.255"))
                    await execute_device_command(
                        db,
                        user_id=owner_uid,
                        device_uuid=device_uuid,
                        cmd_type="action",
                        payload={"type": "keyboard_tap", "key": "KEY_SPACE", "need_ack": True},
                        timeout_ms=4000,
                    )
                    self._publish(f"{base}/switch/pc_power/state", "ON", retain=True)
                    if tgt.get("legacy_topic_prefix"):
                        self._publish(f"{tgt['legacy_topic_prefix']}/switch/power/state", "ON", retain=True)
                elif up in ("OFF", "0", "FALSE"):
                    self._cancel_shutdown_timer(device_uuid, tgt, tp)
                    await execute_device_command(
                        db,
                        user_id=owner_uid,
                        device_uuid=device_uuid,
                        cmd_type="action",
                        payload=build_shutdown_action(tgt.get("pc_os", "windows")),
                        timeout_ms=6000,
                    )
                    self._publish(f"{base}/switch/pc_power/state", "OFF", retain=True)
                    if tgt.get("legacy_topic_prefix"):
                        self._publish(f"{tgt['legacy_topic_prefix']}/switch/power/state", "OFF", retain=True)
                return

            if subpath == "number/timer/set":
                try:
                    mins = max(0, min(240, int(float(payload))))
                except ValueError:
                    return
                self._set_shutdown_timer(device_uuid, mins, tgt, tp)
                return

            if subpath == "timer/parse_text":
                mins = parse_cn_time_text(payload)
                if mins is not None:
                    self._set_shutdown_timer(device_uuid, mins, tgt, tp)
                return

            if subpath == "button/timer_15/set":
                self._set_shutdown_timer(device_uuid, 15, tgt, tp)
                return
            if subpath == "button/timer_30/set":
                self._set_shutdown_timer(device_uuid, 30, tgt, tp)
                return
            if subpath == "button/timer_60/set":
                self._set_shutdown_timer(device_uuid, 60, tgt, tp)
                return
            if subpath == "button/timer_cancel/set":
                self._cancel_shutdown_timer(device_uuid, tgt, tp)
                return

            if subpath.startswith("button/") and subpath.endswith("/set"):
                btn_key = subpath[len("button/") : -len("/set")]
                if btn_key in HID_SHORTCUT_ACTIONS:
                    _label, _icon, steps = HID_SHORTCUT_ACTIONS[btn_key]
                    act = steps[0] if len(steps) == 1 else {"type": "macro", "steps": steps}
                    await execute_device_command(
                        db, user_id=owner_uid, device_uuid=device_uuid, cmd_type="action", payload={**act, "need_ack": True}, timeout_ms=5000
                    )
                    return
                if btn_key.startswith("macro_"):
                    try:
                        mid = int(btn_key[len("macro_"):])
                    except ValueError:
                        return
                    macro = crud_macro.get(db, mid)
                    if macro:
                        await execute_device_command(
                            db,
                            user_id=owner_uid,
                            device_uuid=device_uuid,
                            cmd_type="action",
                            payload={"type": "macro", "steps": macro.steps or [], "need_ack": True},
                            timeout_ms=8000,
                        )
                    return

            if subpath.startswith("gpio/") and subpath.endswith("/set"):
                parts = subpath.split("/")
                if len(parts) >= 3 and parts[1].isdigit():
                    pin = int(parts[1])
                    if len(parts) == 4 and parts[2] == "brightness":
                        try:
                            duty = max(0, min(100, int(float(payload))))
                        except ValueError:
                            return
                        res = await execute_gpio_control(db, user_id=owner_uid, device_uuid=device_uuid, pin=pin, duty=duty)
                        if res.get("status") == "acked":
                            self._publish(f"{base}/gpio/{pin}/brightness/state", duty, retain=True)
                            self._publish(f"{base}/gpio/{pin}/state", "ON" if duty > 0 else "OFF", retain=True)
                    else:
                        on = payload.upper() in ("ON", "1", "TRUE")
                        res = await execute_gpio_control(db, user_id=owner_uid, device_uuid=device_uuid, pin=pin, val=1 if on else 0)
                        if res.get("status") == "acked":
                            self._publish(f"{base}/gpio/{pin}/state", "ON" if on else "OFF", retain=True)
                    return

            if subpath == "display/text/set":
                res = await execute_device_command(
                    db,
                    user_id=owner_uid,
                    device_uuid=device_uuid,
                    cmd_type="display.text",
                    payload={"mode": "text", "lines": [line[:40] for line in payload.splitlines()[:6] or [payload[:40]]]},
                    timeout_ms=4000,
                )
                if res.get("status") == "acked":
                    self._publish(f"{base}/display/text/state", payload[:120], retain=True)
                return

            if subpath == "display/mode/set":
                mode = payload.strip().lower()
                if mode in ("monitor", "text", "clock", "image"):
                    res = await execute_device_command(
                        db, user_id=owner_uid, device_uuid=device_uuid, cmd_type="display.config", payload={"mode": mode}, timeout_ms=4000
                    )
                    if res.get("status") == "acked":
                        self._publish(f"{base}/display/mode/state", mode, retain=True)
                return

            if subpath == "camera/flash/set":
                on = payload.upper() in ("ON", "1", "TRUE")
                res = await execute_device_command(
                    db, user_id=owner_uid, device_uuid=device_uuid, cmd_type="camera.flash", payload={"duty": 128 if on else 0}, timeout_ms=4000
                )
                if res.get("status") == "acked":
                    self._publish(f"{base}/camera/flash/state", "ON" if on else "OFF", retain=True)
                return

            if subpath == "camera/capture/set":
                code, raw_or_err = await self._capture_camera_bytes(device_uuid)
                if code == 200 and isinstance(raw_or_err, bytes):
                    self._publish(f"{base}/camera/image", raw_or_err, retain=True, raw=True)

    async def _capture_camera_bytes(self, device_uuid: str) -> tuple[int, bytes | str]:
        if device_registry.is_online(device_uuid):
            import base64
            from app.services import dispatcher

            with SessionLocal() as db:
                res = await dispatcher.deliver_command(db, device_uuid=device_uuid, cmd_type="camera.capture", payload={}, user_id=None, timeout_ms=8000)
            if res.get("status") == "acked" and "image_b64" in (res.get("result") or {}):
                return 200, base64.b64decode(res["result"]["image_b64"])
        dev = ub.hub_db.get_device(device_uuid)
        if dev and dev.get("ip"):
            ok, st, res = await asyncio.to_thread(ub.fleet_router._direct_device_http, dev, "/capture", "GET", None, 6)
            if ok and isinstance(res, tuple):
                return 200, res[0]
            return st or 502, str(res)
        return 404, "device offline"

    async def _tick_shutdown_timers(self) -> None:
        if not self._shutdown_deadlines:
            return
        now = time.time()
        expired = [did for did, dl in list(self._shutdown_deadlines.items()) if now >= dl]
        with SessionLocal() as db:
            cfg = load_broker_config(db)
            tp = cfg["topic_prefix"]
            for did in expired:
                self._shutdown_deadlines.pop(did, None)
                tgt_row = db.query(HaDeviceTarget).filter(HaDeviceTarget.device_uuid == did).first()
                tgt = serialize_target(tgt_row, did)
                self._publish_timer_state(did, tgt, tp)
                dev_row = crud_device.get_by_uuid(db, did)
                owner_uid = dev_row.owner_user_id if dev_row else None
                logger.info(f"[ha-mqtt] Countdown reached 0 for {did}, triggering PC shutdown action")
                await execute_device_command(
                    db,
                    user_id=owner_uid,
                    device_uuid=did,
                    cmd_type="action",
                    payload=build_shutdown_action(tgt.get("pc_os", "windows")),
                    timeout_ms=6000,
                )

            # Periodically refresh active countdown minute display
            for did in list(self._shutdown_deadlines.keys()):
                tgt_row = db.query(HaDeviceTarget).filter(HaDeviceTarget.device_uuid == did).first()
                tgt = serialize_target(tgt_row, did)
                self._publish_timer_state(did, tgt, tp)

    async def _run_loop(self) -> None:
        last_sync = 0.0
        while self._running:
            try:
                with self._lock:
                    cfg = dict(self._config)
                if cfg.get("enabled") and cfg.get("broker_host"):
                    if self._client is None:
                        self._connect_client()
                    now = time.monotonic()
                    interval = max(3.0, float(cfg.get("sync_interval_sec", 10)))
                    if self._connected and (now - last_sync >= interval or (self._wake_event and self._wake_event.is_set())):
                        if self._wake_event:
                            self._wake_event.clear()
                        with SessionLocal() as db:
                            await self.sync_all_devices(db)
                        last_sync = now
                    await self._tick_shutdown_timers()
                elif self._wake_event and self._wake_event.is_set():
                    self._wake_event.clear()

                wait_s = 1.0 if self._shutdown_deadlines else 3.0
                if self._wake_event:
                    try:
                        await asyncio.wait_for(self._wake_event.wait(), timeout=wait_s)
                    except asyncio.TimeoutError:
                        pass
                else:
                    await asyncio.sleep(wait_s)
            except asyncio.CancelledError:
                break
            except Exception as exc:
                logger.warning(f"[ha-mqtt] bridge loop error: {exc}")
                await asyncio.sleep(3.0)


ha_bridge = HaMqttBridge()


async def start_ha_bridge() -> None:
    await ha_bridge.start()


async def stop_ha_bridge() -> None:
    await ha_bridge.stop()
