"""Exercise production CFI decoding with an emulated NOR query table/bus."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'main/burner/core/burn/burner_gba_lowlevel.c').read_text(encoding='utf-8')

def extract(declaration):
    start = SOURCE.index(declaration)
    pos = SOURCE.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
typedef enum { BURNER_NOR_CMDSET_UNKNOWN, BURNER_NOR_CMDSET_AMD, BURNER_NOR_CMDSET_INTEL } burner_nor_cmdset_t;
typedef struct { uint32_t sector; } burner_nor_geometry_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_TIMEOUT 4
#define BURNER_NOR_GEOMETRY_REGION_MAX 4
#define BURNER_GBA_CMD_DATA_HIGH 1
#define BURNER_GBA_CMD_DATA_LOW 0
#define BURN_GBA_BANK_BYTES 33554432u
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static struct { bool d0d1_swapped; int gba_cmd_data_lane; } s_cart_ctx;
static bool s_gba_cfi_geometry_absent;
static burner_gbabf_probe_state_t s_gbabf;
static uint16_t query[512];
static int fail_address = -1;
static unsigned resets;
static unsigned intel_resets;
static unsigned reset_failure_call;
static bool last_reset_intel;
static uint16_t burner_apply_d0d1_swap_on_read(uint16_t v, bool swap) {
    return swap ? (v & ~3u) | ((v & 1u) << 1) | ((v & 2u) >> 1) : v;
}
#define SWAP_D0D1_U8(v) burner_apply_d0d1_swap_on_read(v, true)
static esp_err_t burner_bacon_rom_read_u16(uint32_t a, uint16_t *v) {
    if ((int)a == fail_address) return ESP_ERR_TIMEOUT;
    assert(a < 512); *v = query[a]; return ESP_OK;
}
static esp_err_t burner_bacon_gba_command_write_u16(uint32_t a, uint16_t v) {
    assert((a == 0x55 || a == 0) && v == 0x98); return ESP_OK;
}
static esp_err_t burner_bacon_rom_write_u16(uint32_t a, uint16_t v) {
    (void)a; (void)v; return ESP_OK;
}
static esp_err_t burner_bacon_gba_intel_reset(void) {
    ++resets; ++intel_resets; last_reset_intel=true;
    return intel_resets==reset_failure_call ? ESP_ERR_TIMEOUT : ESP_OK;
}
static esp_err_t burner_bacon_gba_reset_to_read_mode(void) { ++resets; last_reset_intel=false; return ESP_OK; }
static uint32_t burner_gba_cfi_enter_addr(void) { return 0x55; }
static void burner_nor_geometry_clear(burner_nor_geometry_t *g) { g->sector = 0; }
static burner_nor_cmdset_t burner_nor_cmdset_from_cfi_primary_id(uint16_t id) {
    return id == 1 ? BURNER_NOR_CMDSET_INTEL : id == 2 ? BURNER_NOR_CMDSET_AMD : BURNER_NOR_CMDSET_UNKNOWN;
}
static esp_err_t burner_nor_geometry_build(burner_nor_geometry_t *g, uint32_t bytes,
    uint32_t *counts, uint32_t *sizes, uint32_t n, bool reverse) {
    (void)reverse; uint64_t total = 0;
    for (unsigned i=0; i<n; ++i) total += (uint64_t)counts[i]*sizes[i];
    if (total != bytes) return ESP_ERR_INVALID_SIZE;
    g->sector = sizes[0]; return ESP_OK;
}
static uint32_t burner_nor_geometry_report_sector_size(burner_nor_geometry_t *g) { return g->sector; }
'''

cases = r'''
static esp_err_t run(void) {
    uint32_t size, sector; uint16_t buffer, primary;
    burner_nor_geometry_t geometry; burner_nor_cmdset_t cmd;
    unsigned before = resets;
    esp_err_t result = burner_bacon_gba_get_cfi(&size,&sector,&buffer,&geometry,&cmd,&primary);
    assert(resets > before);
    if (result == ESP_OK) assert(size == 33554432 && sector == 262144 && buffer == 1024 && cmd == BURNER_NOR_CMDSET_INTEL);
    return result;
}
int main(void) {
    // Exercise all eight banks and fragments on both sides of each 32MiB
    // boundary. No wire address or 1024-byte program packet may wrap.
    for (unsigned page=0; page<8; ++page) {
        uint32_t offsets[] = {0, 1022, 33554430};
        for (unsigned i=0; i<3; ++i) {
            uint32_t absolute = page * BURN_GBA_BANK_BYTES + offsets[i];
            uint32_t bank, local, remaining;
            burner_gba_resolve_write_addr(absolute,true,&bank,&local,&remaining);
            assert(bank == page && local == offsets[i]);
            assert(local + remaining == BURN_GBA_BANK_BYTES);
            size_t count = burner_gba_program_safe_chunk_bytes(absolute,65536,1024);
            assert(count && !(count & 1) && count <= remaining);
            assert(local / 1024 == (local + count - 1) / 1024);
        }
    }
    const uint8_t id[8] = {0x89,0,0xB0,0x88,4,0,0x89,0};
    assert(burner_gba_88b0_exact_id(id));
    assert(!burner_gba_88b0_exact_id(NULL));
    for (unsigned i=0; i<8; ++i) { uint8_t other[8]; memcpy(other,id,8); other[i]^=1; assert(!burner_gba_88b0_exact_id(other)); }
    memset(query, 0xFF, sizeof(query));
    query[0x10]=0x52; query[0x11]=0x51; query[0x12]=0x5A;
    s_cart_ctx.d0d1_swapped = true;
    assert(run() == ESP_FAIL && s_gba_cfi_geometry_absent);
    // Stale eligibility cannot survive a bus fault or a missing signature.
    fail_address=0x27;
    assert(run() == ESP_ERR_TIMEOUT && !s_gba_cfi_geometry_absent);
    fail_address=-1; query[0x10]=0xFFFF;
    assert(run() == ESP_FAIL && !s_gba_cfi_geometry_absent);
    query[0x10]=0x52; query[0x13]=2; query[0x14]=0;
    assert(run() == ESP_FAIL && !s_gba_cfi_geometry_absent);
    // A valid swapped Intel table must keep using real CFI geometry.
    query[0x27]=26; // swapped 25 => 32MiB
    query[0x2A]=9;  // swapped 10 => 1024 bytes
    query[0x2C]=2;  // swapped 1 region
    query[0x2D]=127; query[0x2E]=0; query[0x2F]=0; query[0x30]=4;
    assert(run() == ESP_OK && !s_gba_cfi_geometry_absent);
    assert(last_reset_intel && intel_resets >= 2); // Intel CFI must exit via 50/FF, even before probe commit.
    reset_failure_call=intel_resets+2;
    assert(run()==ESP_ERR_TIMEOUT); // A failed return-to-array must not become a successful probe.
    reset_failure_call=0;
    query[0x2D]=0; // inconsistent geometry must be rejected
    assert(run() == ESP_ERR_INVALID_SIZE && !s_gba_cfi_geometry_absent);
    puts("Intel CFI query and 256MiB bank-boundary tests passed");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test.exe'
    probe_header = (ROOT / 'main/burner/core/burn/burner_gbabf_probe.h').read_text(encoding='utf-8')
    c.write_text(probe_header + prefix + '\n'.join(extract(d) for d in (
        'static bool burner_gba_88b0_exact_id(',
        'static bool burner_gba_detect_qry_words(',
        'static esp_err_t burner_bacon_gba_cfi_read_u8(',
        'static esp_err_t burner_bacon_gba_get_cfi(',
        'static void burner_gba_resolve_write_addr(',
        'static size_t burner_gba_program_safe_chunk_bytes(',
    )) + cases, encoding='utf-8')
    subprocess.run([shutil.which('gcc') or 'gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
