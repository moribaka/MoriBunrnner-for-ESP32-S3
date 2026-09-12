/* Read-only, explicit diagnostic. No NVS changes and no SWD operations. */
static void cpld_read_bench(void)
{
    if (burner_task_is_running_snapshot() || ag32_batch_program_is_running()) {
        message("error", "cartridge or firmware job is running"); return;
    }
    uint8_t *expected = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM);
    uint8_t *actual = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM);
    esp_err_t err = expected && actual ? ESP_OK : ESP_ERR_NO_MEM;
    bool perf = power_manager_perf_lock_acquire("cpld_read_bench") == ESP_OK;
    if (!perf && err == ESP_OK) err = ESP_ERR_INVALID_STATE;
    uint64_t elapsed[3] = {0};
    uint64_t ready[2] = {0}, start[2] = {0}, finish[2] = {0};
    uint64_t crc[2] = {0}, copy[2] = {0}, polls[2] = {0};
    uint32_t checked = 0, boundary_passes = 0, failure_address = 0;
    const char *failure_variant = "prepare";
    burner_spi_lock_take();
    ag32_link_preference_t previous = ag32_mcu_link_get_preference();
    if (burner_task_is_running_snapshot() || ag32_batch_program_is_running())
        err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = burner_spi_init();
    // Caller explicitly selects this GBA-only diagnostic; prepare the usual rail.
    if (err == ESP_OK) err = burner_bacon_gba_prepare_power();
    ag32_mcu_link_set_preference(AG32_LINK_PREFERENCE_CPLD);
    // Legacy comparison catches common-mode candidate/baseline faults, and
    // covers 4-byte padding, packet boundaries and the final GBA address.
    static const size_t lengths[] = {2, 4, 6, 1018, 1020, 1022, 2040, 2042, 65536};
    for (unsigned edge = 0; err == ESP_OK && edge < 2; ++edge) {
        for (unsigned i = 0; err == ESP_OK && i < sizeof(lengths)/sizeof(lengths[0]); ++i) {
            size_t count = lengths[i];
            uint32_t address = edge ? 33554432u - count : 0;
            failure_address = address;
            failure_variant = "legacy-boundary";
            ag32_mcu_link_set_preference(AG32_LINK_PREFERENCE_LEGACY);
            err = burner_bacon_gba_verify_read_block_hoststyle(expected, count, address, false);
            ag32_mcu_link_set_preference(AG32_LINK_PREFERENCE_CPLD);
            for (unsigned variant = 0; err == ESP_OK && variant < 2; ++variant) {
                bacon_cpld_read_profile_t p;
                failure_variant = variant ? "pipeline-boundary" : "exclusive-boundary";
                err = bacon_cpld_read_experiment_locked(address, actual, count, variant != 0, &p);
                if (err == ESP_OK && memcmp(expected, actual, count)) err = ESP_ERR_INVALID_RESPONSE;
                if (err == ESP_OK) ++boundary_passes;
            }
        }
    }
    for (uint32_t address = 0; err == ESP_OK && address < 33554432u; address += 65536) {
        failure_address = address;
        failure_variant = "original";
        bool used = false;
        int64_t t = esp_timer_get_time();
        err = bacon_cpld_try_transfer_locked(BACON_CPLD_GBA_READ,
            address, expected, 65536, 0, 2000, &used);
        elapsed[0] += esp_timer_get_time() - t;
        if (err == ESP_OK && !used) err = ESP_ERR_NOT_SUPPORTED;
        // Alternate candidate order to reduce cache/order bias.
        for (unsigned order = 0; err == ESP_OK && order < 2; ++order) {
            unsigned variant = order ^ ((address / 65536u) & 1u);
            bacon_cpld_read_profile_t p;
            failure_variant = variant ? "pipeline" : "exclusive";
            t = esp_timer_get_time();
            err = bacon_cpld_read_experiment_locked(address, actual, 65536, variant != 0, &p);
            elapsed[variant + 1] += esp_timer_get_time() - t;
            ready[variant] += p.ready_us; start[variant] += p.start_us;
            finish[variant] += p.finish_us; crc[variant] += p.crc_us;
            copy[variant] += p.copy_us; polls[variant] += p.status_polls;
            if (err == ESP_OK && memcmp(expected, actual, 65536)) err = ESP_ERR_INVALID_RESPONSE;
        }
        if (err == ESP_OK) checked += 65536;
        if (err == ESP_OK && (checked % (4u * 1024u * 1024u)) == 0) {
            cJSON *progress = event("cpld_read_progress");
            if (progress) cJSON_AddNumberToObject(progress, "checked_bytes", checked);
            reply(progress);
        }
        if (err == ESP_OK) err = burner_cancel_poll();
        burner_task_yield_if_due();
    }
    ag32_mcu_link_set_preference(previous);
    burner_spi_lock_give();
    if (perf) power_manager_perf_lock_release("cpld_read_bench");
    free(expected); free(actual);
    cJSON *json = event("cpld_read_bench");
    if (json) {
        cJSON_AddBoolToObject(json, "ok", err == ESP_OK && checked == 33554432u && boundary_passes == 36);
        cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
        cJSON_AddNumberToObject(json, "checked_bytes", checked);
        cJSON_AddNumberToObject(json, "boundary_passes", boundary_passes);
        cJSON_AddNumberToObject(json, "failure_address", err == ESP_OK ? 0 : failure_address);
        cJSON_AddStringToObject(json, "failure_variant", err == ESP_OK ? "" : failure_variant);
        static const char *names[] = {"original", "exclusive", "pipeline"};
        for (unsigned i = 0; i < 3; ++i) {
            cJSON *p = cJSON_AddObjectToObject(json, names[i]);
            cJSON_AddNumberToObject(p, "elapsed_us", elapsed[i]);
            if (i) {
                cJSON_AddNumberToObject(p, "ready_us", ready[i-1]);
                cJSON_AddNumberToObject(p, "start_us", start[i-1]);
                cJSON_AddNumberToObject(p, "finish_us", finish[i-1]);
                cJSON_AddNumberToObject(p, "crc_us", crc[i-1]);
                cJSON_AddNumberToObject(p, "copy_us", copy[i-1]);
                cJSON_AddNumberToObject(p, "status_polls", polls[i-1]);
            }
        }
    }
    reply(json);
}
