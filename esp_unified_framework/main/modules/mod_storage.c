#include "mod_storage.h"
#include "sdkconfig.h"

#if CONFIG_FEATURE_STORAGE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "cJSON.h"
#include "hal_storage.h"
#include "web_server.h"

static const char *TAG = "mod_storage";

static void send_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static bool extract_safe_filename(httpd_req_t *req, char *out_path, size_t max_len)
{
    char qs[128] = "";
    char name[64] = "";
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        if (httpd_query_key_value(qs, "path", name, sizeof(name)) != ESP_OK) {
            httpd_query_key_value(qs, "name", name, sizeof(name));
        }
    }
    if (name[0] == '\0') return false;

    // Strip any path traversal or directory components; keep pure filename
    const char *base = strrchr(name, '/');
    const char *src = base ? (base + 1) : name;
    if (src[0] == '\0' || strcmp(src, ".") == 0 || strcmp(src, "..") == 0 || strstr(src, "..")) {
        return false;
    }

    snprintf(out_path, max_len, "%s/%.31s", hal_storage_base_path(), src);
    return true;
}

static esp_err_t storage_list_get_handler(httpd_req_t *req)
{
    send_cors(req);
    size_t total = 0, used = 0;
    bool mounted = hal_storage_is_mounted();
    if (mounted) hal_storage_get_usage(&total, &used);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "mounted", mounted);
    cJSON_AddNumberToObject(root, "total_bytes", (double)total);
    cJSON_AddNumberToObject(root, "used_bytes", (double)used);
    cJSON *files = cJSON_AddArrayToObject(root, "files");

    if (mounted) {
        DIR *dir = opendir(hal_storage_base_path());
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                char full[96];
                struct stat st = {0};
                snprintf(full, sizeof(full), "%s/%.64s", hal_storage_base_path(), ent->d_name);
                if (stat(full, &st) == 0) {
                    cJSON *f = cJSON_CreateObject();
                    cJSON_AddStringToObject(f, "name", ent->d_name);
                    cJSON_AddNumberToObject(f, "size", (double)st.st_size);
                    cJSON_AddItemToArray(files, f);
                }
            }
            closedir(dir);
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (!out) return httpd_resp_sendstr(req, "{\"ok\":false}");
    esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
}

static esp_err_t storage_upload_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    send_cors(req);

    if (!hal_storage_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SPIFFS not mounted");
        return ESP_FAIL;
    }

    char filepath[96];
    if (!extract_safe_filename(req, filepath, sizeof(filepath))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Query ?path=<filename> required");
        return ESP_FAIL;
    }

    if (req->content_len < 0 || req->content_len > 524288) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid file length");
        return ESP_FAIL;
    }

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file for write");
        return ESP_FAIL;
    }

    char *buf = malloc(2048);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int written = 0;
    while (remaining > 0) {
        int to_read = remaining < 2048 ? remaining : 2048;
        int n = httpd_req_recv(req, buf, to_read);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            fclose(f);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
            return ESP_FAIL;
        }
        fwrite(buf, 1, n, f);
        written += n;
        remaining -= n;
    }

    free(buf);
    fclose(f);

    char resp[160];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"%s\",\"bytes\":%d}", filepath, written);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t storage_download_get_handler(httpd_req_t *req)
{
    send_cors(req);
    if (!hal_storage_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SPIFFS not mounted");
        return ESP_FAIL;
    }

    char filepath[96];
    if (!extract_safe_filename(req, filepath, sizeof(filepath))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Query ?path=<filename> required");
        return ESP_FAIL;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    char *buf = malloc(2048);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    size_t n;
    while ((n = fread(buf, 1, 2048, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            free(buf);
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    free(buf);
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t storage_delete_post_handler(httpd_req_t *req)
{
    if (!web_server_check_auth(req)) return ESP_FAIL;
    send_cors(req);
    if (!hal_storage_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SPIFFS not mounted");
        return ESP_FAIL;
    }

    char filepath[96] = "";
    if (!extract_safe_filename(req, filepath, sizeof(filepath)) && req->content_len > 0 && req->content_len <= 256) {
        char body[257] = "";
        int r = httpd_req_recv(req, body, req->content_len);
        if (r > 0) {
            body[r] = '\0';
            cJSON *j = cJSON_Parse(body);
            if (j) {
                const cJSON *p = cJSON_GetObjectItem(j, "path");
                if (cJSON_IsString(p) && p->valuestring[0] && !strstr(p->valuestring, "..")) {
                    const char *base = strrchr(p->valuestring, '/');
                    const char *src = base ? (base + 1) : p->valuestring;
                    snprintf(filepath, sizeof(filepath), "%s/%.31s", hal_storage_base_path(), src);
                }
                cJSON_Delete(j);
            }
        }
    }

    if (filepath[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
        return ESP_FAIL;
    }

    bool ok = (unlink(filepath) == 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true,\"deleted\":true}" : "{\"ok\":false,\"error\":\"unlink failed\"}");
}

esp_err_t mod_storage_init(void)
{
    return hal_storage_init();
}

esp_err_t mod_storage_register_routes(httpd_handle_t server)
{
    httpd_uri_t u_list = { .uri = "/api/storage/list", .method = HTTP_GET, .handler = storage_list_get_handler };
    httpd_register_uri_handler(server, &u_list);

    httpd_uri_t u_up = { .uri = "/api/storage/upload", .method = HTTP_POST, .handler = storage_upload_post_handler };
    httpd_register_uri_handler(server, &u_up);

    httpd_uri_t u_dl = { .uri = "/api/storage/download", .method = HTTP_GET, .handler = storage_download_get_handler };
    httpd_register_uri_handler(server, &u_dl);

    httpd_uri_t u_del = { .uri = "/api/storage/delete", .method = HTTP_POST, .handler = storage_delete_post_handler };
    httpd_register_uri_handler(server, &u_del);

    ESP_LOGI(TAG, "Storage REST routes registered (/api/storage/list, upload, download, delete)");
    return ESP_OK;
}

#else

esp_err_t mod_storage_init(void) { return ESP_OK; }
esp_err_t mod_storage_register_routes(httpd_handle_t server) { return ESP_OK; }

#endif
