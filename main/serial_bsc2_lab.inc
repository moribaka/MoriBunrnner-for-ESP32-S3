/* Serial-only lab tools. Actual jobs use the same worker as UI/HTTP.
 * Fixed task-owned filenames prevent accidentally overwriting a user ROM.
 */
#include "esp_rom_crc.h"
#include "mbedtls/sha256.h"
#define BSC2_BACKUP_NAME "bsc2_before_20260912.gba"
#define BSC2_BACKUP_PATH ROM_OUTPUT_DIR_PATH "/" BSC2_BACKUP_NAME

static bool bsc2_lab_busy(void)
{
    return burner_task_is_running_snapshot() || ag32_batch_program_is_running() ||
        usb_msc_tf_in_use_by_host() || s_job_running;
}

static void bsc2_lab_status(void)
{
    burner_status_t s;
    burner_status_snapshot(&s);
    cJSON *json = event("bsc2_job_status");
    if (json) {
        cJSON_AddBoolToObject(json,"running",burner_task_is_running_snapshot());
        cJSON_AddStringToObject(json,"state",burner_state_to_str(s.state));
        cJSON_AddStringToObject(json,"message",s.message);
        cJSON_AddStringToObject(json,"path",s.rom_path);
        cJSON_AddNumberToObject(json,"processed",s.processed_bytes);
        cJSON_AddNumberToObject(json,"total",s.total_bytes);
        cJSON_AddNumberToObject(json,"task_us",s.task_elapsed_us);
        cJSON_AddNumberToObject(json,"erase_us",s.erase_elapsed_us);
        cJSON_AddNumberToObject(json,"write_us",s.write_elapsed_us);
        cJSON_AddNumberToObject(json,"write_bps",s.speed_avg_bps);
        cJSON_AddNumberToObject(json,"physical_program_bytes",s.write_speed_total_bytes);
        cJSON_AddNumberToObject(json,"skipped_bytes",s.write_skipped_bytes);
        cJSON_AddNumberToObject(json,"tf_us",s.tf_to_psram_total_us);
        cJSON_AddNumberToObject(json,"dump_read_us",s.dump_read_total_us);
        cJSON_AddNumberToObject(json,"dump_write_us",s.dump_write_total_us);
        cJSON_AddNumberToObject(json,"dump_wait_us",s.dump_wait_total_us);
    }
    reply(json);
}

static void bsc2_lab_job(const char *action)
{
    if (bsc2_lab_busy()) { message("error","TF or cartridge job busy"); return; }
    bool backup = !strcmp(action,"backup"), write = !strcmp(action,"write");
    if (!backup && !write && strcmp(action,"verify")) { message("error","backup|verify|write"); return; }
    struct stat info;
    bool exists = stat(BSC2_BACKUP_PATH,&info)==0;
    if (backup ? exists : (!exists || info.st_size != 33554432)) {
        message("error",backup ? "backup already exists; refusing overwrite" : "32MiB backup missing"); return;
    }
    if (write) {
        burner_status_t s; burner_status_snapshot(&s);
        if (s.state!=BURNER_STATE_DONE || strcmp(s.message,"verify finished") ||
            strcmp(s.rom_path,BSC2_BACKUP_PATH) || s.processed_bytes!=33554432 || s.total_bytes!=33554432) {
            message("error","verify the preserved backup against the cartridge before each lab write"); return;
        }
    }
    if (backup && mkdir(ROM_OUTPUT_DIR_PATH,0777)!=0 && errno!=EEXIST) {
        message("error","cannot create backup directory"); return;
    }
    esp_err_t err = burner_start_task_ex(
        backup ? BURNER_JOB_READ_ROM : write ? BURNER_JOB_WRITE_ROM : BURNER_JOB_VERIFY_ROM,
        BURNER_CART_MODE_GBA, write ? BURNER_WRITE_PATH_PSRAM : BURNER_WRITE_PATH_DIRECT,
        BURNER_RECIPE_MODE_CHIS, write, false, false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES, BURN_GBA_DUMP_CHUNK_BYTES, 0,
        BSC2_BACKUP_NAME,BSC2_BACKUP_PATH,0,33554432,BURNER_GBA_SAVE_TYPE_SRAM,
        "",false,0,false,NULL);
    cJSON *json=event("bsc2_job_started");
    if(json){ cJSON_AddBoolToObject(json,"ok",err==ESP_OK); cJSON_AddStringToObject(json,"error",esp_err_to_name(err)); cJSON_AddStringToObject(json,"action",action); }
    reply(json);
}

static void bsc2_lab_hash(void)
{
    if(bsc2_lab_busy()){message("error","TF or cartridge job busy");return;}
    FILE *file=fopen(BSC2_BACKUP_PATH,"rb");
    uint8_t *buffer=malloc(4096), digest[32]; size_t total=0;
    esp_err_t err=file && buffer ? ESP_OK : ESP_ERR_NO_MEM;
    mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx);
    if(err==ESP_OK && mbedtls_sha256_starts(&ctx,0))err=ESP_FAIL;
    while(err==ESP_OK){
        size_t n=fread(buffer,1,4096,file); if(!n)break;
        if(mbedtls_sha256_update(&ctx,buffer,n))err=ESP_FAIL;
        total+=n; burner_task_yield_if_due();
    }
    if(file && ferror(file))err=ESP_FAIL;
    if(err==ESP_OK && mbedtls_sha256_finish(&ctx,digest))err=ESP_FAIL;
    mbedtls_sha256_free(&ctx); if(file)fclose(file); free(buffer);
    char hex[65]={0}; if(err==ESP_OK)for(unsigned i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",digest[i]);
    cJSON *json=event("bsc2_backup_hash");
    if(json){cJSON_AddBoolToObject(json,"ok",err==ESP_OK && total==33554432);cJSON_AddStringToObject(json,"sha256",hex);cJSON_AddNumberToObject(json,"bytes",total);}
    reply(json);
}

static void bsc2_lab_upload(const char *args)
{
    unsigned size, expected; char extra;
    if(bsc2_lab_busy() || sscanf(args,"%u %x %c",&size,&expected,&extra)!=2 || !size || size>262144){message("error","invalid or busy upload");return;}
    const char *partial=mount_point "/bsc2_hwtest.bin.part";
    const char *target=mount_point "/bsc2_hwtest.bin";
    FILE *file=fopen(partial,"wb"); if(!file){message("error","cannot create upload");return;}
    uint8_t buffer[1024]; unsigned received=0; uint32_t crc=0;
    esp_err_t err=ESP_OK;
    message("bsc2_upload_ready","send binary in 1024-byte chunks; wait for each ack");
    while(received<size && err==ESP_OK){
        size_t count=size-received; if(count>sizeof(buffer))count=sizeof(buffer);
        size_t have=0; int64_t deadline=esp_timer_get_time()+10000000;
        while(have<count && esp_timer_get_time()<deadline){
            int n=usb_serial_jtag_read_bytes(buffer+have,count-have,pdMS_TO_TICKS(100));
            if(n>0)have+=(size_t)n;
        }
        if(have!=count){err=ESP_ERR_TIMEOUT;break;}
        if(fwrite(buffer,1,count,file)!=count){err=ESP_FAIL;break;}
        crc=esp_rom_crc32_le(crc,buffer,count); received+=count;
        cJSON *ack=event("bsc2_upload_ack");if(ack)cJSON_AddNumberToObject(ack,"received",received);reply(ack);
    }
    if(fclose(file)!=0 && err==ESP_OK)err=ESP_FAIL;
    if(err==ESP_OK && crc!=expected)err=ESP_ERR_INVALID_CRC;
    if(err==ESP_OK && rename(partial,target)!=0)err=ESP_FAIL;
    if(err!=ESP_OK)unlink(partial);
    cJSON *json=event("bsc2_upload_done");
    if(json){cJSON_AddBoolToObject(json,"ok",err==ESP_OK);cJSON_AddStringToObject(json,"error",esp_err_to_name(err));cJSON_AddNumberToObject(json,"bytes",received);cJSON_AddNumberToObject(json,"crc32",crc);}
    reply(json);
}
