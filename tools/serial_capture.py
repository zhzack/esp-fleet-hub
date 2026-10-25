"""tools/serial_capture.py - 被动抓取 ESP32 串口日志（配对调试用）

注意：pyserial 打开 COM5 前必须置 dtr=False rts=False，
否则 DTR 电平会把设备带进下载模式（见 PROJECT_HANDOVER.md 运维提示）。
用法：python tools/serial_capture.py [秒数]  （默认持续 300s，Ctrl+C 退出）
"""
import sys
import time

import serial

PORT = "COM5"
BAUD = 115200
DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 300

# 先配置后 open（dtr/rts 不是构造参数）；open 瞬间默认 DTR=RTS=高电平，
# 不是 esptool 的下载模式组合（GPIO0 拉低+复位），随后立即拉低保持被动监听
ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 1
ser.open()
ser.dtr = False
ser.rts = False
print(f"[serial_capture] {PORT}@{BAUD} listening for {DURATION}s (dtr/rts low)")
start = time.time()
buf = b""
try:
    while time.time() - start < DURATION:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                ts = time.strftime("%H:%M:%S")
                print(f"{ts} {text}", flush=True)
except KeyboardInterrupt:
    pass
finally:
    ser.close()
    print("[serial_capture] closed")
