#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "../../main/reader/epub_native.h"

int main(int argc, char **argv)
{
    assert(argc == 2);
    ui_epub_book_t *book = nullptr;
    assert(ui_epub_book_open(argv[1], &book));
    assert(std::strcmp(ui_epub_book_title(book), "Performance fixture") == 0);
    assert(ui_epub_book_section_count(book) == 32);
    uint32_t hash = 2166136261U;
    size_t bytes = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < 32; ++i) {
            unsigned section = pass == 0 ? i : 31 - i;
            uint8_t *text = nullptr;
            size_t length = 0;
            assert(ui_epub_book_load_section_text(book, section, &text, &length));
            char expected[32];
            std::snprintf(expected, sizeof(expected), "Chapter %u", section);
            assert(std::strstr(reinterpret_cast<char *>(text), expected));
            assert(std::strstr(reinterpret_cast<char *>(text), "bold"));
            assert(std::strchr(reinterpret_cast<char *>(text), '&'));
            assert(std::strstr(reinterpret_cast<char *>(text), "text."));
            for (size_t j = 0; j < length; ++j) hash = (hash ^ text[j]) * 16777619U;
            bytes += length;
            ui_epub_book_free_buffer(text);
        }
    }
    uint8_t *text = nullptr;
    size_t length = 0;
    assert(hash == 0xF1333AC3U && bytes == 218348);
    assert(ui_epub_book_index_build_count(book) == 1);
    assert(!ui_epub_book_load_section_text(book, 32, &text, &length));
    ui_epub_book_close(book);
    std::string path(argv[1]);
    size_t slash = path.find_last_of("/\\");
    std::string root = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    assert(ui_epub_book_open((root + "epub-empty.epub").c_str(), &book));
    assert(ui_epub_book_load_section_text(book, 0, &text, &length) && length == 0);
    ui_epub_book_free_buffer(text);
    ui_epub_book_close(book);
    assert(ui_epub_book_open((root + "epub-bad-crc.epub").c_str(), &book));
    assert(!ui_epub_book_load_section_text(book, 1, &text, &length));
    ui_epub_book_close(book);
    assert(!ui_epub_book_open("missing-epub-test.epub", &book));
    std::printf("EPUB verified: bytes=%zu hash=%08x\n", bytes, hash);
    return 0;
}
