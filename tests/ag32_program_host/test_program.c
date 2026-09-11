/* Fault injection around the real batch programmer; no hardware is touched. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#define WS_SERVER_INTERNAL_H
#define POWER_MANAGER_H
SemaphoreHandle_t s_status_lock = (void *)1;
TaskHandle_t s_burn_task;
bool s_burn_starting, fail_image_alloc;
unsigned static_task_creations, notifications;
esp_err_t burner_backend_init(void) { return ESP_OK; }
esp_err_t power_manager_perf_lock_acquire(const char *s) { (void)s; return ESP_OK; }
void power_manager_perf_lock_release(const char *s) { (void)s; }
void lvgl_port_set_idle_dim_suspended(bool b) { (void)b; }
static unsigned music_owners;
esp_err_t music_player_acquire_burn_priority(void) { ++music_owners; return ESP_OK; }
void music_player_release_burn_priority(void) { assert(music_owners); --music_owners; }
esp_err_t burner_spi_enter_swd_mode(void);
esp_err_t burner_spi_leave_swd_mode(bool restore);
void burner_spi_block_swd_restore(void);
void burner_spi_allow_swd_restore(void);
bool burner_spi_swd_restore_blocked(void);

/* MinGW lacks fmemopen. This test-only stream owns an immutable copy, matching
 * the production stream's seek/read semantics; ESP uses libc fmemopen. */
static FILE *snapshot_stream(void *data, size_t size, const char *mode)
{
    (void)mode;
    FILE *fp = tmpfile();
    assert(fp && fwrite(data, 1, size, fp) == size);
    rewind(fp);
    return fp;
}
#define fmemopen snapshot_stream
#include "../../main/ag32_batch_programmer.c"

enum fault { NONE, ENTER, CONNECT, HALT, ID_READ, ID_VALUE, UNLOCK,
    ERASE, PROGRAM, VERIFY_READ, VERIFY_DATA, RESET, ID_AND_RESUME, RESTORE };
static enum fault fault;
static bool blocked, held, halted, session, usb_busy;
static unsigned resumes, enters, erases, programs;
static uint32_t control, erase_address;
static uint8_t flash_image[AG32_FLASH_SIZE], option_image[AG32_OPTION_SIZE], ack;
static const char *phase;
static const char *remove_source;

bool usb_msc_tf_in_use_by_host(void) { return usb_busy; }
esp_err_t burner_spi_enter_swd_mode(void)
{ assert(music_owners); ++enters; if (fault == ENTER) return ESP_FAIL; assert(!held); held = true; return ESP_OK; }
esp_err_t burner_spi_leave_swd_mode(bool restore)
{
    assert(held); held = false;
    if (restore && fault == RESTORE) { blocked = true; return ESP_ERR_NO_MEM; }
    if (restore) assert(!halted && !blocked);
    return ESP_OK;
}
void burner_spi_block_swd_restore(void) { blocked = true; }
void burner_spi_allow_swd_restore(void) { blocked = false; }
bool burner_spi_swd_restore_blocked(void) { return blocked; }
uint8_t mcu_debug_last_ack(void) { return ack; }
static esp_err_t transport_failure(void) { ack = 2; return ESP_ERR_TIMEOUT; }
esp_err_t mcu_debug_session_begin(uint32_t *id)
{ if (fault == CONNECT) return transport_failure(); session = true; *id = 0x2ba01477; return ESP_OK; }
void mcu_debug_session_end(void) { assert(session); session = false; }
esp_err_t mcu_debug_halt(void)
{ halted = true; return fault == HALT ? transport_failure() : ESP_OK; }
esp_err_t mcu_debug_resume(void)
{
    ++resumes;
    ack = 1;
    if (fault == ID_AND_RESUME) return ESP_ERR_INVALID_CRC;
    halted = false; return ESP_OK;
}
esp_err_t mcu_debug_system_reset(void)
{ if (fault == RESET) return transport_failure(); halted = false; return ESP_OK; }
esp_err_t mcu_debug_read_memory32(uint32_t address, uint32_t *value)
{
    assert(session);
    if (address == AG32_DEVICE_ID_ADDRESS) {
        if (fault == ID_READ || fault == ID_AND_RESUME) return transport_failure();
        *value = fault == ID_VALUE ? 0 : AG32_DEVICE_ID; return ESP_OK;
    }
    if (address == AG32_FLASH_CR) {
        if (fault == UNLOCK) return transport_failure();
        *value = control; return ESP_OK;
    }
    if (address == AG32_FLASH_SR) { *value = 0; return ESP_OK; }
    if (address >= AG32_FLASH_BASE && address < AG32_FLASH_BASE + AG32_FLASH_SIZE) {
        if (fault == VERIFY_READ && !strcmp(phase, "verify")) return transport_failure();
        memcpy(value, flash_image + address - AG32_FLASH_BASE, 4);
        if (fault == VERIFY_DATA && !strcmp(phase, "verify")) *value ^= 1;
        return ESP_OK;
    }
    assert(address >= AG32_OPTION_BASE && address + 4 <= AG32_OPTION_BASE + AG32_OPTION_SIZE);
    memcpy(value, option_image + address - AG32_OPTION_BASE, 4);
    return ESP_OK;
}
esp_err_t mcu_debug_write_memory32(uint32_t address, uint32_t value)
{
    assert(session && halted);
    if (address == AG32_FLASH_OPTKEYR) { control |= AG32_FLASH_CR_OPTWRE; return ESP_OK; }
    if (address == AG32_FLASH_KEYR) { control &= ~AG32_FLASH_CR_LOCK; return ESP_OK; }
    if (address == AG32_FLASH_SR) return ESP_OK;
    if (address == AG32_FLASH_AR) { erase_address = value; return ESP_OK; }
    if (address == AG32_FLASH_CR) {
        control = value;
        if (value & AG32_FLASH_CR_STRT) {
            if (fault == ERASE) return transport_failure();
            ++erases;
            if (value & AG32_FLASH_CR_OPTER) memset(option_image, 0xff, sizeof(option_image));
            else {
                assert(erase_address >= AG32_FLASH_BASE);
                memset(flash_image + erase_address - AG32_FLASH_BASE, 0xff, AG32_FLASH_SECTOR_SIZE);
            }
            control &= ~AG32_FLASH_CR_STRT;
        }
        return ESP_OK;
    }
    assert(address >= AG32_FLASH_BASE && address + 4 <= AG32_FLASH_BASE + AG32_FLASH_SIZE);
    if (fault == PROGRAM) return transport_failure();
    assert(control & AG32_FLASH_CR_PG);
    ++programs;
    memcpy(flash_image + address - AG32_FLASH_BASE, &value, 4);
    return ESP_OK;
}
esp_err_t mcu_debug_write_memory16(uint32_t address, uint16_t value)
{
    assert(session && halted && (control & AG32_FLASH_CR_OPTPG));
    assert(address >= AG32_OPTION_BASE && address + 2 <= AG32_OPTION_BASE + AG32_OPTION_SIZE);
    memcpy(option_image + address - AG32_OPTION_BASE, &value, 2);
    return ESP_OK;
}
static void progress(const char *name, uint32_t done, uint32_t total, void *ctx)
{
    (void)done; (void)total; (void)ctx;
    phase = name;
    if (remove_source && !strcmp(name, "connect")) {
        assert(remove(remove_source) == 0); remove_source = NULL;
    }
}
static ag32_batch_program_report_t run(const char *path, enum fault injected, bool recovery, int expected)
{
    fault = injected; blocked = recovery; held = halted = session = false;
    resumes = enters = erases = programs = 0; ack = 1; control = AG32_FLASH_CR_OPTWRE;
    memset(flash_image, 0xff, sizeof(flash_image));
    memset(option_image, 0xff, sizeof(option_image));
    ag32_batch_program_report_t report;
    char error[160] = "stale error from previous attempt";
    int err = ag32_batch_program_file(path, progress, NULL, &report, error, sizeof(error));
    if (err != expected || report.error != expected || held || session)
        fprintf(stderr, "fault=%d expected=%d actual=%d report=%d phase=%s text=%s held=%d session=%d\n",
            injected, expected, err, report.error, report.phase, error, held, session);
    assert(err == expected && report.error == expected && !held && !session && !music_owners);
    assert(!strstr(error, "stale error"));
    if (err != ESP_OK) assert(error[0]);
    return report;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *path = argv[1];
    ag32_batch_program_report_t r;
    fail_image_alloc = true;
    r = run(path, NONE, false, ESP_ERR_NO_MEM); assert(!enters && !erases);
    fail_image_alloc = false;
    r = run("missing-ag32-fixture.bin", NONE, false, ESP_ERR_NOT_FOUND); assert(!enters);
    r = run(path, ENTER, false, ESP_FAIL); assert(!resumes && !erases && !blocked);
    r = run(path, CONNECT, false, ESP_ERR_TIMEOUT); assert(!resumes && !blocked && r.swd_ack == 2);
    const enum fault early[] = {HALT, ID_READ, ID_VALUE, UNLOCK};
    for (unsigned i = 0; i < sizeof(early)/sizeof(early[0]); ++i) {
        r = run(path, early[i], false, early[i] == ID_VALUE ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_TIMEOUT);
        assert(resumes == 1 && r.mcu_resumed && !halted && !blocked && !erases && !r.destructive_started);
        if (early[i] != ID_VALUE) assert(r.swd_ack == 2); /* Resume must not overwrite first ACK. */
    }
    r = run(path, ID_AND_RESUME, false, ESP_ERR_TIMEOUT);
    assert(resumes == 1 && blocked && halted && r.cleanup_error == ESP_ERR_INVALID_CRC && r.swd_ack == 2);
    const enum fault late[] = {ERASE, PROGRAM, VERIFY_READ, VERIFY_DATA, RESET};
    for (unsigned i = 0; i < sizeof(late)/sizeof(late[0]); ++i) {
        r = run(path, late[i], false, late[i] == VERIFY_DATA ? ESP_ERR_INVALID_CRC : ESP_ERR_TIMEOUT);
        assert(blocked && halted && !resumes && r.destructive_started);
        if (late[i] == VERIFY_READ || late[i] == VERIFY_DATA) assert(!strcmp(r.phase, "verify"));
    }
    r = run(path, ID_READ, true, ESP_ERR_TIMEOUT); assert(blocked && halted && !resumes);
    r = run(path, RESTORE, false, ESP_ERR_NO_MEM);
    assert(!halted && blocked && r.cleanup_error == ESP_ERR_NO_MEM && !strcmp(r.phase, "restore_spi"));
    r = run(path, NONE, true, ESP_OK);
    assert(!blocked && !halted && r.mcu_resumed && r.record_count == 3 &&
        r.programmed_bytes == 108924 && r.verified_bytes == 108924 && erases && programs);

    /* Close/detach the original TF file before touching SWD. */
    const char *copy = "build/test-ag32-snapshot.bin";
    FILE *existing = fopen(copy, "rb"); assert(existing == NULL);
    FILE *src = fopen(path, "rb"), *dst = fopen(copy, "wb"); assert(src && dst);
    unsigned char buf[4096]; size_t got;
    while ((got = fread(buf, 1, sizeof(buf), src))) assert(fwrite(buf, 1, got, dst) == got);
    fclose(src); fclose(dst);
    remove_source = copy;
    r = run(copy, NONE, false, ESP_OK); assert(!remove_source && r.verified_bytes == 108924);

    assert(ag32_batch_programmer_init() == ESP_OK && ag32_batch_programmer_init() == ESP_OK);
    assert(static_task_creations == 1);
    usb_busy = true; assert(ag32_batch_program_start("/sdcard/a.bin") == ESP_ERR_INVALID_STATE);
    usb_busy = false; s_burn_starting = true;
    assert(ag32_batch_program_start("/sdcard/a.bin") == ESP_ERR_INVALID_STATE);
    s_burn_starting = false;
    assert(ag32_batch_program_start("/sdcard/a.bin") == ESP_OK);
    assert(ag32_batch_program_start("/sdcard/b.bin") == ESP_ERR_INVALID_STATE);
    assert(static_task_creations == 1 && notifications == 1 && !strcmp(s_job_status.path, "/sdcard/a.bin"));
    puts("AG32 programmer: 18 failure/success/snapshot cases and static worker admission passed");
}
