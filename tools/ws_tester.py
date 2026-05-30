"""ESP32 ble_hid 固件 WS 协议测试服务器。

不依赖 FastAPI / Vue 前端 / 数据库，纯本机一个进程：
  - 监听 0.0.0.0:8000，路径 /ws/device
  - 等待 ESP32 上线后发 register
  - 按顺序执行测试用例：每条命令带预期结果验证
  - 实时打印 PASS / FAIL / SKIP 及具体 diff

用法：
    pip install websockets
    python tools/ws_tester.py

可选：
    python tools/ws_tester.py --port 9000
    python tools/ws_tester.py --skip ble.scan,wifi.scan,reboot
    python tools/ws_tester.py --only get_status,wifi.status

设备连过来后即开始执行；执行完后会保持连接以观察心跳，按 Ctrl-C 退出。
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import sys
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Awaitable

import websockets

# Windows 控制台 UTF-8
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass


# ---------- session / framing ----------

@dataclass
class Session:
    ws: Any
    device_id: str = ""
    pending: dict[str, asyncio.Future] = field(default_factory=dict)
    heartbeats: int = 0

    async def send_envelope(self, type_: str, payload: dict | None = None,
                            ref_msg_id: str | None = None) -> str:
        msg_id = str(uuid.uuid4())
        env = {
            "header": {
                "msg_id": msg_id,
                "type": type_,
                "device_id": self.device_id,
                "timestamp": int(time.time()),
                "source": "tester",
            },
            "routing": {"target_devices": [self.device_id], "broadcast": False, "group": None},
            "payload": payload or {},
        }
        if ref_msg_id:
            env["header"]["ref_msg_id"] = ref_msg_id
        await self.ws.send(json.dumps(env))
        return msg_id

    async def request(self, type_: str, payload: dict | None = None, timeout: float = 8.0) -> dict:
        """Send envelope, wait for ACK referencing it. Returns full ACK envelope."""
        msg_id = await self.send_envelope(type_, payload)
        fut: asyncio.Future = asyncio.get_event_loop().create_future()
        self.pending[msg_id] = fut
        try:
            return await asyncio.wait_for(fut, timeout=timeout)
        finally:
            self.pending.pop(msg_id, None)


# ---------- test framework ----------

PASS, FAIL, SKIP = "\x1b[32mPASS\x1b[0m", "\x1b[31mFAIL\x1b[0m", "\x1b[33mSKIP\x1b[0m"

@dataclass
class TestResult:
    name: str
    status: str    # "pass" | "fail" | "skip"
    detail: str = ""

TestFn = Callable[[Session], Awaitable[None]]
TESTS: list[tuple[str, TestFn]] = []

def test(name: str):
    def deco(fn: TestFn) -> TestFn:
        TESTS.append((name, fn))
        return fn
    return deco

class AssertionFail(Exception):
    pass

def expect(cond: bool, msg: str):
    if not cond:
        raise AssertionFail(msg)

def get(d: dict, *keys, default=None):
    for k in keys:
        if not isinstance(d, dict) or k not in d:
            return default
        d = d[k]
    return d


# ---------- tests ----------
# 每个测试只关心：发送 → 拿 ACK → 校验关键字段。
# 不试图比对底层完整 JSON，避免被无关字段差异搞挂。

@test("get_status")
async def t_get_status(s: Session):
    ack = await s.request("get_status")
    pl = ack["payload"]
    expect(pl.get("ok") is True, f"ok!=true: {pl}")
    r = pl.get("result", {})
    expect(r.get("device_id") == s.device_id, f"device_id mismatch: {r}")
    expect("fw" in r,        f"missing fw: {r}")
    expect("uptime_ms" in r, f"missing uptime_ms: {r}")
    expect("wifi" in r,      f"missing wifi: {r}")
    expect("ble"  in r,      f"missing ble: {r}")
    expect("usb"  in r,      f"missing usb: {r}")
    expect(r.get("transport") in ("ble", "usb"), f"bad transport: {r}")
    return f"fw={r.get('fw')} transport={r.get('transport')} ble.connected={get(r,'ble','connected')}"


@test("wifi.status")
async def t_wifi_status(s: Session):
    ack = await s.request("wifi.status")
    pl = ack["payload"]
    expect(pl.get("ok") is True, f"ok!=true: {pl}")
    r = pl.get("result", {})
    expect(r.get("connected") is True, f"wifi not connected: {r}")
    expect(r.get("ssid"),  f"empty ssid: {r}")
    expect(r.get("ip"),    f"empty ip: {r}")
    return f"{r.get('ssid')} @ {r.get('ip')} rssi={r.get('rssi')}"


@test("wifi.scan")
async def t_wifi_scan(s: Session):
    ack = await s.request("wifi.scan", timeout=12)
    pl = ack["payload"]
    expect(pl.get("ok") is True, f"ok!=true: {pl}")
    nets = get(pl, "result", "networks") or []
    expect(isinstance(nets, list), f"networks not list: {pl}")
    if nets:
        for n in nets[:5]:
            print(f"      · {n.get('ssid','?'):32s} rssi={n.get('rssi')} auth={n.get('auth')}")
    return f"{len(nets)} APs"


@test("ble.info")
async def t_ble_info(s: Session):
    ack = await s.request("ble.info")
    pl = ack["payload"]
    expect(pl.get("ok") is True, f"ok!=true: {pl}")
    r = pl.get("result", {})
    expect("connected" in r,   f"missing connected: {r}")
    expect("device_name" in r, f"missing device_name: {r}")
    return f"connected={r['connected']} name={r['device_name']} peer={r.get('peer','-')}"


@test("ble.list_bonds")
async def t_ble_bonds(s: Session):
    ack = await s.request("ble.list_bonds")
    pl = ack["payload"]
    expect(pl.get("ok") is True, f"ok!=true: {pl}")
    bonds = get(pl, "result", "bonds") or []
    expect(isinstance(bonds, list), f"bonds not list: {pl}")
    return f"{len(bonds)} bonds"


@test("ble.scan")
async def t_ble_scan(s: Session):
    ack = await s.request("ble.scan", payload={"duration_ms": 4000}, timeout=10)
    pl = ack["payload"]
    expect(pl.get("ok") is True, f"ok!=true: {pl}")
    devs = get(pl, "result", "devices") or []
    expect(isinstance(devs, list), f"devices not list: {pl}")
    for d in devs[:8]:
        print(f"      · {d.get('addr','?'):17s} rssi={d.get('rssi'):>4} name={d.get('name','')!r}")
    return f"{len(devs)} BLE devices"


@test("usb.info")
async def t_usb_info(s: Session):
    ack = await s.request("usb.info")
    pl = ack["payload"]
    expect(pl.get("ok") is True, f"ok!=true: {pl}")
    return f"mounted={get(pl,'result','mounted')}"


@test("transport.get")
async def t_transport_get(s: Session):
    ack = await s.request("transport.get")
    pl = ack["payload"]
    expect(pl.get("ok") is True, f"ok!=true: {pl}")
    r = pl.get("result", {})
    expect(r.get("mode")   in ("auto", "ble", "usb"), f"bad mode: {r}")
    expect(r.get("active") in ("ble", "usb"),         f"bad active: {r}")
    return f"mode={r['mode']} active={r['active']}"


@test("transport.set ble")
async def t_transport_set_ble(s: Session):
    ack = await s.request("transport.set", payload={"mode": "ble"})
    pl = ack["payload"]
    expect(pl.get("ok") is True, f"ok!=true: {pl}")
    # 立即 verify
    g = await s.request("transport.get")
    expect(get(g, "payload", "result", "mode") == "ble", "mode not ble after set")
    return "mode=ble"


@test("transport.set auto")
async def t_transport_set_auto(s: Session):
    ack = await s.request("transport.set", payload={"mode": "auto"})
    expect(ack["payload"].get("ok") is True, f"ok!=true: {ack}")
    return "mode=auto"


@test("action keyboard_tap")
async def t_action_kbd(s: Session):
    ack = await s.request("action", payload={
        "type": "keyboard_tap", "key": "KEY_A"
    })
    # action 回的 ack：ok=true 即 OK；如果 BLE 没连主机，hid_ble_tap 是空操作但 executor 仍 return true
    expect(ack["payload"].get("ok") is True, f"ok!=true: {ack['payload']}")
    return f"exec_time_ms={ack['payload'].get('exec_time_ms')}"


@test("action text")
async def t_action_text(s: Session):
    ack = await s.request("action", payload={
        "type": "text", "content": "hi", "interval_ms": 5
    })
    expect(ack["payload"].get("ok") is True, f"ok!=true: {ack['payload']}")
    return "ok"


@test("ping")
async def t_ping(s: Session):
    """ping 返回的是 type='pong' envelope（不是 ack），单独处理。"""
    fut = asyncio.get_event_loop().create_future()
    def stash(env):
        if get(env, "header", "type") == "pong":
            fut.set_result(env)
    s._pong_hook = stash  # reader 里会读
    msg_id = await s.send_envelope("ping")
    try:
        env = await asyncio.wait_for(fut, timeout=4)
    finally:
        s._pong_hook = None
    expect(get(env, "header", "ref_msg_id") == msg_id, f"ref_msg_id mismatch: {env}")
    return "pong received"


@test("reboot")
async def t_reboot(s: Session):
    """重启后设备会重连，本测试只检查 ACK 收到。"""
    ack = await s.request("reboot", timeout=3)
    expect(ack["payload"].get("ok") is True, f"ok!=true: {ack['payload']}")
    return "device should reboot now"


# ---------- runner ----------

async def reader_loop(s: Session):
    async for raw in s.ws:
        try:
            env = json.loads(raw)
        except json.JSONDecodeError:
            print(f"  ! bad json: {raw[:120]}")
            continue
        h = env.get("header", {}) or {}
        t = h.get("type")
        if t == "register":
            s.device_id = (env.get("payload") or {}).get("device_id") or h.get("device_id") or ""
            print(f"  → register: {s.device_id}  caps={env.get('payload',{}).get('capabilities')}")
            ack_env = {
                "header": {
                    "msg_id": str(uuid.uuid4()),
                    "ref_msg_id": h.get("msg_id"),
                    "type": "ack",
                    "timestamp": int(time.time()),
                },
                "payload": {"status": "ok", "server_session": "tester"},
            }
            await s.ws.send(json.dumps(ack_env))
        elif t == "heartbeat":
            s.heartbeats += 1
        elif t == "ack":
            ref = h.get("ref_msg_id")
            if ref in s.pending:
                s.pending[ref].set_result(env)
        elif t == "pong":
            hook = getattr(s, "_pong_hook", None)
            if hook:
                hook(env)
        elif t == "error":
            ref = h.get("ref_msg_id")
            if ref in s.pending:
                # 当作 ack 处理，让 expect 拿到错误
                s.pending[ref].set_result(env)


async def run_tests(s: Session, only: set[str], skip: set[str]) -> list[TestResult]:
    results: list[TestResult] = []
    for name, fn in TESTS:
        if only and name not in only:
            results.append(TestResult(name, "skip", "filtered"))
            print(f"  {SKIP} {name}")
            continue
        if name in skip:
            results.append(TestResult(name, "skip", "user-skip"))
            print(f"  {SKIP} {name}")
            continue
        t0 = time.time()
        try:
            detail = await fn(s) or ""
            ms = int((time.time() - t0) * 1000)
            results.append(TestResult(name, "pass", detail))
            print(f"  {PASS} {name:24s} [{ms:>4}ms]  {detail}")
        except AssertionFail as e:
            ms = int((time.time() - t0) * 1000)
            results.append(TestResult(name, "fail", str(e)))
            print(f"  {FAIL} {name:24s} [{ms:>4}ms]  {e}")
        except asyncio.TimeoutError:
            ms = int((time.time() - t0) * 1000)
            results.append(TestResult(name, "fail", "timeout"))
            print(f"  {FAIL} {name:24s} [{ms:>4}ms]  timeout")
        except Exception as e:
            ms = int((time.time() - t0) * 1000)
            results.append(TestResult(name, "fail", repr(e)))
            print(f"  {FAIL} {name:24s} [{ms:>4}ms]  {e!r}")
    return results


def summarize(results: list[TestResult]) -> int:
    passed = sum(1 for r in results if r.status == "pass")
    failed = sum(1 for r in results if r.status == "fail")
    skipped = sum(1 for r in results if r.status == "skip")
    print()
    print(f"  total {len(results)}: {passed} passed, {failed} failed, {skipped} skipped")
    if failed:
        print("  failures:")
        for r in results:
            if r.status == "fail":
                print(f"    - {r.name}: {r.detail}")
    return 0 if failed == 0 else 1


async def serve(host: str, port: int, only: set[str], skip: set[str]):
    print(f"== ws_tester listening on ws://{host}:{port}/ws/device ==")
    print(f"   (waiting for ESP32 register; Ctrl-C to quit)")
    print()
    done_evt = asyncio.Event()

    async def handler(ws):
        # websockets >= 14: 新签名只传 ws；path 从 ws.request.path 读
        path = getattr(getattr(ws, "request", None), "path", "/ws/device") or "/ws/device"
        if not path.startswith("/ws/device"):
            await ws.close(code=1008, reason="bad path")
            return
        peer = ws.remote_address
        print(f"-- device connected from {peer[0]}:{peer[1]}")
        s = Session(ws=ws)
        reader = asyncio.create_task(reader_loop(s))
        try:
            # 等 device_id 出现
            for _ in range(50):
                if s.device_id:
                    break
                await asyncio.sleep(0.1)
            if not s.device_id:
                print("  ! never received register; aborting")
                return
            # 跑测试
            results = await run_tests(s, only, skip)
            rc = summarize(results)
            print()
            print(f"  连接保持中（heartbeats={s.heartbeats}），Ctrl-C 退出")
            # 后续保持连接观察
            while True:
                await asyncio.sleep(5)
                print(f"  · still connected, heartbeats so far: {s.heartbeats}")
        except websockets.ConnectionClosed:
            print(f"-- device disconnected from {peer[0]}:{peer[1]}")
        finally:
            reader.cancel()
            done_evt.set()

    async with websockets.serve(handler, host, port, ping_interval=None):
        await done_evt.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--only", default="", help="逗号分隔，仅跑这些测试")
    ap.add_argument("--skip", default="reboot", help="逗号分隔，跳过这些（默认跳 reboot）")
    args = ap.parse_args()

    only = {x.strip() for x in args.only.split(",") if x.strip()}
    skip = {x.strip() for x in args.skip.split(",") if x.strip()}

    # 屏蔽非 WS 客户端（普通 HTTP 探测、FastAPI 残留请求）打印的 traceback
    logging.getLogger("websockets.server").setLevel(logging.CRITICAL)
    logging.getLogger("websockets").setLevel(logging.CRITICAL)

    try:
        asyncio.run(serve(args.host, args.port, only, skip))
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
