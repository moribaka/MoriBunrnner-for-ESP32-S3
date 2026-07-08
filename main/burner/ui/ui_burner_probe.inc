static void ui_burn_probe_task(void *param)
{
    ui_work_request_t *request = (ui_work_request_t *)param;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *ok_text = ui_tr("done");
    char id_text[UI_STATUS_TEXT_MAX_LEN] = {0};
    bool task_with_caps = false;

    if (request == NULL) {
        vTaskDelete(NULL);
        return;
    }
    task_with_caps = request->task_with_caps;

    switch (request->type) {
        case UI_WORK_BURN_READ_ID:
            burner_reset_cart_probe_state();
            err = ui_read_cart_id_once(id_text, sizeof(id_text), request->cart_mode);
            ok_text = (err == ESP_OK) ? id_text : ui_tr("read id failed");
            break;
        case UI_WORK_BURN_UNLOCK_PPB:
            err = ui_unlock_ppb_once(id_text, sizeof(id_text), request->cart_mode);
            ok_text = (err == ESP_OK) ? id_text : ui_tr("PPB unlock failed");
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    ui_finish_work_task(request->type, err, ok_text, request->cart_mode);
    ESP_LOGI(UI_TAG, "UI probe task stack free min=%u bytes", (unsigned)uxTaskGetStackHighWaterMark2(NULL));
    free(request);
    ui_delete_async_task(task_with_caps);
}

