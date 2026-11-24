import urllib.request, os, sys, time

firmware_path = r"g:\code\backend\clouldflare\py_web\esp_unified_framework\build\esp_unified_framework.bin"
url = "http://192.168.68.178/api/ota/upload"

file_size = os.path.getsize(firmware_path)
print(f">>> [Wireless OTA] Starting OTA upload: {firmware_path} ({file_size / 1024:.1f} KB) -> {url}")

with open(firmware_path, "rb") as f:
    data = f.read()

req = urllib.request.Request(url, data=data, headers={
    "Content-Type": "application/octet-stream",
    "Content-Length": str(file_size)
})

t0 = time.time()
try:
    with urllib.request.urlopen(req, timeout=30) as resp:
        body = resp.read().decode('utf-8', errors='replace')
        elapsed = time.time() - t0
        speed = (file_size / 1024) / elapsed
        print(f"\n🎉 [OTA SUCCESS] HTTP {resp.status} in {elapsed:.2f}s ({speed:.1f} KB/s)!")
        print("Response:", body)
except Exception as e:
    print(f"\n❌ [OTA ERROR]: {e}")
    sys.exit(1)
