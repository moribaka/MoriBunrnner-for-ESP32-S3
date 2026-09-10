/* Cartridge helpers shared across GBC/MBC5 and GBA operation paths. */

bool burner_get_mbc5_slot_range(bool ram_range, uint32_t slot, uint32_t *addr_begin, uint32_t *addr_end)
{
    const uint32_t (*table)[2] = ram_range ? s_mbc5_multi_ram_range : s_mbc5_multi_rom_range;

    if (addr_begin == NULL || addr_end == NULL || slot > BURNER_MBC5_SLOT_MAX) {
        return false;
    }

    *addr_begin = table[slot][0];
    *addr_end = table[slot][1];
    return true;
}

esp_err_t burner_apply_mbc5_slot_limit(
    bool ram_range,
    uint32_t slot,
    uint32_t requested_size,
    uint32_t *addr_begin,
    uint32_t *effective_size)
{
    uint32_t slot_begin;
    uint32_t slot_end;
    uint64_t slot_size;

    if (addr_begin == NULL || effective_size == NULL || requested_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot > BURNER_MBC5_SLOT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (slot == 0u) {
        *addr_begin = 0u;
        *effective_size = requested_size;
        return ESP_OK;
    }

    if (!burner_get_mbc5_slot_range(ram_range, slot, &slot_begin, &slot_end) || slot_end < slot_begin) {
        return ESP_ERR_INVALID_ARG;
    }
    slot_size = (uint64_t)slot_end - (uint64_t)slot_begin + 1u;
    if ((uint64_t)requested_size > slot_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    *addr_begin = slot_begin;
    *effective_size = requested_size;
    return ESP_OK;
}

esp_err_t burner_apply_gba_slot_limit(
    uint32_t slot,
    uint32_t requested_size,
    uint32_t *addr_begin,
    uint32_t *effective_size,
    bool *force_multi)
{
    uint64_t base = 0;

    if (addr_begin == NULL || effective_size == NULL || force_multi == NULL || requested_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (slot == 0u) {
        base = 0u;
        *force_multi = false;
    } else if (slot == 1u) {
        base = 0u;
        *force_multi = true;
    } else {
        /*
         * Host mission_gba.cs:
         * index >= 2 => base = (8 + 4 * (index - 2)) MB.
         */
        base = (uint64_t)(8u + (4u * (slot - 2u))) * 1024u * 1024u;
        *force_multi = true;
    }

    if (base > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    *addr_begin = (uint32_t)base;
    *effective_size = requested_size;
    return ESP_OK;
}

bool burner_parse_ram_mode(const char *ram_type_text, bool *fram_mode)
{
    if (fram_mode == NULL) {
        return false;
    }

    *fram_mode = false;
    if (ram_type_text == NULL || ram_type_text[0] == '\0') {
        return true;
    }

    if (strcasecmp(ram_type_text, "sram") == 0) {
        *fram_mode = false;
        return true;
    }
    if (strcasecmp(ram_type_text, "fram") == 0) {
        *fram_mode = true;
        return true;
    }

    return false;
}

static void burner_nor_geometry_clear(burner_nor_geometry_t *geometry)
{
    if (geometry == NULL) {
        return;
    }
    memset(geometry, 0, sizeof(*geometry));
}

static bool burner_nor_geometry_is_valid(const burner_nor_geometry_t *geometry)
{
    uint32_t prev_end = 0u;

    if (geometry == NULL || geometry->region_count == 0u ||
        geometry->region_count > BURNER_NOR_GEOMETRY_REGION_MAX ||
        geometry->largest_sector_size == 0u) {
        return false;
    }

    for (uint32_t i = 0u; i < geometry->region_count; ++i) {
        const burner_nor_region_t *region = &geometry->regions[i];

        if (region->sector_size == 0u || region->addr_end <= region->addr_begin) {
            return false;
        }
        if (((region->addr_end - region->addr_begin) % region->sector_size) != 0u) {
            return false;
        }
        if (i > 0u && region->addr_begin != prev_end) {
            return false;
        }
        prev_end = region->addr_end;
    }

    return true;
}

static bool burner_nor_geometry_is_uniform(const burner_nor_geometry_t *geometry)
{
    return burner_nor_geometry_is_valid(geometry) && geometry->uniform_sector_size > 0u;
}

static uint32_t burner_nor_geometry_display_sector_size(const burner_nor_geometry_t *geometry)
{
    return burner_nor_geometry_is_uniform(geometry) ? geometry->uniform_sector_size : 0u;
}

static uint32_t burner_nor_geometry_largest_sector_size(const burner_nor_geometry_t *geometry)
{
    return burner_nor_geometry_is_valid(geometry) ? geometry->largest_sector_size : 0u;
}

static uint32_t burner_nor_geometry_report_sector_size(const burner_nor_geometry_t *geometry)
{
    uint32_t sector_size = burner_nor_geometry_display_sector_size(geometry);

    if (sector_size == 0u) {
        sector_size = burner_nor_geometry_largest_sector_size(geometry);
    }
    return sector_size;
}

const char *burner_gb_mapper_name(burner_gb_mapper_t mapper)
{
    switch (mapper) {
        case BURNER_GB_MAPPER_MBC3:
            return "MBC3";
        case BURNER_GB_MAPPER_MBC5:
            return "MBC5";
        default:
            return "UNKNOWN";
    }
}

static bool burner_nor_geometry_equal(
    const burner_nor_geometry_t *left,
    const burner_nor_geometry_t *right)
{
    if (!burner_nor_geometry_is_valid(left) || !burner_nor_geometry_is_valid(right) ||
        left->region_count != right->region_count ||
        left->uniform_sector_size != right->uniform_sector_size ||
        left->smallest_sector_size != right->smallest_sector_size ||
        left->largest_sector_size != right->largest_sector_size) {
        return false;
    }

    for (uint32_t i = 0u; i < left->region_count; ++i) {
        if (left->regions[i].addr_begin != right->regions[i].addr_begin ||
            left->regions[i].addr_end != right->regions[i].addr_end ||
            left->regions[i].sector_size != right->regions[i].sector_size) {
            return false;
        }
    }
    return true;
}


static esp_err_t burner_nor_geometry_set_uniform(
    burner_nor_geometry_t *geometry,
    uint32_t device_size,
    uint32_t sector_size)
{
    burner_nor_geometry_clear(geometry);
    if (geometry == NULL || device_size == 0u || sector_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    geometry->region_count = 1u;
    geometry->uniform_sector_size = sector_size;
    geometry->smallest_sector_size = sector_size;
    geometry->largest_sector_size = sector_size;
    geometry->regions[0].addr_begin = 0u;
    geometry->regions[0].addr_end = device_size;
    geometry->regions[0].sector_size = sector_size;
    return ESP_OK;
}

static esp_err_t burner_nor_geometry_build(
    burner_nor_geometry_t *geometry,
    uint32_t device_size,
    const uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX],
    const uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX],
    uint32_t region_count,
    bool reverse_order)
{
    uint32_t current_addr = 0u;
    uint32_t uniform_sector_size = 0u;
    uint32_t smallest_sector_size = UINT32_MAX;
    uint32_t largest_sector_size = 0u;
    uint64_t total_size = 0u;

    burner_nor_geometry_clear(geometry);
    if (geometry == NULL || sector_counts == NULL || sector_sizes == NULL ||
        region_count == 0u || region_count > BURNER_NOR_GEOMETRY_REGION_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uniform_sector_size = sector_sizes[0];
    for (uint32_t i = 0u; i < region_count; ++i) {
        uint64_t region_size;

        if (sector_counts[i] == 0u || sector_sizes[i] == 0u) {
            return ESP_ERR_INVALID_SIZE;
        }
        region_size = (uint64_t)sector_counts[i] * (uint64_t)sector_sizes[i];
        if (region_size == 0u || region_size > UINT32_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        total_size += region_size;
        if (total_size > UINT32_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (sector_sizes[i] < smallest_sector_size) {
            smallest_sector_size = sector_sizes[i];
        }
        if (sector_sizes[i] > largest_sector_size) {
            largest_sector_size = sector_sizes[i];
        }
        if (sector_sizes[i] != uniform_sector_size) {
            uniform_sector_size = 0u;
        }
    }

    if (device_size == 0u) {
        device_size = (uint32_t)total_size;
    } else if ((uint64_t)device_size != total_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    geometry->region_count = (uint8_t)region_count;
    geometry->uniform_sector_size = uniform_sector_size;
    geometry->smallest_sector_size = smallest_sector_size;
    geometry->largest_sector_size = largest_sector_size;

    if (reverse_order) {
        current_addr = device_size;
        for (uint32_t i = 0u; i < region_count; ++i) {
            uint32_t region_span = sector_counts[i] * sector_sizes[i];
            uint32_t dst = region_count - 1u - i;

            if (region_span > current_addr) {
                burner_nor_geometry_clear(geometry);
                return ESP_ERR_INVALID_SIZE;
            }
            current_addr -= region_span;
            geometry->regions[dst].addr_begin = current_addr;
            geometry->regions[dst].addr_end = current_addr + region_span;
            geometry->regions[dst].sector_size = sector_sizes[i];
        }
    } else {
        current_addr = 0u;
        for (uint32_t i = 0u; i < region_count; ++i) {
            uint32_t region_span = sector_counts[i] * sector_sizes[i];

            geometry->regions[i].addr_begin = current_addr;
            geometry->regions[i].addr_end = current_addr + region_span;
            geometry->regions[i].sector_size = sector_sizes[i];
            current_addr += region_span;
        }
    }

    if (!burner_nor_geometry_is_valid(geometry) ||
        geometry->regions[geometry->region_count - 1u].addr_end != device_size) {
        burner_nor_geometry_clear(geometry);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t burner_nor_geometry_sector_bounds(
    const burner_nor_geometry_t *geometry,
    uint32_t addr,
    uint32_t *sector_begin_out,
    uint32_t *sector_end_out,
    uint32_t *sector_size_out)
{
    if (!burner_nor_geometry_is_valid(geometry)) {
        return ESP_ERR_INVALID_STATE;
    }

    for (uint32_t i = 0u; i < geometry->region_count; ++i) {
        const burner_nor_region_t *region = &geometry->regions[i];

        if (addr >= region->addr_begin && addr < region->addr_end) {
            uint32_t offset = addr - region->addr_begin;
            uint32_t sector_index = offset / region->sector_size;
            uint32_t sector_begin = region->addr_begin + (sector_index * region->sector_size);
            uint32_t sector_end = sector_begin + region->sector_size;

            if (sector_begin_out != NULL) {
                *sector_begin_out = sector_begin;
            }
            if (sector_end_out != NULL) {
                *sector_end_out = sector_end;
            }
            if (sector_size_out != NULL) {
                *sector_size_out = region->sector_size;
            }
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t burner_nor_geometry_next_sector_begin(
    const burner_nor_geometry_t *geometry,
    uint32_t sector_begin,
    uint32_t *next_sector_begin_out)
{
    uint32_t current_sector_begin = 0u;
    uint32_t current_sector_end = 0u;

    if (next_sector_begin_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (burner_nor_geometry_sector_bounds(
            geometry,
            sector_begin,
            &current_sector_begin,
            &current_sector_end,
            NULL) != ESP_OK ||
        current_sector_begin != sector_begin) {
        return ESP_ERR_INVALID_ARG;
    }

    if (burner_nor_geometry_sector_bounds(geometry, current_sector_end, next_sector_begin_out, NULL, NULL) == ESP_OK) {
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t burner_nor_geometry_sector_begin_ceil(
    const burner_nor_geometry_t *geometry,
    uint32_t addr,
    uint32_t *sector_begin_out)
{
    uint32_t sector_begin = 0u;
    uint32_t sector_end = 0u;
    esp_err_t err;

    if (sector_begin_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_nor_geometry_sector_bounds(geometry, addr, &sector_begin, &sector_end, NULL);
    if (err != ESP_OK) {
        return err;
    }
    if (addr == sector_begin) {
        *sector_begin_out = sector_begin;
        return ESP_OK;
    }
    return burner_nor_geometry_next_sector_begin(geometry, sector_begin, sector_begin_out);
}

static void burner_nor_region_cursor_clear(burner_nor_region_cursor_t *cursor)
{
    if (cursor == NULL) {
        return;
    }
    memset(cursor, 0, sizeof(*cursor));
}

static bool burner_nor_region_cursor_is_valid(const burner_nor_region_cursor_t *cursor)
{
    return cursor != NULL &&
           cursor->region_index < BURNER_NOR_GEOMETRY_REGION_MAX &&
           cursor->sector_size > 0u &&
           cursor->addr_end > cursor->addr_begin;
}

static esp_err_t burner_nor_geometry_region_cursor_load(
    const burner_nor_geometry_t *geometry,
    uint32_t region_index,
    burner_nor_region_cursor_t *cursor)
{
    const burner_nor_region_t *region;

    if (!burner_nor_geometry_is_valid(geometry) || cursor == NULL || region_index >= geometry->region_count) {
        return ESP_ERR_INVALID_ARG;
    }

    region = &geometry->regions[region_index];
    cursor->region_index = (uint8_t)region_index;
    cursor->addr_begin = region->addr_begin;
    cursor->addr_end = region->addr_end;
    cursor->sector_size = region->sector_size;
    return ESP_OK;
}

static esp_err_t burner_nor_geometry_region_cursor_begin(
    const burner_nor_geometry_t *geometry,
    uint32_t addr,
    burner_nor_region_cursor_t *cursor)
{
    if (!burner_nor_geometry_is_valid(geometry) || cursor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = 0u; i < geometry->region_count; ++i) {
        const burner_nor_region_t *region = &geometry->regions[i];

        if (addr >= region->addr_begin && addr < region->addr_end) {
            return burner_nor_geometry_region_cursor_load(geometry, i, cursor);
        }
    }

    burner_nor_region_cursor_clear(cursor);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t burner_nor_geometry_region_cursor_advance(
    const burner_nor_geometry_t *geometry,
    burner_nor_region_cursor_t *cursor)
{
    if (!burner_nor_region_cursor_is_valid(cursor)) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_nor_geometry_region_cursor_load(geometry, (uint32_t)cursor->region_index + 1u, cursor);
}

static esp_err_t burner_nor_geometry_region_cursor_seek_forward(
    const burner_nor_geometry_t *geometry,
    uint32_t addr,
    burner_nor_region_cursor_t *cursor)
{
    esp_err_t err;

    if (cursor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!burner_nor_region_cursor_is_valid(cursor)) {
        return burner_nor_geometry_region_cursor_begin(geometry, addr, cursor);
    }
    if (addr >= cursor->addr_begin && addr < cursor->addr_end) {
        return ESP_OK;
    }
    if (addr < cursor->addr_begin) {
        return burner_nor_geometry_region_cursor_begin(geometry, addr, cursor);
    }

    while (addr >= cursor->addr_end) {
        err = burner_nor_geometry_region_cursor_advance(geometry, cursor);
        if (err != ESP_OK) {
            burner_nor_region_cursor_clear(cursor);
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t burner_nor_geometry_sector_bounds_in_cursor(
    const burner_nor_region_cursor_t *cursor,
    uint32_t addr,
    uint32_t *sector_begin_out,
    uint32_t *sector_end_out,
    uint32_t *sector_size_out)
{
    uint32_t offset;
    uint32_t sector_begin;
    uint32_t sector_end;

    if (!burner_nor_region_cursor_is_valid(cursor) || addr < cursor->addr_begin || addr >= cursor->addr_end) {
        return ESP_ERR_INVALID_ARG;
    }

    offset = addr - cursor->addr_begin;
    sector_begin = cursor->addr_begin + ((offset / cursor->sector_size) * cursor->sector_size);
    sector_end = sector_begin + cursor->sector_size;
    if (sector_begin_out != NULL) {
        *sector_begin_out = sector_begin;
    }
    if (sector_end_out != NULL) {
        *sector_end_out = sector_end;
    }
    if (sector_size_out != NULL) {
        *sector_size_out = cursor->sector_size;
    }
    return ESP_OK;
}

static esp_err_t burner_nor_geometry_stage_bytes_in_cursor(
    const burner_nor_region_cursor_t *cursor,
    uint32_t addr,
    uint32_t remaining_bytes,
    uint32_t *stage_bytes_out)
{
    uint32_t sector_end = 0u;
    uint32_t stage_bytes;
    esp_err_t err;

    if (stage_bytes_out == NULL || remaining_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_nor_geometry_sector_bounds_in_cursor(cursor, addr, NULL, &sector_end, NULL);
    if (err != ESP_OK || sector_end <= addr) {
        return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
    }

    stage_bytes = sector_end - addr;
    if (stage_bytes > remaining_bytes) {
        stage_bytes = remaining_bytes;
    }
    if (stage_bytes == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    *stage_bytes_out = stage_bytes;
    return ESP_OK;
}

static esp_err_t burner_nor_geometry_limit_prefix(
    burner_nor_geometry_t *geometry,
    uint32_t device_size_limit)
{
    burner_nor_geometry_t src;
    uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    uint32_t remaining_bytes;
    uint32_t region_count = 0u;

    if (geometry == NULL || !burner_nor_geometry_is_valid(geometry) || device_size_limit == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (device_size_limit >= geometry->regions[geometry->region_count - 1u].addr_end) {
        return ESP_OK;
    }

    src = *geometry;
    remaining_bytes = device_size_limit;
    for (uint32_t i = 0u; i < src.region_count && remaining_bytes > 0u; ++i) {
        const burner_nor_region_t *region = &src.regions[i];
        uint32_t region_bytes = region->addr_end - region->addr_begin;
        uint32_t take_bytes = (region_bytes < remaining_bytes) ? region_bytes : remaining_bytes;

        if (take_bytes == 0u || (take_bytes % region->sector_size) != 0u ||
            region_count >= BURNER_NOR_GEOMETRY_REGION_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }

        sector_counts[region_count] = take_bytes / region->sector_size;
        sector_sizes[region_count] = region->sector_size;
        remaining_bytes -= take_bytes;
        ++region_count;
    }

    if (remaining_bytes != 0u || region_count == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    return burner_nor_geometry_build(
        geometry,
        device_size_limit,
        sector_counts,
        sector_sizes,
        region_count,
        false);
}

static uint32_t burner_gb_mapper_device_size_limit(burner_gb_mapper_t mapper)
{
    return (mapper == BURNER_GB_MAPPER_MBC3) ? (2u * 1024u * 1024u) : 0u;
}

static uint8_t burner_gb_mapper_normalize_rom_bank(burner_gb_mapper_t mapper, uint16_t bank)
{
    if (mapper == BURNER_GB_MAPPER_MBC3 && bank == 0u) {
        return 1u;
    }
    return (uint8_t)(bank & 0xFFu);
}

static void burner_mbc5_addr_to_program_window(
    uint32_t flash_addr,
    uint16_t *bank_out,
    uint16_t *cart_addr_out,
    uint32_t *bank_off_out)
{
    uint32_t bank = flash_addr / BURN_MBC5_ROM_BANK_BYTES;
    uint32_t bank_off = flash_addr % BURN_MBC5_ROM_BANK_BYTES;
    uint16_t cart_addr = (uint16_t)(0x4000u + bank_off);

    /*
     * GB bank 0 always lives in the fixed 0x0000-0x3FFF window.
     * The switchable 0x4000-0x7FFF window starts at bank 1.
     */
    if (bank == 0u) {
        cart_addr = (uint16_t)bank_off;
    }

    if (bank_out != NULL) {
        *bank_out = (uint16_t)bank;
    }
    if (cart_addr_out != NULL) {
        *cart_addr_out = cart_addr;
    }
    if (bank_off_out != NULL) {
        *bank_off_out = bank_off;
    }
}

static esp_err_t burner_nor_geometry_largest_sector_size_in_range(
    const burner_nor_geometry_t *geometry,
    uint32_t addr_begin,
    uint32_t total_bytes,
    uint32_t *largest_sector_size_out)
{
    uint32_t addr_end;
    uint32_t largest_sector_size = 0u;
    burner_nor_region_cursor_t cursor = {0};
    esp_err_t err;

    if (largest_sector_size_out == NULL || total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (addr_begin > (UINT32_MAX - (total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    addr_end = addr_begin + total_bytes - 1u;
    err = burner_nor_geometry_region_cursor_begin(geometry, addr_begin, &cursor);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    while (cursor.addr_begin <= addr_end) {
        if (cursor.sector_size > largest_sector_size) {
            largest_sector_size = cursor.sector_size;
        }
        if (cursor.addr_end > addr_end) {
            break;
        }
        if (burner_nor_geometry_region_cursor_advance(geometry, &cursor) != ESP_OK) {
            break;
        }
    }

    if (largest_sector_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    *largest_sector_size_out = largest_sector_size;
    return ESP_OK;
}

static uint32_t burner_nor_geometry_sector_count_from_range(
    const burner_nor_geometry_t *geometry,
    uint32_t addr_begin,
    uint32_t addr_end)
{
    uint64_t total_sectors = 0u;
    burner_nor_region_cursor_t cursor = {0};
    uint32_t sector_addr;
    uint64_t range_end_exclusive;

    if (addr_end < addr_begin ||
        burner_nor_geometry_region_cursor_begin(geometry, addr_begin, &cursor) != ESP_OK) {
        return 0u;
    }

    sector_addr = cursor.addr_begin + (((addr_begin - cursor.addr_begin) / cursor.sector_size) * cursor.sector_size);
    range_end_exclusive = (uint64_t)addr_end + 1u;
    while ((uint64_t)sector_addr < range_end_exclusive) {
        uint64_t region_end_exclusive =
            ((uint64_t)cursor.addr_end < range_end_exclusive) ? (uint64_t)cursor.addr_end : range_end_exclusive;
        uint64_t region_sectors;

        if (region_end_exclusive <= (uint64_t)sector_addr) {
            break;
        }
        region_sectors = ((region_end_exclusive - (uint64_t)sector_addr) + (uint64_t)cursor.sector_size - 1u) /
                         (uint64_t)cursor.sector_size;
        total_sectors += region_sectors;
        if (total_sectors > UINT32_MAX) {
            return UINT32_MAX;
        }
        if (region_end_exclusive >= range_end_exclusive) {
            break;
        }
        if (burner_nor_geometry_region_cursor_advance(geometry, &cursor) != ESP_OK) {
            break;
        }
        sector_addr = cursor.addr_begin;
    }

    return (uint32_t)total_sectors;
}

static uint32_t burner_nor_geometry_erase_bytes_from_range(
    const burner_nor_geometry_t *geometry,
    uint32_t addr_begin,
    uint32_t addr_end)
{
    uint64_t total_bytes = 0u;
    burner_nor_region_cursor_t cursor = {0};
    uint32_t sector_addr;
    uint64_t range_end_exclusive;

    if (addr_end < addr_begin ||
        burner_nor_geometry_region_cursor_begin(geometry, addr_begin, &cursor) != ESP_OK) {
        return 0u;
    }

    sector_addr = cursor.addr_begin + (((addr_begin - cursor.addr_begin) / cursor.sector_size) * cursor.sector_size);
    range_end_exclusive = (uint64_t)addr_end + 1u;
    while ((uint64_t)sector_addr < range_end_exclusive) {
        uint64_t region_end_exclusive =
            ((uint64_t)cursor.addr_end < range_end_exclusive) ? (uint64_t)cursor.addr_end : range_end_exclusive;
        uint64_t region_sectors;

        if (region_end_exclusive <= (uint64_t)sector_addr) {
            break;
        }
        region_sectors = ((region_end_exclusive - (uint64_t)sector_addr) + (uint64_t)cursor.sector_size - 1u) /
                         (uint64_t)cursor.sector_size;
        total_bytes += region_sectors * (uint64_t)cursor.sector_size;
        if (total_bytes > UINT32_MAX) {
            return UINT32_MAX;
        }
        if (region_end_exclusive >= range_end_exclusive) {
            break;
        }
        if (burner_nor_geometry_region_cursor_advance(geometry, &cursor) != ESP_OK) {
            break;
        }
        sector_addr = cursor.addr_begin;
    }

    return (uint32_t)total_bytes;
}

static uint32_t burner_nor_geometry_planned_stage_erase_sector_count(
    const burner_nor_geometry_t *geometry,
    uint32_t addr_begin,
    uint32_t total_bytes,
    uint32_t stage_capacity)
{
    uint32_t processed = 0u;
    uint64_t total_sectors = 0u;

    if (!burner_nor_geometry_is_valid(geometry) || total_bytes == 0u || stage_capacity == 0u) {
        return 0u;
    }

    while (processed < total_bytes) {
        uint32_t stage_addr = addr_begin + processed;
        uint32_t stage_bytes = total_bytes - processed;
        uint32_t stage_erase_begin = stage_addr;
        uint32_t stage_erase_end;

        if (stage_bytes > stage_capacity) {
            stage_bytes = stage_capacity;
        }
        stage_erase_end = stage_addr + stage_bytes - 1u;
        if (processed > 0u) {
            if (burner_nor_geometry_sector_begin_ceil(geometry, stage_addr, &stage_erase_begin) != ESP_OK ||
                stage_erase_begin > stage_erase_end) {
                processed += stage_bytes;
                continue;
            }
        }

        total_sectors += burner_nor_geometry_sector_count_from_range(
            geometry,
            stage_erase_begin,
            stage_erase_end);
        if (total_sectors > UINT32_MAX) {
            return UINT32_MAX;
        }
        processed += stage_bytes;
    }

    return (uint32_t)total_sectors;
}
