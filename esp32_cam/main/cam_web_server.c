#include "cam_web_server.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "camera_driver.h"
#include "esp_ota_ops.h"
#include "log_capture.h"
#include "wifi_mgr.h"
#include "app_config.h"

static const char *TAG = "cam_web";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t s_httpd = NULL;
static char s_device_id[32] = "esp32_cam_001";
static bool s_flash_state = false;

/* Beautiful HTML5 Web UI embedded */
static const char INDEX_HTML[] = 
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-CAM Live Portal</title>"
"<style>"
":root{--bg:#0f172a;--card:#1e293b;--text:#f8fafc;--primary:#3b82f6;--accent:#10b981;--warn:#ef4444;--border:#334155;}"
"*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif}"
"body{background:var(--bg);color:var(--text);padding:16px;display:flex;flex-direction:column;align-items:center}"
".container{width:100%;max-width:800px;display:flex;flex-direction:column;gap:16px}"
".header{background:var(--card);padding:14px 20px;border-radius:12px;border:1px solid var(--border);display:flex;justify-content:space-between;align-items:center}"
".header h1{font-size:1.15rem;display:flex;align-items:center;gap:8px}"
".tag{background:#22c55e22;color:#4ade80;font-size:0.75rem;padding:3px 8px;border-radius:99px;font-weight:600}"
".video-card{background:var(--card);border-radius:12px;border:1px solid var(--border);padding:12px;display:flex;flex-direction:column;align-items:center;position:relative;overflow:hidden}"
"#stream{width:100%;height:auto;border-radius:8px;background:#000;min-height:240px;object-fit:contain;display:block}"
".controls{background:var(--card);border-radius:12px;border:1px solid var(--border);padding:16px;display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px}"
"button,select{background:#334155;color:#fff;border:1px solid #475569;padding:10px 14px;border-radius:8px;font-size:0.9rem;cursor:pointer;transition:all 0.15s;outline:none}"
"button:hover{background:#475569}"
"button.active{background:var(--primary);border-color:var(--primary)}"
"button.flash-on{background:#eab308;color:#000;border-color:#fde047;font-weight:700}"
".card{background:var(--card);border-radius:12px;border:1px solid var(--border);padding:16px;display:flex;flex-direction:column;gap:10px}"
".card h2{font-size:0.95rem;color:#94a3b8;font-weight:600;text-transform:uppercase;letter-spacing:0.05em}"
".grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px;font-size:0.85rem}"
".stat-item{background:#0f172a88;padding:8px 12px;border-radius:6px;display:flex;justify-content:space-between}"
".stat-val{font-weight:600;color:var(--primary)}"
"pre{background:#0b0f19;padding:10px;border-radius:6px;font-size:0.75rem;max-height:160px;overflow-y:auto;color:#a5f3fc;font-family:monospace}"
".progress-bar{width:100%;background:#334155;height:8px;border-radius:4px;overflow:hidden;display:none;margin-top:6px}"
".progress-fill{height:100%;width:0%;background:var(--accent);transition:width 0.2s}"
"</style></head><body>"
"<div class='container'>"
"  <div class='header'>"
"    <h1><span>📷</span> ESP32-CAM HD Streamer</h1>"
"    <span class='tag' id='fps'>LIVE</span>"
"  </div>"
"  <div class='video-card'>"
"    <img id='stream' src='/stream' onerror='this.src=\"\";setTimeout(()=>{this.src=\"/stream\"},2000)'>"
"  </div>"
"  <div class='controls'>"
"    <button id='btn-stream' class='active' onclick='toggleStream()'>⏸ 暂停实时流</button>"
"    <button id='btn-snap' onclick='takeSnap()'>📸 抓拍另存</button>"
"    <button id='btn-flash' onclick='toggleFlash()'>💡 补光灯: 关</button>"
"    <select id='framesize' onchange='setResolution(this.value)'>"
"      <option value='9'>UXGA (1600x1200)</option>"
"      <option value='8'>SXGA (1280x1024)</option>"
"      <option value='7'>XGA (1024x768)</option>"
"      <option value='6'>SVGA (800x600)</option>"
"      <option value='5' selected>VGA (640x480)</option>"
"      <option value='4'>CIF (400x296)</option>"
"      <option value='3'>QVGA (320x240)</option>"
"    </select>"
"  </div>"
"  <div class='card'>"
"    <h2>系统信息</h2>"
"    <div class='grid2'>"
"      <div class='stat-item'><span>IP 地址</span><span class='stat-val' id='ip'>-</span></div>"
"      <div class='stat-item'><span>WiFi 信号</span><span class='stat-val' id='rssi'>-</span></div>"
"      <div class='stat-item'><span>可用 PSRAM</span><span class='stat-val' id='psram'>-</span></div>"
"      <div class='stat-item'><span>运行时间</span><span class='stat-val' id='uptime'>-</span></div>"
"    </div>"
"  </div>"
"  <div class='card'>"
"    <h2>无线 OTA 固件更新</h2>"
"    <div style='display:flex;gap:10px;align-items:center'>"
"      <input type='file' id='ota-file' accept='.bin' style='flex:1;font-size:0.85rem'>"
"      <button onclick='startOTA()' style='background:var(--accent);font-weight:600'>⚡ 上传并升级</button>"
"    </div>"
"    <div class='progress-bar' id='ota-pb'><div class='progress-fill' id='ota-pf'></div></div>"
"    <div id='ota-msg' style='font-size:0.8rem;color:#94a3b8'></div>"
"  </div>"
"  <div class='card'>"
"    <h2>实时运行日志</h2>"
"    <pre id='logs'>加载日志中...</pre>"
"  </div>"
"</div>"
"<script>"
"let streaming=true;"
"function toggleStream(){"
"  const img=document.getElementById('stream');"
"  const btn=document.getElementById('btn-stream');"
"  if(streaming){img.src='';btn.innerText='▶ 恢复实时流';btn.classList.remove('active');streaming=false;}"
"  else{img.src='/stream';btn.innerText='⏸ 暂停实时流';btn.classList.add('active');streaming=true;}"
"}"
"function takeSnap(){window.open('/capture','_blank');}"
"async function toggleFlash(){"
"  const res=await fetch('/api/flash',{method:'POST'});"
"  const j=await res.json();"
"  const btn=document.getElementById('btn-flash');"
"  if(j.flash){btn.innerText='💡 补光灯: 开';btn.classList.add('flash-on');}"
"  else{btn.innerText='💡 补光灯: 关';btn.classList.remove('flash-on');}"
"}"
"async function setResolution(v){await fetch('/api/framesize?size='+v,{method:'POST'});}"
"async function updateStats(){"
"  try{"
"    const r=await fetch('/api/status');const j=await r.json();"
"    if(j.ok&&j.result){"
"      const res=j.result;"
"      document.getElementById('ip').innerText=res.ip||'-';"
"      document.getElementById('rssi').innerText=(res.wifi?res.wifi.rssi+' dBm':'-');"
"      document.getElementById('psram').innerText=(res.free_psram?(res.free_psram/1024/1024).toFixed(2)+' MB':'-');"
"      document.getElementById('uptime').innerText=Math.floor(res.uptime_ms/1000)+' 秒';"
"    }"
"    const lr=await fetch('/api/logs?raw=1');"
"    const lt=await lr.text();"
"    const el=document.getElementById('logs');el.innerText=lt;el.scrollTop=el.scrollHeight;"
"  }catch(e){}"
"}"
"async function startOTA(){"
"  const file=document.getElementById('ota-file').files[0];"
"  if(!file){alert('请先选择固件 .bin 文件');return;}"
"  const pb=document.getElementById('ota-pb');const pf=document.getElementById('ota-pf');const msg=document.getElementById('ota-msg');"
"  pb.style.display='block';msg.innerText='正在上传固件 ('+file.size+' 字节)...';"
"  const xhr=new XMLHttpRequest();"
"  xhr.open('POST','/api/ota/upload');"
"  xhr.upload.onprogress=e=>{if(e.lengthComputable){pf.style.width=(e.loaded/e.total*100)+'%';}};"
"  xhr.onload=()=>{msg.innerText='升级成功！设备正在自动重启...';setTimeout(()=>location.reload(),6000);};"
"  xhr.onerror=()=>{msg.innerText='上传失败，请检查网络或固件。';};"
"  xhr.send(file);"
"}"
"setInterval(updateStats,3000);updateStats();"
"</script></body></html>";

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
}

static esp_err_t h_capture(httpd_req_t *req)
{
    camera_fb_t *fb = camera_driver_get_fb();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    camera_driver_return_fb(fb);
    return res;
}

static esp_err_t h_stream(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char part_buf[128];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        fb = camera_driver_get_fb();
        if (!fb) {
            ESP_LOGE(TAG, "Camera frame capture failed");
            res = ESP_FAIL;
            break;
        }

        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;

        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }

        camera_driver_return_fb(fb);
        fb = NULL;

        if (res != ESP_OK) {
            break; // Client closed connection
        }
    }

    return res;
}

static esp_err_t h_flash_toggle(httpd_req_t *req)
{
    s_flash_state = !s_flash_state;
    camera_driver_set_flash(s_flash_state);

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"flash\":%s}", s_flash_state ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, strlen(buf));
}

static esp_err_t h_framesize(httpd_req_t *req)
{
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {0};
        if (httpd_query_key_value(query, "size", val, sizeof(val)) == ESP_OK) {
            int sz = atoi(val);
            camera_driver_set_framesize((framesize_t)sz);
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "device_id", s_device_id);
    cJSON_AddStringToObject(res, "fw", "esp32-cam-1.0");
    cJSON_AddNumberToObject(res, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(res, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(res, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    char ip_buf[32] = {0};
    char ssid_buf[33] = {0};
    bool connected = wifi_mgr_connected();
    wifi_mgr_get_ip(ip_buf, sizeof(ip_buf));
    wifi_mgr_get_ssid(ssid_buf, sizeof(ssid_buf));
    int rssi = wifi_mgr_rssi();

    cJSON *w = cJSON_CreateObject();
    cJSON_AddBoolToObject(w, "connected", connected);
    cJSON_AddStringToObject(w, "ssid", ssid_buf);
    cJSON_AddStringToObject(w, "ip", ip_buf);
    cJSON_AddNumberToObject(w, "rssi", rssi);
    cJSON_AddItemToObject(res, "wifi", w);
    cJSON_AddStringToObject(res, "ip", ip_buf);

    cJSON_AddItemToObject(root, "result", res);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, out, strlen(out));
    free(out);
    return ret;
}

static esp_err_t h_logs(httpd_req_t *req)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    uint32_t seq = 0;
    size_t n = log_capture_read(buf, 4095, &seq);
    buf[n] = '\0';

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, buf, n);
    free(buf);
    return ret;
}

static void delayed_reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t h_ota_upload(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Starting OTA upload (%d bytes)...", req->content_len);
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > 4096 ? 4096 : remaining;
        int ret = httpd_req_recv(req, buf, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "OTA recv failed");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, ret);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        remaining -= ret;
    }
    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA upgrade successful! Next boot partition: %s. Rebooting...", update_partition->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"rebooting\":true}", -1);

    xTaskCreate(delayed_reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"rebooting\":true}", -1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

esp_err_t cam_web_server_start(const char *device_id)
{
    if (s_httpd) return ESP_OK;
    if (device_id) strlcpy(s_device_id, device_id, sizeof(s_device_id));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.server_port = 80;
    config.stack_size = 10240; // Adequate stack for streaming loop

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start httpd: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t u_idx = { .uri = "/", .method = HTTP_GET, .handler = h_index };
    httpd_uri_t u_stream = { .uri = "/stream", .method = HTTP_GET, .handler = h_stream };
    httpd_uri_t u_capture = { .uri = "/capture", .method = HTTP_GET, .handler = h_capture };
    httpd_uri_t u_flash = { .uri = "/api/flash", .method = HTTP_POST, .handler = h_flash_toggle };
    httpd_uri_t u_fsize = { .uri = "/api/framesize", .method = HTTP_POST, .handler = h_framesize };
    httpd_uri_t u_status = { .uri = "/api/status", .method = HTTP_GET, .handler = h_status };
    httpd_uri_t u_logs = { .uri = "/api/logs", .method = HTTP_GET, .handler = h_logs };
    httpd_uri_t u_ota = { .uri = "/api/ota/upload", .method = HTTP_POST, .handler = h_ota_upload };
    httpd_uri_t u_reboot = { .uri = "/api/system/reboot", .method = HTTP_POST, .handler = h_reboot };

    httpd_register_uri_handler(s_httpd, &u_idx);
    httpd_register_uri_handler(s_httpd, &u_stream);
    httpd_register_uri_handler(s_httpd, &u_capture);
    httpd_register_uri_handler(s_httpd, &u_flash);
    httpd_register_uri_handler(s_httpd, &u_fsize);
    httpd_register_uri_handler(s_httpd, &u_status);
    httpd_register_uri_handler(s_httpd, &u_logs);
    httpd_register_uri_handler(s_httpd, &u_ota);
    httpd_register_uri_handler(s_httpd, &u_reboot);

    ESP_LOGI(TAG, "ESP32-CAM Web Portal & MJPEG Stream server started on :80");
    return ESP_OK;
}
