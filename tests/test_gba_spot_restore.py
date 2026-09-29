"""Run the production spot-test recovery flow against an aliased/faulting NOR."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'main/burner/core/burn/burner_gba_spot_test.c').read_text(encoding='utf-8')
read_source = (ROOT / 'main/burner/core/burn/burner_gba_flash_lowlevel.c').read_text(encoding='utf-8')
start = read_source.index('esp_err_t burner_bacon_gba_verify_read_block_hoststyle(')
end = read_source.index('\nstatic esp_err_t burner_bacon_gba_reset_aso_diag', start)
read_source = read_source[start:end]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#ifdef _WIN32
#include <io.h>
#define fsync _commit
#define open(path,flags,mode) _open(path,(flags)|_O_BINARY,mode)
#endif
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_INVALID_RESPONSE 5
#define ESP_ERR_TIMEOUT 6
#define ESP_ERR_INVALID_SIZE 7
#define BURN_GBA_BANK_BYTES 33554432u
#define BURNER_GBA_88B0_SUPPORTED_BYTES 268435456u
#define BURNER_ROM_POLL_TIMEOUT_MS 2000
#define BURNER_ROM_POLL_INTERVAL_US 50
#define BURNER_JOB_READ_ROM 1
#define BURNER_CART_MODE_GBA 1
#define BURNER_RECIPE_MODE_CHIS 1
static int s_burn_recipe_mode_default = BURNER_RECIPE_MODE_CHIS;
#define BURNER_NOR_CMDSET_INTEL 2
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef struct { int mode, cart_mode, recipe_mode; uint32_t total_bytes; } burner_task_param_t;
typedef struct {
 bool backup_verified, destructive_started;
 uint32_t points_total, recipe_id;
 uint32_t markers_verified, sectors_restored, sectors_verified, mismatch_address;
 esp_err_t test_error, restore_error;
} burner_gba_spot_report_t;
static bool s_gba_probe_88b0_window = true;
typedef struct { uint32_t addr_begin, addr_end, sector_size; } burner_nor_region_t;
typedef struct { unsigned region_count; burner_nor_region_t regions[2]; } burner_nor_geometry_t;
static struct {
 int gba_cmdset; uint32_t sector_size, program_buffer_write_bytes; bool d0d1_swapped;
 bool probe_cfi_ok; uint32_t device_size, buffer_write_bytes; burner_nor_geometry_t geometry;
} s_cart_ctx;
static bool m36;
static bool burner_nor_geometry_is_valid(const burner_nor_geometry_t *g) { return g->region_count==2; }
static esp_err_t burner_bacon_gba_read_id(uint8_t *id, bool swapped) {
 (void)swapped; const uint8_t expected[8]={0x20,0,0x0d,0x88,0xff,0xff,0xff,0xff}; memcpy(id,expected,8); return ESP_OK;
}
static esp_err_t burner_bacon_gba_intel_reset(void) { return ESP_OK; }
static uint8_t flash[16*262144], initial[16*262144];
static unsigned banks, program_calls, erase_calls, fail_program, fail_erase;
static unsigned selected_bank;
static unsigned raw_phase, raw_words, raw_pages;
static uint32_t raw_base;
static uint16_t raw_status = 0x80;
static int64_t mock_time;
static int64_t esp_timer_get_time(void) { return mock_time += 100000; }
static void esp_rom_delay_us(unsigned us) { (void)us; }
static void burner_task_yield_if_due(void) {}
static uint16_t burner_apply_d0d1_swap_on_read(uint16_t v, bool swap) {
 return swap ? (v & ~3u) | ((v & 1u)<<1) | ((v & 2u)>>1) : v;
}
static bool burner_gba_intel_status_has_error(uint16_t v) { return (v & 0x3Au) != 0; }
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static esp_err_t burner_spi_prepare_burn_gba(const burner_task_param_t *job) {
 assert((job->total_bytes == 268435456 || job->total_bytes == 2) && job->mode == BURNER_JOB_READ_ROM); return ESP_OK;
}
static uint8_t *location(uint32_t address, size_t n) {
 unsigned bank = address / BURN_GBA_BANK_BYTES, off = address % BURN_GBA_BANK_BYTES;
 bool end = off >= BURN_GBA_BANK_BYTES - 262144u;
 unsigned within = end ? off - (BURN_GBA_BANK_BYTES - 262144u) : off;
 assert(bank < 8 && within + n <= 262144u);
 return flash + ((bank % banks)*2 + end)*262144u + within;
}
static void burner_gba_resolve_write_addr(uint32_t a,bool multi,uint32_t *bank,uint32_t *local,uint32_t *remaining) {
 assert(multi != m36); *bank=a/BURN_GBA_BANK_BYTES; *local=a%BURN_GBA_BANK_BYTES; *remaining=BURN_GBA_BANK_BYTES-*local;
}
static esp_err_t burner_cancel_poll(void) { return ESP_OK; }
static esp_err_t burner_bacon_rom_verify_read_u16_batched_hoststyle(uint32_t word,uint8_t *out,size_t count) {
 // Hardware cannot stream past the low 16-bit word-address counter.
 assert(((word*2u)&0x1FFFFu)+count*2u <= 0x20000u);
 memcpy(out,location(selected_bank*BURN_GBA_BANK_BYTES+word*2u,count*2u),count*2u); return ESP_OK;
}
static esp_err_t burner_bacon_rom_read_u16(uint32_t word,uint16_t *value) {
 if (raw_phase == 6) { assert(word==raw_base); *value=raw_status; return ESP_OK; }
 memcpy(value,location(selected_bank*BURN_GBA_BANK_BYTES+word*2u,2),2); return ESP_OK;
}
static esp_err_t burner_bacon_rom_write_u16(uint32_t word, uint16_t value) {
 if (raw_phase==0) { assert(value==0x50); raw_base=word; raw_phase=1; }
 else if (raw_phase==1) { assert(word==raw_base && value==0xFF); raw_phase=2; }
 else if (raw_phase==2) { assert(word==raw_base && value==(m36 ? 0x40 : 0xEA)); raw_phase=m36 ? 7 : 3; ++raw_pages; }
 else if (raw_phase==3) { assert(word==raw_base && value==0x1FF); raw_phase=4; raw_words=0; }
 else if (raw_phase==4) {
  assert(word==raw_base+raw_words); uint8_t *p=location(selected_bank*BURN_GBA_BANK_BYTES+word*2u,2);
  p[0]&=(uint8_t)value; p[1]&=(uint8_t)(value>>8);
  if (++raw_words==512) raw_phase=5;
 } else if (raw_phase==7) {
  assert(word==raw_base); uint8_t *p=location(word*2u,2); p[0]&=(uint8_t)value; p[1]&=(uint8_t)(value>>8); raw_phase=6;
 } else if (raw_phase==5) { assert(word==raw_base && value==0xD0); raw_phase=6; }
 else { assert(raw_phase==6 && word==raw_base && value==0xFF); raw_phase=0; }
 return ESP_OK;
}
static esp_err_t burner_bacon_gba_erase_sector(uint32_t a, bool multi, uint32_t timeout) {
 assert(multi != m36 && timeout == 10000); ++erase_calls;
 raw_phase=0;
 size_t n=m36 ? (a < 0x20000u ? 32768u : 131072u) : 262144u;
 memset(location(a,n),0xFF,n);
 return erase_calls == fail_erase ? ESP_FAIL : ESP_OK;
}
static esp_err_t burner_bacon_gba_program_block(const uint8_t *in, size_t n, uint32_t a, bool multi, bool prepare) {
 assert(multi != m36 && !prepare); ++program_calls;
 if (m36) assert(s_cart_ctx.program_buffer_write_bytes==64 && n==1024);
 uint8_t *out=location(a,n); size_t count=program_calls == fail_program ? n/2 : n;
 for (size_t i=0; i<count; ++i) out[i] &= in[i];
 return program_calls == fail_program ? ESP_FAIL : ESP_OK;
}
static esp_err_t burner_gba_switch_bank_if_needed(uint32_t bank) { assert(bank<8); selected_bank=bank; return ESP_OK; }
static esp_err_t burner_bacon_finish_cart_access(void) { return ESP_OK; }
'''
cases = r'''
static void setup(unsigned bank_count, unsigned program_fault, unsigned erase_fault) {
 banks=bank_count; program_calls=erase_calls=0; fail_program=program_fault; fail_erase=erase_fault;
 m36=false; s_gba_probe_88b0_window=true;
 s_cart_ctx.gba_cmdset=2; s_cart_ctx.sector_size=262144; s_cart_ctx.program_buffer_write_bytes=1024;
 s_cart_ctx.d0d1_swapped=true; s_cart_ctx.probe_cfi_ok=false; s_cart_ctx.device_size=268435456;
 raw_phase=raw_pages=0; raw_status=0x80; mock_time=0;
 for (unsigned i=0; i<sizeof(flash); ++i) flash[i]=(uint8_t)((i*31u) ^ (i>>12) ^ (i>>18));
 memcpy(initial,flash,sizeof(flash));
}
int main(void) {
 burner_gba_spot_report_t r;
 setup(8,0,0);
 assert(burner_gba_88b0_spot_test_locked("success.bin",&r)==ESP_OK);
 assert(r.backup_verified && r.markers_verified==16 && r.sectors_restored==16 && r.sectors_verified==16);
 assert(!memcmp(flash,initial,sizeof(flash)));
 // Replay raw 50/FF/EA/1FF, 512 halfwords, D0/FF at all 16 points.
 setup(8,0,0);
 assert(burner_gba_88b0_gbabf_test_locked("gbabf.bin",&r)==ESP_OK);
 assert(raw_pages==16 && r.markers_verified==16 && r.sectors_verified==16);
 assert(!memcmp(flash,initial,sizeof(flash)));
 setup(8,0,0); raw_status=0x81; // swapped DQ1: protected
 assert(burner_gba_88b0_gbabf_test_locked("gbabf-protect.bin",&r)==ESP_ERR_INVALID_RESPONSE);
 assert(r.markers_verified==0 && r.sectors_verified==16 && !memcmp(flash,initial,sizeof(flash)));
 setup(8,0,0); raw_status=0xFFFF;
 assert(burner_gba_88b0_gbabf_test_locked("gbabf-timeout.bin",&r)==ESP_ERR_TIMEOUT);
 assert(r.markers_verified==0 && r.sectors_verified==16 && !memcmp(flash,initial,sizeof(flash)));
 // Existing backup must be preserved and must prevent any cartridge erase.
 setup(8,0,0);
 assert(burner_gba_88b0_spot_test_locked("success.bin",&r)!=ESP_OK);
 assert(!r.destructive_started && erase_calls==0 && !memcmp(flash,initial,sizeof(flash)));
 // A 128MiB or 64MiB physical device must fail a claimed 256MiB test,
 // yet all original physical sectors must be restored after detection.
 setup(4,0,0);
 assert(burner_gba_88b0_spot_test_locked("alias128.bin",&r)==ESP_ERR_INVALID_RESPONSE);
 assert(r.mismatch_address!=UINT32_MAX && r.markers_verified<16 && r.sectors_verified==16);
 assert(!memcmp(flash,initial,sizeof(flash)));
 setup(2,0,0);
 assert(burner_gba_88b0_spot_test_locked("alias64.bin",&r)==ESP_ERR_INVALID_RESPONSE);
 assert(r.sectors_verified==16 && !memcmp(flash,initial,sizeof(flash)));
 // Partially completed erase and program failures still trigger recovery.
 setup(8,1,0);
 assert(burner_gba_88b0_spot_test_locked("partial-program.bin",&r)==ESP_FAIL);
 assert(r.destructive_started && r.restore_error==ESP_OK && r.sectors_verified==16);
 assert(!memcmp(flash,initial,sizeof(flash)));
 setup(8,0,1);
 assert(burner_gba_88b0_spot_test_locked("partial-erase.bin",&r)==ESP_FAIL);
 assert(r.destructive_started && r.sectors_verified==16 && !memcmp(flash,initial,sizeof(flash)));
 // A restoration fault must be reported; never label it a passing test.
 setup(8,17,0);
 assert(burner_gba_88b0_spot_test_locked("restore-fault.bin",&r)!=ESP_OK);
 assert(r.restore_error!=ESP_OK && r.sectors_verified<16);
 // The live replacement card uses valid mixed CFI and Type 10 word writes.
 setup(8,0,0); m36=true; s_gba_probe_88b0_window=false;
 s_cart_ctx.probe_cfi_ok=true; s_cart_ctx.device_size=33554432; s_cart_ctx.buffer_write_bytes=64;
 s_cart_ctx.geometry=(burner_nor_geometry_t){2,{{0,0x20000,32768},{0x20000,33554432,131072}}};
 assert(burner_gba_88b0_gbabf_test_locked("m36-word.bin",&r)==ESP_OK);
 assert(r.points_total==4 && r.recipe_id==3 && r.markers_verified==4 && r.sectors_verified==4);
 assert(raw_pages>2048 && !memcmp(flash,initial,sizeof(flash)));
 s_cart_ctx.program_buffer_write_bytes=0;
 assert(burner_gba_m36_native_test_locked("m36-native.bin",&r)==ESP_OK);
 assert(r.recipe_id==4 && r.points_total==4 && r.markers_verified==4 && r.sectors_verified==4);
 assert(s_cart_ctx.program_buffer_write_bytes==0 && !memcmp(flash,initial,sizeof(flash)));
 fail_program=program_calls+1;
 assert(burner_gba_m36_native_test_locked("m36-native-fail.bin",&r)==ESP_FAIL);
 assert(r.markers_verified==0 && r.sectors_verified==4 && s_cart_ctx.program_buffer_write_bytes==0);
 assert(!memcmp(flash,initial,sizeof(flash)));
 puts("Spot-test success, alias detection, no-overwrite and recovery fault tests passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c, exe = Path(tmp)/'test.c', Path(tmp)/'test.exe'
    c.write_text(prefix+read_source+source+cases, encoding='utf-8')
    subprocess.run([shutil.which('gcc') or 'gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=tmp,check=True)
