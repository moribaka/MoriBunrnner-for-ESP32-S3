/* Production sector pipeline against a NOR model: programming may only clear
 * bits, erase destroys a whole sector, and prefetch owns a distinct slot. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "../../main/burner/core/burner_source_reader.h"
enum { BURNER_CART_MODE_GBA, BURNER_CART_MODE_MBC5, BURNER_WRITE_PATH_DIRECT = 10,
       MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, BURNER_STATE_BURNING, BURNER_STATE_ERROR };
#define BURN_GBA_PROGRAM_CHUNK_BYTES 65536
typedef struct {
    uint32_t addr_begin, total_bytes, source_size;
    int cart_mode, write_path;
    bool erase_always;
    const char *rom_name, *rom_path;
    uint32_t mbc5_program_chunk_bytes;
} burner_task_param_t;
typedef struct { uint32_t begin, size; } burner_nor_region_cursor_t;
typedef struct { FILE *fp; bool running, inflight; uint64_t read_us; esp_err_t err; } burner_tf_reader_ctx_t;
static struct { int geometry; uint16_t program_buffer_write_bytes, buffer_write_bytes; uint32_t device_size; } s_cart_ctx;
static struct { uint32_t skipped_ff_bytes; } s_gba_chis_diag;
enum { FLASH_BYTES = 65536 + 131072 + 32768 };
static uint8_t flash[FLASH_BYTES], want[FLASH_BYTES], source[FLASH_BYTES];
static uint32_t sizes[] = {65536, 131072, 32768}, source_size, matched, skipped, erased, programmed, advanced;
static bool bad_read, bad_erase, bad_program, cancelled;
static unsigned reader_starts, reader_stops;
static int active_mode;
static void *heap_caps_malloc(size_t size, int caps) { (void)caps; return malloc(size); }
static uint64_t esp_timer_get_time(void) { static uint64_t n; return ++n; }
static bool burner_nor_geometry_is_valid(const int *g) { (void)g; return true; }
static esp_err_t burner_nor_geometry_largest_sector_size_in_range(const int *g, uint32_t a, uint32_t n, uint32_t *out) {
    (void)g; (void)a; (void)n; *out = 131072; return ESP_OK;
}
static esp_err_t burner_nor_geometry_region_cursor_begin(const int *g, uint32_t a, burner_nor_region_cursor_t *c) {
    (void)g; uint32_t start = 0;
    for (unsigned i=0; i<3; ++i) { if (a < start + sizes[i]) { c->begin=start; c->size=sizes[i]; return ESP_OK; } start+=sizes[i]; }
    return ESP_ERR_INVALID_ARG;
}
static esp_err_t burner_nor_geometry_sector_bounds_in_cursor(burner_nor_region_cursor_t *c, uint32_t a,
    uint32_t *begin, uint32_t *end, uint32_t *size) {
    assert(a >= c->begin && a < c->begin+c->size);
    if (begin) *begin=c->begin;
    if (end) *end=c->begin+c->size;
    if (size) *size=c->size;
    return ESP_OK;
}
static uint32_t burner_nor_geometry_sector_count_from_range(const int *g, uint32_t a, uint32_t z) {
    uint32_t count=0; burner_nor_region_cursor_t c;
    while (a<=z) { assert(burner_nor_geometry_region_cursor_begin(g,a,&c)==ESP_OK); a=c.begin+c.size; ++count; }
    return count;
}
static uint32_t burner_nor_geometry_erase_bytes_from_range(const int *g, uint32_t a, uint32_t z) {
    burner_nor_region_cursor_t c,d; burner_nor_geometry_region_cursor_begin(g,a,&c); burner_nor_geometry_region_cursor_begin(g,z,&d);
    return d.begin+d.size-c.begin;
}
static uint32_t burner_nor_geometry_report_sector_size(const int *g) { (void)g; return 0; }
static bool burner_is_gba_multi_card(const burner_task_param_t *j) { (void)j; return false; }
static void burner_spi_lock_take(void) {}
static void burner_spi_lock_give(void) {}
static void burner_task_yield_if_due(void) {}
static esp_err_t burner_cancel_poll(void) { return cancelled ? ESP_ERR_INVALID_STATE : ESP_OK; }
static void burner_status_mark_erase_begin(void) {}
static void burner_status_mark_erase_end(void) {}
static void burner_status_mark_write_begin(void) {}
static uint64_t burner_status_mark_write_end(void) { return 1; }
static void burner_status_mark_write_manual_begin(void) {}
static void burner_status_record_erase_sectors(uint32_t n, uint32_t size) { (void)size; erased+=n; }
static void burner_status_record_write_skipped(uint32_t n) { skipped+=n; }
static void burner_status_record_write_matched(uint32_t n) { matched+=n; }
static void burner_status_record_write_sample(uint32_t n, uint64_t us) { (void)us; programmed+=n; }
static void burner_status_record_tf_to_psram_copy(uint32_t n, uint64_t us) { (void)n; (void)us; }
static void burner_status_plan_erase_phase(uint32_t n, uint32_t bytes, uint32_t size) { (void)n; (void)bytes; (void)size; }
static void burner_status_advance_erase_phase(uint32_t n, uint32_t size) { (void)size; advanced+=n; }
static int burner_calc_progress_percent_u64(uint32_t done, uint32_t total) { return 100ull*done/total; }
static void burner_status_update(int state, int progress, uint32_t done, uint32_t total,
    const char *message, const char *name, const char *path) {
    (void)state; (void)progress; (void)done; (void)total; (void)message; (void)name; (void)path;
}
static void burner_emit_progress_cb(int p, uint32_t n) { (void)p; (void)n; }
static void burner_gba_chis_diag_add_erase(uint64_t n) { (void)n; }
static void burner_gba_chis_diag_add_tf_read(uint64_t n) { (void)n; }
static void burner_gba_chis_diag_add_prefetch_wait(uint64_t n) { (void)n; }
static uint32_t burner_erase_timeout_ms_for_bytes(uint32_t n) { return n; }
static esp_err_t read_flash(uint8_t *out, size_t n, uint32_t a) {
    if (bad_read) return ESP_FAIL;
    assert(a+n<=FLASH_BYTES); memcpy(out,flash+a,n); return ESP_OK;
}
static esp_err_t burner_bacon_gba_verify_read_block_hoststyle(uint8_t *out, size_t n, uint32_t a, bool multi) {
    (void)multi; return read_flash(out,n,a);
}
static esp_err_t burner_gbc_blank_read(uint8_t *out, size_t n, uint32_t a) { return read_flash(out,n,a); }
static esp_err_t burner_bacon_mbc5_read_block_hoststyle(uint8_t *out, size_t n, uint32_t a) { return read_flash(out,n,a); }
static esp_err_t erase_flash(uint32_t a) {
    if (bad_erase) return ESP_FAIL;
    burner_nor_region_cursor_t c = {0}; assert(burner_nor_geometry_region_cursor_begin(&s_cart_ctx.geometry,a,&c)==ESP_OK);
    assert(a==c.begin); memset(flash+a,0xff,c.size); return ESP_OK;
}
static esp_err_t burner_bacon_gba_erase_sector(uint32_t a, bool m, uint32_t t) { (void)m; (void)t; return erase_flash(a); }
static esp_err_t burner_bacon_mbc5_erase_sector(uint32_t a, uint32_t t) { (void)t; return erase_flash(a); }
static esp_err_t program_flash(const uint8_t *data, size_t n, uint32_t a) {
    if (bad_program) return ESP_FAIL;
    assert(a+n<=FLASH_BYTES);
    assert((a % 64u) == 0 && (n % 64u) == 0); // No byte-sized spans inside a buffered page.
    for (size_t i=0; i<n; ++i) { assert((flash[a+i]&data[i])==data[i]); flash[a+i]&=data[i]; }
    return ESP_OK;
}
static esp_err_t burner_bacon_gba_program_block(const uint8_t *data,size_t n,uint32_t a,bool m,bool prepare) {
    (void)m; assert(!prepare && !(a&1) && !(n&1)); return program_flash(data,n,a);
}
static esp_err_t burner_bacon_mbc5_program_erased_block(const uint8_t *data,size_t n,uint32_t a,uint32_t *written) {
    *written=n; return program_flash(data,n,a);
}
static bool burner_gbc_gbx_is_active(void) { return false; }
static esp_err_t burner_gbc_gbx_reset_to_read_mode(bool all, uint32_t n) { (void)all; (void)n; return ESP_OK; }
static esp_err_t burner_bacon_gba_finalize_write(bool m) { (void)m; return ESP_OK; }
static FILE *burner_file_open_read(const char *path) { return fopen(path,"rb"); }
static void burner_tf_reader_set_source_size(uint32_t n) { source_size=n; }
static esp_err_t burner_tf_read_exact(FILE *fp,uint8_t *dst,size_t n) { return burner_source_read_exact(fp,dst,n,source_size); }
static esp_err_t burner_tf_reader_start(burner_tf_reader_ctx_t *c,FILE *fp) { c->fp=fp; c->running=true; ++reader_starts; return ESP_OK; }
static esp_err_t burner_tf_reader_submit(burner_tf_reader_ctx_t *c,uint8_t *dst,size_t n) {
    assert(c->running && !c->inflight); c->inflight=true; c->read_us=1;
    c->err=burner_tf_read_exact(c->fp,dst,n); return ESP_OK;
}
static esp_err_t burner_tf_reader_wait(burner_tf_reader_ctx_t *c) { assert(c->inflight); c->inflight=false; return c->err; }
static void burner_tf_reader_stop(burner_tf_reader_ctx_t *c) {
    if (!c->running) return;
    if (c->inflight) (void)burner_tf_reader_wait(c);
    c->running=false; ++reader_stops;
}
static void burner_apply_write_transform(const burner_task_param_t *j,uint8_t *data,size_t n,uint32_t offset) {
    if (j->cart_mode==BURNER_CART_MODE_GBA && offset<=12345 && n>12345-offset) data[12345-offset]^=1;
}
#include "../../main/burner/core/burn/burner_write_pipeline.c"
static void reset(void) {
    matched=skipped=erased=programmed=advanced=reader_starts=reader_stops=0;
    bad_read=bad_erase=bad_program=cancelled=false;
    s_cart_ctx.program_buffer_write_bytes=active_mode==BURNER_CART_MODE_MBC5 ? 0 : 64;
    s_cart_ctx.buffer_write_bytes=64; s_cart_ctx.device_size=FLASH_BYTES;
    for (size_t i=0; i<FLASH_BYTES; ++i) flash[i]=(i*37)^(i>>8);
}
static void file(size_t n) { FILE *fp=fopen("write_fixture.bin","wb"); assert(fp); assert(fwrite(source,1,n,fp)==n); fclose(fp); }
int main(void) {
    for (int mode=0; mode<2; ++mode) for (int overlap=0; overlap<2; ++overlap) {
        active_mode=mode;
        reset(); memcpy(source,flash,FLASH_BYTES);
        burner_task_param_t j={0,FLASH_BYTES,FLASH_BYTES,mode,overlap?20:BURNER_WRITE_PATH_DIRECT,false,"fixture","write_fixture.bin",16384};
        if (mode==BURNER_CART_MODE_GBA) flash[12345]^=1;
        memcpy(want,flash,FLASH_BYTES); file(FLASH_BYTES);
        assert(burner_write_sector_pipeline(&j)==ESP_OK);
        assert(matched==FLASH_BYTES && !erased && !programmed && memcmp(flash,want,FLASH_BYTES)==0);
        assert(reader_starts==(unsigned)overlap && reader_stops==reader_starts);
        reset(); memcpy(want,flash,FLASH_BYTES); j.addr_begin=100; j.total_bytes=j.source_size=20003;
        memcpy(source,flash+100,j.total_bytes); source[17]^=1; memcpy(want+100,source,j.total_bytes);
        if (mode==BURNER_CART_MODE_GBA) want[100+12345]^=1;
        file(j.total_bytes); assert(burner_write_sector_pipeline(&j)==ESP_OK);
        assert(erased==1 && memcmp(flash,want,FLASH_BYTES)==0 && reader_stops==reader_starts);
        reset(); j.addr_begin=0; j.total_bytes=j.source_size=FLASH_BYTES; j.erase_always=true;
        memset(source,0xff,FLASH_BYTES); memcpy(want,source,FLASH_BYTES);
        if (mode==BURNER_CART_MODE_GBA) want[12345]^=1;
        file(FLASH_BYTES); assert(burner_write_sector_pipeline(&j)==ESP_OK);
        assert(erased==3 && !matched && memcmp(flash,want,FLASH_BYTES)==0);
        assert(skipped>FLASH_BYTES-128);
        reset(); j.erase_always=false; memset(flash,0xff,FLASH_BYTES);
        for (size_t i=0; i<FLASH_BYTES; ++i) source[i]=(i*19)^(i>>8);
        memcpy(want,source,FLASH_BYTES); if (mode==BURNER_CART_MODE_GBA) want[12345]^=1;
        file(FLASH_BYTES); assert(burner_write_sector_pipeline(&j)==ESP_OK && !erased && programmed);
        assert(memcmp(flash,want,FLASH_BYTES)==0);
    }
    for (int fault=0; fault<5; ++fault) {
        reset(); memset(source,0,FLASH_BYTES); file(fault==0?1:FLASH_BYTES);
        burner_task_param_t j={0,FLASH_BYTES,FLASH_BYTES,BURNER_CART_MODE_MBC5,20,false,"fixture","write_fixture.bin",16384};
        bad_read=fault==1; bad_erase=fault==2; bad_program=fault==3; cancelled=fault==4;
        assert(burner_write_sector_pipeline(&j)!=ESP_OK && reader_stops==reader_starts);
        if (fault<3) assert(!programmed);
    }
    remove("write_fixture.bin");
    puts("Production sector pipeline: mixed sectors, identical/blank/force, partial preservation, patch transform, prefetch lifetime and failures passed");
}
