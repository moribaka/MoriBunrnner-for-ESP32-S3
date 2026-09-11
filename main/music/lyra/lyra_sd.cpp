// MoriBurnner storage adapter for Lyra. TF uses VFS; SMB uses a seekable FILE stream.
#include "lyra_sd.h"
#include "freertos/semphr.h"
#include <cerrno>
#include <cstring>
#include <new>
extern "C" {
#include "smb_client.h"
#include "usb_msc_tf.h"
}
namespace {
StaticSemaphore_t lock_storage;
SemaphoreHandle_t gate;
struct Remote {
    smb_client_file_t *file;
    uint64_t size;
};
int remote_read(void *cookie, char *data, int count) {
    auto *r = static_cast<Remote *>(cookie);
    int n = smb_client_file_read(r->file, reinterpret_cast<uint8_t *>(data), count);
    if (n < 0)
        errno = EIO;
    return n;
}
fpos_t remote_seek(void *cookie, fpos_t offset, int origin) {
    auto *r = static_cast<Remote *>(cookie);
    int64_t base = origin == SEEK_SET   ? 0
                   : origin == SEEK_CUR ? smb_client_file_tell(r->file)
                                        : r->size;
    int64_t target = base + offset;
    if (target < 0 || smb_client_file_seek(r->file, target) != ESP_OK) {
        errno = EINVAL;
        return -1;
    }
    return target;
}
int remote_close(void *cookie) {
    auto *r = static_cast<Remote *>(cookie);
    smb_client_file_close(r->file);
    delete r;
    return 0;
}
} // namespace
namespace lyra::sd {
bool init() {
    if (!gate)
        gate = xSemaphoreCreateMutexStatic(&lock_storage);
    return gate != nullptr;
}
bool acquire(Client, TickType_t timeout) { return gate && xSemaphoreTake(gate, timeout) == pdTRUE; }
void release(Client) { xSemaphoreGive(gate); }
FILE *open(const char *path, const char *mode, Client client) {
    if (!acquire(client))
        return nullptr;
    FILE *f = nullptr;
    if (std::strncmp(path, "/smb/", 5) == 0) {
        auto *r = new (std::nothrow) Remote{};
        if (r && smb_client_open_file(path + 5, &r->file, &r->size) == ESP_OK) {
            f = funopen(r, remote_read, nullptr, remote_seek, remote_close);
            if (!f)
                remote_close(r);
        } else
            delete r;
    } else if (!usb_msc_tf_in_use_by_host())
        f = std::fopen(path, mode);
    release(client);
    return f;
}
int close(FILE *f, Client client) {
    if (!acquire(client))
        return -1;
    int n = std::fclose(f);
    release(client);
    return n;
}
size_t read(FILE *f, void *data, size_t size, Client client) {
    if (!acquire(client))
        return 0;
    size_t n = std::fread(data, 1, size, f);
    release(client);
    return n;
}
int seek(FILE *f, long offset, int origin, Client client) {
    if (!acquire(client))
        return -1;
    int n = std::fseek(f, offset, origin);
    release(client);
    return n;
}
} // namespace lyra::sd
