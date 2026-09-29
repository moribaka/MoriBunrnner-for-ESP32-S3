"""Exercise the production combined-probe policy with controlled detector outcomes."""
from pathlib import Path
import subprocess,shutil,re,tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'main/burner/core/burn/burner_gba_lowlevel.c').read_text(encoding='utf-8')
a=s.index('static bool s_auto_probe_gbabf;')
b=s.index('esp_err_t burner_chislink_gba_probe_locked(',a)
actual=s[a:b]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_INVALID_CRC 5
#define ESP_LOGI(...) ((void)0)
static struct {bool gba_likely_read_only;} s_cart_ctx;
static int chis_result,gbabf_result,chis_calls,gbabf_calls;
static bool chis_readonly;
static esp_err_t burner_bacon_gba_probe_locked(uint8_t *id,uint32_t *bytes,uint32_t *sector,uint16_t *buffer,bool *cfi) {
 ++chis_calls; s_cart_ctx.gba_likely_read_only=chis_readonly;
 id[0]=0x20;*bytes=33554432;*sector=131072;*buffer=64;*cfi=!chis_readonly;
 return chis_result;
}
static esp_err_t burner_gbabf_gba_probe_locked(uint8_t *id,uint32_t *bytes,uint32_t *sector,uint16_t *buffer,bool *cfi) {
 ++gbabf_calls;s_cart_ctx.gba_likely_read_only=false;
 id[0]=0x89;*bytes=33554432;*sector=131072;*buffer=1024;*cfi=true;
 return gbabf_result;
}
'''
cases=r'''
static int run(int a,int b,bool readonly) {
 chis_result=a;gbabf_result=b;chis_readonly=readonly;chis_calls=gbabf_calls=0;
 uint8_t id[8];uint32_t size,sector;uint16_t buffer;bool cfi;
 return burner_auto_gba_probe_locked(id,&size,&sector,&buffer,&cfi);
}
int main(void) {
 assert(run(ESP_OK,ESP_OK,false)==ESP_OK && chis_calls==1 && gbabf_calls==0);
 assert(!strcmp(burner_auto_gba_probe_source(),"AUTO:CHIS"));
 assert(run(ESP_ERR_NOT_FOUND,ESP_OK,false)==ESP_OK && chis_calls==1 && gbabf_calls==1);
 assert(!strcmp(burner_auto_gba_probe_source(),"AUTO:GBABF"));
 assert(run(ESP_ERR_NOT_SUPPORTED,ESP_ERR_INVALID_SIZE,false)==ESP_ERR_INVALID_SIZE);
 assert(run(ESP_ERR_TIMEOUT,ESP_OK,false)==ESP_ERR_TIMEOUT && gbabf_calls==0);
 assert(run(ESP_ERR_INVALID_CRC,ESP_OK,false)==ESP_ERR_INVALID_CRC && gbabf_calls==0);
 assert(run(ESP_OK,ESP_OK,true)==ESP_OK && gbabf_calls==1 && !s_cart_ctx.gba_likely_read_only);
 assert(run(ESP_OK,ESP_ERR_NOT_FOUND,true)==ESP_OK && chis_calls==2 && s_cart_ctx.gba_likely_read_only);
 assert(!strcmp(burner_auto_gba_probe_source(),"AUTO:CHIS"));
 assert(run(ESP_OK,ESP_ERR_TIMEOUT,true)==ESP_ERR_TIMEOUT && chis_calls==1);
 puts("Combined CHIS/GBABF detection, transport-error and read-only handling passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test.exe'
    c.write_text(prefix+actual+cases,encoding='utf-8')
    subprocess.run([shutil.which('gcc') or 'gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
