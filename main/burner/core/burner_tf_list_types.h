#pragma once

/* Shared HTTP directory JSON scratch layout. Requires TF_PATH_LEN_MAX. */
typedef struct {
    char esc_path[TF_PATH_LEN_MAX * 2 + 8];
    char head[TF_PATH_LEN_MAX * 2 + 120];
    char child_rel[TF_PATH_LEN_MAX];
    char child_full[TF_PATH_LEN_MAX + 64];
    char esc_name[TF_PATH_LEN_MAX * 2 + 8];
    char esc_child[TF_PATH_LEN_MAX * 2 + 8];
    char line[TF_PATH_LEN_MAX * 4 + 128];
    char batch[8192];
    size_t batch_used;
} burner_tf_list_buf_t;
