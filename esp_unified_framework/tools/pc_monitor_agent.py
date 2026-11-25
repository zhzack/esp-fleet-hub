#!/usr/bin/env python3
"""
ESP32-C3 Wi-Fi/Hub PC Monitor & Media Remote Agent
--------------------------------------------------
Collects local Windows/Linux CPU, RAM, Disk C:, network speed, uptime, and clock,
pushes metrics to ESP32-C3 /api/monitor/push (directly via IP/mDNS or through Fleet Hub),
and polls /api/joystick/state (`seq`-based) to trigger OS media/volume keys cleanly.

Usage:
  python tools/pc_monitor_agent.py --host 192.168.68.210
  python tools/pc_monitor_agent.py --hub http://127.0.0.1:8000 --hub-token <JWT> --device esp32c3-72f320
"""

import argparse
import ctypes
import json
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime

try:
    import psutil
except ImportError:
    print("Please install psutil first: pip install psutil")
    sys.exit(1)


OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

VK_MAP = {
    "VOL+":     0xAF,  # VK_VOLUME_UP
    "VOL-":     0xAE,  # VK_VOLUME_DOWN
    "PREV":     0xB1,  # VK_MEDIA_PREV_TRACK
    "NEXT":     0xB0,  # VK_MEDIA_NEXT_TRACK
    "PLAY/PAU": 0xB3,  # VK_MEDIA_PLAY_PAUSE
}


def press_media_key(badge: str):
    vk = VK_MAP.get(badge)
    if not vk or sys.platform != "win32":
        print(f"\n[Media] Joystick action -> {badge}")
        return
    try:
        ctypes.windll.user32.keybd_event(vk, 0, 0, 0)
        ctypes.windll.user32.keybd_event(vk, 0, 2, 0)
        print(f"\n[Media] Executed Windows key: {badge} (0x{vk:02X})")
    except Exception as e:
        print(f"\n[Media] Key inject error: {e}")


def format_speed(bps: float) -> str:
    if bps >= 1024 * 1024:
        return f"{bps / (1024 * 1024):.1f}M/s"
    if bps >= 1024:
        return f"{bps / 1024:.0f}K/s"
    return f"{bps:.0f}B/s"


def resolve_device_host(args) -> str:
    if args.host:
        h = args.host.strip()
        return h if h.startswith("http") else f"http://{h}"

    # Query Fleet Hub for online PC Monitor device IP
    hub = args.hub.rstrip("/")
    headers = {}
    if getattr(args, "hub_token", ""):
        headers["Authorization"] = f"Bearer {args.hub_token}"
    req = urllib.request.Request(f"{hub}/api/v2/unified/devices", headers=headers)
    data = json.loads(OPENER.open(req, timeout=4).read().decode())
    for dev in data.get("devices", []):
        caps = dev.get("capabilities") or {}
        if args.device and dev.get("device_id") != args.device:
            continue
        if (args.device or caps.get("pc_monitor", {}).get("enabled")) and dev.get("ip"):
            return f"http://{dev['ip']}:{dev.get('web_port', 80)}"
    raise RuntimeError("No target PC Monitor device found in Fleet Hub")


def main():
    parser = argparse.ArgumentParser(description="ESP32-C3 Wi-Fi PC Monitor & Joystick Media Agent")
    parser.add_argument("--host", default="", help="Direct device IP or base URL (e.g. 192.168.68.210)")
    parser.add_argument("--hub", default="http://127.0.0.1:8000", help="Fleet Hub URL for auto-discovery")
    parser.add_argument("--hub-token", default="", help="Bearer JWT when resolving via Fleet Hub API")
    parser.add_argument("--device", default="", help="Target device_id when resolving via Fleet Hub")
    parser.add_argument("--token", default="", help="Optional API Token (X-API-Key)")
    parser.add_argument("--interval", type=float, default=1.0, help="Push interval seconds")
    args = parser.parse_args()

    base_url = resolve_device_host(args)
    print(f"[+] PC Monitor Agent targeting: {base_url}")

    psutil.cpu_percent(interval=None)
    last_net = psutil.net_io_counters()
    last_t = time.time()
    last_seq = None

    headers_post = {"Content-Type": "text/plain"}
    headers_get = {}
    if args.token:
        headers_post["X-API-Key"] = args.token
        headers_get["X-API-Key"] = args.token

    while True:
        try:
            cpu = psutil.cpu_percent(interval=None)
            ram = psutil.virtual_memory().percent
            try:
                disk = psutil.disk_usage("C:\\" if sys.platform == "win32" else "/").percent
            except Exception:
                disk = 0.0

            now = time.time()
            dt = max(now - last_t, 0.05)
            cur_net = psutil.net_io_counters()
            dl = format_speed(max(cur_net.bytes_recv - last_net.bytes_recv, 0) / dt)
            ul = format_speed(max(cur_net.bytes_sent - last_net.bytes_sent, 0) / dt)
            last_net = cur_net
            last_t = now

            up_s = int(time.time() - psutil.boot_time())
            up_str = f"{up_s // 3600}h{(up_s % 3600) // 60:02d}m"
            t_str = datetime.now().strftime("%H:%M:%S")

            payload = f"C:{cpu:.1f},R:{ram:.1f},D:{disk:.1f},DL:{dl},UL:{ul},UP:{up_str},T:{t_str}"
            req = urllib.request.Request(
                f"{base_url}/api/monitor/push",
                data=payload.encode("utf-8"),
                headers=headers_post,
                method="POST",
            )
            OPENER.open(req, timeout=2.5).read()

            # Poll joystick state delta using monotonic sequence ID
            jreq = urllib.request.Request(f"{base_url}/api/joystick/state", headers=headers_get)
            jst = json.loads(OPENER.open(jreq, timeout=2.5).read().decode())
            seq = jst.get("seq", 0)
            if last_seq is None:
                last_seq = seq
            elif seq != last_seq:
                last_seq = seq
                if jst.get("last_key"):
                    press_media_key(jst["last_key"])

            print(
                f"\r[PC->C3] CPU:{cpu:4.1f}% | RAM:{ram:4.1f}% | Disk:{disk:4.1f}% | "
                f"↓{dl} ↑{ul} | {t_str} (joy_seq={seq})",
                end="", flush=True,
            )
        except KeyboardInterrupt:
            print("\n[+] Exiting PC Monitor Agent.")
            break
        except Exception as e:
            print(f"\n[!] Link warning ({e}), retrying...")
            if not args.host:
                try:
                    base_url = resolve_device_host(args)
                except Exception:
                    pass
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
