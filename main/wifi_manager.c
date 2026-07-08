#include "wifi_manager.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

#define MAX_CONNECT_RETRY 5
#define MAX_SCAN_AP 20
#define WIFI_SCAN_TASK_CORE_ID 0

#define WIFI_CFG_NAMESPACE "wifi_cfg"
#define WIFI_CFG_KEY_SSID "ssid"
#define WIFI_CFG_KEY_PASS "password"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define WIFI_SSID_BUF_LEN 33
#define WIFI_PASS_BUF_LEN 65

#define PROVISION_AP_SSID "MORI-GBA-SETUP"
#define PROVISION_AP_PASS ""

static int sta_connect_cnt = 0;
static bool is_sta_connected = false;
static bool wifi_started = false;
static bool wifi_initialized = false;
static bool provisioning_waiting_confirm = false;
static bool wifi_shutting_down_for_reboot = false;

static char target_ssid[WIFI_SSID_BUF_LEN] = {0};

static p_wifi_state_cb wifi_callback = NULL;
static SemaphoreHandle_t scan_sem = NULL;
static EventGroupHandle_t wifi_event_group = NULL;
static httpd_handle_t provision_httpd = NULL;

static esp_event_handler_instance_t wifi_event_instance;
static esp_event_handler_instance_t ip_event_instance;

static int hex_to_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = (char)tolower((unsigned char)ch);
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static void url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t si = 0;
    size_t di = 0;

    if (dst_len == 0) {
        return;
    }

    while (src[si] != '\0' && di + 1 < dst_len) {
        if (src[si] == '%' && src[si + 1] != '\0' && src[si + 2] != '\0') {
            int hi = hex_to_nibble(src[si + 1]);
            int lo = hex_to_nibble(src[si + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                si += 3;
                continue;
            }
        }

        if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
        si++;
    }

    dst[di] = '\0';
}

static void get_form_value(const char *body, const char *key, char *out, size_t out_len)
{
    const char *segment = body;
    size_t key_len = strlen(key);

    if (out_len == 0) {
        return;
    }
    out[0] = '\0';

    while (segment != NULL && *segment != '\0') {
        const char *equal = strchr(segment, '=');
        const char *amp = NULL;
        size_t name_len;
        size_t encoded_len;
        size_t copy_len;
        char encoded[WIFI_PASS_BUF_LEN * 3];

        if (equal == NULL) {
            return;
        }

        amp = strchr(equal + 1, '&');
        if (amp == NULL) {
            amp = segment + strlen(segment);
        }

        name_len = (size_t)(equal - segment);
        if (name_len == key_len && strncmp(segment, key, key_len) == 0) {
            encoded_len = (size_t)(amp - (equal + 1));
            copy_len = encoded_len;
            if (copy_len >= sizeof(encoded)) {
                copy_len = sizeof(encoded) - 1;
            }
            memcpy(encoded, equal + 1, copy_len);
            encoded[copy_len] = '\0';
            url_decode(encoded, out, out_len);
            return;
        }

        if (*amp == '\0') {
            return;
        }
        segment = amp + 1;
    }
}

static esp_err_t recv_request_body(httpd_req_t *req, char *buf, size_t len)
{
    int received = 0;

    while (received < (int)len) {
        int ret = httpd_req_recv(req, buf + received, len - (size_t)received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        received += ret;
    }

    return ESP_OK;
}

static esp_err_t send_http_chunk(httpd_req_t *req, const char *chunk)
{
    if (req == NULL || chunk == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN);
}

static void stop_provision_http_server(void);

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char ip[32] = {0};
    bool waiting_confirm = provisioning_waiting_confirm;
    static const char page[] =
        "<!doctype html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>MORI Wi-Fi Setup</title>"
        "<style>body{font-family:Arial,sans-serif;max-width:560px;margin:32px auto;padding:0 16px;line-height:1.55;}"
        "h2,p{margin:0 0 12px;}label{display:block;margin-top:10px;font-weight:700;}"
        "input,button{width:100%;padding:10px;margin:8px 0;box-sizing:border-box;}"
        "button{cursor:pointer;} .note{padding:12px;border:1px solid #ddd;border-radius:10px;background:#f7f7f7;margin-bottom:16px;}"
        ".lang{color:#555;font-size:14px;} code{background:#f1f1f1;padding:2px 4px;border-radius:4px;}</style>"
        "</head><body>";
    static const char setup_tail[] =
        "<h2>MORI GBA Burner Wi-Fi Setup<br><span class='lang'>MORI GBA Burner Wi-Fi 配网</span></h2>"
        "<div class='note'>"
        "<strong>Open hotspot / 开放热点</strong><br>"
        "SSID: <code>MORI-GBA-SETUP</code><br>"
        "Password: <code>none</code> / 无需密码"
        "</div>"
        "<p>Enter your router Wi-Fi below, then tap save.<br>"
        "<span class='lang'>请填写你家路由器的 Wi-Fi 名称和密码，然后点击保存连接。</span></p>"
        "<form method='post' action='/save'>"
        "<label>SSID / Wi-Fi Name / Wi-Fi 名称</label><input name='ssid' maxlength='32' required>"
        "<label>Password / 密码</label><input name='password' maxlength='63' type='password' placeholder='Leave empty if your Wi-Fi has no password / 无密码可留空'>"
        "<button type='submit'>Save and Connect / 保存并连接</button>"
        "</form>"
        "</body></html>";
    static const char confirm_head[] =
        "<h2>Wi-Fi Connected<br><span class='lang'>Wi-Fi 已连接</span></h2>"
        "<p>ESP32 is now connected to your router.<br>"
        "<span class='lang'>设备已经连上你的路由器。</span></p>"
        "<p>Tap confirm below to close the setup hotspot, then open this address on your phone or computer:<br>"
        "<span class='lang'>点击下方确认后会关闭配网热点，然后请在手机或电脑上打开这个地址：</span></p>";
    static const char confirm_tail[] =
        "<form method='get' action='/'>"
        "<button type='submit'>Refresh Status / 刷新状态</button>"
        "</form>"
        "<form method='post' action='/confirm'>"
        "<button type='submit'>Confirm and Close Setup Hotspot / 确认并关闭配网热点</button>"
        "</form>"
        "<form method='post' action='/later'>"
        "<button type='submit'>Keep Hotspot On For Now / 暂时保留配网热点</button>"
        "</form>"
        "<p>Later, you can close the hotspot from the device UI or revisit this page.<br>"
        "<span class='lang'>之后也可以在设备界面里关闭热点，或者稍后再回来确认。</span></p>"
        "</body></html>";
    char ip_link[128];
    int n;
    esp_err_t err;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (waiting_confirm && wifi_maneger_get_sta_ip(ip, sizeof(ip)) == ESP_OK) {
        err = send_http_chunk(req, page);
        if (err != ESP_OK) {
            return err;
        }
        err = send_http_chunk(req, confirm_head);
        if (err != ESP_OK) {
            return err;
        }
        n = snprintf(
            ip_link,
            sizeof(ip_link),
            "<p><a href='http://%s/' target='_blank'>http://%s/</a></p>",
            ip,
            ip);
        if (n <= 0 || n >= (int)sizeof(ip_link)) {
            httpd_resp_sendstr_chunk(req, NULL);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "page build failed");
        }
        err = send_http_chunk(req, ip_link);
        if (err != ESP_OK) {
            return err;
        }
        err = send_http_chunk(req, confirm_tail);
        if (err != ESP_OK) {
            return err;
        }
        return httpd_resp_sendstr_chunk(req, NULL);
    }
    err = send_http_chunk(req, page);
    if (err != ESP_OK) {
        return err;
    }
    err = send_http_chunk(req, setup_tail);
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    esp_err_t err;
    char *body = NULL;
    char ssid[WIFI_SSID_BUF_LEN] = {0};
    char password[WIFI_PASS_BUF_LEN] = {0};

    if (req->content_len <= 0 || req->content_len > 256) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body length");
    }

    body = (char *)calloc((size_t)req->content_len + 1, 1);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    err = recv_request_body(req, body, (size_t)req->content_len);
    if (err != ESP_OK) {
        free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request body");
    }
    body[req->content_len] = '\0';

    get_form_value(body, "ssid", ssid, sizeof(ssid));
    get_form_value(body, "password", password, sizeof(password));
    free(body);

    if (ssid[0] == '\0') {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "SSID is required / 必须填写 SSID");
    }

    err = wifi_maneger_save_sta_config(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(wifi_manager_tag, "save credentials failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Save failed / 保存失败");
    }

    wifi_maneger_connect(ssid, password);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(
        req,
        "<!doctype html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='3;url=/'>"
        "<title>Connecting...</title>"
        "<style>body{font-family:Arial,sans-serif;max-width:560px;margin:32px auto;padding:0 16px;line-height:1.55;}"
        ".note{padding:12px;border:1px solid #ddd;border-radius:10px;background:#f7f7f7;margin:16px 0;}"
        "button{width:100%;padding:10px;margin-top:16px;cursor:pointer;}</style>"
        "</head><body>"
        "<h2>Connecting to Wi-Fi...<br><span style='color:#555;font-size:14px;'>正在连接 Wi-Fi...</span></h2>"
        "<div class='note'>"
        "Saved. ESP32 is connecting to your router now.<br>"
        "已保存，设备正在连接你的路由器。"
        "</div>"
        "<p>Please keep your phone connected to <code>MORI-GBA-SETUP</code>. "
        "This page will refresh automatically and show the new LAN IP when ready.<br>"
        "请让手机继续连接 <code>MORI-GBA-SETUP</code>。准备好后，本页会自动刷新并显示新的局域网地址。</p>"
        "<form method='get' action='/'><button type='submit'>Refresh Now / 立即刷新</button></form>"
        "</body></html>");
}

static esp_err_t confirm_post_handler(httpd_req_t *req)
{
    esp_err_t err = wifi_maneger_provisioning_confirm();

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Confirm failed / 确认失败");
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(
        req,
        "Setup hotspot closed. Please continue from the shown router IP.\n"
        "配网热点已关闭，请继续使用上面显示的路由器地址。");
}

static esp_err_t later_post_handler(httpd_req_t *req)
{
    esp_err_t err = wifi_maneger_provisioning_keep_ap();

    if (err != ESP_OK) {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Keep AP failed / 保留热点失败");
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(
        req,
        "Setup hotspot remains active. You can close it later from the device UI or revisit this page.\n"
        "配网热点会继续保留，你可以稍后在设备界面关闭，或回到此页面再确认。");
}

static esp_err_t start_provision_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t err;

    if (provision_httpd != NULL) {
        return ESP_OK;
    }

    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.core_id = 0;

    err = httpd_start(&provision_httpd, &config);
    if (err != ESP_OK) {
        return err;
    }

    {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL,
        };
        err = httpd_register_uri_handler(provision_httpd, &root_uri);
        if (err != ESP_OK) {
            stop_provision_http_server();
            return err;
        }
    }

    {
        httpd_uri_t save_uri = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = save_post_handler,
            .user_ctx = NULL,
        };
        err = httpd_register_uri_handler(provision_httpd, &save_uri);
        if (err != ESP_OK) {
            stop_provision_http_server();
            return err;
        }
    }
    {
        httpd_uri_t confirm_uri = {
            .uri = "/confirm",
            .method = HTTP_POST,
            .handler = confirm_post_handler,
            .user_ctx = NULL,
        };
        err = httpd_register_uri_handler(provision_httpd, &confirm_uri);
        if (err != ESP_OK) {
            stop_provision_http_server();
            return err;
        }
    }
    {
        httpd_uri_t later_uri = {
            .uri = "/later",
            .method = HTTP_POST,
            .handler = later_post_handler,
            .user_ctx = NULL,
        };
        err = httpd_register_uri_handler(provision_httpd, &later_uri);
        if (err != ESP_OK) {
            stop_provision_http_server();
            return err;
        }
    }

    return ESP_OK;
}

static void stop_provision_http_server(void)
{
    if (provision_httpd != NULL) {
        httpd_stop(provision_httpd);
        provision_httpd = NULL;
    }
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                if (!wifi_shutting_down_for_reboot && target_ssid[0] != '\0') {
                    esp_wifi_connect();
                }
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
                if (!wifi_shutting_down_for_reboot) {
                    ESP_LOGW(
                        wifi_manager_tag,
                        "STA disconnected, reason=%d retry=%d/%d",
                        disc ? disc->reason : -1,
                        sta_connect_cnt,
                        MAX_CONNECT_RETRY);
                }

                if (is_sta_connected) {
                    is_sta_connected = false;
                    if (!wifi_shutting_down_for_reboot && wifi_callback != NULL) {
                        wifi_callback(WIFI_STATE_DISCONNECTED);
                    }
                }

                if (!wifi_shutting_down_for_reboot &&
                    target_ssid[0] != '\0' &&
                    sta_connect_cnt < MAX_CONNECT_RETRY) {
                    sta_connect_cnt++;
                    esp_wifi_connect();
                } else if (!wifi_shutting_down_for_reboot && wifi_event_group != NULL) {
                    xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
                }
                break;
            }

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(wifi_manager_tag, "STA connected to AP");
                break;

            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(wifi_manager_tag, "Device connected to provisioning AP");
                break;

            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(wifi_manager_tag, "Device disconnected from provisioning AP");
                break;

            default:
                break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(wifi_manager_tag, "STA got IP: " IPSTR, IP2STR(&got_ip->ip_info.ip));
        is_sta_connected = true;
        sta_connect_cnt = 0;

        if (wifi_event_group != NULL) {
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        }

        if (wifi_callback != NULL) {
            wifi_callback(provision_httpd != NULL ? WIFI_STATE_PROVISIONING_CONNECTED : WIFI_STATE_CONNECTED);
        }

        if (provision_httpd != NULL) {
            provisioning_waiting_confirm = true;
            ESP_LOGI(wifi_manager_tag, "Provisioning connected, waiting for user confirm before closing AP");
        }
    }
}

static esp_err_t load_saved_sta_config(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t err;
    size_t nvs_ssid_len = ssid_len;
    size_t nvs_pass_len = pass_len;

    err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(nvs, WIFI_CFG_KEY_SSID, ssid, &nvs_ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    err = nvs_get_str(nvs, WIFI_CFG_KEY_PASS, password, &nvs_pass_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        password[0] = '\0';
        err = ESP_OK;
    }

    nvs_close(nvs);
    return err;
}

esp_err_t wifi_maneger_init(p_wifi_state_cb f)
{
    esp_err_t err;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    wifi_callback = f;
    if (wifi_initialized) {
        return ESP_OK;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(wifi_manager_tag, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(wifi_manager_tag, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(wifi_manager_tag, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &wifi_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(wifi_manager_tag, "wifi event register failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &ip_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(wifi_manager_tag, "ip event register failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(wifi_manager_tag, "create wifi event group failed");
        return ESP_ERR_NO_MEM;
    }

    scan_sem = xSemaphoreCreateBinary();
    if (scan_sem != NULL) {
        xSemaphoreGive(scan_sem);
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(wifi_manager_tag, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(wifi_manager_tag, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_started = true;
    wifi_initialized = true;
    return ESP_OK;
}

bool wifi_maneger_ready(void)
{
    return wifi_initialized;
}

void wifi_maneger_connect(const char *ssid, const char *password)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_config_t wifi_config = {0};

    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGE(wifi_manager_tag, "ssid is empty");
        return;
    }

    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", ssid);
    snprintf(
        (char *)wifi_config.sta.password,
        sizeof(wifi_config.sta.password),
        "%s",
        password ? password : "");
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    snprintf(target_ssid, sizeof(target_ssid), "%s", ssid);
    sta_connect_cnt = 0;
    is_sta_connected = false;

    if (wifi_event_group != NULL) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        if (mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(WIFI_MODE_APSTA);
        } else if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
    }

    {
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (err != ESP_OK) {
            ESP_LOGE(wifi_manager_tag, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
            if (wifi_callback != NULL) {
                wifi_callback(WIFI_STATE_DISCONNECTED);
            }
            return;
        }
    }

    if (!wifi_started) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(wifi_manager_tag, "esp_wifi_start failed: %s", esp_err_to_name(err));
            if (wifi_callback != NULL) {
                wifi_callback(WIFI_STATE_DISCONNECTED);
            }
            return;
        }
        wifi_started = true;
    }

    esp_wifi_disconnect();
    {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(wifi_manager_tag, "esp_wifi_connect failed: %s", esp_err_to_name(err));
            if (wifi_callback != NULL) {
                wifi_callback(WIFI_STATE_DISCONNECTED);
            }
            return;
        }
    }
}

void wifi_maneger_disconnect(void)
{
    target_ssid[0] = '\0';
    sta_connect_cnt = 0;

    if (wifi_event_group != NULL) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    if (wifi_started) {
        esp_wifi_disconnect();
    }
}

esp_err_t wifi_maneger_shutdown_for_reboot(void)
{
    esp_err_t err = ESP_OK;
    esp_err_t stop_err;

    wifi_shutting_down_for_reboot = true;
    target_ssid[0] = '\0';
    sta_connect_cnt = 0;
    is_sta_connected = false;
    provisioning_waiting_confirm = false;

    if (wifi_event_group != NULL) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    stop_provision_http_server();

    if (!wifi_initialized || !wifi_started) {
        return ESP_OK;
    }

    stop_err = esp_wifi_disconnect();
    if (stop_err != ESP_OK &&
        stop_err != ESP_ERR_WIFI_NOT_STARTED &&
        stop_err != ESP_ERR_WIFI_CONN) {
        err = stop_err;
    }

    stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        if (err == ESP_OK) {
            err = stop_err;
        }
    } else {
        wifi_started = false;
    }

    return err;
}

bool wifi_maneger_has_saved_sta(void)
{
    char ssid[WIFI_SSID_BUF_LEN] = {0};
    char password[WIFI_PASS_BUF_LEN] = {0};
    esp_err_t err = load_saved_sta_config(ssid, sizeof(ssid), password, sizeof(password));
    return (err == ESP_OK) && (ssid[0] != '\0');
}

esp_err_t wifi_maneger_save_sta_config(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err;
    size_t ssid_len;
    size_t pass_len;

    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ssid_len = strlen(ssid);
    pass_len = password ? strlen(password) : 0;
    if (ssid_len == 0 || ssid_len > 32 || pass_len > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, WIFI_CFG_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_CFG_KEY_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

esp_err_t wifi_maneger_clear_sta_config(void)
{
    nvs_handle_t nvs;
    esp_err_t err;
    esp_err_t erase_err;

    err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    erase_err = nvs_erase_key(nvs, WIFI_CFG_KEY_SSID);
    if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return erase_err;
    }

    erase_err = nvs_erase_key(nvs, WIFI_CFG_KEY_PASS);
    if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return erase_err;
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t wifi_maneger_connect_saved(uint32_t timeout_ms)
{
    char ssid[WIFI_SSID_BUF_LEN] = {0};
    char password[WIFI_PASS_BUF_LEN] = {0};
    EventBits_t bits;
    esp_err_t err;

    if (wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    err = load_saved_sta_config(ssid, sizeof(ssid), password, sizeof(password));
    if (err != ESP_OK) {
        return err;
    }

    wifi_maneger_connect(ssid, password);
    if (timeout_ms == 0) {
        timeout_ms = 15000;
    }

    bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }
    if ((bits & WIFI_FAIL_BIT) != 0) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_maneger_get_sta_ip(char *ip, size_t ip_len)
{
    esp_netif_t *sta_netif;
    esp_netif_ip_info_t ip_info;
    const char *converted;
    esp_err_t err;

    if (ip == NULL || ip_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ip[0] = '\0';

    sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_netif_get_ip_info(sta_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    if (ip_info.ip.addr == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    converted = ip4addr_ntoa_r((const ip4_addr_t *)&ip_info.ip, ip, (int)ip_len);
    if (converted == NULL) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wifi_maneger_ap(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_config_t ap_config = {0};
    esp_err_t err;

    err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        return err;
    }
    if (mode != WIFI_MODE_APSTA) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            return err;
        }
    }

    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", PROVISION_AP_SSID);
    snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s", PROVISION_AP_PASS);
    ap_config.ap.ssid_len = strlen(PROVISION_AP_SSID);
    ap_config.ap.channel = 1;
    ap_config.ap.ssid_hidden = 0;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        return err;
    }
    if (!wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            return err;
        }
        wifi_started = true;
    }

    err = start_provision_http_server();
    if (err != ESP_OK) {
        return err;
    }
    provisioning_waiting_confirm = false;
    ESP_LOGI(
        wifi_manager_tag,
        "Provision AP started. SSID=%s, security=OPEN, URL=http://192.168.4.1/",
        PROVISION_AP_SSID);

    if (wifi_callback != NULL) {
        wifi_callback(WIFI_STATE_PROVISIONING);
    }

    return ESP_OK;
}

bool wifi_maneger_provisioning_waiting_confirm(void)
{
    return provisioning_waiting_confirm;
}

esp_err_t wifi_maneger_provisioning_confirm(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err;

    provisioning_waiting_confirm = false;
    stop_provision_http_server();
    err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        return err;
    }
    if (mode == WIFI_MODE_APSTA) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            ESP_LOGI(wifi_manager_tag, "Provisioning confirmed, switched to STA mode");
        }
        if (err != ESP_OK) {
            return err;
        }
    }

    if (is_sta_connected && wifi_callback != NULL) {
        wifi_callback(WIFI_STATE_CONNECTED);
    }
    return ESP_OK;
}

esp_err_t wifi_maneger_provisioning_keep_ap(void)
{
    provisioning_waiting_confirm = true;
    return ESP_OK;
}

static void scan_task(void *param)
{
    p_wifi_scan_cb callback = (p_wifi_scan_cb)param;
    uint16_t ap_count = 0;
    uint16_t ap_num = MAX_SCAN_AP;
    wifi_ap_record_t *ap_list =
        (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * (size_t)ap_num);

    if (ap_list == NULL) {
        if (scan_sem != NULL) {
            xSemaphoreGive(scan_sem);
        }
        vTaskDelete(NULL);
        return;
    }

    if (esp_wifi_scan_start(NULL, true) == ESP_OK &&
        esp_wifi_scan_get_ap_num(&ap_count) == ESP_OK &&
        esp_wifi_scan_get_ap_records(&ap_num, ap_list) == ESP_OK) {
        ESP_LOGI(wifi_manager_tag, "scan done: total=%u returned=%u", ap_count, ap_num);
        if (callback != NULL) {
            callback(ap_num, ap_list);
        }
    } else {
        ESP_LOGW(wifi_manager_tag, "scan failed");
    }

    free(ap_list);
    if (scan_sem != NULL) {
        xSemaphoreGive(scan_sem);
    }
    vTaskDelete(NULL);
}

esp_err_t wifi_maneger_scan(p_wifi_scan_cb f)
{
    BaseType_t task_ret;

    if (scan_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (pdTRUE != xSemaphoreTake(scan_sem, 0)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_wifi_clear_ap_list();
    task_ret = xTaskCreatePinnedToCore(scan_task, "wifi_scan", 4096, f, 3, NULL, WIFI_SCAN_TASK_CORE_ID);
    if (task_ret != pdPASS) {
        xSemaphoreGive(scan_sem);
        return ESP_FAIL;
    }

    return ESP_OK;
}
