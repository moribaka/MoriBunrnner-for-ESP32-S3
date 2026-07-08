static void ui_fill_music_player_row(
    const ui_model_t *model,
    uint16_t index,
    char *title,
    size_t title_len,
    char *hint,
    size_t hint_len)
{
    music_player_snapshot_t snap = {0};
    uint32_t percent = 0U;
    char percent_text[12] = {0};

    (void)model;
    (void)index;
    music_player_get_snapshot(&snap);
    if (snap.file_size != 0U) {
        percent = (uint32_t)(((uint64_t)snap.position * 100ULL) / (uint64_t)snap.file_size);
        if (percent > 100U) {
            percent = 100U;
        }
    }
    snprintf(percent_text, sizeof(percent_text), "%lu%%", (unsigned long)percent);
    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("Track"));
            snprintf(hint, hint_len, "%s", snap.name[0] != '\0' ? snap.name : "--");
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("State"));
            switch (snap.state) {
                case MUSIC_PLAYER_STATE_LOADING:
                    snprintf(hint, hint_len, "%s", ui_tr("loading"));
                    break;
                case MUSIC_PLAYER_STATE_PLAYING:
                    snprintf(hint, hint_len, "%s", ui_tr("playing"));
                    break;
                case MUSIC_PLAYER_STATE_PAUSED:
                    snprintf(hint, hint_len, "%s", ui_tr("paused"));
                    break;
                case MUSIC_PLAYER_STATE_FINISHED:
                    snprintf(hint, hint_len, "%s", ui_tr("done"));
                    break;
                case MUSIC_PLAYER_STATE_ERROR:
                    snprintf(hint, hint_len, "%s", snap.message[0] != '\0' ? snap.message : ui_tr("error"));
                    break;
                case MUSIC_PLAYER_STATE_IDLE:
                default:
                    snprintf(hint, hint_len, "%s", ui_tr("idle"));
                    break;
            }
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Progress"));
            snprintf(hint, hint_len, "%s", percent_text);
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Volume"));
            snprintf(hint, hint_len, "%u%%", snap.volume_percent);
            break;
        default:
            title[0] = '\0';
            hint[0] = '\0';
            break;
    }
}

