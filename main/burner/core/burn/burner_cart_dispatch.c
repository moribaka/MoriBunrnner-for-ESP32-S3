/* Cartridge job dispatch wrappers. */

esp_err_t burner_run_write_job(const burner_task_param_t *job)
{
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (job->cart_mode == BURNER_CART_MODE_GBA) {
        return burner_run_write_job_gba(job);
    }
    return burner_run_write_job_mbc5(job);
}

esp_err_t burner_run_read_job(const burner_task_param_t *job)
{
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (job->cart_mode == BURNER_CART_MODE_GBA) {
        return burner_run_read_job_gba(job);
    }
    return burner_run_read_job_mbc5(job);
}

esp_err_t burner_run_verify_rom_job(const burner_task_param_t *job)
{
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (job->cart_mode == BURNER_CART_MODE_GBA) {
        return burner_run_verify_rom_job_gba(job);
    }
    return burner_run_verify_rom_job_mbc5(job);
}

esp_err_t burner_run_erase_rom_job(const burner_task_param_t *job)
{
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (job->cart_mode == BURNER_CART_MODE_GBA) {
        return burner_run_erase_rom_job_gba(job);
    }
    return burner_run_erase_rom_job_mbc5(job);
}


esp_err_t burner_run_write_gba_save_job_new(const burner_task_param_t *job)
{
    (void)job;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t burner_run_read_gba_save_job_new(const burner_task_param_t *job)
{
    (void)job;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t burner_run_verify_gba_save_job_new(const burner_task_param_t *job)
{
    (void)job;
    return ESP_ERR_NOT_SUPPORTED;
}
