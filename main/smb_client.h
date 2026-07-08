#ifndef SMB_CLIENT_H
#define SMB_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMB_CLIENT_HOST_MAX 64
#define SMB_CLIENT_SHARE_MAX 64
#define SMB_CLIENT_USER_MAX 64
#define SMB_CLIENT_PASSWORD_MAX 96
#define SMB_CLIENT_DOMAIN_MAX 64
#define SMB_CLIENT_PATH_MAX 240
#define SMB_CLIENT_NAME_MAX 128
#define SMB_CLIENT_MESSAGE_MAX 128
#define SMB_CLIENT_DISCOVERY_MAX 32
#define SMB_CLIENT_SHARE_ENUM_MAX 32
#define SMB_CLIENT_FAVORITE_MAX 8
#define SMB_CLIENT_AUTH_MAX 8

typedef struct {
    char host[SMB_CLIENT_HOST_MAX];
    char name[SMB_CLIENT_NAME_MAX];
    int port;
} smb_client_discovery_entry_t;

typedef struct {
    char name[SMB_CLIENT_SHARE_MAX];
    char comment[SMB_CLIENT_NAME_MAX];
    uint32_t type;
    bool hidden;
} smb_client_share_entry_t;

typedef struct {
    char host[SMB_CLIENT_HOST_MAX];
    char share[SMB_CLIENT_SHARE_MAX];
    char user[SMB_CLIENT_USER_MAX];
    char password[SMB_CLIENT_PASSWORD_MAX];
    char domain[SMB_CLIENT_DOMAIN_MAX];
    int port;
    bool signing;
} smb_client_config_t;

typedef struct {
    uint32_t id;
    char label[SMB_CLIENT_NAME_MAX];
    smb_client_config_t config;
} smb_client_favorite_t;

typedef struct {
    bool connected;
    smb_client_config_t config;
    char last_error[SMB_CLIENT_MESSAGE_MAX];
} smb_client_status_t;

typedef struct {
    char name[SMB_CLIENT_NAME_MAX];
    char path[SMB_CLIENT_PATH_MAX];
    bool is_dir;
    uint64_t size;
    uint64_t mtime;
} smb_client_dirent_t;

typedef esp_err_t (*smb_client_list_cb_t)(const smb_client_dirent_t *entry, void *user_ctx);
typedef esp_err_t (*smb_client_discovery_cb_t)(const smb_client_discovery_entry_t *entry, void *user_ctx);
typedef esp_err_t (*smb_client_share_cb_t)(const smb_client_share_entry_t *entry, void *user_ctx);
typedef esp_err_t (*smb_client_favorite_cb_t)(const smb_client_favorite_t *entry, void *user_ctx);

typedef struct smb_client_file smb_client_file_t;

esp_err_t smb_client_connect(const smb_client_config_t *config);
esp_err_t smb_client_discover(uint32_t timeout_ms, smb_client_discovery_cb_t cb, void *user_ctx);
esp_err_t smb_client_discovery_network(char *network_out, size_t network_len);
esp_err_t smb_client_list_shares(const smb_client_config_t *config, smb_client_share_cb_t cb, void *user_ctx);
esp_err_t smb_client_list_favorites(smb_client_favorite_cb_t cb, void *user_ctx);
esp_err_t smb_client_save_favorite(const smb_client_config_t *config, smb_client_favorite_t *favorite_out);
esp_err_t smb_client_get_favorite(uint32_t id, smb_client_favorite_t *favorite_out);
esp_err_t smb_client_remove_favorite(uint32_t id);
esp_err_t smb_client_connect_favorite(uint32_t id);
esp_err_t smb_client_save_server_auth(const smb_client_config_t *config);
esp_err_t smb_client_apply_saved_auth(smb_client_config_t *config);
void smb_client_disconnect(void);
void smb_client_get_status(smb_client_status_t *status);
esp_err_t smb_client_list(const char *path, smb_client_list_cb_t cb, void *user_ctx);
esp_err_t smb_client_stat(const char *path, smb_client_dirent_t *entry_out);
esp_err_t smb_client_open_file(const char *path, smb_client_file_t **file_out, uint64_t *size_out);
int smb_client_file_read(smb_client_file_t *file, uint8_t *buf, size_t len);
esp_err_t smb_client_file_seek(smb_client_file_t *file, uint64_t offset);
uint64_t smb_client_file_tell(smb_client_file_t *file);
void smb_client_file_close(smb_client_file_t *file);
esp_err_t smb_client_set_music_dir(const char *path);
void smb_client_get_music_dir(char *path_out, size_t path_len);
bool smb_client_normalize_path(const char *input, char *output, size_t output_len, bool allow_empty);
const char *smb_client_basename(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SMB_CLIENT_H */
