#ifndef BURNER_NOR_DB_H
#define BURNER_NOR_DB_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BURNER_NOR_CMDSET_UNKNOWN = 0,
    BURNER_NOR_CMDSET_AMD,
    BURNER_NOR_CMDSET_INTEL,
} burner_nor_cmdset_t;

typedef enum {
    BURNER_NOR_IDBUS_GBA = 0,
    BURNER_NOR_IDBUS_MBC5,
} burner_nor_idbus_t;

typedef enum {
    BURNER_NOR_FLAG_NONE = 0u,
    BURNER_NOR_FLAG_DUAL_DIE = 1u << 0,
    BURNER_NOR_FLAG_LIMIT_BUFFER_TO_ID = 1u << 1,
    BURNER_NOR_FLAG_INTEL_88B0 = 1u << 2,
} burner_nor_flags_t;

typedef struct {
    const char *name;
    const char *gba_profile;
    burner_nor_cmdset_t cmdset;
    uint32_t flags;
    uint32_t device_size;
    uint32_t sector_size;
    uint16_t buffer_write_bytes_gba;
    uint16_t buffer_write_bytes_mbc5;
} burner_nor_family_t;

typedef struct {
    burner_nor_idbus_t bus;
    const burner_nor_family_t *family;
    const char *name;
    const char *profile;
    burner_nor_cmdset_t cmdset;
    uint32_t device_size;
    uint32_t sector_size;
    uint16_t buffer_write_bytes;
    uint32_t flags;
    uint8_t id_len;
    uint8_t id[8];
    uint8_t id_mask[8];
} burner_nor_entry_t;

const burner_nor_entry_t *burner_nor_db_lookup_gba(const uint8_t id[8]);
const burner_nor_entry_t *burner_nor_db_lookup_mbc5(const uint8_t id[4]);
const char *burner_nor_cmdset_name(burner_nor_cmdset_t cmdset);
const burner_nor_family_t *burner_gba_family_from_id(const uint8_t id[8]);
const burner_nor_family_t *burner_mbc5_family_from_id(const uint8_t id[4]);
const char *burner_nor_entry_name(const burner_nor_entry_t *entry);
burner_nor_cmdset_t burner_nor_entry_cmdset(const burner_nor_entry_t *entry);
uint32_t burner_nor_entry_flags(const burner_nor_entry_t *entry);
uint32_t burner_nor_entry_device_size(const burner_nor_entry_t *entry);
uint32_t burner_nor_entry_sector_size(const burner_nor_entry_t *entry);
uint16_t burner_nor_entry_buffer_write_bytes(const burner_nor_entry_t *entry);

const char *burner_gba_chip_name(const uint8_t id[8]);
const char *burner_gba_profile_name(const uint8_t id[8]);
bool burner_gba_geometry_from_id(
    const uint8_t id[8],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes);
burner_nor_cmdset_t burner_gba_cmdset_from_id(const uint8_t id[8]);
uint32_t burner_gba_nor_flags_from_id(const uint8_t id[8]);
bool burner_gba_nor_has_flag(const uint8_t id[8], uint32_t flag);

const char *burner_mbc5_chip_name(const uint8_t id[4]);
bool burner_mbc5_geometry_from_id(
    const uint8_t id[4],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes);
burner_nor_cmdset_t burner_mbc5_cmdset_from_id(const uint8_t id[4]);
uint32_t burner_mbc5_nor_flags_from_id(const uint8_t id[4]);
bool burner_mbc5_nor_has_flag(const uint8_t id[4], uint32_t flag);

#endif
