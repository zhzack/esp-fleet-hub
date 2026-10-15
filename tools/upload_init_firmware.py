import io
import json
import urllib.request

login_data = json.dumps({'username': 'admin', 'password': 'admin123'}).encode()
req = urllib.request.Request(
    'http://127.0.0.1:8000/api/v1/auth/login',
    data=login_data,
    headers={'Content-Type': 'application/json'},
)
token = json.loads(urllib.request.urlopen(req).read())['access_token']

boundary = '----WebKitFormBoundary7MA4YWxkTrZu0gW'
body = []

def add_field(name, value):
    body.extend([
        f'--{boundary}'.encode(),
        f'Content-Disposition: form-data; name="{name}"'.encode(),
        b'',
        value.encode('utf-8') if isinstance(value, str) else value,
    ])

add_field('version', 'v1.2.1-ota')
add_field('description', '内置双分区 OTA 远程升级支持与固件校验')
add_field('target_chip', 'esp32s3')

with open('ble_hid/build/ble_hid.bin', 'rb') as f:
    bin_data = f.read()

body.extend([
    f'--{boundary}'.encode(),
    b'Content-Disposition: form-data; name="file"; filename="ble_hid_v1.2.1_ota.bin"',
    b'Content-Type: application/octet-stream',
    b'',
    bin_data,
])
body.extend([f'--{boundary}--'.encode(), b''])

body_bytes = b'\r\n'.join(body)

up_req = urllib.request.Request(
    'http://127.0.0.1:8000/api/v2/firmwares/upload',
    data=body_bytes,
    headers={
        'Authorization': f'Bearer {token}',
        'Content-Type': f'multipart/form-data; boundary={boundary}',
        'Content-Length': str(len(body_bytes)),
    },
)

res = json.loads(urllib.request.urlopen(up_req).read())
print('Upload Result:', res)
