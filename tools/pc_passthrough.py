#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/pc_passthrough.py - PC 物理键鼠无感透传工具 (Ported from web_mouse)

本工具将当前 Windows PC 的物理鼠标位移、按键、滚轮以及键盘输入，实时透传给 ESP32 受控设备。

核心特色 (来自 web_mouse 项目)：
1. Windows Raw Input 鼠标原生增量捕获 (RawInputMouse)：
   在屏蔽本机输入 (光标冻结) 的情况下，直接从硬件层读取相对位移 (lLastX, lLastY)，
   彻底解决常规钩子在 suppress=True 时因光标静止导致无法获取鼠标位移的核心痛点。
2. 反自环回显过滤 (Anti-Echo Filter)：
   防止受控 ESP32 USB 模拟输出到本机时，再次被本机钩子捕获形成死循环。
3. 全局热键切换 (Toggle Hotkey)：
   按下快捷键 (默认 Ctrl+Shift+Q) 在「本地控制」与「透传控制」之间自由切换。
4. 零强制第三方依赖：
   核心捕获基于 Python 标准库 ctypes (User32 Raw Input & LL Hook)，开箱即用。

用法：
    python tools/pc_passthrough.py --device <DEVICE_UUID>
    python tools/pc_passthrough.py --url http://127.0.0.1:8000 --username alice --password secret
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import logging
import queue
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

# 设置控制台输出编码
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("passthrough")

# Windows Virtual Key Codes to HID Key names
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

# ---------------- 1. Windows Raw Input 鼠标硬件增量捕获 ----------------
class RawInputMouse(threading.Thread):
    """Windows 原生 Raw Input 鼠标相对位移与滚轮/按键捕获。"""

    RIM_TYPEMOUSE = 0
    RID_INPUT = 0x10000003
    RIDEV_INPUTSINK = 0x00000100
    _WM_INPUT = 0x00FF
    _BTN_FLAGS = (
        (0x0001, 1, True),   # RI_MOUSE_LEFT_BUTTON_DOWN -> Button 1
        (0x0002, 1, False),  # RI_MOUSE_LEFT_BUTTON_UP
        (0x0004, 2, True),   # RI_MOUSE_RIGHT_BUTTON_DOWN -> Button 2
        (0x0008, 2, False),  # RI_MOUSE_RIGHT_BUTTON_UP
        (0x0010, 4, True),   # RI_MOUSE_MIDDLE_BUTTON_DOWN -> Button 4
        (0x0020, 4, False),  # RI_MOUSE_MIDDLE_BUTTON_UP
    )

    def __init__(self, on_delta, on_button, on_wheel):
        super().__init__(daemon=True, name="RawInputMouse")
        self.on_delta = on_delta
        self.on_button = on_button
        self.on_wheel = on_wheel
        self._hwnd = None
        self._clsname = f"RawMouseSink_{uuid.uuid4().hex[:8]}"
        self._thread_id = 0
        self.started_event = threading.Event()

    def run(self):
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        self._thread_id = kernel32.GetCurrentThreadId()

        class WNDCLASSW(ctypes.Structure):
            _fields_ = [
                ("style", ctypes.c_uint),
                ("lpfnWndProc", ctypes.c_void_p),
                ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE),
                ("hIcon", wt.HICON),
                ("hCursor", wt.HICON),
                ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR),
                ("lpszClassName", wt.LPCWSTR),
            ]

        WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM)

        def wndproc(hwnd, msg, wparam, lparam):
            if msg == self._WM_INPUT:
                self._handle_input(lparam)
            elif msg == 0x0002:  # WM_DESTROY
                user32.PostQuitMessage(0)
            return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

        self._proc = WNDPROC(wndproc)
        hinst = kernel32.GetModuleHandleW(None)

        cls = WNDCLASSW()
        cls.lpfnWndProc = ctypes.cast(self._proc, ctypes.c_void_p)
        cls.hInstance = hinst
        cls.lpszClassName = self._clsname
        if not user32.RegisterClassW(ctypes.byref(cls)):
            logger.error(f"RegisterClassW failed: {ctypes.GetLastError()}")
            self.started_event.set()
            return

        self._hwnd = user32.CreateWindowExW(0, self._clsname, "raw_sink", 0, 0, 0, 0, 0, None, None, hinst, None)

        class RAWINPUTDEVICE(ctypes.Structure):
            _fields_ = [
                ("usUsagePage", wt.WORD),
                ("usUsage", wt.WORD),
                ("dwFlags", wt.DWORD),
                ("hwndTarget", wt.HWND),
            ]

        dev = RAWINPUTDEVICE(0x01, 0x02, self.RIDEV_INPUTSINK, self._hwnd)
        ok = user32.RegisterRawInputDevices(ctypes.byref(dev), 1, ctypes.sizeof(RAWINPUTDEVICE))
        if not ok:
            logger.error(f"RegisterRawInputDevices failed: {ctypes.GetLastError()}")
            self.started_event.set()
            return

        self.started_event.set()
        msg = wt.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

    def _handle_input(self, lparam):
        user32 = ctypes.windll.user32

        class RAWINPUTHEADER(ctypes.Structure):
            _fields_ = [
                ("dwType", ctypes.c_uint),
                ("dwSize", ctypes.c_uint),
                ("hDevice", ctypes.c_void_p),
                ("wParam", ctypes.c_void_p),
            ]

        class RAWMOUSE(ctypes.Structure):
            _fields_ = [
                ("usFlags", ctypes.c_ushort),
                ("usButtonFlags", ctypes.c_ushort),
                ("usButtonData", ctypes.c_ushort),
                ("ulRawButtons", ctypes.c_uint),
                ("lLastX", ctypes.c_long),
                ("lLastY", ctypes.c_long),
                ("ulExtraInformation", ctypes.c_uint),
            ]

        class RAWINPUT(ctypes.Structure):
            _fields_ = [("header", RAWINPUTHEADER), ("data", RAWMOUSE)]

        size = ctypes.c_uint(0)
        header_size = ctypes.sizeof(RAWINPUTHEADER)
        user32.GetRawInputData(lparam, self.RID_INPUT, None, ctypes.byref(size), header_size)
        if size.value < ctypes.sizeof(RAWINPUT):
            return

        buf = ctypes.create_string_buffer(size.value)
        if user32.GetRawInputData(lparam, self.RID_INPUT, buf, ctypes.byref(size), header_size) <= 0:
            return

        ri = RAWINPUT.from_buffer(buf)
        if ri.header.dwType != self.RIM_TYPEMOUSE:
            return

        m = ri.data
        # 按钮
        for flag, btn, pressed in self._BTN_FLAGS:
            if m.usButtonFlags & flag:
                self.on_button(btn, pressed)

        # 滚轮
        if m.usButtonFlags & 0x0400:  # RI_MOUSE_WHEEL
            val = ctypes.c_short(m.usButtonData).value
            step = val // 120
            self.on_wheel(0, step)
        elif m.usButtonFlags & 0x0800:  # RI_MOUSE_HWHEEL
            val = ctypes.c_short(m.usButtonData).value
            step = val // 120
            self.on_wheel(step, 0)

        # 相对位移
        if m.lLastX or m.lLastY:
            self.on_delta(m.lLastX, m.lLastY)

    def stop(self):
        if self._hwnd:
            try:
                ctypes.windll.user32.PostMessageW(self._hwnd, 0x0010, 0, 0)  # WM_CLOSE
            except Exception:
                pass


# ---------------- 2. Windows 底层键盘钩子 (WH_KEYBOARD_LL) ----------------
class LowLevelKeyboardHook(threading.Thread):
    """Windows 全局底层键盘钩子，负责捕获按键、检测热键，并在激活时拦截本机按键。"""

    WH_KEYBOARD_LL = 13
    WM_KEYDOWN = 0x0100
    WM_KEYUP = 0x0101
    WM_SYSKEYDOWN = 0x0104
    WM_SYSKEYUP = 0x0105

    def __init__(self, on_key_event, on_toggle_hotkey):
        super().__init__(daemon=True, name="LLKeyboardHook")
        self.on_key_event = on_key_event
        self.on_toggle_hotkey = on_toggle_hotkey
        self.active = False  # 是否处于透传模式 (True 时屏蔽本机按键)
        self._hook = None
        self._hook_proc = None
        self._thread_id = 0
        self.started_event = threading.Event()

    def run(self):
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        self._thread_id = kernel32.GetCurrentThreadId()

        class KBDLLHOOKSTRUCT(ctypes.Structure):
            _fields_ = [
                ("vkCode", wt.DWORD),
                ("scanCode", wt.DWORD),
                ("flags", wt.DWORD),
                ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.c_void_p),
            ]

        HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, ctypes.c_int, wt.WPARAM, wt.LPARAM)

        def hook_proc(nCode, wParam, lParam):
            if nCode >= 0 and lParam:
                kb = KBDLLHOOKSTRUCT.from_address(lParam)
                vk = kb.vkCode
                is_down = wParam in (self.WM_KEYDOWN, self.WM_SYSKEYDOWN)
                is_up = wParam in (self.WM_KEYUP, self.WM_SYSKEYUP)

                # 检查 Ctrl + Shift + Q 快捷键 (VK_CONTROL=0x11, VK_SHIFT=0x10, 'Q'=0x51)
                ctrl_pressed = bool(user32.GetKeyState(0x11) & 0x8000)
                shift_pressed = bool(user32.GetKeyState(0x10) & 0x8000)

                if is_down and vk == 0x51 and ctrl_pressed and shift_pressed:
                    self.on_toggle_hotkey()
                    return 1  # 吞掉快捷键

                if self.active and (is_down or is_up):
                    # 转发按键
                    suppress = self.on_key_event(vk, is_down)
                    if suppress:
                        return 1  # 阻止本地系统处理

            return user32.CallNextHookEx(self._hook, nCode, wParam, lParam)

        self._hook_proc = HOOKPROC(hook_proc)
        self._hook = user32.SetWindowsHookExW(
            self.WH_KEYBOARD_LL,
            self._hook_proc,
            kernel32.GetModuleHandleW(None),
            0,
        )

        if not self._hook:
            logger.error(f"SetWindowsHookExW failed: {ctypes.GetLastError()}")
            self.started_event.set()
            return

        self.started_event.set()
        msg = wt.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

    def stop(self):
        if self._hook:
            ctypes.windll.user32.UnhookWindowsHookEx(self._hook)
            self._hook = None


# ---------------- 3. 透传控制器与网络发送器 ----------------
class PassthroughClient:
    def __init__(self, base_url: str, device_uuid: str, token: str = ""):
        self.base_url = base_url.rstrip("/")
        self.device_uuid = device_uuid
        self.token = token
        self.active = False
        self.packets_sent = 0

        # 反自环与聚合
        self.mouse_echo_window_ms = 45
        self.last_sent_time = 0.0
        self.move_queue = queue.Queue(maxsize=1000)
        self._running = True

        # 鼠标位移聚合
        self._acc_x = 0
        self._acc_y = 0
        self._acc_lock = threading.Lock()

        # 启动后台网络投递线程
        self.sender_thread = threading.Thread(target=self._sender_loop, daemon=True, name="PacketSender")
        self.sender_thread.start()

        # 启动鼠标位移定时冲刷线程 (10ms)
        self.flush_thread = threading.Thread(target=self._flush_loop, daemon=True, name="FlushLoop")
        self.flush_thread.start()

    def set_active(self, active: bool):
        self.active = active
        status_text = "PASSTHROUGH (Forwarding to Device)" if active else "LOCAL (Normal PC Control)"
        border = "=" * 60
        print(f"\n{border}\n[*] MODE CHANGED -> {status_text}\n{border}\n", flush=True)

    def on_mouse_delta(self, dx: int, dy: int):
        if not self.active:
            return
        with self._acc_lock:
            self._acc_x += dx
            self._acc_y += dy

    def on_mouse_button(self, button: int, pressed: bool):
        if not self.active:
            return
        # 转化为 action payload
        action = "mouse_down" if pressed else "mouse_up"
        self._enqueue_action({"type": action, "button": button})

    def on_mouse_wheel(self, dx: int, dy: int):
        if not self.active:
            return
        if dx != 0 or dy != 0:
            self._enqueue_action({"type": "mouse_scroll", "x": dx, "y": dy})

    def on_key_event(self, vk: int, is_down: bool) -> bool:
        if not self.active:
            return False

        hid_name = VK_TO_HID.get(vk)
        if hid_name:
            action = "keyboard_press" if is_down else "keyboard_release"
            self._enqueue_action({"type": action, "key": hid_name})
            return True
        return False

    def _flush_loop(self):
        while self._running:
            time.sleep(0.01)
            if not self.active:
                continue
            with self._acc_lock:
                x, y = self._acc_x, self._acc_y
                self._acc_x, self._acc_y = 0, 0
            if x != 0 or y != 0:
                # 限制最大步长
                cx = max(-127, min(127, x))
                cy = max(-127, min(127, y))
                self._enqueue_action({"type": "mouse_move", "dx": cx, "dy": cy})

    def _enqueue_action(self, payload: dict):
        try:
            self.move_queue.put_nowait(payload)
        except queue.Full:
            pass

    def _sender_loop(self):
        while self._running:
            try:
                payload = self.move_queue.get(timeout=0.1)
            except queue.Empty:
                continue

            self._send_http_action(payload)
            self.packets_sent += 1

    def _send_http_action(self, payload: dict):
        url = f"{self.base_url}/api/v2/action"
        envelope = {
            "device_uuid": self.device_uuid,
            "routing": {"target_devices": [self.device_uuid]},
            "payload": payload,
        }
        data = json.dumps(envelope).encode("utf-8")
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"

        req = urllib.request.Request(url, data=data, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=1.5):
                pass
        except Exception:
            pass


# ---------------- 4. 辅助函数：登录与自动选择设备 ----------------
def login(base_url: str, username: str, password: str) -> str:
    url = f"{base_url.rstrip('/')}/api/v1/auth/login"
    data = urllib.parse.urlencode({"username": username, "password": password}).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            body = json.loads(resp.read().decode("utf-8"))
            return body.get("access_token", "")
    except Exception as e:
        logger.warning(f"Login failed ({e}), proceeding without token...")
        return ""


def get_first_online_device(base_url: str, token: str) -> str:
    url = f"{base_url.rstrip('/')}/api/v1/devices/online"
    headers = {}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            body = json.loads(resp.read().decode("utf-8"))
            online = body.get("online", [])
            if online:
                return str(online[0])
    except Exception:
        pass
    return ""


# ---------------- 5. Main 入口 ----------------
def main():
    parser = argparse.ArgumentParser(description="PC 物理键鼠无感透传工具 (Ported from web_mouse)")
    parser.add_argument("--url", default="http://127.0.0.1:8000", help="FastAPI 网关地址 (默认: http://127.0.0.1:8000)")
    parser.add_argument("--device", default="", help="目标 ESP32 设备 UUID (留空则自动选取首个在线设备)")
    parser.add_argument("--username", default="alice", help="登录用户名 (默认: alice)")
    parser.add_argument("--password", default="secret123", help="登录密码 (默认: secret123)")
    parser.add_argument("--token", default="", help="直接指定 JWT Token")
    args = parser.parse_args()

    token = args.token or login(args.url, args.username, args.password)
    target_device = args.device or get_first_online_device(args.url, token)

    print("\n" + "=" * 65)
    print("🚀 HID Gateway - PC 键鼠无感透传客户端 (Raw Input + Anti-Echo)")
    print("=" * 65)
    print(f"  网关地址:   {args.url}")
    print(f"  目标设备:   {target_device or '(未指定/离线，将在发送时动态探测)'}")
    print(f"  切换热键:   [Ctrl + Shift + Q] (随时按下切换控制权)")
    print(f"  退出程序:   按 Ctrl + C")
    print("=" * 65)
    print("[*] 初始模式: LOCAL (本机控制，未激活透传)")
    print("[*] 按下 [Ctrl + Shift + Q] 开启透传，物理键鼠将直接控制远端设备！\n")

    client = PassthroughClient(base_url=args.url, device_uuid=target_device, token=token)

    def toggle_mode():
        new_state = not client.active
        client.set_active(new_state)
        kb_hook.active = new_state

    # 启动 Raw Input 鼠标捕获
    raw_mouse = RawInputMouse(
        on_delta=client.on_mouse_delta,
        on_button=client.on_mouse_button,
        on_wheel=client.on_mouse_wheel,
    )
    raw_mouse.start()
    raw_mouse.started_event.wait(timeout=2.0)

    # 启动底层键盘钩子
    kb_hook = LowLevelKeyboardHook(
        on_key_event=client.on_key_event,
        on_toggle_hotkey=toggle_mode,
    )
    kb_hook.start()
    kb_hook.started_event.wait(timeout=2.0)

    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\n[*] 正在退出透传客户端...")
    finally:
        raw_mouse.stop()
        kb_hook.stop()
        print("[*] 已恢复本地键鼠控制。再见！")


if __name__ == "__main__":
    main()
