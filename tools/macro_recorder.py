#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/macro_recorder.py - PC 键鼠动作录制与宏生成工具 (Ported from web_mouse)

本工具可实时监听并录制用户的物理鼠标移动、点击、滚轮以及键盘按键操作，
自动计算时间间隔 (delay_ms)，并导出为 py_web 规范的宏步骤 (Macro steps) JSON 格式。

核心功能：
1. 快捷热键控制：
   - 按 [F9] 开始 / 暂停录制
   - 按 [F10] 或 [ESC] 停止录制并保存
2. 智能消抖与位移聚合：
   - 过滤微小抖动，按可调节间隔 (默认 20ms) 聚合并采样鼠标轨迹，生成紧凑高效的宏。
3. 导出与直传：
   - 保存为本地 JSON 文件，可随时在 Web 控制台「宏管理」中导入使用；
   - 支持 --upload 参数一键通过 REST API 同步进当前用户的宏库。
4. 纯 Python 标准库 (ctypes User32 Hook)，无需额外 pip 安装。

用法：
    python tools/macro_recorder.py
    python tools/macro_recorder.py --name "Auto_Login_Macro" --output login_macro.json
    python tools/macro_recorder.py --upload --url http://127.0.0.1:8000
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
from datetime import datetime
import json
import logging
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("recorder")

# Windows Virtual Key Codes to HID Usage names
VK_TO_HID: dict[int, str] = {
    0x08: "KEY_BACKSPACE",
    0x09: "KEY_TAB",
    0x0D: "KEY_ENTER",
    0x1B: "KEY_ESC",
    0x20: "KEY_SPACE",
    0x25: "KEY_LEFT",
    0x26: "KEY_UP",
    0x27: "KEY_RIGHT",
    0x28: "KEY_DOWN",
    0x2D: "KEY_INSERT",
    0x2E: "KEY_DELETE",
    0x30: "KEY_0",
    0x31: "KEY_1",
    0x32: "KEY_2",
    0x33: "KEY_3",
    0x34: "KEY_4",
    0x35: "KEY_5",
    0x36: "KEY_6",
    0x37: "KEY_7",
    0x38: "KEY_8",
    0x39: "KEY_9",
    0x41: "KEY_A",
    0x42: "KEY_B",
    0x43: "KEY_C",
    0x44: "KEY_D",
    0x45: "KEY_E",
    0x46: "KEY_F",
    0x47: "KEY_G",
    0x48: "KEY_H",
    0x49: "KEY_I",
    0x4A: "KEY_J",
    0x4B: "KEY_K",
    0x4C: "KEY_L",
    0x4D: "KEY_M",
    0x4E: "KEY_N",
    0x4F: "KEY_O",
    0x50: "KEY_P",
    0x51: "KEY_Q",
    0x52: "KEY_R",
    0x53: "KEY_S",
    0x54: "KEY_T",
    0x55: "KEY_U",
    0x56: "KEY_V",
    0x57: "KEY_W",
    0x58: "KEY_X",
    0x59: "KEY_Y",
    0x5A: "KEY_Z",
    0x70: "KEY_F1",
    0x71: "KEY_F2",
    0x72: "KEY_F3",
    0x73: "KEY_F4",
    0x74: "KEY_F5",
    0x75: "KEY_F6",
    0x76: "KEY_F7",
    0x77: "KEY_F8",
    0x78: "KEY_F9",
    0x79: "KEY_F10",
    0x7A: "KEY_F11",
    0x7B: "KEY_F12",
    0xBD: "KEY_MINUS",
    0xBB: "KEY_EQUAL",
}


class MacroRecorder:
    def __init__(self, sample_interval_ms: int = 24):
        self.sample_interval_ms = sample_interval_ms
        self.recording = False
        self.steps: list[dict] = []
        self.last_event_time = 0.0

        self.last_mouse_x = 0
        self.last_mouse_y = 0
        self.has_prev_pos = False

        self._acc_dx = 0
        self._acc_dy = 0
        self._acc_time = 0.0
        self._lock = threading.Lock()

        # 钩子句柄
        self._kb_hook = None
        self._mouse_hook = None
        self._stop_event = threading.Event()

    def start_recording(self):
        with self._lock:
            self.recording = True
            self.steps = []
            self.last_event_time = time.time()
            self.has_prev_pos = False
            self._acc_dx = 0
            self._acc_dy = 0
        print("\n" + "=" * 50)
        print("🔴 RECORDING STARTED... (按 [F9] 暂停/继续，按 [F10] 结束并保存)")
        print("=" * 50, flush=True)

    def pause_or_resume(self):
        with self._lock:
            self.recording = not self.recording
            state = "🔴 RESUMED" if self.recording else "⏸️ PAUSED"
            self.last_event_time = time.time()
        print(f"[*] Recording state: {state}", flush=True)

    def stop_recording(self):
        self._flush_mouse_move()
        with self._lock:
            self.recording = False
        print("\n" + "=" * 50)
        print(f"⏹️ RECORDING STOPPED. 累计录制了 {len(self.steps)} 个动作步骤。")
        print("=" * 50, flush=True)

    def record_step(self, step: dict):
        if not self.recording:
            return

        self._flush_mouse_move()

        now = time.time()
        elapsed_ms = int((now - self.last_event_time) * 1000)
        self.last_event_time = now

        step["delay_ms"] = max(10, min(5000, elapsed_ms))
        with self._lock:
            self.steps.append(step)
            print(f"  -> [{len(self.steps):03d}] {step['type']} (delay {step['delay_ms']}ms)")

    def record_mouse_move_pos(self, x: int, y: int):
        if not self.recording:
            return

        with self._lock:
            if not self.has_prev_pos:
                self.last_mouse_x = x
                self.last_mouse_y = y
                self.has_prev_pos = True
                self._acc_time = time.time()
                return

            dx = x - self.last_mouse_x
            dy = y - self.last_mouse_y
            self.last_mouse_x = x
            self.last_mouse_y = y

            self._acc_dx += dx
            self._acc_dy += dy

            now = time.time()
            if (now - self._acc_time) * 1000.0 >= self.sample_interval_ms:
                self._flush_mouse_move_locked(now)

    def _flush_mouse_move(self):
        with self._lock:
            self._flush_mouse_move_locked(time.time())

    def _flush_mouse_move_locked(self, now: float):
        if self._acc_dx != 0 or self._acc_dy != 0:
            elapsed_ms = int((now - self.last_event_time) * 1000)
            self.last_event_time = now
            cx = max(-127, min(127, self._acc_dx))
            cy = max(-127, min(127, self._acc_dy))
            step = {
                "type": "mouse_move",
                "dx": cx,
                "dy": cy,
                "delay_ms": max(8, min(1000, elapsed_ms)),
            }
            self.steps.append(step)
            self._acc_dx = 0
            self._acc_dy = 0
            self._acc_time = now

    def to_macro_dict(self, name: str, description: str = "") -> dict:
        return {
            "name": name,
            "description": description or f"Recorded at {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            "content_json": {
                "steps": self.steps,
                "config": {
                    "execution_mode": "ble_targeted",
                    "action_timeout_ms": 10000,
                },
            },
        }


# ---------------- Windows 钩子线程 ----------------
def run_hooks(recorder: MacroRecorder, on_stop_callback):
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32

    WH_KEYBOARD_LL = 13
    WH_MOUSE_LL = 14

    class POINT(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

    class MSLLHOOKSTRUCT(ctypes.Structure):
        _fields_ = [
            ("pt", POINT),
            ("mouseData", ctypes.c_uint),
            ("flags", ctypes.c_uint),
            ("time", ctypes.c_uint),
            ("dwExtraInfo", ctypes.c_void_p),
        ]

    class KBDLLHOOKSTRUCT(ctypes.Structure):
        _fields_ = [
            ("vkCode", wt.DWORD),
            ("scanCode", wt.DWORD),
            ("flags", wt.DWORD),
            ("time", wt.DWORD),
            ("dwExtraInfo", ctypes.c_void_p),
        ]

    HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, ctypes.c_int, wt.WPARAM, wt.LPARAM)

    def kb_proc(nCode, wParam, lParam):
        if nCode >= 0 and lParam:
            kb = KBDLLHOOKSTRUCT.from_address(lParam)
            vk = kb.vkCode
            is_down = wParam in (0x0100, 0x0104)

            # 控制热键
            if is_down and vk == 0x78:  # F9: 开始 / 暂停
                if not recorder.recording:
                    recorder.start_recording()
                else:
                    recorder.pause_or_resume()
                return 0

            if is_down and vk in (0x79, 0x1B):  # F10 or ESC: 停止并保存
                if recorder.recording:
                    recorder.stop_recording()
                    on_stop_callback()
                return 0

            # 正常录制按键
            if recorder.recording and is_down:
                hid_key = VK_TO_HID.get(vk)
                if hid_key:
                    recorder.record_step({"type": "keyboard_tap", "key": hid_key})

        return user32.CallNextHookEx(None, nCode, wParam, lParam)

    def mouse_proc(nCode, wParam, lParam):
        if nCode >= 0 and lParam and recorder.recording:
            ms = MSLLHOOKSTRUCT.from_address(lParam)
            # WM_MOUSEMOVE = 0x0200
            if wParam == 0x0200:
                recorder.record_mouse_move_pos(ms.pt.x, ms.pt.y)
            # WM_LBUTTONDOWN = 0x0201
            elif wParam == 0x0201:
                recorder.record_step({"type": "mouse_click", "button": 1})
            # WM_RBUTTONDOWN = 0x0204
            elif wParam == 0x0204:
                recorder.record_step({"type": "mouse_click", "button": 2})
            # WM_MBUTTONDOWN = 0x0207
            elif wParam == 0x0207:
                recorder.record_step({"type": "mouse_click", "button": 4})
            # WM_MOUSEWHEEL = 0x020A
            elif wParam == 0x020A:
                val = ctypes.c_short(ms.mouseData >> 16).value
                step = val // 120
                recorder.record_step({"type": "mouse_scroll", "x": 0, "y": step})

        return user32.CallNextHookEx(None, nCode, wParam, lParam)

    c_kb_proc = HOOKPROC(kb_proc)
    c_ms_proc = HOOKPROC(mouse_proc)
    hinst = kernel32.GetModuleHandleW(None)

    kb_hook = user32.SetWindowsHookExW(WH_KEYBOARD_LL, c_kb_proc, hinst, 0)
    ms_hook = user32.SetWindowsHookExW(WH_MOUSE_LL, c_ms_proc, hinst, 0)

    if not kb_hook or not ms_hook:
        logger.error(f"Failed to install hooks: {ctypes.GetLastError()}")
        return

    msg = wt.MSG()
    while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))

    user32.UnhookWindowsHookEx(kb_hook)
    user32.UnhookWindowsHookEx(ms_hook)


def upload_macro(base_url: str, token: str, macro_data: dict):
    url = f"{base_url.rstrip('/')}/api/v1/macros"
    data = json.dumps(macro_data).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"

    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            res = json.loads(resp.read().decode("utf-8"))
            print(f"✅ 成功将宏上传至网关！ID={res.get('id', 'unknown')}")
    except Exception as e:
        logger.error(f"上传宏失败: {e}")


def main():
    parser = argparse.ArgumentParser(description="PC 键鼠动作录制与宏生成工具 (Ported from web_mouse)")
    parser.add_argument("--name", default="", help="生成的宏名称 (默认自动带时间戳)")
    parser.add_argument("--output", default="", help="保存的本地 JSON 文件路径")
    parser.add_argument("--upload", action="store_true", help="录制完成后直接上传至 py_web 网关")
    parser.add_argument("--url", default="http://127.0.0.1:8000", help="FastAPI 网关地址")
    parser.add_argument("--username", default="alice", help="登录用户名")
    parser.add_argument("--password", default="secret123", help="登录密码")
    args = parser.parse_args()

    macro_name = args.name or f"Macro_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    recorder = MacroRecorder()

    print("\n" + "=" * 65)
    print("🎬 HID Gateway - PC 键鼠动作录制器 (Input Recorder)")
    print("=" * 65)
    print(f"  宏名称:     {macro_name}")
    print(f"  快捷操作:   [F9]  -> 开始 / 暂停录制")
    print(f"              [F10] 或 [ESC] -> 停止并保存")
    print("=" * 65)
    print("[*] 录制器已就绪！请切换至目标界面，按 [F9] 开始录制。\n")

    def on_stop():
        macro_dict = recorder.to_macro_dict(macro_name)
        out_path = args.output or f"recorded_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(macro_dict, f, ensure_ascii=False, indent=2)
        print(f"\n💾 宏定义文件已保存至: {os.path.abspath(out_path)}")

        if args.upload:
            print("[*] 正在登录并上传宏...")
            from tools.pc_passthrough import login
            token = login(args.url, args.username, args.password)
            upload_macro(args.url, token, macro_dict)

        print("[*] 退出程序。")
        ctypes.windll.user32.PostQuitMessage(0)

    t = threading.Thread(target=run_hooks, args=(recorder, on_stop), daemon=True)
    t.start()

    try:
        while t.is_alive():
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n[*] 用户中止。")


if __name__ == "__main__":
    main()
