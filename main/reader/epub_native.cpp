#include "epub_native.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "esp_log.h"
#include "third_party/tinyxml2/tinyxml2.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "third_party/miniz/miniz.h"

namespace {

constexpr const char *TAG = "ui_epub";

struct HtmlEntityMapping {
    const char *name;
    const char *value;
};

constexpr HtmlEntityMapping kHtmlEntities[] = {
    {"&amp;", "&"},
    {"&lt;", "<"},
    {"&gt;", ">"},
    {"&quot;", "\""},
    {"&apos;", "'"},
    {"&nbsp;", " "},
    {"&ensp;", " "},
    {"&emsp;", " "},
    {"&thinsp;", " "},
    {"&ldquo;", "\""},
    {"&rdquo;", "\""},
    {"&lsquo;", "'"},
    {"&rsquo;", "'"},
    {"&mdash;", "-"},
    {"&ndash;", "-"},
    {"&hellip;", "..."},
    {"&bull;", "*"},
    {"&middot;", "*"},
    {"&copy;", "(c)"},
    {"&reg;", "(r)"},
    {"&trade;", "(tm)"},
};

static void append_utf8(uint32_t codepoint, std::string *out)
{
    if (out == nullptr) {
        return;
    }
    if (codepoint < 0x80U) {
        out->push_back(static_cast<char>(codepoint));
        return;
    }
    if (codepoint < 0x800U) {
        out->push_back(static_cast<char>(0xC0U | (codepoint >> 6)));
        out->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        return;
    }
    if (codepoint < 0x10000U) {
        out->push_back(static_cast<char>(0xE0U | (codepoint >> 12)));
        out->push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3FU)));
        out->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        return;
    }
    if (codepoint <= 0x10FFFFU) {
        out->push_back(static_cast<char>(0xF0U | (codepoint >> 18)));
        out->push_back(static_cast<char>(0x80U | ((codepoint >> 12) & 0x3FU)));
        out->push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3FU)));
        out->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

static bool decode_named_entity(const std::string &entity, std::string *out)
{
    for (const HtmlEntityMapping &mapping : kHtmlEntities) {
        if (entity == mapping.name) {
            out->append(mapping.value);
            return true;
        }
    }
    return false;
}

static bool decode_numeric_entity(const std::string &entity, std::string *out)
{
    char *end = nullptr;
    unsigned long value = 0UL;

    if (entity.size() < 4U || entity[0] != '&' || entity[1] != '#') {
        return false;
    }

    if (entity[2] == 'x' || entity[2] == 'X') {
        value = strtoul(entity.c_str() + 3, &end, 16);
    } else {
        value = strtoul(entity.c_str() + 2, &end, 10);
    }
    if (end == nullptr || *end != ';' || value == 0UL) {
        return false;
    }

    if (value == 0xA0UL) {
        out->push_back(' ');
        return true;
    }
    append_utf8(static_cast<uint32_t>(value), out);
    return true;
}

static std::string replace_html_entities(const std::string &input)
{
    std::string output;
    size_t offset = 0U;

    output.reserve(input.size());
    while (offset < input.size()) {
        if (input[offset] != '&') {
            output.push_back(input[offset++]);
            continue;
        }

        size_t semi = input.find(';', offset + 1U);
        if (semi == std::string::npos || semi - offset > 16U) {
            output.push_back(input[offset++]);
            continue;
        }

        std::string entity = input.substr(offset, semi - offset + 1U);
        std::string decoded;
        bool handled = false;

        if (entity.size() > 3U && entity[1] == '#') {
            handled = decode_numeric_entity(entity, &decoded);
        } else {
            handled = decode_named_entity(entity, &decoded);
        }

        if (handled) {
            output += decoded;
        } else {
            output.push_back(' ');
        }
        offset = semi + 1U;
    }

    return output;
}

static std::string normalize_zip_path(const std::string &path)
{
    std::vector<std::string> parts;
    std::string current;

    for (char ch : path) {
        if (ch == '\\') {
            ch = '/';
        }
        if (ch == '/') {
            if (current == "..") {
                if (!parts.empty()) {
                    parts.pop_back();
                }
            } else if (!current.empty() && current != ".") {
                parts.push_back(current);
            }
            current.clear();
            continue;
        }
        current.push_back(ch);
    }

    if (current == "..") {
        if (!parts.empty()) {
            parts.pop_back();
        }
    } else if (!current.empty() && current != ".") {
        parts.push_back(current);
    }

    std::string normalized;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0U) {
            normalized.push_back('/');
        }
        normalized += parts[i];
    }
    return normalized;
}

static std::string zip_join_path(const std::string &base, const std::string &child)
{
    if (child.empty()) {
        return normalize_zip_path(base);
    }
    if (!child.empty() && child[0] == '/') {
        return normalize_zip_path(child.substr(1));
    }
    if (base.empty()) {
        return normalize_zip_path(child);
    }
    return normalize_zip_path(base + child);
}

class ZipArchiveReader {
public:
    explicit ZipArchiveReader(std::string path) : path_(std::move(path)) {}

    bool read_file(const std::string &entry_name, std::string *out) const
    {
        mz_zip_archive archive = {};
        mz_uint32 file_index = 0U;
        mz_zip_archive_file_stat stat = {};
        void *data = nullptr;
        bool ok = false;

        if (out == nullptr) {
            return false;
        }
        out->clear();

        if (!mz_zip_reader_init_file(&archive, path_.c_str(), 0U)) {
            ESP_LOGW(TAG, "open zip failed: %s", path_.c_str());
            return false;
        }
        if (!mz_zip_reader_locate_file_v2(&archive, entry_name.c_str(), nullptr, 0U, &file_index)) {
            mz_zip_reader_end(&archive);
            return false;
        }
        if (!mz_zip_reader_file_stat(&archive, file_index, &stat)) {
            mz_zip_reader_end(&archive);
            return false;
        }

        data = mz_zip_reader_extract_to_heap(&archive, file_index, nullptr, 0U);
        if (data == nullptr) {
            mz_zip_reader_end(&archive);
            return false;
        }

        out->assign(static_cast<const char *>(data), static_cast<size_t>(stat.m_uncomp_size));
        mz_free(data);
        ok = true;
        mz_zip_reader_end(&archive);
        return ok;
    }

private:
    std::string path_;
};

static const tinyxml2::XMLElement *first_child_element_any_namespace(
    const tinyxml2::XMLNode *node,
    const char *local_name)
{
    const tinyxml2::XMLElement *element = nullptr;

    if (node == nullptr || local_name == nullptr) {
        return nullptr;
    }

    element = node->FirstChildElement();
    while (element != nullptr) {
        const char *name = element->Name();
        const char *colon = (name != nullptr) ? strrchr(name, ':') : nullptr;
        const char *candidate = (colon != nullptr) ? (colon + 1) : name;

        if (candidate != nullptr && strcmp(candidate, local_name) == 0) {
            return element;
        }
        element = element->NextSiblingElement();
    }
    return nullptr;
}

class HtmlTextExtractor : public tinyxml2::XMLVisitor {
public:
    std::string extract(const std::string &html)
    {
        tinyxml2::XMLDocument document(false, tinyxml2::COLLAPSE_WHITESPACE);
        std::string sanitized = replace_html_entities(html);
        text_.clear();
        preserve_whitespace_ = false;

        if (document.Parse(sanitized.data(), sanitized.size()) != tinyxml2::XML_SUCCESS) {
            return fallback_strip_tags(sanitized);
        }

        document.Accept(this);
        cleanup_text();
        return text_;
    }

    bool VisitEnter(const tinyxml2::XMLElement &element, const tinyxml2::XMLAttribute *) override
    {
        std::string tag = canonical_tag_name(element.Name());

        if (tag == "head" || tag == "style" || tag == "script" || tag == "svg") {
            return false;
        }
        if (tag == "br") {
            append_break(false);
            return true;
        }
        if (tag == "p" || tag == "div" || tag == "li" || tag == "blockquote" ||
            tag == "section" || tag == "article" || tag == "h1" || tag == "h2" ||
            tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6") {
            append_break(true);
        }
        if (tag == "pre") {
            append_break(true);
            preserve_whitespace_ = true;
        }
        if (tag == "img") {
            append_break(false);
        }
        return true;
    }

    bool Visit(const tinyxml2::XMLText &text) override
    {
        append_text(text.Value());
        return true;
    }

    bool VisitExit(const tinyxml2::XMLElement &element) override
    {
        std::string tag = canonical_tag_name(element.Name());

        if (tag == "pre") {
            preserve_whitespace_ = false;
        }
        if (tag == "p" || tag == "div" || tag == "li" || tag == "blockquote" ||
            tag == "section" || tag == "article" || tag == "h1" || tag == "h2" ||
            tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6" || tag == "pre") {
            append_break(true);
        }
        return true;
    }

private:
    std::string text_;
    bool preserve_whitespace_ = false;

    static std::string canonical_tag_name(const char *name)
    {
        std::string tag = (name != nullptr) ? name : "";
        size_t colon = tag.rfind(':');

        if (colon != std::string::npos) {
            tag = tag.substr(colon + 1U);
        }
        std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char ch) {
            return static_cast<char>(tolower(ch));
        });
        return tag;
    }

    void append_break(bool paragraph_break)
    {
        size_t newline_count = 0U;
        size_t desired = paragraph_break ? 2U : 1U;

        while (!text_.empty() && text_.back() == ' ') {
            text_.pop_back();
        }
        while (newline_count < text_.size() && text_[text_.size() - newline_count - 1U] == '\n') {
            newline_count++;
        }
        while (newline_count < desired) {
            text_.push_back('\n');
            newline_count++;
        }
    }

    void append_text(const char *value)
    {
        bool pending_space = false;

        if (value == nullptr) {
            return;
        }
        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p != 0U; ++p) {
            if (!preserve_whitespace_ && (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')) {
                pending_space = !text_.empty() && text_.back() != ' ' && text_.back() != '\n';
                continue;
            }
            if (preserve_whitespace_ && (*p == '\r' || *p == '\n')) {
                append_break(false);
                pending_space = false;
                continue;
            }
            if (preserve_whitespace_ && *p == '\t') {
                pending_space = true;
                continue;
            }
            if (pending_space) {
                text_.push_back(' ');
                pending_space = false;
            }
            text_.push_back(static_cast<char>(*p));
        }
    }

    void cleanup_text()
    {
        std::string normalized;
        size_t newline_run = 0U;

        normalized.reserve(text_.size());
        for (char ch : text_) {
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                newline_run++;
                if (newline_run <= 2U) {
                    normalized.push_back('\n');
                }
                continue;
            }
            newline_run = 0U;
            normalized.push_back(ch);
        }

        while (!normalized.empty() &&
               (normalized.back() == '\n' || normalized.back() == ' ' || normalized.back() == '\t')) {
            normalized.pop_back();
        }
        text_.swap(normalized);
    }

    static std::string fallback_strip_tags(const std::string &html)
    {
        std::string out;
        bool in_tag = false;

        out.reserve(html.size());
        for (size_t i = 0; i < html.size(); ++i) {
            char ch = html[i];

            if (ch == '<') {
                in_tag = true;
                if (!out.empty() && out.back() != '\n') {
                    out.push_back('\n');
                }
                continue;
            }
            if (ch == '>') {
                in_tag = false;
                continue;
            }
            if (!in_tag) {
                out.push_back(ch);
            }
        }
        return out;
    }
};

class EpubBookImpl {
public:
    explicit EpubBookImpl(std::string path) : zip_(path), path_(std::move(path)) {}

    bool load()
    {
        std::string container_xml;
        std::string content_opf_path;
        tinyxml2::XMLDocument document;
        const tinyxml2::XMLElement *container = nullptr;
        const tinyxml2::XMLElement *rootfiles = nullptr;
        const tinyxml2::XMLElement *rootfile = nullptr;

        if (!zip_.read_file("META-INF/container.xml", &container_xml)) {
            ESP_LOGW(TAG, "missing META-INF/container.xml");
            return false;
        }
        container_xml = replace_html_entities(container_xml);
        if (document.Parse(container_xml.data(), container_xml.size()) != tinyxml2::XML_SUCCESS) {
            ESP_LOGW(TAG, "parse container.xml failed");
            return false;
        }

        container = document.FirstChildElement("container");
        rootfiles = first_child_element_any_namespace(container, "rootfiles");
        rootfile = first_child_element_any_namespace(rootfiles, "rootfile");
        while (rootfile != nullptr) {
            const char *media_type = rootfile->Attribute("media-type");
            const char *full_path = rootfile->Attribute("full-path");

            if (media_type != nullptr && full_path != nullptr &&
                strcmp(media_type, "application/oebps-package+xml") == 0) {
                content_opf_path = full_path;
                break;
            }
            rootfile = rootfile->NextSiblingElement();
        }

        if (content_opf_path.empty()) {
            ESP_LOGW(TAG, "content.opf not found");
            return false;
        }
        return parse_content_opf(normalize_zip_path(content_opf_path));
    }

    const std::string &title() const
    {
        return title_;
    }

    uint32_t section_count() const
    {
        return static_cast<uint32_t>(spine_items_.size());
    }

    bool load_section_text(uint32_t section_index, std::string *out_text)
    {
        std::string html;
        HtmlTextExtractor extractor;

        if (out_text == nullptr || section_index >= spine_items_.size()) {
            return false;
        }
        if (!zip_.read_file(spine_items_[section_index], &html)) {
            return false;
        }
        *out_text = extractor.extract(html);
        return true;
    }

private:
    ZipArchiveReader zip_;
    std::string path_;
    std::string title_;
    std::string base_path_;
    std::vector<std::string> spine_items_;

    bool parse_content_opf(const std::string &opf_path)
    {
        std::string opf_text;
        tinyxml2::XMLDocument document;
        const tinyxml2::XMLElement *package = nullptr;
        const tinyxml2::XMLElement *metadata = nullptr;
        const tinyxml2::XMLElement *manifest = nullptr;
        const tinyxml2::XMLElement *spine = nullptr;
        std::map<std::string, std::string> items;
        size_t slash = opf_path.find_last_of('/');

        if (!zip_.read_file(opf_path, &opf_text)) {
            return false;
        }
        opf_text = replace_html_entities(opf_text);
        if (document.Parse(opf_text.data(), opf_text.size()) != tinyxml2::XML_SUCCESS) {
            ESP_LOGW(TAG, "parse opf failed: %s", opf_path.c_str());
            return false;
        }

        base_path_ = (slash == std::string::npos) ? "" : opf_path.substr(0, slash + 1U);
        package = document.FirstChildElement("package");
        if (package == nullptr) {
            package = document.RootElement();
        }
        metadata = first_child_element_any_namespace(package, "metadata");
        if (metadata != nullptr) {
            const tinyxml2::XMLElement *title = first_child_element_any_namespace(metadata, "title");
            if (title != nullptr && title->GetText() != nullptr) {
                title_ = title->GetText();
            }
        }

        manifest = first_child_element_any_namespace(package, "manifest");
        if (manifest == nullptr) {
            return false;
        }
        for (const tinyxml2::XMLElement *item = manifest->FirstChildElement(); item != nullptr; item = item->NextSiblingElement()) {
            const char *id = item->Attribute("id");
            const char *href = item->Attribute("href");

            if (id == nullptr || href == nullptr) {
                continue;
            }
            items[id] = zip_join_path(base_path_, href);
        }

        spine = first_child_element_any_namespace(package, "spine");
        if (spine == nullptr) {
            return false;
        }
        for (const tinyxml2::XMLElement *itemref = spine->FirstChildElement(); itemref != nullptr; itemref = itemref->NextSiblingElement()) {
            const char *idref = itemref->Attribute("idref");
            auto found = (idref != nullptr) ? items.find(idref) : items.end();

            if (found != items.end()) {
                spine_items_.push_back(found->second);
            }
        }

        if (title_.empty()) {
            title_ = path_;
        }
        return !spine_items_.empty();
    }
};

}  // namespace

struct ui_epub_book {
    EpubBookImpl *impl;
};

extern "C" bool ui_epub_book_open(const char *path, ui_epub_book_t **out_book)
{
    std::unique_ptr<ui_epub_book_t> book;

    if (out_book == nullptr) {
        return false;
    }
    *out_book = nullptr;
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    book.reset(new ui_epub_book_t());
    book->impl = new EpubBookImpl(path);
    if (!book->impl->load()) {
        delete book->impl;
        return false;
    }

    *out_book = book.release();
    return true;
}

extern "C" void ui_epub_book_close(ui_epub_book_t *book)
{
    if (book == nullptr) {
        return;
    }
    delete book->impl;
    delete book;
}

extern "C" const char *ui_epub_book_title(const ui_epub_book_t *book)
{
    if (book == nullptr || book->impl == nullptr) {
        return "";
    }
    return book->impl->title().c_str();
}

extern "C" uint32_t ui_epub_book_section_count(const ui_epub_book_t *book)
{
    if (book == nullptr || book->impl == nullptr) {
        return 0U;
    }
    return book->impl->section_count();
}

extern "C" bool ui_epub_book_load_section_text(
    ui_epub_book_t *book,
    uint32_t section_index,
    uint8_t **text_out,
    size_t *text_len_out)
{
    std::string text;
    uint8_t *buffer = nullptr;

    if (text_out != nullptr) {
        *text_out = nullptr;
    }
    if (text_len_out != nullptr) {
        *text_len_out = 0U;
    }
    if (book == nullptr || book->impl == nullptr || text_out == nullptr) {
        return false;
    }
    if (!book->impl->load_section_text(section_index, &text)) {
        return false;
    }

    buffer = static_cast<uint8_t *>(malloc(text.size() + 1U));
    if (buffer == nullptr) {
        return false;
    }
    if (!text.empty()) {
        memcpy(buffer, text.data(), text.size());
    }
    buffer[text.size()] = '\0';
    *text_out = buffer;
    if (text_len_out != nullptr) {
        *text_len_out = text.size();
    }
    return true;
}

extern "C" void ui_epub_book_free_buffer(void *ptr)
{
    free(ptr);
}
