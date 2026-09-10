/* Capture the source length and final GBA header once. Programming, sector
 * comparison and automatic readback all use this same immutable definition. */
esp_err_t burner_prepare_write_source(burner_task_param_t *job)
{
    FILE *fp = burner_file_open_read(job->rom_path);
    if (!fp) return ESP_FAIL;
    struct stat st;
    esp_err_t err = ESP_OK;
    if (fstat(fileno(fp), &st) != 0 || st.st_size <= 0 || (uint64_t)st.st_size > UINT32_MAX) {
        fclose(fp); return ESP_ERR_INVALID_SIZE;
    }
    job->source_size = (uint32_t)st.st_size;
    job->gba_header_checksum_valid = false;
    if (job->gba_patch_plan && job->gba_patch_plan->source_size != job->source_size) {
        fclose(fp); return ESP_ERR_INVALID_SIZE;
    }
    if (job->gba_patch_plan ? job->gba_patch_plan->output_size != job->total_bytes : job->source_size != job->total_bytes) {
        fclose(fp); return ESP_ERR_INVALID_SIZE;
    }
    if (job->cart_mode == BURNER_CART_MODE_GBA && job->total_bytes > 0xBDu) {
        uint8_t header[0xC0];
        err = burner_source_read_exact(fp, header, sizeof(header), job->source_size);
        if (err == ESP_OK) {
            if (job->gba_patch_plan) burner_apply_gba_patch_plan(header, sizeof(header), 0, job->gba_patch_plan);
            uint8_t checksum = 0;
            for (size_t i = 0xA0; i < 0xBD; ++i) checksum -= header[i];
            job->gba_header_checksum = checksum - 0x19u;
            job->gba_header_checksum_valid = true;
        }
    }
    fclose(fp);
    return err;
}

static void burner_apply_write_transform(const burner_task_param_t *job,
    uint8_t *data, size_t bytes, uint32_t file_offset)
{
    if (job->cart_mode != BURNER_CART_MODE_GBA) return;
    if (job->gba_patch_plan) burner_apply_gba_patch_plan(data, bytes, file_offset, job->gba_patch_plan);
    if (job->gba_header_checksum_valid && file_offset <= 0xBDu && bytes > 0xBDu - file_offset)
        data[0xBDu - file_offset] = job->gba_header_checksum;
}
