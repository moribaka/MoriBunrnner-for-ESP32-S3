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

static void cart32_lab_job(const char *action, const char *name, const char *path, uint32_t bytes,
                           burner_recipe_mode_t recipe)
{
    if (bsc2_lab_busy()) { message("error","TF or cartridge job busy"); return; }
    bool backup = !strcmp(action,"backup"), write = !strcmp(action,"write");
    if (!backup && !write && strcmp(action,"verify")) { message("error","backup|verify|write"); return; }
    struct stat info;
    bool exists = stat(path,&info)==0;
    if (backup ? exists : (!exists || info.st_size != bytes)) {
        message("error",backup ? "backup already exists; refusing overwrite" : "backup missing or size mismatch"); return;
    }
    if (write) {
        burner_status_t s; burner_status_snapshot(&s);
        if (s.state!=BURNER_STATE_DONE || strcmp(s.message,"verify finished") ||
            strcmp(s.rom_path,path) || s.processed_bytes!=bytes || s.total_bytes!=bytes) {
            message("error","verify the preserved backup against the cartridge before each lab write"); return;
        }
        /* The regular writer fixes the GBA header checksum. A restoration
         * test must not silently change the preserved image. */
        uint8_t header[192];
        FILE *source = fopen(path, "rb");
        size_t got = source ? fread(header, 1, sizeof(header), source) : 0;
        if (source) fclose(source);
        uint8_t checksum = 0xE7u;
        if (got == sizeof(header)) {
            for (unsigned i = 0xA0u; i < 0xBDu; ++i) checksum -= header[i];
        }
        if (got != sizeof(header) || checksum != header[0xBD]) {
            message("error", "backup has invalid GBA header checksum; refusing to alter a restoration image"); return;
        }
    }
    if (backup && mkdir(ROM_OUTPUT_DIR_PATH,0777)!=0 && errno!=EEXIST) {
        message("error","cannot create backup directory"); return;
    }
    esp_err_t err = burner_start_task_ex(
        backup ? BURNER_JOB_READ_ROM : write ? BURNER_JOB_WRITE_ROM : BURNER_JOB_VERIFY_ROM,
        BURNER_CART_MODE_GBA, write ? BURNER_WRITE_PATH_PSRAM : BURNER_WRITE_PATH_DIRECT,
        recipe, write, false, false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES, BURN_GBA_DUMP_CHUNK_BYTES, 0,
        name,path,0,bytes,BURNER_GBA_SAVE_TYPE_SRAM,
        "",false,0,false,NULL);
    if (err == ESP_OK) ui_show_burn_task_status(bytes);
    cJSON *json=event("bsc2_job_started");
    if(json){ cJSON_AddBoolToObject(json,"ok",err==ESP_OK); cJSON_AddStringToObject(json,"error",esp_err_to_name(err)); cJSON_AddStringToObject(json,"action",action); }
    reply(json);
}

static void bsc2_lab_job(const char *action)
{
    cart32_lab_job(action, BSC2_BACKUP_NAME, BSC2_BACKUP_PATH, 33554432u, BURNER_RECIPE_MODE_CHIS);
}

/* Explicit GBA 32 MiB backup/verify/restore for serial-only board testing.
 * A new backup cannot overwrite a file; restore requires a completed full
 * comparison of that exact file with the inserted cartridge. */
static void cart32_lab_hash(const char *path, uint32_t bytes);

static void cart32_lab_command(const char *args, uint32_t bytes)
{
    char action[8], name[128], extra, path[304];
    if (sscanf(args, "%7s %127s %c", action, name, &extra) != 2 ||
        strchr(name, '/') || strchr(name, '\\') || strchr(name, ':') ||
        strstr(name, "..") || strlen(name) < 5u ||
        strcmp(name + strlen(name) - 4u, ".gba")) {
        message("error", "gba-cart32 backup|verify|write NAME.gba (ROM_OUTPUT directory)");
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", ROM_OUTPUT_DIR_PATH, name);
    if (!strcmp(action, "hash")) { cart32_lab_hash(path, bytes); return; }
    cart32_lab_job(action, name, path, bytes, BURNER_RECIPE_MODE_CHIS);
}

/* Small full-worker regression, restricted to the qualified M36 and its
 * already verified native64 backup. All writes preserve original bytes. */
static void m36_native_worker_test(const char *action)
{
    if (bsc2_lab_busy()) { message("error", "TF or cartridge job busy"); return; }
    bool prepare = !strcmp(action,"prepare");
    bool recover = !strcmp(action,"recover");
    bool hash = !strcmp(action,"hash");
    if (!prepare && !recover && !hash && strcmp(action,"write") && strcmp(action,"verify")) {
        message("error", "gba-native-worker prepare|write|verify|recover|hash"); return;
    }
    uint8_t id[8]; uint32_t bytes=0, sector=0, begin=0, end=0; uint16_t buffer=0; bool cfi=false;
    burner_spi_lock_take();
    esp_err_t err=burner_spi_init();
    if(err==ESP_OK) err=burner_bacon_gba_prepare_power();
    if(err==ESP_OK) err=burner_auto_gba_probe_locked(id,&bytes,&sector,&buffer,&cfi);
    if(err==ESP_OK) {
        if(!s_cart_ctx.geometry.region_count) err=ESP_ERR_INVALID_SIZE;
        else {
            begin=s_cart_ctx.geometry.regions[0].addr_begin;
            end=begin+s_cart_ctx.geometry.regions[0].sector_size;
        }
    }
    if(err==ESP_OK && !(id[0]==0x20 && id[1]==0 && id[2]==0x0D && id[3]==0x88 &&
        cfi && bytes==33554432u && buffer==64u && begin==0 && end==131072u)) err=ESP_ERR_NOT_SUPPORTED;
    esp_err_t finish=burner_bacon_finish_cart_access();
    if(err==ESP_OK) err=finish;
    burner_spi_lock_give();
    if(err!=ESP_OK) { message("error", "current cartridge does not match the qualified M36 sector layout"); return; }
    const char *name="m36_native_sector0.gba";
    const char *path=ROM_OUTPUT_DIR_PATH "/m36_native_sector0.gba";
    if (hash) { cart32_lab_hash(path,131072u); return; }
    if (recover) {
        burner_status_t *previous=malloc(sizeof(*previous));
        if(!previous) { message("error", "no recovery status memory"); return; }
        burner_status_snapshot(previous);
        bool allowed=previous->state==BURNER_STATE_ERROR && !strcmp(previous->rom_path,path);
        free(previous);
        if (!allowed) {
            message("error", "recovery requires a failed test of the preserved M36 first sector"); return;
        }
        const char *backup=ROM_OUTPUT_DIR_PATH "/m36_native64_qualification.bin";
        struct stat a,b;
        if(stat(backup,&a)!=0 || stat(path,&b)!=0 || a.st_size!=327680 || b.st_size!=131072) {
            message("error", "recovery backup size mismatch"); return;
        }
        FILE *original=fopen(backup,"rb"), *sector_file=fopen(path,"rb");
        uint8_t *buffers=malloc(8192);
        err=original && sector_file && buffers ? ESP_OK : ESP_ERR_NO_MEM;
        for(unsigned copied=0; err==ESP_OK && copied<131072u; copied+=4096u) {
            if(fread(buffers,1,4096,original)!=4096 || fread(buffers+4096,1,4096,sector_file)!=4096 ||
               memcmp(buffers,buffers+4096,4096)) err=ESP_FAIL;
        }
        if(original) fclose(original);
        if(sector_file) fclose(sector_file);
        free(buffers);
        if(err!=ESP_OK) { message("error", "sector image differs from preserved original backup"); return; }
        (void)burner_gba_intel_speed_set(0);
        err=burner_start_task_ex(BURNER_JOB_WRITE_ROM,BURNER_CART_MODE_GBA,BURNER_WRITE_PATH_PSRAM,
            BURNER_RECIPE_MODE_AUTO,true,false,false,BURN_MBC5_PROGRAM_CHUNK_BYTES,BURN_GBA_DUMP_CHUNK_BYTES,
            0,name,path,0,131072u,BURNER_GBA_SAVE_TYPE_SRAM,"",false,0,false,NULL);
        cJSON *json=event("bsc2_job_started");
        if(json){cJSON_AddBoolToObject(json,"ok",err==ESP_OK);cJSON_AddStringToObject(json,"action","recover");
            cJSON_AddStringToObject(json,"error",esp_err_to_name(err));}
        reply(json); return;
    }
    if(prepare) {
        const char *source=ROM_OUTPUT_DIR_PATH "/m36_native64_qualification.bin";
        struct stat st;
        if(stat(source,&st)!=0 || st.st_size!=327680 || stat(path,&st)==0) {
            message("error", "verified 320KiB backup missing or sector file already exists"); return;
        }
        FILE *in=fopen(source,"rb"); FILE *out=fopen(path,"wb");
        uint8_t *chunk=malloc(4096);
        err=in && out && chunk ? ESP_OK : ESP_ERR_NO_MEM;
        for(unsigned copied=0; err==ESP_OK && copied<131072u; copied+=4096u)
            if(fread(chunk,1,4096,in)!=4096 || fwrite(chunk,1,4096,out)!=4096) err=ESP_FAIL;
        if(in) fclose(in);
        if(out && fclose(out)!=0) err=ESP_FAIL;
        free(chunk);
        if(err!=ESP_OK) { message("error", "cannot extract preserved first sector"); return; }
    }
    cart32_lab_job(prepare ? "verify" : action, name, path, 131072u, BURNER_RECIPE_MODE_AUTO);
}

static void cart32_lab_hash(const char *path, uint32_t bytes)
{
    if(bsc2_lab_busy()){message("error","TF or cartridge job busy");return;}
    FILE *file=fopen(path,"rb");
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
    if(json){cJSON_AddBoolToObject(json,"ok",err==ESP_OK && total==bytes);cJSON_AddStringToObject(json,"sha256",hex);cJSON_AddNumberToObject(json,"bytes",total);}
    reply(json);
}

static void bsc2_lab_hash(void)
{
    cart32_lab_hash(BSC2_BACKUP_PATH, 33554432u);
}

/* Read-only bank switching check; duplicate blank banks are inconclusive,
 * so report distinctness separately from read stability. */
static void cart128_bank_map(void)
{
    if (bsc2_lab_busy()) { message("error", "TF or cartridge job busy"); return; }
    uint8_t *buffer = heap_caps_malloc(65536u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) { message("error", "no bank-map buffer"); return; }
    burner_task_param_t job = {0};
    job.mode = BURNER_JOB_READ_ROM;
    job.cart_mode = BURNER_CART_MODE_GBA;
    job.recipe_mode = BURNER_RECIPE_MODE_CHIS;
    job.total_bytes = 134217728u;
    uint32_t signature[4] = {0};
    static const unsigned order[] = {0,1,2,3,0,3,2,1};
    static const uint32_t offsets[] = {0, 0x100000u, 0x1000000u, 0x1FF0000u};
    bool stable = true, distinct = true;
    unsigned passes = 0;
    esp_err_t err = power_manager_perf_lock_acquire("bank_map");
    bool perf = err == ESP_OK;
    burner_spi_lock_take();
    if (err == ESP_OK) err = burner_spi_init();
    if (err == ESP_OK) err = burner_spi_prepare_burn_gba(&job);
    for (unsigned round = 0; err == ESP_OK && round < 8u; ++round) {
        unsigned bank = order[round];
        uint32_t crc = 0, non_ff = 0;
        for (unsigned sample = 0; err == ESP_OK && sample < 4u; ++sample) {
            err = burner_bacon_gba_verify_read_block_hoststyle(buffer, 65536u,
                bank * 33554432u + offsets[sample], true);
            if (err != ESP_OK) break;
            crc = esp_rom_crc32_le(crc, buffer, 65536u);
            for (unsigned i = 0; i < 65536u; ++i) non_ff += buffer[i] != 0xFF;
        }
        if (err == ESP_OK) {
            if (round < 4u) signature[bank] = crc;
            else if (signature[bank] != crc) stable = false;
            ++passes;
            cJSON *row = event("gba_bank_sample");
            if (row) {
                cJSON_AddNumberToObject(row, "bank", bank);
                cJSON_AddNumberToObject(row, "round", round);
                cJSON_AddNumberToObject(row, "crc32", crc);
                cJSON_AddNumberToObject(row, "sample_non_ff", non_ff);
            }
            reply(row);
        }
    }
    esp_err_t restore = burner_bacon_gba_verify_read_block_hoststyle(buffer, 64u, 0u, true);
    if (err == ESP_OK) err = restore;
    burner_spi_lock_give();
    if (perf) power_manager_perf_lock_release("bank_map");
    free(buffer);
    for (unsigned a=0; a<4; ++a) for (unsigned b=a+1; b<4; ++b)
        if (signature[a] == signature[b]) distinct = false;
    cJSON *json = event("gba_bank_map");
    if (json) {
        cJSON_AddBoolToObject(json, "ok", err == ESP_OK && stable && passes == 8);
        cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
        cJSON_AddBoolToObject(json, "stable", stable);
        cJSON_AddBoolToObject(json, "distinct", distinct);
        cJSON_AddNumberToObject(json, "passes", passes);
    }
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

typedef struct { char path[304]; bool gbabf, native_buffer; } cart_spot_job_t;

static void cart256_spot_worker(void *arg)
{
    cart_spot_job_t *job = arg;
    const char *path = job->path;
    burner_gba_spot_report_t report = {0};
    int64_t start = esp_timer_get_time();
    esp_err_t err = power_manager_perf_lock_acquire("gba_spot256");
    bool perf = err == ESP_OK;
    burner_spi_lock_take();
    if (err == ESP_OK) err = burner_spi_init();
    if (err == ESP_OK) err = job->native_buffer ? burner_gba_m36_native_test_locked(path, &report) :
        job->gbabf ? burner_gba_88b0_gbabf_test_locked(path, &report) :
        burner_gba_88b0_spot_test_locked(path, &report);
    burner_spi_lock_give();
    if (perf) power_manager_perf_lock_release("gba_spot256");
    cJSON *json = event("gba_spot_done");
    if (json) {
        cJSON_AddBoolToObject(json, "ok", err == ESP_OK && report.points_total > 0 &&
            report.markers_verified == report.points_total && report.sectors_verified == report.points_total);
        cJSON_AddNumberToObject(json, "points_total", report.points_total);
        cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
        cJSON_AddStringToObject(json, "test_error", esp_err_to_name(report.test_error));
        cJSON_AddStringToObject(json, "restore_error", esp_err_to_name(report.restore_error));
        cJSON_AddBoolToObject(json, "backup_verified", report.backup_verified);
        cJSON_AddBoolToObject(json, "destructive_started", report.destructive_started);
        cJSON_AddNumberToObject(json, "markers_verified", report.markers_verified);
        cJSON_AddNumberToObject(json, "sectors_restored", report.sectors_restored);
        cJSON_AddNumberToObject(json, "sectors_verified", report.sectors_verified);
        cJSON_AddNumberToObject(json, "mismatch_address", report.mismatch_address);
        cJSON_AddNumberToObject(json, "elapsed_ms", (esp_timer_get_time() - start) / 1000);
        cJSON_AddStringToObject(json, "backup", path);
        cJSON_AddStringToObject(json, "recipe", report.recipe_id == 4 ? "native Intel 64B buffer" :
            report.recipe_id == 3 ? "GBABF raw 50/FF/40 word" :
            report.recipe_id == 2 ? "GBABF raw 50/FF/EA 1024B" : "native Intel 1024B");
    }
    free(job);
    portENTER_CRITICAL(&s_job_lock);
    s_job_running = false;
    portEXIT_CRITICAL(&s_job_lock);
    reply(json);
    vTaskDelete(NULL);
}

static void cart256_spot_command(const char *name, bool gbabf, bool native_buffer)
{
    if (bsc2_lab_busy()) { message("error", "TF or cartridge job busy"); return; }
    if (strlen(name) < 5u || strlen(name) > 120u || strchr(name, '/') ||
        strchr(name, '\\') || strchr(name, ':') || strstr(name, "..") ||
        strcmp(name + strlen(name) - 4u, ".bin")) {
        message("error", "gba-spot256 NAME.bin; tests/restores 16 sectors with a new ROM_OUTPUT backup"); return;
    }
    if (mkdir(ROM_OUTPUT_DIR_PATH, 0777) != 0 && errno != EEXIST) {
        message("error", "cannot create ROM_OUTPUT"); return;
    }
    cart_spot_job_t *job = malloc(sizeof(*job));
    if (!job) { message("error", "no memory"); return; }
    snprintf(job->path, sizeof(job->path), "%s/%s", ROM_OUTPUT_DIR_PATH, name);
    job->gbabf = gbabf;
    job->native_buffer = native_buffer;
    portENTER_CRITICAL(&s_job_lock);
    bool busy = s_job_running;
    if (!busy) s_job_running = true;
    portEXIT_CRITICAL(&s_job_lock);
    if (busy) { free(job); message("error", "debug job busy"); return; }
    if (xTaskCreatePinnedToCore(cart256_spot_worker, "gba_spot256", 24576, job, 2, NULL, 1) != pdPASS) {
        free(job);
        portENTER_CRITICAL(&s_job_lock); s_job_running = false; portEXIT_CRITICAL(&s_job_lock);
        message("error", "cannot start spot worker"); return;
    }
    message("gba_spot_started", "identify current card, back up selected sectors, test, restore and verify");
}
