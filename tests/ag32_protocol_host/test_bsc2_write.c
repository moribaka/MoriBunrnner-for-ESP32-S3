#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bacon_cpld_transport.h"
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#define portMAX_DELAY -1
#define BURNER_SPI_CS_MODE_0 0
#define AG32_LINK_PREFERENCE_CPLD 3
typedef struct { size_t length; const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
static bacon_cpld_write_profile_t s_write_profile;
static struct { uint32_t status_polls; } s_stats;
static uint8_t s_enter_flags;
static void *s_mcu_spi = (void *)1;
static bool selected, inflight, fail_alloc, fail_enter, fail_ready, fail_cancel, capability=true;
static unsigned starts, ends, exits, frees, fail_start, fail_end, fail_prefix, fail_error;
static int preference=AG32_LINK_PREFERENCE_CPLD;
static uint32_t position, remain, page;
static const uint8_t *source;
static uint8_t snapshot[1028];
static spi_transaction_t *active;
static int64_t now;
static int64_t esp_timer_get_time(void) { return ++now; }
static int ag32_mcu_link_get_preference(void) { return preference; }
static void ag32_link_mark_cpld_active(void) {}
static esp_err_t burner_spi_init(void) { return ESP_OK; }
esp_err_t bacon_cpld_probe_locked(void) { return s_write_profile.mode==2 && !capability ? ESP_ERR_NOT_SUPPORTED : ESP_OK; }
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return fail_alloc ? NULL : malloc(n); }
static void checked_free(void *p) { assert(!inflight && !selected); ++frees; free(p); }
static esp_err_t enter(void) { s_enter_flags = capability ? 0x80 : 0; return fail_enter ? ESP_FAIL : ESP_OK; }
static esp_err_t mode_key(const char *key) { assert(!inflight && !selected && !strcmp(key,BACON_CPLD_EXIT_MAGIC)); ++exits; return ESP_OK; }
static uint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t *data, size_t n) { assert(!seed); return ag32_mcu_crc32(data,n); }
static esp_err_t burner_spi_transfer_cs(int mode, const uint8_t *tx, uint8_t *rx, size_t n)
{
    assert(mode==0 && !rx && n==20 && !inflight);
    assert(ag32_mcu_read_le32(tx)==BACON_CPLD_V2_DESCRIPTOR_MAGIC && tx[4]==3);
    assert(ag32_mcu_read_le32(tx+16)==ag32_mcu_crc32(tx,16));
    position=ag32_mcu_read_le32(tx+8); remain=ag32_mcu_read_le32(tx+12);
    page=ag32_mcu_read_le16(tx+6)+1;
    return ESP_OK;
}
static esp_err_t wait_flag(uint8_t flag,uint32_t total,int64_t deadline)
{
    (void)total;(void)deadline; assert(!inflight); ++s_stats.status_polls;
    if(flag==BACON_CPLD_DONE) assert(!remain);
    return fail_ready && flag==BACON_CPLD_RX_READY ? ESP_ERR_TIMEOUT : ESP_OK;
}
static esp_err_t burner_spi_begin_cs(int mode) { assert(mode==0 && !selected); selected=true; return ESP_OK; }
static void burner_spi_end_cs(int mode) { assert(mode==0 && selected && !inflight); selected=false; }
static esp_err_t spi_device_polling_start(void *d,spi_transaction_t *t,int timeout)
{
    (void)d;(void)timeout; assert(selected && !inflight); ++starts;
    if(starts==fail_start) return ESP_FAIL;
    assert(t->length/8<=sizeof(snapshot) && t->tx_buffer!=t->rx_buffer);
    assert((((uintptr_t)t->rx_buffer | (uintptr_t)t->tx_buffer | (t->length/8)) & 3u)==0);
    memcpy(snapshot,t->tx_buffer,t->length/8); active=t; inflight=true; return ESP_OK;
}
static esp_err_t spi_device_polling_end(void *d,int timeout)
{
    (void)d;(void)timeout; assert(inflight); ++ends;
    assert(!memcmp(snapshot,active->tx_buffer,active->length/8));
    size_t count=page-(position&(page-1)); if(count>remain)count=remain;
    size_t padded=(count+3)&~(size_t)3;
    assert(active->length==(padded+4)*8);
    assert(!memcmp(snapshot,source,count));
    assert(ag32_mcu_read_le32(snapshot+padded)==ag32_mcu_crc32(snapshot,count));
    for(size_t i=count;i<padded;++i)assert(snapshot[i]==0);
    source+=count; remain-=count; position+=count;
    uint8_t *rx=active->rx_buffer; memset(rx,0,active->length/8);
    rx[4]=ends==fail_prefix ? 0xfa : 0xba; rx[5]=0xce; rx[6]=1;
    rx[7]=3 | ((ends&1) ? 0x80 : 0) | (ends==fail_error ? 8 : 0);
    inflight=false;
    return ends==fail_end ? ESP_FAIL : ESP_OK;
}
static esp_err_t burner_cancel_poll(void) { assert(!inflight); return fail_cancel ? ESP_FAIL : ESP_OK; }
static void burner_task_yield_if_due(void) { assert(!inflight); }
#define free checked_free
#include "../../main/bacon_cpld_write_experiment.inc"
#undef free
static void reset(void)
{
    assert(!selected && !inflight);
    starts=ends=exits=frees=fail_start=fail_end=fail_prefix=fail_error=0;
    fail_alloc=fail_enter=fail_ready=fail_cancel=false; capability=true;
    memset(&s_write_profile,0,sizeof(s_write_profile)); s_write_profile.mode=2;
}
int main(void)
{
    static uint8_t data[65536];
    for(unsigned i=0;i<sizeof(data);++i)data[i]=(i*73)^(i>>3);
    const unsigned sizes[]={2,4,6,1022,1024,1026,2048,2050,65536};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i){
        reset();source=data;
        assert(bsc2_program_locked(126,data,sizes[i],1024,2000)==ESP_OK);
        assert(!remain && starts==ends && exits==1 && frees==1);
        assert(s_write_profile.bytes==sizes[i]);
        if(starts>1)assert(s_write_profile.credits>0);
    }
    reset();source=data;
    assert(bsc2_program_locked(0,data,6,2,2000)==ESP_OK && starts==3);
    for(unsigned fault=0;fault<9;++fault){
        reset();source=data;
        if(fault==0)fail_alloc=true;
        if(fault==1)fail_enter=true;
        if(fault==2)fail_ready=true;
        if(fault==3)fail_start=2;
        if(fault==4)fail_end=1;
        if(fault==5)fail_prefix=1;
        if(fault==6)fail_error=1;
        if(fault==7)fail_cancel=true;
        if(fault==8)capability=false;
        assert(bsc2_program_locked(0,data,4096,1024,2000)!=ESP_OK);
        assert(!selected && !inflight && !s_write_profile.bytes);
        assert(starts==ends+(fault==3));
        assert(frees==(fault!=0) && exits==(fault!=0));
    }
    reset();capability=false;
    assert(bacon_cpld_write_experiment_configure_locked(2)==ESP_ERR_NOT_SUPPORTED);
    assert(bacon_cpld_write_experiment_configure_locked(1)==ESP_OK);
    preference=0;
    assert(bacon_cpld_write_experiment_configure_locked(2)==ESP_ERR_INVALID_STATE);
    assert(bacon_cpld_write_experiment_configure_locked(0)==ESP_OK);
    puts("PASS: BSC2 descriptor/CRC, DMA buffer ownership, credit fallback, boundaries and failure cleanup");
    return 0;
}
