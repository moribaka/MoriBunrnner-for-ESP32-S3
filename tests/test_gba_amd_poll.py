"""Compile the production paired-read encoder and poll loop with a scripted bus."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, name):
    source = (ROOT / path).read_text(encoding="utf-8")
    start = source.index("static ", source.index(name + "(") - 22)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_INVALID_STATE 3
#define BURNER_SPI_CS_MODE_0 0
#define BURNER_ROM_POLL_INTERVAL_US 50
static bool s_gba_amd_poll_pair_enabled;
static uint16_t samples[16];
static unsigned count, next, transfers, cancel_after;
static int bus_error;
static int64_t now;
static int64_t esp_timer_get_time(void) { return now += 10; }
static void esp_rom_delay_us(unsigned us) { now += us; }
static void burner_task_yield_if_due(void) {}
static esp_err_t burner_cancel_poll(void) {
    return transfers >= cancel_after ? ESP_ERR_INVALID_STATE : ESP_OK;
}
static uint16_t sample(void) {
    unsigned index = next < count ? next++ : count - 1;
    return samples[index];
}
static esp_err_t burner_bacon_rom_read_u16(uint32_t address, uint16_t *out) {
    assert(address == 0x123456); ++transfers;
    if (bus_error) return bus_error;
    *out = sample(); return ESP_OK;
}
static esp_err_t burner_spi_transfer_cs_legacy(int mode, const uint8_t *tx, uint8_t *rx, size_t len) {
    assert(mode == 0 && len == 20); ++transfers;
    if (bus_error) return bus_error;
    for (size_t base = 0; base < 20; base += 10) {
        // Address setup, RD low, sample, release, then repeat the same address.
        assert(tx[base] == 0xFF && tx[base+1] == 0x56 && tx[base+2] == 0x34 && tx[base+3] == 0x12);
        assert(tx[base+4] == 0x3B && tx[base+5] == 0x29 && tx[base+6] == 0xAB && tx[base+9] == 0x3F);
        uint16_t value = sample(); rx[base+7] = value; rx[base+8] = value >> 8;
    }
    return ESP_OK;
}
'''

cases = r'''
static void reset(bool pair, uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    s_gba_amd_poll_pair_enabled = pair;
    samples[0]=a; samples[1]=b; samples[2]=c; samples[3]=d;
    count=4; next=transfers=0; cancel_after=100000; bus_error=0; now=0;
}
int main(void) {
    for (int pair = 0; pair <= 1; ++pair) {
        uint16_t status = 0;
        reset(pair, 0x0080, 0x0080, 0x0080, 0x0080);
        assert(burner_bacon_gba_amd_wait_program_complete(0x123456,0x80,2,&status)==ESP_OK && status==0x80);
        assert(transfers == (pair ? 1u : 2u));
        reset(pair, 0, 0, 0x80, 0x80);
        assert(burner_bacon_gba_amd_wait_program_complete(0x123456,0x80,2,NULL)==ESP_OK);
        reset(pair, 0x80, 0x80, 0, 0);
        assert(burner_bacon_gba_amd_wait_program_complete(0x123456,0,2,NULL)==ESP_OK);
        reset(pair, 0x20, 0x80, 0x80, 0x80);
        assert(burner_bacon_gba_amd_wait_program_complete(0x123456,0x80,2,&status)==ESP_OK && status==0x80);
        reset(pair, 0xA0, 0x20, 0x80, 0x80);
        assert(burner_bacon_gba_amd_wait_program_complete(0x123456,0x80,2,&status)==ESP_OK && next==3);
        reset(pair, 0x20, 0x20, 0x20, 0x20);
        assert(burner_bacon_gba_amd_wait_program_complete(0x123456,0x80,2,NULL)==ESP_ERR_TIMEOUT);
        reset(pair, 0, 0, 0, 0);
        assert(burner_bacon_gba_amd_wait_program_complete(0x123456,0x80,1,NULL)==ESP_ERR_TIMEOUT);
        reset(pair, 0, 0, 0, 0); bus_error=99;
        assert(burner_bacon_gba_amd_wait_program_complete(0x123456,0x80,2,NULL)==99);
        reset(pair, 0, 0, 0, 0); cancel_after=0;
        assert(burner_bacon_gba_amd_wait_program_complete(0x123456,0x80,2,NULL)==ESP_ERR_INVALID_STATE && transfers==0);
        reset(pair, 0, 0, 0, 0); cancel_after=1;
        assert(burner_bacon_gba_amd_wait_program_complete(0x123456,0x80,2,NULL)==ESP_ERR_INVALID_STATE);
        reset(pair, 0, 0, 0, 0);
        assert(burner_bacon_gba_amd_wait_program_complete(0x123456,0x80,0,NULL)==ESP_ERR_TIMEOUT && transfers==0);
    }
    puts("AMD poll: paired encoding, DQ7 polarity, busy, DQ5 race, errors, timeout and cancellation passed");
}
'''

low = "main/burner/core/burn/burner_gba_lowlevel.c"
flash = "main/burner/core/burn/burner_gba_flash_lowlevel.c"
source = "\n".join([prefix, function(low, "burner_bacon_rom_read_u16_pair"),
                    function(flash, "burner_gba_amd_status_matches_dq7"),
                    function(flash, "burner_bacon_gba_amd_wait_program_complete"), cases])
with tempfile.TemporaryDirectory(prefix="mori-poll-") as tmp:
    cfile = Path(tmp) / "poll.c"
    binary = Path(tmp) / "poll.exe"
    cfile.write_text(source, encoding="utf-8")
    subprocess.run([shutil.which("gcc") or "gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
