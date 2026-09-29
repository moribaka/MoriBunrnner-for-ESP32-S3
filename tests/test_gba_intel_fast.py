"""Run the production Intel poll packets with scripted flash status samples."""
from pathlib import Path
import subprocess,shutil,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'main/burner/core/burn/burner_gba_intel_fast.inc').read_text(encoding='utf-8')
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_INVALID_RESPONSE 3
#define ESP_ERR_INVALID_STATE 4
#define BURNER_SPI_CS_MODE_0 0
#define BURN_GBA_LINEAR_ADDR_BYTES 33554432u
#define BURNER_ROM_POLL_INTERVAL_US 50
static struct {bool d0d1_swapped;} s_cart_ctx={true};
static uint16_t samples[64]; static unsigned count,next,transfers,commands,delays,cancels,cancel_at,diagnostics;
static int bus_error; static int64_t now;
static uint16_t swap(uint16_t v){return (v&~3u)|((v&1u)<<1)|((v&2u)>>1);}
static uint16_t burner_apply_d0d1_swap_on_write(uint16_t v,bool s){return s?swap(v):v;}
static uint16_t burner_apply_d0d1_swap_on_read(uint16_t v,bool s){return s?swap(v):v;}
static bool burner_gba_intel_status_has_error(uint16_t v){return (v&0x3A)!=0;}
static int64_t esp_timer_get_time(void){return now+=50;}
static uint64_t burner_gba_diag_now_us(void){return esp_timer_get_time();}
static void burner_gba_chis_diag_add_wait_ready(uint64_t us){assert(us);++diagnostics;}
static esp_err_t burner_cancel_poll(void){return ++cancels>=cancel_at?ESP_ERR_INVALID_STATE:ESP_OK;}
static void esp_rom_delay_us(unsigned us){assert(us==50);++delays;now+=us;}
static void burner_task_yield_if_due(void){}
static esp_err_t burner_bacon_gba_command_write_u16(uint32_t word,uint16_t cmd){assert(word==0x123456 && cmd==0xE9);++commands;return bus_error;}
static esp_err_t burner_spi_transfer_cs_legacy(int mode,const uint8_t *tx,uint8_t *rx,size_t bytes){
 assert(mode==0 && tx!=rx && !((uintptr_t)tx&3) && !((uintptr_t)rx&3));++transfers;
 if(bus_error)return bus_error;
 unsigned base=bytes%10?11:0;assert(bytes%10==0 || bytes%10==1);
 if(base){
  const uint8_t expected[]={0xFF,0x56,0x34,0x12,0x3B,0xBB,0xEA,0,0x3A,0x3B,0x3F};
  assert(!memcmp(tx,expected,11));++commands;
 }
 unsigned reads=(bytes-base)/10;assert(reads>=1 && reads<=4);
 for(unsigned i=0;i<reads;++i){
  unsigned at=base+10*i;
  const uint8_t expected[]={0xFF,0x56,0x34,0x12,0x3B,0x29,0xAB,0,0,0x3F};
  assert(!memcmp(tx+at,expected,10));
  uint16_t v=samples[next<count?next++:count-1];rx[at+7]=v;rx[at+8]=v>>8;
 }
 return ESP_OK;
}
'''
cases=r'''
static void reset(void){
 memset(samples,0,sizeof(samples));count=64;next=transfers=commands=delays=cancels=diagnostics=0;
 cancel_at=100000;bus_error=0;now=0;
}
int main(void){
 assert(!s_gba_intel_speed_eligible);
 assert(burner_gba_intel_speed_set(3)==ESP_ERR_INVALID_ARG);
 for(unsigned mode=1;mode<=2;++mode){
  assert(burner_gba_intel_speed_set(mode)==ESP_OK && burner_gba_intel_speed_get()==mode);
  reset();samples[0]=0x80;uint16_t status;
  assert(burner_intel_wait_ready_optimized(0x2468AC,0xE9,10,&status)==ESP_OK);
  assert(status==0x80 && commands==1 && transfers==1 && delays==0 && diagnostics==1);
  reset();for(unsigned i=4;i<64;++i)samples[i]=0x80;
  assert(burner_intel_fast_poll(0x2468AC,0,10,&status)==ESP_OK && status==0x80 && delays==0);
  reset();for(unsigned i=0;i<64;++i)samples[i]=0x81; // raw swapped protect -> SR bit1
  assert(burner_intel_fast_poll(0x2468AC,0,10,&status)==ESP_ERR_INVALID_RESPONSE && status==0x82);
  reset();memset(samples,0xFF,sizeof(samples));
  assert(burner_intel_fast_poll(0x2468AC,0,2,&status)==ESP_ERR_TIMEOUT && delays>0);
  reset();bus_error=ESP_ERR_TIMEOUT;
  assert(burner_intel_fast_poll(0x2468AC,0,10,&status)==ESP_ERR_TIMEOUT && transfers==1);
  reset();cancel_at=2;
  assert(burner_intel_fast_poll(0x2468AC,0,10,&status)==ESP_ERR_INVALID_STATE && transfers==0);
  reset();assert(burner_intel_fast_poll(0x2468AC,0,0,&status)==ESP_ERR_TIMEOUT && transfers==0);
 }
 burner_gba_intel_speed_set(2);reset();samples[0]=0x81;samples[1]=samples[2]=samples[3]=0x80;
 uint16_t status;assert(burner_intel_fast_poll(0x2468AC,0,10,&status)==ESP_ERR_INVALID_RESPONSE);
 assert(burner_intel_status_packet(0x1000000,0,false,1,&status)==ESP_ERR_INVALID_ARG);
 puts("Intel command/read packets, normalization, ready/error/cancel/timeout checks passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test.exe'
    c.write_text(prefix+source+cases,encoding='utf-8')
    subprocess.run([shutil.which('gcc') or 'gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
