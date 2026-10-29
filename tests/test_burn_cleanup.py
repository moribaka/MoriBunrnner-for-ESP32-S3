"""执行生产代码，验证单路读回、擦除前边界检查和 GBX 换卡处理。"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BURN = (ROOT / "main/burner/core/burn/burner_gba_burn.c").read_text(encoding="utf-8")
GBC = (ROOT / "main/burner/core/burn/burner_gbc_burn.c").read_text(encoding="utf-8")
LOW = (ROOT / "main/burner/core/burn/burner_gba_lowlevel.c").read_text(encoding="utf-8")


def extract(source, declaration):
    start = source.index(declaration)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def compile_run(text):
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "test.c"
        exe = Path(tmp) / "test.exe"
        source.write_text(text, encoding="utf-8")
        subprocess.run([shutil.which("gcc") or "gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_TIMEOUT 5
#define BURNER_NOR_CMDSET_INTEL 1
#define BURNER_TAG "test"
#define ESP_LOGW(tag, ...) ((void)printf(__VA_ARGS__), (void)putchar('\n'))
#define ESP_LOGD ESP_LOGW
#define ESP_LOGI ESP_LOGW
#define burner_status_update(...) ((void)0)
typedef struct {
    uint32_t total_bytes, addr_begin;
    void *gba_patch_plan;
} burner_task_param_t;
static struct { bool probe_cfi_ok; int gba_cmdset; uint32_t device_size; } s_cart_ctx;
static bool s_gba_probe_88b0_window, intel, gbx;
static uint8_t expected[512];
static unsigned reads, resets, locks, patches;
static int read_error, reset_error, prepare_error;
static bool mismatch, recover;
static void burner_spi_lock_take(void) { ++locks; }
static void burner_spi_lock_give(void) { --locks; }
static bool burner_is_gba_multi_card(const burner_task_param_t *job) { return job->addr_begin != 0; }
static bool burner_gba_gbx_is_active(void) { return gbx; }
static bool burner_gba_nor_is_intel_active(void) { return intel; }
static const char *esp_err_to_name(int err) { (void)err; return "error"; }
static void burner_apply_gba_patch_plan(uint8_t *buf, size_t len, uint32_t offset, void *plan) {
    (void)len; (void)offset; (void)plan; buf[4]^=0x80; ++patches;
}
static esp_err_t burner_bacon_gba_verify_read_block_hoststyle(uint8_t *out, size_t len,
        uint32_t addr, bool multi) {
    assert(locks==1 && multi==(addr!=0)); ++reads;
    if (read_error) return read_error;
    memcpy(out,expected,len);
    if (mismatch && !(recover && resets)) out[0]^=1;
    return ESP_OK;
}
static esp_err_t burner_bacon_gba_finalize_write(bool multi) {
    (void)multi; assert(locks==1); ++resets; return reset_error;
}
static esp_err_t burner_spi_prepare_burn_gba(const burner_task_param_t *job) {
    (void)job; return prepare_error;
}
static esp_err_t burner_spi_prepare_burn_mbc5(const burner_task_param_t *job) {
    (void)job; return prepare_error;
}
'''

header = "\n".join(extract(BURN, name) for name in (
    "static void burner_gba_post_write_diag_log_first_mismatch(",
    "static bool burner_gba_apply_header_checksum_fix(",
    "static esp_err_t burner_gba_post_write_header_diag(",
))


def worker_validation(source, declaration, end, name):
    body = extract(source, declaration)
    start = body.index("    burner_spi_lock_take();")
    finish = body.index(end, start)
    return (f"static esp_err_t {name}(const burner_task_param_t *job, unsigned *erases) {{\n"
            "uint32_t addr_begin=job->addr_begin; esp_err_t err;\n" + body[start:finish] +
            "++*erases; return ESP_OK;\n}\n")


workers = worker_validation(BURN, "static esp_err_t burner_run_write_job_gba(",
                            "    burner_gba_chis_diag_begin(job);", "check_gba")
workers += worker_validation(GBC, "static esp_err_t burner_run_write_job_mbc5(",
                             "    fp = burner_file_open_read(job->rom_path);", "check_gbc")

cases = r'''
static FILE *image(burner_task_param_t *job) {
    uint8_t file[512];
    for (unsigned i=0;i<512;++i) file[i]=(uint8_t)(i*7u);
    memcpy(expected,file,512);
    if(job->gba_patch_plan) expected[4]^=0x80;
    burner_gba_apply_header_checksum_fix(expected,512,0,false);
    FILE *fp=tmpfile(); assert(fp); assert(fwrite(file,1,512,fp)==512); fflush(fp); return fp;
}
static void clear(void) {
    reads=resets=locks=patches=0; read_error=reset_error=prepare_error=0;
    intel=gbx=mismatch=recover=false;
    s_cart_ctx.probe_cfi_ok=true; s_cart_ctx.gba_cmdset=BURNER_NOR_CMDSET_INTEL;
    s_cart_ctx.device_size=32u*1024u*1024u;
}
int main(void) {
    burner_task_param_t job={.total_bytes=512};
    clear();intel=true;FILE *fp=image(&job);
    assert(burner_gba_post_write_header_diag(fp,&job)==ESP_OK && reads==1 && resets==0 && locks==0);fclose(fp);
    clear();intel=mismatch=recover=true;fp=image(&job);
    assert(burner_gba_post_write_header_diag(fp,&job)==ESP_OK && reads==2 && resets==1 && locks==0);fclose(fp);
    clear();intel=mismatch=true;fp=image(&job);
    assert(burner_gba_post_write_header_diag(fp,&job)==ESP_FAIL && reads==2 && resets==1 && locks==0);fclose(fp);
    clear();mismatch=true;fp=image(&job);
    assert(burner_gba_post_write_header_diag(fp,&job)==ESP_FAIL && reads==1 && resets==0 && locks==0);fclose(fp);
    clear();intel=gbx=mismatch=true;fp=image(&job);
    assert(burner_gba_post_write_header_diag(fp,&job)==ESP_FAIL && reads==1 && resets==0);fclose(fp);
    clear();read_error=ESP_ERR_TIMEOUT;fp=image(&job);
    assert(burner_gba_post_write_header_diag(fp,&job)==ESP_ERR_TIMEOUT && reads==1 && resets==0 && locks==0);fclose(fp);
    clear();intel=mismatch=true;reset_error=ESP_ERR_TIMEOUT;fp=image(&job);
    assert(burner_gba_post_write_header_diag(fp,&job)==ESP_ERR_TIMEOUT && reads==1 && resets==1 && locks==0);fclose(fp);
    clear();job.addr_begin=0x2000000;job.gba_patch_plan=&job;fp=image(&job);
    assert(burner_gba_post_write_header_diag(fp,&job)==ESP_OK && reads==1 && patches==1);fclose(fp);
    clear();job.addr_begin=0;job.gba_patch_plan=NULL;fp=tmpfile();assert(fp);
    assert(burner_gba_post_write_header_diag(fp,&job)==ESP_FAIL && reads==0);fclose(fp);
    /* 执行实际工作任务的准备和容量检查，只有通过后才计入擦除。 */
    esp_err_t (*checks[])(const burner_task_param_t *,unsigned *)={check_gba,check_gbc};
    for(unsigned i=0;i<2;++i) {
        unsigned erases=0;clear();job.addr_begin=31u*1024u*1024u;job.total_bytes=1024u*1024u;
        assert(checks[i](&job,&erases)==ESP_OK && erases==1);
        ++job.total_bytes;assert(checks[i](&job,&erases)==ESP_ERR_INVALID_SIZE && erases==1);
        prepare_error=ESP_ERR_NOT_SUPPORTED;job.total_bytes=512;
        assert(checks[i](&job,&erases)==ESP_ERR_NOT_SUPPORTED && erases==1 && locks==0);
    }
    clear();unsigned erases=0;job.addr_begin=0;job.total_bytes=512;s_cart_ctx.probe_cfi_ok=false;
    assert(check_gba(&job,&erases)==ESP_ERR_NOT_SUPPORTED && erases==0);
    puts("Single-path header checks, Intel recovery, transport failures and pre-erase bounds passed");
}
'''
compile_run(prefix + header + workers + cases)

prepare = extract(LOW, "static esp_err_t burner_bacon_gba_prepare(")
start = prepare.index("    if (job->recipe_mode == BURNER_RECIPE_MODE_GBX) {")
end = prepare.index(" else if (job->recipe_mode == BURNER_RECIPE_MODE_AUTO)", start)
gbx_branch = prepare[start:end]
gbx_test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BURNER_RECIPE_MODE_GBX 2
#define BURNER_JOB_WRITE_ROM 1
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 1
typedef int esp_err_t;
typedef struct { int recipe_mode,mode; char gbx_profile_file[32]; } burner_task_param_t;
static bool valid;
static unsigned fresh,takes,clears;
static void burner_gba_gbx_clear_cached_probe(void) { valid=false;++clears; }
static bool burner_gba_gbx_take_cached_probe(uint8_t *id,uint32_t *size,uint32_t *sector,uint16_t *buffer,bool *cfi,bool *matched) {
    (void)id;(void)sector;(void)buffer;(void)cfi;++takes;if(!valid)return false;
    *size=8;*matched=true;return true;
}
static esp_err_t burner_gba_gbx_prepare_manual_profile(const char *path,uint8_t *id,uint32_t *size,uint32_t *sector,uint16_t *buffer,bool *cfi) {
    (void)path;(void)id;(void)sector;(void)buffer;(void)cfi;++fresh;*size=32;return ESP_OK;
}
static struct { struct { bool active; } gbx; } s_cart_ctx;
static esp_err_t burner_gba_gbx_probe_locked(uint8_t *id,void *unused,uint32_t *size,uint32_t *sector,uint16_t *buffer,bool *cfi) {
    (void)unused;s_cart_ctx.gbx.active=true;
    return burner_gba_gbx_prepare_manual_profile(NULL,id,size,sector,buffer,cfi);
}
static unsigned run(const burner_task_param_t *job) {
    uint8_t id[8];uint32_t device_size=0,sector_size=0;uint16_t buffer_write_bytes=0;
    bool cfi_ok=false,gbx_profile_matched=false,cached_probe_used=false;esp_err_t err;
'''
gbx_end = r'''
    assert(err==ESP_OK && gbx_profile_matched);
    return device_size;
}
int main(void) {
    burner_task_param_t job={.recipe_mode=BURNER_RECIPE_MODE_GBX,.mode=BURNER_JOB_WRITE_ROM};
    valid=true;assert(run(&job)==32 && fresh==1 && clears==1);
    job.mode=0;valid=true;assert(run(&job)==8 && fresh==1);
    job.mode=BURNER_JOB_WRITE_ROM;strcpy(job.gbx_profile_file,"manual.gbx");valid=true;
    assert(run(&job)==32 && fresh==2 && clears==2);
    puts("GBX writes freshly detect the card and preserve manual profiles");
}
'''
compile_run(gbx_test + gbx_branch + gbx_end)
