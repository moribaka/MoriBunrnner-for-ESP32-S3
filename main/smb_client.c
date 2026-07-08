#include "smb_client.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "smb2/smb2.h"
#include "smb2/libsmb2.h"
#include "smb2/libsmb2-raw.h"
#include "smb2/libsmb2-dcerpc-srvsvc.h"

#define SMB_CLIENT_TAG "smb_client"
#define SMB_CLIENT_DEFAULT_PORT 445
#define SMB_CLIENT_NETBIOS_NS_PORT 137
#define SMB_CLIENT_TIMEOUT_SECONDS 8
#define SMB_CLIENT_DISCOVERY_CONNECT_TIMEOUT_MS 250U
#define SMB_CLIENT_DISCOVERY_BATCH 6U
#define SMB_CLIENT_DISCOVERY_MIN_TIMEOUT_MS 800U
#define SMB_CLIENT_DISCOVERY_MAX_TIMEOUT_MS 20000U
#define SMB_CLIENT_NVS_NAMESPACE "smb_cfg"
#define SMB_CLIENT_NVS_NEXT_ID_KEY "next_id"
#define SMB_CLIENT_AUTH_SHARE "__server_auth__"
#define SMB_CLIENT_DOMAIN_MATCH_EXACT 2
#define SMB_CLIENT_DOMAIN_MATCH_FALLBACK 1

struct smb_client_file {
    struct smb2_context *ctx;
    struct smb2fh *fh;
    uint64_t offset;
    uint64_t size;
};

static SemaphoreHandle_t s_smb_lock = NULL;
static smb_client_config_t s_smb_config = {0};
static bool s_smb_has_config = false;
static char s_smb_last_error[SMB_CLIENT_MESSAGE_MAX] = {0};
static char s_smb_music_dir[SMB_CLIENT_PATH_MAX] = {0};

static bool smb_client_validate_host(const char *host);

static esp_err_t smb_client_ensure_lock(void)
{
    if (s_smb_lock == NULL) {
        s_smb_lock = xSemaphoreCreateMutex();
        if (s_smb_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void smb_client_set_last_error(const char *message)
{
    if (message == NULL) {
        message = "";
    }
    if (smb_client_ensure_lock() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_smb_lock, portMAX_DELAY);
    snprintf(s_smb_last_error, sizeof(s_smb_last_error), "%s", message);
    xSemaphoreGive(s_smb_lock);
}

static void smb_client_error_from_context(struct smb2_context *ctx, const char *fallback)
{
    const char *err = NULL;

    if (ctx != NULL) {
        err = smb2_get_error(ctx);
    }
    if (err == NULL || err[0] == '\0') {
        err = fallback;
    }
    smb_client_set_last_error(err);
}

static bool smb_client_copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0U) {
        return false;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return true;
    }
    return snprintf(dst, dst_len, "%s", src) >= 0;
}

static void smb_client_strip_line(char *line)
{
    size_t len;

    if (line == NULL) {
        return;
    }
    len = strlen(line);
    while (len > 0U && (line[len - 1U] == '\r' || line[len - 1U] == '\n')) {
        line[--len] = '\0';
    }
}

static bool smb_client_parse_bool_text(const char *text)
{
    return text != NULL &&
           (strcmp(text, "1") == 0 ||
            strcasecmp(text, "true") == 0 ||
            strcasecmp(text, "yes") == 0 ||
            strcasecmp(text, "on") == 0);
}

static void smb_client_favorite_make_label(const smb_client_config_t *config, char *label, size_t label_len)
{
    if (label == NULL || label_len == 0U) {
        return;
    }
    if (config == NULL) {
        label[0] = '\0';
        return;
    }
    snprintf(label, label_len, "//%s/%s", config->host, config->share);
}

static bool smb_client_encode_favorite(
    const smb_client_favorite_t *favorite,
    char *out,
    size_t out_len)
{
    int n;

    if (favorite == NULL || out == NULL || out_len == 0U) {
        return false;
    }
    n = snprintf(
        out,
        out_len,
        "id=%" PRIu32 "\n"
        "label=%s\n"
        "host=%s\n"
        "share=%s\n"
        "user=%s\n"
        "password=%s\n"
        "domain=%s\n"
        "port=%d\n"
        "signing=%d\n",
        favorite->id,
        favorite->label,
        favorite->config.host,
        favorite->config.share,
        favorite->config.user,
        favorite->config.password,
        favorite->config.domain,
        favorite->config.port,
        favorite->config.signing ? 1 : 0);
    return n > 0 && n < (int)out_len;
}

static bool smb_client_decode_favorite(const char *text, smb_client_favorite_t *favorite)
{
    char temp[768] = {0};
    char *save = NULL;
    char *line = NULL;

    if (text == NULL || favorite == NULL) {
        return false;
    }
    memset(favorite, 0, sizeof(*favorite));
    if (snprintf(temp, sizeof(temp), "%s", text) < 0) {
        return false;
    }

    line = strtok_r(temp, "\n", &save);
    while (line != NULL) {
        char *eq;

        smb_client_strip_line(line);
        eq = strchr(line, '=');
        if (eq != NULL) {
            const char *key = line;
            const char *value;

            *eq = '\0';
            value = eq + 1;
            if (strcmp(key, "id") == 0) {
                favorite->id = (uint32_t)strtoul(value, NULL, 10);
            } else if (strcmp(key, "label") == 0) {
                smb_client_copy_text(favorite->label, sizeof(favorite->label), value);
            } else if (strcmp(key, "host") == 0) {
                smb_client_copy_text(favorite->config.host, sizeof(favorite->config.host), value);
            } else if (strcmp(key, "share") == 0) {
                smb_client_copy_text(favorite->config.share, sizeof(favorite->config.share), value);
            } else if (strcmp(key, "user") == 0) {
                smb_client_copy_text(favorite->config.user, sizeof(favorite->config.user), value);
            } else if (strcmp(key, "password") == 0) {
                smb_client_copy_text(favorite->config.password, sizeof(favorite->config.password), value);
            } else if (strcmp(key, "domain") == 0) {
                smb_client_copy_text(favorite->config.domain, sizeof(favorite->config.domain), value);
            } else if (strcmp(key, "port") == 0) {
                favorite->config.port = (int)strtol(value, NULL, 10);
            } else if (strcmp(key, "signing") == 0) {
                favorite->config.signing = smb_client_parse_bool_text(value);
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }

    if (favorite->config.port <= 0) {
        favorite->config.port = SMB_CLIENT_DEFAULT_PORT;
    }
    if (favorite->label[0] == '\0') {
        smb_client_favorite_make_label(&favorite->config, favorite->label, sizeof(favorite->label));
    }
    return favorite->id != 0U && smb_client_validate_host(favorite->config.host) &&
           favorite->config.share[0] != '\0';
}

static bool smb_client_valid_segment(const char *segment)
{
    return segment != NULL &&
           segment[0] != '\0' &&
           strcmp(segment, ".") != 0 &&
           strcmp(segment, "..") != 0;
}

bool smb_client_normalize_path(const char *input, char *output, size_t output_len, bool allow_empty)
{
    char temp[SMB_CLIENT_PATH_MAX] = {0};
    char *save = NULL;
    char *token = NULL;
    size_t out_idx = 0;

    if (output == NULL || output_len == 0U) {
        return false;
    }
    output[0] = '\0';

    if (input == NULL || input[0] == '\0') {
        return allow_empty;
    }
    if (snprintf(temp, sizeof(temp), "%s", input) < 0) {
        return false;
    }

    for (size_t i = 0; temp[i] != '\0'; ++i) {
        if (temp[i] == '\\') {
            temp[i] = '/';
        }
    }

    token = strtok_r(temp, "/", &save);
    while (token != NULL) {
        size_t seg_len = strlen(token);

        if (!smb_client_valid_segment(token)) {
            return false;
        }
        if (out_idx != 0U) {
            if (out_idx + 1U >= output_len) {
                return false;
            }
            output[out_idx++] = '/';
        }
        if (out_idx + seg_len >= output_len) {
            return false;
        }
        memcpy(output + out_idx, token, seg_len);
        out_idx += seg_len;
        output[out_idx] = '\0';
        token = strtok_r(NULL, "/", &save);
    }

    if (out_idx == 0U) {
        return allow_empty;
    }
    return true;
}

const char *smb_client_basename(const char *path)
{
    const char *slash = NULL;

    if (path == NULL || path[0] == '\0') {
        return "";
    }
    slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        return path;
    }
    return slash + 1;
}

static void smb_client_path_to_remote(const char *normalized, char *remote, size_t remote_len)
{
    if (remote == NULL || remote_len == 0U) {
        return;
    }
    if (normalized == NULL || normalized[0] == '\0') {
        remote[0] = '\0';
        return;
    }
    snprintf(remote, remote_len, "%s", normalized);
}

static void smb_client_build_child_path(
    const char *parent,
    const char *name,
    char *out,
    size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (parent == NULL || parent[0] == '\0') {
        snprintf(out, out_len, "%s", name != NULL ? name : "");
    } else {
        snprintf(out, out_len, "%s/%s", parent, name != NULL ? name : "");
    }
}

static uint64_t smb_client_filetime_to_unix_seconds(uint64_t filetime)
{
    const uint64_t epoch_diff_100ns = 116444736000000000ULL;

    if (filetime <= epoch_diff_100ns) {
        return 0;
    }
    return (filetime - epoch_diff_100ns) / 10000000ULL;
}

static esp_err_t smb_client_snapshot_config(smb_client_config_t *config_out)
{
    if (config_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (smb_client_ensure_lock() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_smb_lock, portMAX_DELAY);
    if (!s_smb_has_config) {
        xSemaphoreGive(s_smb_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *config_out = s_smb_config;
    xSemaphoreGive(s_smb_lock);
    return ESP_OK;
}

typedef struct {
    int fd;
    uint32_t ip_addr;
    uint32_t started_ms;
} smb_client_probe_t;

typedef struct {
    bool finished;
    int status;
    void *data;
} smb_client_async_state_t;

static esp_err_t smb_client_emit_discovery_named(
    uint32_t ip_addr,
    const char *name,
    smb_client_discovery_cb_t cb,
    void *user_ctx);

static uint32_t smb_client_discovery_host_for_index(uint32_t index)
{
    if (index == 0U || index > 254U) {
        return 0;
    }
    if ((index & 1U) != 0U) {
        return (index + 1U) / 2U;
    }
    return 255U - (index / 2U);
}

static bool smb_client_discovery_seen(const uint32_t *ips, uint32_t count, uint32_t ip_addr)
{
    if (ips == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (ips[i] == ip_addr) {
            return true;
        }
    }
    return false;
}

static void smb_client_discovery_remember(uint32_t *ips, uint32_t *count, uint32_t ip_addr)
{
    if (ips == NULL || count == NULL || *count >= SMB_CLIENT_DISCOVERY_MAX) {
        return;
    }
    if (!smb_client_discovery_seen(ips, *count, ip_addr)) {
        ips[(*count)++] = ip_addr;
    }
}

static void smb_client_probe_close(smb_client_probe_t *probe)
{
    if (probe == NULL) {
        return;
    }
    if (probe->fd >= 0) {
        close(probe->fd);
    }
    probe->fd = -1;
    probe->ip_addr = 0;
    probe->started_ms = 0;
}

static bool smb_client_probe_start(smb_client_probe_t *probe, uint32_t ip_addr, uint16_t port, bool *connected)
{
    struct sockaddr_in addr = {0};
    int flags;
    int ret;

    if (probe == NULL || connected == NULL) {
        return false;
    }
    *connected = false;
    probe->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (probe->fd < 0) {
        return false;
    }
    flags = fcntl(probe->fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(probe->fd, F_SETFL, flags | O_NONBLOCK);
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip_addr;
    probe->ip_addr = ip_addr;
    probe->started_ms = esp_log_timestamp();

    ret = connect(probe->fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) {
        *connected = true;
        smb_client_probe_close(probe);
        return true;
    }
    if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN) {
        return true;
    }

    smb_client_probe_close(probe);
    return false;
}

static esp_err_t smb_client_emit_discovery(uint32_t ip_addr, smb_client_discovery_cb_t cb, void *user_ctx)
{
    return smb_client_emit_discovery_named(ip_addr, NULL, cb, user_ctx);
}

static esp_err_t smb_client_emit_discovery_named(
    uint32_t ip_addr,
    const char *name,
    smb_client_discovery_cb_t cb,
    void *user_ctx)
{
    smb_client_discovery_entry_t entry = {0};
    ip4_addr_t addr;

    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    addr.addr = ip_addr;
    (void)ip4addr_ntoa_r(&addr, entry.host, sizeof(entry.host));
    if (name != NULL && name[0] != '\0') {
        snprintf(entry.name, sizeof(entry.name), "%s", name);
    } else {
        snprintf(entry.name, sizeof(entry.name), "%s", entry.host);
    }
    entry.port = SMB_CLIENT_DEFAULT_PORT;
    return cb(&entry, user_ctx);
}

static void smb_client_trim_netbios_name(char *name)
{
    size_t len;

    if (name == NULL) {
        return;
    }
    len = strlen(name);
    while (len > 0U && name[len - 1U] == ' ') {
        name[--len] = '\0';
    }
}

static bool smb_client_netbios_parse_file_server_name(const uint8_t *resp, size_t resp_len, char *name_out, size_t name_len)
{
    uint8_t name_count;
    size_t offset;

    if (resp == NULL || name_out == NULL || name_len == 0U || resp_len < 57U) {
        return false;
    }
    name_out[0] = '\0';
    name_count = resp[56];
    offset = 57U;
    for (uint8_t i = 0; i < name_count && offset + 18U <= resp_len; ++i, offset += 18U) {
        uint8_t suffix = resp[offset + 15U];
        uint16_t flags = ((uint16_t)resp[offset + 16U] << 8) | resp[offset + 17U];
        bool group = (flags & 0x8000U) != 0U;

        if (!group && suffix == 0x20U) {
            size_t copy_len = (name_len - 1U < 15U) ? (name_len - 1U) : 15U;

            memcpy(name_out, &resp[offset], copy_len);
            name_out[copy_len] = '\0';
            smb_client_trim_netbios_name(name_out);
            return name_out[0] != '\0';
        }
    }
    return false;
}

static esp_err_t smb_client_discover_netbios(
    uint32_t broadcast_addr,
    uint32_t timeout_ms,
    smb_client_discovery_cb_t cb,
    void *user_ctx,
    uint32_t *seen_ips,
    uint32_t *seen_count,
    uint32_t *found)
{
    static const uint8_t query[] = {
        0x4d, 0x43, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x20,
        'C', 'K', 'A', 'A', 'A', 'A', 'A', 'A',
        'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A',
        'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A',
        'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A',
        0x00, 0x00, 0x21, 0x00, 0x01,
    };
    _Static_assert(sizeof(query) == 50U, "NetBIOS node status query must be 50 bytes");
    struct sockaddr_in addr = {0};
    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = 120000,
    };
    int fd;
    int yes = 1;
    uint32_t deadline;
    esp_err_t err = ESP_OK;

    if (cb == NULL || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        return ESP_OK;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SMB_CLIENT_NETBIOS_NS_PORT);
    addr.sin_addr.s_addr = broadcast_addr;

    (void)sendto(fd, query, sizeof(query), 0, (const struct sockaddr *)&addr, sizeof(addr));
    deadline = esp_log_timestamp() + timeout_ms;
    while ((int32_t)(esp_log_timestamp() - deadline) < 0) {
        uint8_t resp[576] = {0};
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        char name[SMB_CLIENT_NAME_MAX] = {0};
        int ret = recvfrom(fd, resp, sizeof(resp), 0, (struct sockaddr *)&from, &from_len);

        if (ret <= 0) {
            continue;
        }
        if (from.sin_addr.s_addr == 0 ||
            smb_client_discovery_seen(seen_ips, seen_count != NULL ? *seen_count : 0U, from.sin_addr.s_addr)) {
            continue;
        }
        if (!smb_client_netbios_parse_file_server_name(resp, (size_t)ret, name, sizeof(name))) {
            continue;
        }
        err = smb_client_emit_discovery_named(from.sin_addr.s_addr, name, cb, user_ctx);
        if (err != ESP_OK) {
            break;
        }
        smb_client_discovery_remember(seen_ips, seen_count, from.sin_addr.s_addr);
        (*found)++;
        if (*found >= SMB_CLIENT_DISCOVERY_MAX) {
            break;
        }
    }
    close(fd);
    return err;
}

esp_err_t smb_client_discovery_network(char *network_out, size_t network_len)
{
    esp_netif_t *sta_netif = NULL;
    esp_netif_ip_info_t ip_info = {0};
    uint32_t network_host;
    ip4_addr_t addr;
    char network[SMB_CLIENT_HOST_MAX] = {0};
    esp_err_t err;

    if (network_out == NULL || network_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    network_out[0] = '\0';
    sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_netif_get_ip_info(sta_netif, &ip_info);
    if (err != ESP_OK || ip_info.ip.addr == 0 || ip_info.netmask.addr == 0) {
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_STATE;
    }
    network_host = ntohl(ip_info.ip.addr) & 0xFFFFFF00U;
    addr.addr = htonl(network_host);
    (void)ip4addr_ntoa_r(&addr, network, sizeof(network));
    snprintf(network_out, network_len, "%s/24", network);
    return ESP_OK;
}

esp_err_t smb_client_discover(uint32_t timeout_ms, smb_client_discovery_cb_t cb, void *user_ctx)
{
    esp_netif_t *sta_netif = NULL;
    esp_netif_ip_info_t ip_info = {0};
    uint32_t ip_host;
    uint32_t network_host;
    uint32_t broadcast_addr;
    uint32_t deadline;
    uint32_t each_timeout_ms = SMB_CLIENT_DISCOVERY_CONNECT_TIMEOUT_MS;
    smb_client_probe_t probes[SMB_CLIENT_DISCOVERY_BATCH];
    uint32_t seen_ips[SMB_CLIENT_DISCOVERY_MAX] = {0};
    uint32_t seen_count = 0;
    uint32_t next_index = 1U;
    uint32_t active = 0;
    uint32_t found = 0;
    esp_err_t err;

    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms < SMB_CLIENT_DISCOVERY_MIN_TIMEOUT_MS) {
        timeout_ms = SMB_CLIENT_DISCOVERY_MIN_TIMEOUT_MS;
    } else if (timeout_ms > SMB_CLIENT_DISCOVERY_MAX_TIMEOUT_MS) {
        timeout_ms = SMB_CLIENT_DISCOVERY_MAX_TIMEOUT_MS;
    }

    sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL) {
        smb_client_set_last_error("wifi sta is not ready");
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_netif_get_ip_info(sta_netif, &ip_info);
    if (err != ESP_OK || ip_info.ip.addr == 0 || ip_info.netmask.addr == 0) {
        smb_client_set_last_error("wifi ip is not ready");
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_STATE;
    }

    ip_host = ntohl(ip_info.ip.addr);
    network_host = ip_host & 0xFFFFFF00U;
    broadcast_addr = htonl(network_host | 255U);
    err = smb_client_discover_netbios(
        broadcast_addr,
        timeout_ms > 1500U ? 1500U : timeout_ms,
        cb,
        user_ctx,
        seen_ips,
        &seen_count,
        &found);
    if (err != ESP_OK || found >= SMB_CLIENT_DISCOVERY_MAX) {
        return err;
    }
    deadline = esp_log_timestamp() + timeout_ms;
    for (uint32_t i = 0; i < SMB_CLIENT_DISCOVERY_BATCH; ++i) {
        probes[i].fd = -1;
        probes[i].ip_addr = 0;
        probes[i].started_ms = 0;
    }

    while ((next_index <= 254U || active > 0U) && found < SMB_CLIENT_DISCOVERY_MAX) {
        uint32_t now = esp_log_timestamp();
        fd_set write_set;
        int max_fd = -1;
        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = 20000,
        };
        int ret;

        if ((int32_t)(now - deadline) >= 0) {
            break;
        }

        for (uint32_t i = 0; i < SMB_CLIENT_DISCOVERY_BATCH && next_index <= 254U; ++i) {
            if (probes[i].fd >= 0) {
                continue;
            }
            while (next_index <= 254U) {
                uint32_t host = smb_client_discovery_host_for_index(next_index++);
                uint32_t candidate;
                bool connected = false;

                if (host == 0U) {
                    continue;
                }
                candidate = htonl(network_host | host);
                if (candidate == ip_info.ip.addr) {
                    continue;
                }
                if (smb_client_discovery_seen(seen_ips, seen_count, candidate)) {
                    continue;
                }
                if (!smb_client_probe_start(&probes[i], candidate, SMB_CLIENT_DEFAULT_PORT, &connected)) {
                    continue;
                }
                if (connected) {
                    err = smb_client_emit_discovery(candidate, cb, user_ctx);
                    if (err != ESP_OK) {
                        goto cleanup;
                    }
                    found++;
                    smb_client_discovery_remember(seen_ips, &seen_count, candidate);
                    if (found >= SMB_CLIENT_DISCOVERY_MAX) {
                        goto cleanup;
                    }
                } else {
                    active++;
                }
                break;
            }
        }

        FD_ZERO(&write_set);
        for (uint32_t i = 0; i < SMB_CLIENT_DISCOVERY_BATCH; ++i) {
            if (probes[i].fd >= 0) {
                FD_SET(probes[i].fd, &write_set);
                if (probes[i].fd > max_fd) {
                    max_fd = probes[i].fd;
                }
            }
        }
        if (max_fd < 0) {
            continue;
        }

        ret = select(max_fd + 1, NULL, &write_set, NULL, &tv);
        now = esp_log_timestamp();
        for (uint32_t i = 0; i < SMB_CLIENT_DISCOVERY_BATCH; ++i) {
            bool timed_out;

            if (probes[i].fd < 0) {
                continue;
            }
            timed_out = (uint32_t)(now - probes[i].started_ms) >= each_timeout_ms;
            if (ret > 0 && FD_ISSET(probes[i].fd, &write_set)) {
                int sock_err = 0;
                socklen_t sock_err_len = sizeof(sock_err);
                uint32_t candidate = probes[i].ip_addr;

                if (getsockopt(probes[i].fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len) == 0 &&
                    sock_err == 0) {
                    smb_client_probe_close(&probes[i]);
                    active--;
                    err = smb_client_emit_discovery(candidate, cb, user_ctx);
                    if (err != ESP_OK) {
                        goto cleanup;
                    }
                    found++;
                    smb_client_discovery_remember(seen_ips, &seen_count, candidate);
                    if (found >= SMB_CLIENT_DISCOVERY_MAX) {
                        goto cleanup;
                    }
                    continue;
                }
                smb_client_probe_close(&probes[i]);
                active--;
            } else if (timed_out) {
                smb_client_probe_close(&probes[i]);
                active--;
            }
        }
    }

    err = ESP_OK;

cleanup:
    for (uint32_t i = 0; i < SMB_CLIENT_DISCOVERY_BATCH; ++i) {
        smb_client_probe_close(&probes[i]);
    }
    return err;
}

static bool smb_client_validate_config(const smb_client_config_t *config)
{
    if (config == NULL || config->host[0] == '\0' || config->share[0] == '\0') {
        return false;
    }
    if (strchr(config->host, '/') != NULL || strchr(config->host, '\\') != NULL) {
        return false;
    }
    if (strchr(config->share, '/') != NULL || strchr(config->share, '\\') != NULL) {
        return false;
    }
    return true;
}

static bool smb_client_validate_host(const char *host)
{
    return host != NULL &&
           host[0] != '\0' &&
           strchr(host, '/') == NULL &&
           strchr(host, '\\') == NULL;
}

static void smb_client_setup_context_options(struct smb2_context *ctx, const smb_client_config_t *config)
{
    if (ctx == NULL || config == NULL) {
        return;
    }
    smb2_set_timeout(ctx, SMB_CLIENT_TIMEOUT_SECONDS);
    smb2_set_version(ctx, SMB2_VERSION_ANY);
    if (config->signing) {
        smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
        smb2_set_sign(ctx, 1);
    }
    if (config->domain[0] != '\0') {
        smb2_set_domain(ctx, config->domain);
    }
    if (config->password[0] != '\0') {
        smb2_set_password(ctx, config->password);
    }
}

static void smb_client_share_enum_cb(
    struct smb2_context *ctx,
    int status,
    void *command_data,
    void *cb_data)
{
    smb_client_async_state_t *state = (smb_client_async_state_t *)cb_data;

    (void)ctx;
    if (state == NULL) {
        return;
    }
    state->status = status;
    state->data = command_data;
    state->finished = true;
}

static int smb_client_wait_async(struct smb2_context *ctx, smb_client_async_state_t *state)
{
    uint32_t deadline;

    if (ctx == NULL || state == NULL) {
        return -EINVAL;
    }
    deadline = esp_log_timestamp() + (SMB_CLIENT_TIMEOUT_SECONDS * 1000U);
    while (!state->finished) {
        int fd = smb2_get_fd(ctx);
        int events = smb2_which_events(ctx);
        int revents = 0;
        fd_set read_set;
        fd_set write_set;
        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = 200000,
        };
        int ret;

        if ((int32_t)(esp_log_timestamp() - deadline) >= 0) {
            smb_client_set_last_error("smb share enum timeout");
            return -ETIMEDOUT;
        }
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        if ((events & POLLIN) != 0) {
            FD_SET(fd, &read_set);
        }
        if ((events & POLLOUT) != 0) {
            FD_SET(fd, &write_set);
        }

        ret = select(fd + 1, &read_set, &write_set, NULL, &tv);
        if (ret < 0) {
            smb_client_error_from_context(ctx, "smb share enum select failed");
            return -errno;
        }
        if (ret == 0) {
            continue;
        }
        if (FD_ISSET(fd, &read_set)) {
            revents |= POLLIN;
        }
        if (FD_ISSET(fd, &write_set)) {
            revents |= POLLOUT;
        }
        if (revents != 0 && smb2_service(ctx, revents) < 0) {
            smb_client_error_from_context(ctx, "smb share enum service failed");
            return -EIO;
        }
    }
    return state->status;
}

static struct smb2_context *smb_client_connect_context(const smb_client_config_t *config)
{
    struct smb2_context *ctx = NULL;
    char server[SMB_CLIENT_HOST_MAX + 12] = {0};
    int ret;

    if (!smb_client_validate_config(config)) {
        smb_client_set_last_error("invalid smb config");
        return NULL;
    }

    ctx = smb2_init_context();
    if (ctx == NULL) {
        smb_client_set_last_error("smb context init failed");
        return NULL;
    }

    smb_client_setup_context_options(ctx, config);

    if (config->port > 0 && config->port != SMB_CLIENT_DEFAULT_PORT) {
        snprintf(server, sizeof(server), "%s:%d", config->host, config->port);
    } else {
        snprintf(server, sizeof(server), "%s", config->host);
    }

    ret = smb2_connect_share(ctx, server, config->share, config->user[0] != '\0' ? config->user : NULL);
    if (ret != 0) {
        smb_client_error_from_context(ctx, "smb connect failed");
        ESP_LOGW(SMB_CLIENT_TAG, "connect //%s/%s failed: %d %s", server, config->share, ret, smb2_get_error(ctx));
        smb2_destroy_context(ctx);
        return NULL;
    }

    return ctx;
}

esp_err_t smb_client_list_shares(const smb_client_config_t *config, smb_client_share_cb_t cb, void *user_ctx)
{
    smb_client_config_t clean = {0};
    struct smb2_context *ctx = NULL;
    struct srvsvc_netshareenumall_rep *rep = NULL;
    smb_client_async_state_t state = {0};
    char server[SMB_CLIENT_HOST_MAX + 12] = {0};
    int ret;
    esp_err_t err = ESP_OK;

    if (config == NULL || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    smb_client_copy_text(clean.host, sizeof(clean.host), config->host);
    smb_client_copy_text(clean.user, sizeof(clean.user), config->user);
    smb_client_copy_text(clean.password, sizeof(clean.password), config->password);
    smb_client_copy_text(clean.domain, sizeof(clean.domain), config->domain);
    clean.port = (config->port > 0) ? config->port : SMB_CLIENT_DEFAULT_PORT;
    clean.signing = config->signing;

    if (!smb_client_validate_host(clean.host)) {
        smb_client_set_last_error("host is required");
        return ESP_ERR_INVALID_ARG;
    }

    ctx = smb2_init_context();
    if (ctx == NULL) {
        smb_client_set_last_error("smb context init failed");
        return ESP_ERR_NO_MEM;
    }
    smb_client_setup_context_options(ctx, &clean);

    if (clean.port > 0 && clean.port != SMB_CLIENT_DEFAULT_PORT) {
        snprintf(server, sizeof(server), "%s:%d", clean.host, clean.port);
    } else {
        snprintf(server, sizeof(server), "%s", clean.host);
    }

    ret = smb2_connect_share(ctx, server, "IPC$", clean.user[0] != '\0' ? clean.user : NULL);
    if (ret != 0) {
        smb_client_error_from_context(ctx, "connect IPC$ failed");
        ESP_LOGW(SMB_CLIENT_TAG, "connect //%s/IPC$ failed: %d %s", server, ret, smb2_get_error(ctx));
        smb2_destroy_context(ctx);
        return ESP_FAIL;
    }

    ret = smb2_share_enum_async(ctx, smb_client_share_enum_cb, &state);
    if (ret < 0) {
        smb_client_error_from_context(ctx, "share enum start failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    ret = smb_client_wait_async(ctx, &state);
    if (ret != 0) {
        smb_client_error_from_context(ctx, "share enum failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    rep = (struct srvsvc_netshareenumall_rep *)state.data;
    if (rep == NULL || rep->ctr == NULL || rep->ctr->level != 1U) {
        smb_client_set_last_error("share enum returned no data");
        err = ESP_FAIL;
        goto cleanup;
    }

    for (uint32_t i = 0; i < rep->ctr->ctr1.count && i < SMB_CLIENT_SHARE_ENUM_MAX; ++i) {
        const struct srvsvc_netshareinfo1 *info = &rep->ctr->ctr1.array[i];
        smb_client_share_entry_t entry = {0};
        uint32_t base_type;

        if (info->name == NULL || info->name[0] == '\0') {
            continue;
        }
        base_type = info->type & 0x3U;
        if (base_type != SHARE_TYPE_DISKTREE) {
            continue;
        }
        snprintf(entry.name, sizeof(entry.name), "%s", info->name);
        if (info->comment != NULL) {
            snprintf(entry.comment, sizeof(entry.comment), "%s", info->comment);
        }
        entry.type = info->type;
        entry.hidden = (info->type & SHARE_TYPE_HIDDEN) != 0U;
        err = cb(&entry, user_ctx);
        if (err != ESP_OK) {
            break;
        }
    }

cleanup:
    if (rep != NULL) {
        smb2_free_data(ctx, rep);
    }
    smb2_disconnect_share(ctx);
    smb2_destroy_context(ctx);
    return err;
}

static void smb_client_favorite_key(uint32_t slot, char *key, size_t key_len)
{
    if (key == NULL || key_len == 0U) {
        return;
    }
    snprintf(key, key_len, "fav%" PRIu32, slot);
}

static void smb_client_auth_key(uint32_t slot, char *key, size_t key_len)
{
    if (key == NULL || key_len == 0U) {
        return;
    }
    snprintf(key, key_len, "auth%" PRIu32, slot);
}

static bool smb_client_favorite_same_target(
    const smb_client_favorite_t *favorite,
    const smb_client_config_t *config)
{
    if (favorite == NULL || config == NULL) {
        return false;
    }
    if (strcasecmp(favorite->config.host, config->host) != 0 ||
        strcasecmp(favorite->config.share, config->share) != 0 ||
        favorite->config.port != config->port) {
        return false;
    }
    if (favorite->config.domain[0] != '\0' && config->domain[0] != '\0' &&
        strcasecmp(favorite->config.domain, config->domain) != 0) {
        return false;
    }
    return true;
}

static int smb_client_auth_match_score(
    const smb_client_favorite_t *favorite,
    const smb_client_config_t *config)
{
    if (favorite == NULL || config == NULL) {
        return 0;
    }
    if (strcasecmp(favorite->config.host, config->host) != 0 ||
        favorite->config.port != config->port) {
        return 0;
    }
    if (favorite->config.domain[0] != '\0' && config->domain[0] != '\0' &&
        strcasecmp(favorite->config.domain, config->domain) != 0) {
        return 0;
    }
    if (strcasecmp(favorite->config.domain, config->domain) == 0) {
        return SMB_CLIENT_DOMAIN_MATCH_EXACT;
    }
    return SMB_CLIENT_DOMAIN_MATCH_FALLBACK;
}

static esp_err_t smb_client_read_favorite_slot(
    nvs_handle_t nvs,
    uint32_t slot,
    smb_client_favorite_t *favorite)
{
    char key[12] = {0};
    char value[768] = {0};
    size_t value_len = sizeof(value);
    esp_err_t err;

    if (favorite == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    smb_client_favorite_key(slot, key, sizeof(key));
    err = nvs_get_str(nvs, key, value, &value_len);
    if (err != ESP_OK) {
        return err;
    }
    return smb_client_decode_favorite(value, favorite) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t smb_client_read_auth_slot(
    nvs_handle_t nvs,
    uint32_t slot,
    smb_client_favorite_t *auth)
{
    char key[12] = {0};
    char value[768] = {0};
    size_t value_len = sizeof(value);
    esp_err_t err;

    if (auth == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    smb_client_auth_key(slot, key, sizeof(key));
    err = nvs_get_str(nvs, key, value, &value_len);
    if (err != ESP_OK) {
        return err;
    }
    return smb_client_decode_favorite(value, auth) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void smb_client_apply_auth_entry(
    smb_client_config_t *config,
    const smb_client_favorite_t *auth)
{
    if (config == NULL || auth == NULL) {
        return;
    }
    smb_client_copy_text(config->user, sizeof(config->user), auth->config.user);
    smb_client_copy_text(config->password, sizeof(config->password), auth->config.password);
    if (config->domain[0] == '\0') {
        smb_client_copy_text(config->domain, sizeof(config->domain), auth->config.domain);
    }
    config->signing = auth->config.signing;
}

static void smb_client_update_auth_fields(
    smb_client_config_t *dst,
    const smb_client_config_t *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    smb_client_copy_text(dst->user, sizeof(dst->user), src->user);
    smb_client_copy_text(dst->password, sizeof(dst->password), src->password);
    if (src->domain[0] != '\0' || dst->domain[0] == '\0') {
        smb_client_copy_text(dst->domain, sizeof(dst->domain), src->domain);
    }
    dst->signing = src->signing;
}

esp_err_t smb_client_list_favorites(smb_client_favorite_cb_t cb, void *user_ctx)
{
    nvs_handle_t nvs;
    esp_err_t err;

    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(SMB_CLIENT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    for (uint32_t slot = 0; slot < SMB_CLIENT_FAVORITE_MAX; ++slot) {
        smb_client_favorite_t favorite = {0};

        err = smb_client_read_favorite_slot(nvs, slot, &favorite);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            continue;
        }
        if (strcmp(favorite.config.share, SMB_CLIENT_AUTH_SHARE) == 0) {
            continue;
        }
        err = cb(&favorite, user_ctx);
        if (err != ESP_OK) {
            break;
        }
    }
    nvs_close(nvs);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
}

esp_err_t smb_client_apply_saved_auth(smb_client_config_t *config)
{
    nvs_handle_t nvs;
    esp_err_t err;
    smb_client_favorite_t best = {0};
    int best_score = 0;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!smb_client_validate_host(config->host)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->port <= 0) {
        config->port = SMB_CLIENT_DEFAULT_PORT;
    }

    err = nvs_open(SMB_CLIENT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    for (uint32_t slot = 0; slot < SMB_CLIENT_AUTH_MAX; ++slot) {
        smb_client_favorite_t auth = {0};
        int score;

        err = smb_client_read_auth_slot(nvs, slot, &auth);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            continue;
        }
        score = smb_client_auth_match_score(&auth, config);
        if (score > best_score) {
            best = auth;
            best_score = score;
        }
        if (best_score == SMB_CLIENT_DOMAIN_MATCH_EXACT) {
            break;
        }
    }

    for (uint32_t slot = 0; slot < SMB_CLIENT_FAVORITE_MAX; ++slot) {
        smb_client_favorite_t existing = {0};
        int score;

        err = smb_client_read_favorite_slot(nvs, slot, &existing);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            continue;
        }
        score = smb_client_auth_match_score(&existing, config);
        if (score > best_score) {
            best = existing;
            best_score = score;
        }
    }
    nvs_close(nvs);
    if (best_score <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    smb_client_apply_auth_entry(config, &best);
    return ESP_OK;
}

esp_err_t smb_client_save_server_auth(const smb_client_config_t *config)
{
    smb_client_config_t auth = {0};
    nvs_handle_t nvs;
    uint32_t target_auth_slot = UINT32_MAX;
    uint32_t empty_auth_slot = UINT32_MAX;
    int best_auth_score = 0;
    char key[12] = {0};
    char value[768] = {0};
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    smb_client_copy_text(auth.host, sizeof(auth.host), config->host);
    smb_client_copy_text(auth.user, sizeof(auth.user), config->user);
    smb_client_copy_text(auth.password, sizeof(auth.password), config->password);
    smb_client_copy_text(auth.domain, sizeof(auth.domain), config->domain);
    auth.port = (config->port > 0) ? config->port : SMB_CLIENT_DEFAULT_PORT;
    auth.signing = config->signing;
    if (!smb_client_validate_host(auth.host)) {
        smb_client_set_last_error("host is required");
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(SMB_CLIENT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    for (uint32_t slot = 0; slot < SMB_CLIENT_AUTH_MAX; ++slot) {
        smb_client_favorite_t existing = {0};
        int score;

        err = smb_client_read_auth_slot(nvs, slot, &existing);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            if (empty_auth_slot == UINT32_MAX) {
                empty_auth_slot = slot;
            }
            continue;
        }
        if (err != ESP_OK) {
            continue;
        }
        score = smb_client_auth_match_score(&existing, &auth);
        if (score > best_auth_score) {
            target_auth_slot = slot;
            best_auth_score = score;
        }
    }

    for (uint32_t slot = 0; slot < SMB_CLIENT_FAVORITE_MAX; ++slot) {
        smb_client_favorite_t existing = {0};
        esp_err_t read_err = smb_client_read_favorite_slot(nvs, slot, &existing);

        if (read_err == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (read_err != ESP_OK) {
            continue;
        }
        if (smb_client_auth_match_score(&existing, &auth) <= 0) {
            continue;
        }
        smb_client_update_auth_fields(&existing.config, &auth);
        if (!smb_client_encode_favorite(&existing, value, sizeof(value))) {
            nvs_close(nvs);
            return ESP_ERR_INVALID_SIZE;
        }
        smb_client_favorite_key(slot, key, sizeof(key));
        err = nvs_set_str(nvs, key, value);
        if (err != ESP_OK) {
            nvs_close(nvs);
            return err;
        }
    }

    if (target_auth_slot == UINT32_MAX) {
        target_auth_slot = empty_auth_slot;
    }
    if (target_auth_slot != UINT32_MAX) {
        smb_client_favorite_t auth_entry = {0};

        auth_entry.id = target_auth_slot + 1U;
        auth_entry.config = auth;
        smb_client_copy_text(auth_entry.config.share, sizeof(auth_entry.config.share), SMB_CLIENT_AUTH_SHARE);
        snprintf(auth_entry.label, sizeof(auth_entry.label), "//%s", auth.host);
        if (!smb_client_encode_favorite(&auth_entry, value, sizeof(value))) {
            nvs_close(nvs);
            return ESP_ERR_INVALID_SIZE;
        }
        smb_client_auth_key(target_auth_slot, key, sizeof(key));
        err = nvs_set_str(nvs, key, value);
        if (err != ESP_OK) {
            nvs_close(nvs);
            return err;
        }
    }
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t smb_client_save_favorite(const smb_client_config_t *config, smb_client_favorite_t *favorite_out)
{
    smb_client_config_t clean = {0};
    smb_client_favorite_t favorite = {0};
    nvs_handle_t nvs;
    uint32_t next_id = 1U;
    uint32_t target_slot = UINT32_MAX;
    char key[12] = {0};
    char value[768] = {0};
    bool has_credentials;
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    smb_client_copy_text(clean.host, sizeof(clean.host), config->host);
    smb_client_copy_text(clean.share, sizeof(clean.share), config->share);
    smb_client_copy_text(clean.user, sizeof(clean.user), config->user);
    smb_client_copy_text(clean.password, sizeof(clean.password), config->password);
    smb_client_copy_text(clean.domain, sizeof(clean.domain), config->domain);
    clean.port = (config->port > 0) ? config->port : SMB_CLIENT_DEFAULT_PORT;
    clean.signing = config->signing;
    has_credentials = clean.user[0] != '\0' || clean.password[0] != '\0';
    if (!smb_client_validate_config(&clean)) {
        smb_client_set_last_error("host and share are required");
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(SMB_CLIENT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    (void)nvs_get_u32(nvs, SMB_CLIENT_NVS_NEXT_ID_KEY, &next_id);
    if (next_id == 0U) {
        next_id = 1U;
    }

    for (uint32_t slot = 0; slot < SMB_CLIENT_FAVORITE_MAX; ++slot) {
        smb_client_favorite_t existing = {0};
        esp_err_t read_err = smb_client_read_favorite_slot(nvs, slot, &existing);

        if (read_err == ESP_ERR_NVS_NOT_FOUND) {
            if (target_slot == UINT32_MAX) {
                target_slot = slot;
            }
            continue;
        }
        if (read_err != ESP_OK) {
            continue;
        }
        if (smb_client_favorite_same_target(&existing, &clean)) {
            target_slot = slot;
            favorite.id = existing.id;
            break;
        }
    }

    if (target_slot == UINT32_MAX) {
        nvs_close(nvs);
        smb_client_set_last_error("smb favorites full");
        return ESP_ERR_NO_MEM;
    }
    if (!has_credentials) {
        smb_client_favorite_t best_auth = {0};
        int best_score = 0;

        for (uint32_t slot = 0; slot < SMB_CLIENT_AUTH_MAX; ++slot) {
            smb_client_favorite_t auth = {0};
            int score;
            esp_err_t read_err = smb_client_read_auth_slot(nvs, slot, &auth);

            if (read_err == ESP_ERR_NVS_NOT_FOUND) {
                continue;
            }
            if (read_err != ESP_OK) {
                continue;
            }
            score = smb_client_auth_match_score(&auth, &clean);
            if (score > best_score) {
                best_auth = auth;
                best_score = score;
            }
        }
        if (best_score > 0) {
            smb_client_update_auth_fields(&clean, &best_auth.config);
        }
    }
    if (favorite.id == 0U) {
        favorite.id = next_id++;
    }
    favorite.config = clean;
    smb_client_favorite_make_label(&clean, favorite.label, sizeof(favorite.label));
    if (!smb_client_encode_favorite(&favorite, value, sizeof(value))) {
        nvs_close(nvs);
        return ESP_ERR_INVALID_SIZE;
    }
    smb_client_favorite_key(target_slot, key, sizeof(key));
    err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, SMB_CLIENT_NVS_NEXT_ID_KEY, next_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK && favorite_out != NULL) {
        *favorite_out = favorite;
    }
    return err;
}

esp_err_t smb_client_get_favorite(uint32_t id, smb_client_favorite_t *favorite_out)
{
    nvs_handle_t nvs;
    esp_err_t err;

    if (id == 0U || favorite_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(SMB_CLIENT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    for (uint32_t slot = 0; slot < SMB_CLIENT_FAVORITE_MAX; ++slot) {
        smb_client_favorite_t favorite = {0};

        err = smb_client_read_favorite_slot(nvs, slot, &favorite);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            continue;
        }
        if (favorite.id == id) {
            *favorite_out = favorite;
            nvs_close(nvs);
            return ESP_OK;
        }
    }
    nvs_close(nvs);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t smb_client_remove_favorite(uint32_t id)
{
    nvs_handle_t nvs;
    esp_err_t err;

    if (id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(SMB_CLIENT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    for (uint32_t slot = 0; slot < SMB_CLIENT_FAVORITE_MAX; ++slot) {
        smb_client_favorite_t favorite = {0};
        char key[12] = {0};

        err = smb_client_read_favorite_slot(nvs, slot, &favorite);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            continue;
        }
        if (favorite.id == id) {
            smb_client_favorite_key(slot, key, sizeof(key));
            err = nvs_erase_key(nvs, key);
            if (err == ESP_OK) {
                err = nvs_commit(nvs);
            }
            nvs_close(nvs);
            return err;
        }
    }
    nvs_close(nvs);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t smb_client_connect_favorite(uint32_t id)
{
    smb_client_favorite_t favorite = {0};
    esp_err_t err;

    err = smb_client_get_favorite(id, &favorite);
    if (err != ESP_OK) {
        smb_client_set_last_error("smb favorite not found");
        return err;
    }
    if (favorite.config.user[0] == '\0' && favorite.config.password[0] == '\0') {
        (void)smb_client_apply_saved_auth(&favorite.config);
    }
    return smb_client_connect(&favorite.config);
}

esp_err_t smb_client_connect(const smb_client_config_t *config)
{
    smb_client_config_t clean = {0};
    struct smb2_context *test_ctx = NULL;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    smb_client_copy_text(clean.host, sizeof(clean.host), config->host);
    smb_client_copy_text(clean.share, sizeof(clean.share), config->share);
    smb_client_copy_text(clean.user, sizeof(clean.user), config->user);
    smb_client_copy_text(clean.password, sizeof(clean.password), config->password);
    smb_client_copy_text(clean.domain, sizeof(clean.domain), config->domain);
    clean.port = (config->port > 0) ? config->port : SMB_CLIENT_DEFAULT_PORT;
    clean.signing = config->signing;

    if (!smb_client_validate_config(&clean)) {
        smb_client_set_last_error("host and share are required");
        return ESP_ERR_INVALID_ARG;
    }

    test_ctx = smb_client_connect_context(&clean);
    if (test_ctx == NULL) {
        return ESP_FAIL;
    }
    smb2_disconnect_share(test_ctx);
    smb2_destroy_context(test_ctx);

    if (smb_client_ensure_lock() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_smb_lock, portMAX_DELAY);
    s_smb_config = clean;
    s_smb_has_config = true;
    s_smb_last_error[0] = '\0';
    xSemaphoreGive(s_smb_lock);
    return ESP_OK;
}

void smb_client_disconnect(void)
{
    if (smb_client_ensure_lock() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_smb_lock, portMAX_DELAY);
    memset(&s_smb_config, 0, sizeof(s_smb_config));
    s_smb_has_config = false;
    s_smb_last_error[0] = '\0';
    xSemaphoreGive(s_smb_lock);
}

void smb_client_get_status(smb_client_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (smb_client_ensure_lock() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_smb_lock, portMAX_DELAY);
    status->connected = s_smb_has_config;
    status->config = s_smb_config;
    snprintf(status->last_error, sizeof(status->last_error), "%s", s_smb_last_error);
    xSemaphoreGive(s_smb_lock);
}

esp_err_t smb_client_list(const char *path, smb_client_list_cb_t cb, void *user_ctx)
{
    smb_client_config_t config = {0};
    char normalized[SMB_CLIENT_PATH_MAX] = {0};
    char remote[SMB_CLIENT_PATH_MAX] = {0};
    struct smb2_context *ctx = NULL;
    struct smb2dir *dir = NULL;
    struct smb2dirent *dirent = NULL;
    esp_err_t err = ESP_OK;

    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!smb_client_normalize_path(path, normalized, sizeof(normalized), true)) {
        smb_client_set_last_error("invalid path");
        return ESP_ERR_INVALID_ARG;
    }
    err = smb_client_snapshot_config(&config);
    if (err != ESP_OK) {
        smb_client_set_last_error("smb server is not connected");
        return err;
    }

    ctx = smb_client_connect_context(&config);
    if (ctx == NULL) {
        return ESP_FAIL;
    }

    smb_client_path_to_remote(normalized, remote, sizeof(remote));
    dir = smb2_opendir(ctx, remote);
    if (dir == NULL) {
        smb_client_error_from_context(ctx, "open smb directory failed");
        smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        return ESP_FAIL;
    }

    while ((dirent = smb2_readdir(ctx, dir)) != NULL) {
        smb_client_dirent_t entry = {0};

        if (dirent->name == NULL ||
            strcmp(dirent->name, ".") == 0 ||
            strcmp(dirent->name, "..") == 0) {
            continue;
        }

        snprintf(entry.name, sizeof(entry.name), "%s", dirent->name);
        smb_client_build_child_path(normalized, dirent->name, entry.path, sizeof(entry.path));
        entry.is_dir = (dirent->st.smb2_type == SMB2_TYPE_DIRECTORY);
        entry.size = dirent->st.smb2_size;
        entry.mtime = smb_client_filetime_to_unix_seconds(dirent->st.smb2_mtime);
        err = cb(&entry, user_ctx);
        if (err != ESP_OK) {
            break;
        }
    }

    smb2_closedir(ctx, dir);
    smb2_disconnect_share(ctx);
    smb2_destroy_context(ctx);
    return err;
}

esp_err_t smb_client_stat(const char *path, smb_client_dirent_t *entry_out)
{
    smb_client_config_t config = {0};
    char normalized[SMB_CLIENT_PATH_MAX] = {0};
    char remote[SMB_CLIENT_PATH_MAX] = {0};
    struct smb2_context *ctx = NULL;
    struct smb2_stat_64 st = {0};
    int ret;
    esp_err_t err;

    if (entry_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!smb_client_normalize_path(path, normalized, sizeof(normalized), false)) {
        smb_client_set_last_error("invalid path");
        return ESP_ERR_INVALID_ARG;
    }
    err = smb_client_snapshot_config(&config);
    if (err != ESP_OK) {
        smb_client_set_last_error("smb server is not connected");
        return err;
    }

    ctx = smb_client_connect_context(&config);
    if (ctx == NULL) {
        return ESP_FAIL;
    }

    smb_client_path_to_remote(normalized, remote, sizeof(remote));
    ret = smb2_stat(ctx, remote, &st);
    if (ret != 0) {
        smb_client_error_from_context(ctx, "smb stat failed");
        smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        return ESP_FAIL;
    }

    memset(entry_out, 0, sizeof(*entry_out));
    snprintf(entry_out->path, sizeof(entry_out->path), "%s", normalized);
    snprintf(entry_out->name, sizeof(entry_out->name), "%s", smb_client_basename(normalized));
    entry_out->is_dir = (st.smb2_type == SMB2_TYPE_DIRECTORY);
    entry_out->size = st.smb2_size;
    entry_out->mtime = smb_client_filetime_to_unix_seconds(st.smb2_mtime);

    smb2_disconnect_share(ctx);
    smb2_destroy_context(ctx);
    return ESP_OK;
}

esp_err_t smb_client_open_file(const char *path, smb_client_file_t **file_out, uint64_t *size_out)
{
    smb_client_config_t config = {0};
    char normalized[SMB_CLIENT_PATH_MAX] = {0};
    char remote[SMB_CLIENT_PATH_MAX] = {0};
    struct smb2_context *ctx = NULL;
    struct smb2fh *fh = NULL;
    struct smb2_stat_64 st = {0};
    smb_client_file_t *file = NULL;
    esp_err_t err;

    if (file_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *file_out = NULL;
    if (size_out != NULL) {
        *size_out = 0;
    }
    if (!smb_client_normalize_path(path, normalized, sizeof(normalized), false)) {
        smb_client_set_last_error("invalid path");
        return ESP_ERR_INVALID_ARG;
    }
    err = smb_client_snapshot_config(&config);
    if (err != ESP_OK) {
        smb_client_set_last_error("smb server is not connected");
        return err;
    }

    ctx = smb_client_connect_context(&config);
    if (ctx == NULL) {
        return ESP_FAIL;
    }

    smb_client_path_to_remote(normalized, remote, sizeof(remote));
    fh = smb2_open(ctx, remote, O_RDONLY);
    if (fh == NULL) {
        smb_client_error_from_context(ctx, "open smb file failed");
        smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        return ESP_FAIL;
    }

    if (smb2_fstat(ctx, fh, &st) != 0 || st.smb2_type == SMB2_TYPE_DIRECTORY) {
        smb_client_error_from_context(ctx, "smb file stat failed");
        smb2_close(ctx, fh);
        smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        return ESP_FAIL;
    }

    file = (smb_client_file_t *)calloc(1, sizeof(*file));
    if (file == NULL) {
        smb_client_set_last_error("no memory");
        smb2_close(ctx, fh);
        smb2_disconnect_share(ctx);
        smb2_destroy_context(ctx);
        return ESP_ERR_NO_MEM;
    }

    file->ctx = ctx;
    file->fh = fh;
    file->size = st.smb2_size;
    file->offset = 0;
    if (size_out != NULL) {
        *size_out = file->size;
    }
    *file_out = file;
    return ESP_OK;
}

int smb_client_file_read(smb_client_file_t *file, uint8_t *buf, size_t len)
{
    int ret;
    uint32_t chunk;
    uint32_t max_read;

    if (file == NULL || file->ctx == NULL || file->fh == NULL || buf == NULL || len == 0U) {
        return -EINVAL;
    }
    max_read = smb2_get_max_read_size(file->ctx);
    if (max_read == 0U || max_read > 64U * 1024U) {
        max_read = 64U * 1024U;
    }
    chunk = (len > (size_t)max_read) ? max_read : (uint32_t)len;
    ret = smb2_read(file->ctx, file->fh, buf, chunk);
    if (ret > 0) {
        file->offset += (uint64_t)ret;
    } else if (ret < 0) {
        smb_client_error_from_context(file->ctx, "smb read failed");
    }
    return ret;
}

esp_err_t smb_client_file_seek(smb_client_file_t *file, uint64_t offset)
{
    uint64_t current = 0;
    int64_t ret;

    if (file == NULL || file->ctx == NULL || file->fh == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ret = smb2_lseek(file->ctx, file->fh, (int64_t)offset, SEEK_SET, &current);
    if (ret < 0) {
        smb_client_error_from_context(file->ctx, "smb seek failed");
        return ESP_FAIL;
    }
    file->offset = current;
    return ESP_OK;
}

uint64_t smb_client_file_tell(smb_client_file_t *file)
{
    if (file == NULL) {
        return 0;
    }
    return file->offset;
}

void smb_client_file_close(smb_client_file_t *file)
{
    if (file == NULL) {
        return;
    }
    if (file->ctx != NULL && file->fh != NULL) {
        smb2_close(file->ctx, file->fh);
    }
    if (file->ctx != NULL) {
        smb2_disconnect_share(file->ctx);
        smb2_destroy_context(file->ctx);
    }
    free(file);
}

esp_err_t smb_client_set_music_dir(const char *path)
{
    char normalized[SMB_CLIENT_PATH_MAX] = {0};

    if (!smb_client_normalize_path(path, normalized, sizeof(normalized), true)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (smb_client_ensure_lock() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_smb_lock, portMAX_DELAY);
    snprintf(s_smb_music_dir, sizeof(s_smb_music_dir), "%s", normalized);
    xSemaphoreGive(s_smb_lock);
    return ESP_OK;
}

void smb_client_get_music_dir(char *path_out, size_t path_len)
{
    if (path_out == NULL || path_len == 0U) {
        return;
    }
    path_out[0] = '\0';
    if (smb_client_ensure_lock() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_smb_lock, portMAX_DELAY);
    snprintf(path_out, path_len, "%s", s_smb_music_dir);
    xSemaphoreGive(s_smb_lock);
}
