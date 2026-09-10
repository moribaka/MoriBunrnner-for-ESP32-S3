#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "../../main/burner/core/burner_source_reader.h"
enum { BURNER_CART_MODE_GBA, BURNER_CART_MODE_MBC5 };
typedef struct { uint32_t source_size, output_size; } burner_gba_patch_plan_t;
typedef struct {
    const char *rom_path;
    uint32_t total_bytes, source_size;
    int cart_mode;
    burner_gba_patch_plan_t *gba_patch_plan;
    uint8_t gba_header_checksum;
    bool gba_header_checksum_valid;
} burner_task_param_t;
static FILE *burner_file_open_read(const char *p) { return fopen(p,"rb"); }
static void burner_apply_gba_patch_plan(uint8_t *p, size_t n, uint32_t offset, const burner_gba_patch_plan_t *plan) {
    (void)plan;
    if (offset<=0xA0 && n>0xA0-offset) p[0xA0-offset]^=0x40;
}
#include "../../main/burner/core/burn/burner_write_expected.c"
int main(void) {
    uint8_t original[512], whole[512], split[512];
    for (size_t i=0;i<sizeof(original);++i) original[i]=(uint8_t)(i*17);
    FILE *fp=fopen("expected_fixture.bin","wb"); assert(fp);
    assert(fwrite(original,1,sizeof(original),fp)==sizeof(original)); fclose(fp);
    burner_gba_patch_plan_t plan={512,1024};
    burner_task_param_t job={"expected_fixture.bin",1024,0,BURNER_CART_MODE_GBA,&plan,0,false};
    assert(burner_prepare_write_source(&job)==ESP_OK && job.source_size==512 && job.gba_header_checksum_valid);
    memcpy(whole,original,sizeof(whole));
    burner_apply_write_transform(&job,whole,sizeof(whole),0);
    unsigned sum=0x19;
    for (size_t i=0xA0;i<=0xBD;++i) sum+=whole[i];
    assert((sum&255)==0 && whole[0xA0]==(uint8_t)(original[0xA0]^0x40));
    for (size_t chunk=1; chunk<33; ++chunk) {
        memcpy(split,original,sizeof(split));
        for (size_t offset=0;offset<sizeof(split);offset+=chunk) {
            size_t n=sizeof(split)-offset; if(n>chunk)n=chunk;
            burner_apply_write_transform(&job,split+offset,n,offset);
        }
        assert(memcmp(split,whole,sizeof(split))==0);
    }
    plan.source_size=511;
    assert(burner_prepare_write_source(&job)==ESP_ERR_INVALID_SIZE);
    job.gba_patch_plan=NULL; job.total_bytes=513;
    assert(burner_prepare_write_source(&job)==ESP_ERR_INVALID_SIZE);
    job.total_bytes=512; job.cart_mode=BURNER_CART_MODE_MBC5;
    assert(burner_prepare_write_source(&job)==ESP_OK && !job.gba_header_checksum_valid);
    memcpy(split,original,sizeof(split)); burner_apply_write_transform(&job,split,sizeof(split),0);
    assert(memcmp(split,original,sizeof(split))==0);
    remove("expected_fixture.bin");
    puts("Write expected bytes: patch-before-checksum, arbitrary chunk splits, source-size validation and GBC no-patch passed");
}
