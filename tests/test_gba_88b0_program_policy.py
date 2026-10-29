"""执行实际准备阶段的缓冲区策略，检查 88B0 兼容性和擦除前拦截。"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/burner/core/burn/burner_gba_lowlevel.c").read_text(encoding="utf-8")


def extract(declaration):
    start = SOURCE.index(declaration)
    brace = SOURCE.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


prepare = extract("static esp_err_t burner_bacon_gba_prepare(")
start = prepare.index("    if (!burner_gba_gbx_is_active()) {\n        chislink_intel_compat")
end = prepare.index("    s_cart_ctx.prepared = true;", start)
policy = prepare[start:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
typedef int esp_err_t;
typedef enum { BURNER_NOR_CMDSET_UNKNOWN, BURNER_NOR_CMDSET_AMD, BURNER_NOR_CMDSET_INTEL } burner_nor_cmdset_t;
enum { BURNER_GBA_CMD_DATA_LOW, BURNER_GBA_CMD_DATA_HIGH };
enum { BURNER_JOB_WRITE_ROM, BURNER_JOB_VERIFY_ROM, BURNER_JOB_ERASE_ROM };
enum { BURNER_NOR_FLAG_DISABLE_BUFFER_PROGRAM=1, BURNER_NOR_FLAG_INTEL_88B0=2 };
#define ESP_OK 0
#define ESP_ERR_NOT_SUPPORTED 0x106
#define BURN_GBA_LINEAR_ADDR_BYTES 33554432u
#define BURN_GBA_BANK_BYTES 33554432u
#define BURNER_TAG "test"
#define ESP_LOGI(tag, ...) ((void)printf(__VA_ARGS__), (void)putchar('\n'))
#define ESP_LOGE ESP_LOGI
typedef struct { int mode; } burner_task_param_t;
static struct {
    burner_nor_cmdset_t gba_cmdset;
    int gba_cmd_data_lane;
    bool d0d1_swapped, prepared;
    uint16_t program_buffer_write_bytes;
} s_cart_ctx;
static bool gbx_active, chislink_active;
static bool s_gba_probe_88b0_window, s_gba_active_intel_generic_cfi, s_gba_active_intel_e9_entry;
static bool burner_gba_gbx_is_active(void) { return gbx_active; }
static bool burner_gba_chislink_is_active(void) { return chislink_active; }
static const char *burner_gba_chip_name(const uint8_t *id) { (void)id; return "test"; }
'''
functions = "\n".join(extract(name) for name in (
    "static bool burner_gba_88b0_exact_id(",
    "static bool burner_gba_88b0_cfi_buffer_supported(",
    "static uint16_t burner_gba_program_buffer_write_bytes(",
))
wrapper = r'''
static esp_err_t run_policy(const uint8_t id[8], bool cfi_ok, uint16_t buffer_write_bytes,
                            uint32_t nor_flags, int mode) {
    burner_task_param_t request = {.mode=mode};
    const burner_task_param_t *job = &request;
    uint16_t program_buffer_write_bytes = 0;
    bool chislink_intel_compat = false, native_88b0_buffer_compat = false;
    uint32_t device_size=134217728u, sector_size=262144u;
    s_cart_ctx.prepared = false;
'''
cases = r'''
    s_cart_ctx.prepared = true;
    s_cart_ctx.program_buffer_write_bytes = program_buffer_write_bytes;
    return ESP_OK;
}
static void reset(void) {
    memset(&s_cart_ctx, 0, sizeof(s_cart_ctx));
    s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_INTEL;
    s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
    s_cart_ctx.d0d1_swapped = true;
    gbx_active=chislink_active=s_gba_probe_88b0_window=false;
}
int main(void) {
    const uint8_t exact[] = {0x89,0,0xB0,0x88,4,0,0x89,0};
    const uint8_t other[] = {0x20,0,0x0D,0x88,0xFF,0xFF,0xFF,0xFF};
    reset();
    assert(run_policy(exact,true,1024,BURNER_NOR_FLAG_INTEL_88B0,BURNER_JOB_WRITE_ROM)==ESP_OK);
    assert(s_cart_ctx.prepared && s_cart_ctx.program_buffer_write_bytes==1024);
    /* 共享 ID 前缀不足以放宽其他芯片的写入限制。 */
    for (unsigned i=0; i<8; ++i) {
        uint8_t changed[8]; memcpy(changed,exact,8); changed[i]^=1;
        reset();
        assert(run_policy(changed,true,1024,BURNER_NOR_FLAG_INTEL_88B0,BURNER_JOB_WRITE_ROM)==ESP_ERR_NOT_SUPPORTED);
        assert(!s_cart_ctx.prepared);
    }
    for (unsigned bytes=0; bytes<=2048; bytes=bytes ? bytes*2 : 1) {
        if (bytes==1024) continue;
        reset();
        assert(run_policy(exact,true,bytes,BURNER_NOR_FLAG_INTEL_88B0,BURNER_JOB_WRITE_ROM)==ESP_ERR_NOT_SUPPORTED);
        assert(!s_cart_ctx.prepared);
    }
    reset();
    assert(run_policy(exact,false,1024,BURNER_NOR_FLAG_INTEL_88B0,BURNER_JOB_WRITE_ROM)==ESP_ERR_NOT_SUPPORTED);
    assert(!s_cart_ctx.prepared);
    reset();s_cart_ctx.d0d1_swapped=false;
    assert(run_policy(exact,true,1024,BURNER_NOR_FLAG_INTEL_88B0,BURNER_JOB_WRITE_ROM)==ESP_ERR_NOT_SUPPORTED);
    reset();s_cart_ctx.gba_cmd_data_lane=BURNER_GBA_CMD_DATA_HIGH;
    assert(run_policy(exact,true,1024,BURNER_NOR_FLAG_INTEL_88B0,BURNER_JOB_WRITE_ROM)==ESP_ERR_NOT_SUPPORTED);
    /* 原有无 CFI 窗口和 CHISLINK 路径继续保留缓冲区。 */
    reset();s_gba_probe_88b0_window=true;
    assert(run_policy(exact,false,1024,BURNER_NOR_FLAG_INTEL_88B0,BURNER_JOB_WRITE_ROM)==ESP_OK);
    assert(s_cart_ctx.program_buffer_write_bytes==1024);
    reset();chislink_active=true;
    assert(run_policy(exact,true,1024,BURNER_NOR_FLAG_INTEL_88B0,BURNER_JOB_WRITE_ROM)==ESP_OK);
    assert(s_cart_ctx.program_buffer_write_bytes==1024);
    reset();
    assert(run_policy(other,true,64,0,BURNER_JOB_WRITE_ROM)==ESP_OK);
    assert(s_cart_ctx.program_buffer_write_bytes==64);
    reset();
    assert(run_policy(other,true,64,BURNER_NOR_FLAG_DISABLE_BUFFER_PROGRAM,BURNER_JOB_WRITE_ROM)==ESP_ERR_NOT_SUPPORTED);
    assert(!s_cart_ctx.prepared);
    /* 禁止写入不能阻断只读校验或单独擦除。 */
    for (int mode=BURNER_JOB_VERIFY_ROM; mode<=BURNER_JOB_ERASE_ROM; ++mode) {
        reset();
        assert(run_policy(exact,true,1024,BURNER_NOR_FLAG_INTEL_88B0,mode)==ESP_OK);
        assert(s_cart_ctx.prepared && s_cart_ctx.program_buffer_write_bytes==1024);
        reset();
        assert(run_policy(other,true,64,BURNER_NOR_FLAG_DISABLE_BUFFER_PROGRAM,mode)==ESP_OK);
        assert(s_cart_ctx.prepared && s_cart_ctx.program_buffer_write_bytes==0);
    }
    reset();s_cart_ctx.gba_cmdset=BURNER_NOR_CMDSET_AMD;
    assert(run_policy(other,true,64,BURNER_NOR_FLAG_DISABLE_BUFFER_PROGRAM,BURNER_JOB_WRITE_ROM)==ESP_OK);
    assert(s_cart_ctx.prepared && s_cart_ctx.program_buffer_write_bytes==0);
    puts("88B0 program policy and pre-erase rejection checks passed");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test.c"
    executable = Path(tmp) / "test.exe"
    source.write_text(prefix + functions + wrapper + policy + cases, encoding="utf-8")
    subprocess.run([shutil.which("gcc") or "gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
