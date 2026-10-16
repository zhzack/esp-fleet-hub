import urllib.request
import json
import time

login_data = json.dumps({'username': 'admin', 'password': 'admin123'}).encode()
token = json.loads(urllib.request.urlopen(urllib.request.Request(
    'http://127.0.0.1:8000/api/v1/auth/login',
    data=login_data,
    headers={'Content-Type': 'application/json'}
)).read())['access_token']
headers = {'Authorization': f'Bearer {token}', 'Content-Type': 'application/json'}

push_data = json.dumps({'device_uuids': ['esp32_001']}).encode()
push_res = json.loads(urllib.request.urlopen(urllib.request.Request(
    'http://127.0.0.1:8000/api/v2/firmwares/1/push',
    data=push_data,
    headers=headers
)).read())
print('Push Dispatched:', json.dumps(push_res, indent=2))

for i in range(25):
    time.sleep(1)
    p = json.loads(urllib.request.urlopen(urllib.request.Request(
        'http://127.0.0.1:8000/api/v2/firmwares/progress/esp32_001',
        headers=headers
    )).read())
    print(f'[{i+1}s] Progress: {p.get("progress")}% | Status: {p.get("status")} | Read: {p.get("bytes_read")}/{p.get("total_bytes")} | Target: {p.get("target_partition")}')
    if p.get('status') in ('success', 'failed'):
        print('Terminal state reached:', p.get('status'))
        break
