#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_epub_book ui_epub_book_t;

bool ui_epub_book_open(const char *path, ui_epub_book_t **out_book);
void ui_epub_book_close(ui_epub_book_t *book);

const char *ui_epub_book_title(const ui_epub_book_t *book);
uint32_t ui_epub_book_section_count(const ui_epub_book_t *book);

bool ui_epub_book_load_section_text(
    ui_epub_book_t *book,
    uint32_t section_index,
    uint8_t **text_out,
    size_t *text_len_out);

void ui_epub_book_free_buffer(void *ptr);

#ifdef __cplusplus
}
#endif
