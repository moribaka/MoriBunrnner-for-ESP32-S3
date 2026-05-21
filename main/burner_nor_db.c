#include "burner_nor_db.h"

#include <stddef.h>

static bool burner_nor_id_match(const uint8_t *id, const burner_nor_entry_t *entry)
{
    size_t i;

    if (id == NULL || entry == NULL) {
        return false;
    }
    for (i = 0u; i < entry->id_len; ++i) {
        if ((id[i] & entry->id_mask[i]) != (entry->id[i] & entry->id_mask[i])) {
            return false;
        }
    }
    return true;
}

static const burner_nor_entry_t *burner_nor_db_lookup(
    burner_nor_idbus_t bus,
    const uint8_t *id,
    size_t id_len)
{
    static const burner_nor_entry_t s_nor_db[] = {
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "S29GL128N/P/S",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 512u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = {0x01u, 0x00u, 0x7Eu, 0x22u, 0x21u, 0x22u, 0x01u, 0x22u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "S29GL256N/P/S",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 512u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = {0x01u, 0x00u, 0x7Eu, 0x22u, 0x22u, 0x22u, 0x01u, 0x22u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "S29GL512N/P/S",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 64u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 512u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = {0x01u, 0x00u, 0x7Eu, 0x22u, 0x23u, 0x22u, 0x01u, 0x22u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "JS28F256 / MT28EW256ABA",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 1024u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = {0x89u, 0x00u, 0x7Eu, 0x22u, 0x22u, 0x22u, 0x01u, 0x22u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "S29GL01GP/GS",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 128u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 512u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = {0x01u, 0x00u, 0x7Eu, 0x22u, 0x28u, 0x22u, 0x01u, 0x22u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "S70GL02GS",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 256u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 512u,
            .flags = BURNER_NOR_FLAG_DUAL_DIE,
            .id_len = 8u,
            .id = {0x01u, 0x00u, 0x7Eu, 0x22u, 0x48u, 0x22u, 0x01u, 0x22u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "MX29GL256E/F",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 64u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = {0xC2u, 0x00u, 0x7Eu, 0x22u, 0x22u, 0x22u, 0x01u, 0x22u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M29W256G",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 64u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = {0x20u, 0x00u, 0x7Eu, 0x22u, 0x22u, 0x22u, 0x01u, 0x22u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "Intel/Micron 0x88B0 family",
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 128u * 1024u * 1024u,
            .sector_size = 256u * 1024u,
            .buffer_write_bytes = 1024u,
            .flags = BURNER_NOR_FLAG_INTEL_88B0,
            .id_len = 8u,
            .id = {0x89u, 0x00u, 0xB0u, 0x88u, 0x04u, 0x00u, 0x89u, 0x00u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_MBC5,
            .name = "MX29LV640EB",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 8u * 1024u * 1024u,
            .sector_size = 64u * 1024u,
            .buffer_write_bytes = 32u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 4u,
            .id = {0xC2u, 0xCBu, 0x00u, 0x00u},
            .id_mask = {0xFFu, 0xFFu, 0x00u, 0x00u},
        },
        {
            .bus = BURNER_NOR_IDBUS_MBC5,
            .name = "MX29LV640ET",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 8u * 1024u * 1024u,
            .sector_size = 64u * 1024u,
            .buffer_write_bytes = 32u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 4u,
            .id = {0xC2u, 0xC9u, 0x00u, 0x00u},
            .id_mask = {0xFFu, 0xFFu, 0x00u, 0x00u},
        },
        {
            .bus = BURNER_NOR_IDBUS_MBC5,
            .name = "S29GL128N/P/S",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 32u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 4u,
            .id = {0x01u, 0x7Eu, 0x21u, 0x01u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_MBC5,
            .name = "S29GL256N/P/S",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 32u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 4u,
            .id = {0x01u, 0x7Eu, 0x22u, 0x01u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_MBC5,
            .name = "S29GL512N/P/S",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 64u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 32u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 4u,
            .id = {0x01u, 0x7Eu, 0x23u, 0x01u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_MBC5,
            .name = "S29GL01GP/GS",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 128u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 64u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 4u,
            .id = {0x01u, 0x7Eu, 0x28u, 0x01u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_MBC5,
            .name = "S70GL02GS",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 256u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 512u,
            .flags = BURNER_NOR_FLAG_DUAL_DIE,
            .id_len = 4u,
            .id = {0x01u, 0x7Eu, 0x48u, 0x01u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_MBC5,
            .name = "MX29GL256E/F",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 64u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 4u,
            .id = {0xC2u, 0x7Eu, 0x22u, 0x01u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
        {
            .bus = BURNER_NOR_IDBUS_MBC5,
            .name = "JS28F256 / MT28EW256ABA",
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 256u,
            .flags = BURNER_NOR_FLAG_LIMIT_BUFFER_TO_ID,
            .id_len = 4u,
            .id = {0x89u, 0x7Eu, 0x22u, 0x01u},
            .id_mask = {0xFFu, 0xFFu, 0xFFu, 0xFFu},
        },
    };
    size_t i;

    if (id == NULL) {
        return NULL;
    }

    for (i = 0u; i < sizeof(s_nor_db) / sizeof(s_nor_db[0]); ++i) {
        if (s_nor_db[i].bus != bus || s_nor_db[i].id_len != id_len) {
            continue;
        }
        if (burner_nor_id_match(id, &s_nor_db[i])) {
            return &s_nor_db[i];
        }
    }
    return NULL;
}

const burner_nor_entry_t *burner_nor_db_lookup_gba(const uint8_t id[8])
{
    return burner_nor_db_lookup(BURNER_NOR_IDBUS_GBA, id, 8u);
}

const burner_nor_entry_t *burner_nor_db_lookup_mbc5(const uint8_t id[4])
{
    return burner_nor_db_lookup(BURNER_NOR_IDBUS_MBC5, id, 4u);
}

const char *burner_nor_cmdset_name(burner_nor_cmdset_t cmdset)
{
    switch (cmdset) {
        case BURNER_NOR_CMDSET_AMD:
            return "amd";
        case BURNER_NOR_CMDSET_INTEL:
            return "intel";
        case BURNER_NOR_CMDSET_UNKNOWN:
        default:
            return "unknown";
    }
}

const char *burner_gba_chip_name(const uint8_t id[8])
{
    const burner_nor_entry_t *entry = burner_nor_db_lookup_gba(id);
    return (entry != NULL && entry->name != NULL) ? entry->name : "unknown";
}

bool burner_gba_geometry_from_id(
    const uint8_t id[8],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes)
{
    const burner_nor_entry_t *entry = burner_nor_db_lookup_gba(id);

    if (entry == NULL || device_size == NULL || sector_size == NULL || buffer_write_bytes == NULL) {
        return false;
    }
    *device_size = entry->device_size;
    *sector_size = entry->sector_size;
    *buffer_write_bytes = entry->buffer_write_bytes;
    return true;
}

burner_nor_cmdset_t burner_gba_cmdset_from_id(const uint8_t id[8])
{
    const burner_nor_entry_t *entry = burner_nor_db_lookup_gba(id);
    return (entry != NULL) ? entry->cmdset : BURNER_NOR_CMDSET_UNKNOWN;
}

uint32_t burner_gba_nor_flags_from_id(const uint8_t id[8])
{
    const burner_nor_entry_t *entry = burner_nor_db_lookup_gba(id);
    return (entry != NULL) ? entry->flags : BURNER_NOR_FLAG_NONE;
}

bool burner_gba_nor_has_flag(const uint8_t id[8], uint32_t flag)
{
    return (burner_gba_nor_flags_from_id(id) & flag) != 0u;
}

const char *burner_mbc5_chip_name(const uint8_t id[4])
{
    const burner_nor_entry_t *entry = burner_nor_db_lookup_mbc5(id);
    return (entry != NULL && entry->name != NULL) ? entry->name : "unknown";
}

bool burner_mbc5_geometry_from_id(
    const uint8_t id[4],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes)
{
    const burner_nor_entry_t *entry = burner_nor_db_lookup_mbc5(id);

    if (entry == NULL || device_size == NULL || sector_size == NULL || buffer_write_bytes == NULL) {
        return false;
    }
    *device_size = entry->device_size;
    *sector_size = entry->sector_size;
    *buffer_write_bytes = entry->buffer_write_bytes;
    return true;
}

burner_nor_cmdset_t burner_mbc5_cmdset_from_id(const uint8_t id[4])
{
    const burner_nor_entry_t *entry = burner_nor_db_lookup_mbc5(id);
    return (entry != NULL) ? entry->cmdset : BURNER_NOR_CMDSET_UNKNOWN;
}

uint32_t burner_mbc5_nor_flags_from_id(const uint8_t id[4])
{
    const burner_nor_entry_t *entry = burner_nor_db_lookup_mbc5(id);
    return (entry != NULL) ? entry->flags : BURNER_NOR_FLAG_NONE;
}

bool burner_mbc5_nor_has_flag(const uint8_t id[4], uint32_t flag)
{
    return (burner_mbc5_nor_flags_from_id(id) & flag) != 0u;
}
