#include "burner_nor_db_data.h"

#include <stddef.h>

/* Data-only NOR database shared by GBA (16-bit) and MBC5/GB (8-bit) probes. */

#define BURNER_NOR_ID_MASK8_EXACT {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu}
#define BURNER_NOR_ID_MASK4_EXACT {0xFFu, 0xFFu, 0xFFu, 0xFFu}
#define BURNER_NOR_ID_MASK8_PREFIX4 {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x00u, 0x00u, 0x00u, 0x00u}
#define BURNER_NOR_ID4_EXACT(a, b, c, d) {a, b, c, d}
#define BURNER_NOR_ID8_EXACT(a, b, c, d, e, f, g, h) {a, b, c, d, e, f, g, h}
#define BURNER_NOR_ID8_PREFIX4(a, b, c, d) {a, b, c, d, 0x00u, 0x00u, 0x00u, 0x00u}

#define BURNER_GBA_PROFILE_AMD_AAA_AA "AGB:AMD:AAA/555:AA/55"
#define BURNER_GBA_PROFILE_AMD_AAA_A9 "AGB:AMD:AAA/555:A9/56"
#define BURNER_GBA_PROFILE_AMD_555_AA "AGB:AMD:555/2AA:AA/55"
#define BURNER_GBA_PROFILE_INTEL_DIRECT "AGB:INTEL:direct-id"
#define BURNER_GBA_PROFILE_INTEL_DUAL_DIRECT "AGB:INTEL:dual-direct-id"
#define BURNER_GBA_PROFILE_SHARP_DIRECT "AGB:SHARP:direct-id"

#define BURNER_NOR_FAMILY_DEFINE( \
    tag, \
    family_name, \
    family_gba_profile, \
    family_cmdset, \
    family_flags, \
    family_device_size, \
    family_sector_size, \
    family_buffer_gba, \
    family_buffer_mbc5) \
    static const burner_nor_family_t tag = { \
        .name = family_name, \
        .gba_profile = family_gba_profile, \
        .cmdset = family_cmdset, \
        .flags = family_flags, \
        .device_size = family_device_size, \
        .sector_size = family_sector_size, \
        .buffer_write_bytes_gba = family_buffer_gba, \
        .buffer_write_bytes_mbc5 = family_buffer_mbc5, \
    }

#define BURNER_NOR_GBA_FAMILY_ENTRY(family_tag, id_init) \
    { \
        .bus = BURNER_NOR_IDBUS_GBA, \
        .family = &family_tag, \
        .id_len = 8u, \
        .id = id_init, \
        .id_mask = BURNER_NOR_ID_MASK8_EXACT, \
    }

#define BURNER_NOR_MBC5_FAMILY_ENTRY(family_tag, extra_flags, id_init) \
    { \
        .bus = BURNER_NOR_IDBUS_MBC5, \
        .family = &family_tag, \
        .flags = extra_flags, \
        .id_len = 4u, \
        .id = id_init, \
        .id_mask = BURNER_NOR_ID_MASK4_EXACT, \
    }

/* Shared family metadata reused by both 16-bit GBA and 8-bit MBC5 modes. */

BURNER_NOR_FAMILY_DEFINE(
    s_family_s29gl128,
    "S29GL128N/P/S",
    BURNER_GBA_PROFILE_AMD_AAA_AA,
    BURNER_NOR_CMDSET_AMD,
    BURNER_NOR_FLAG_NONE,
    16u * 1024u * 1024u,
    128u * 1024u,
    512u,
    32u);
BURNER_NOR_FAMILY_DEFINE(
    s_family_s29gl256,
    "S29GL256N/P/S",
    BURNER_GBA_PROFILE_AMD_AAA_AA,
    BURNER_NOR_CMDSET_AMD,
    BURNER_NOR_FLAG_NONE,
    32u * 1024u * 1024u,
    128u * 1024u,
    512u,
    32u);
BURNER_NOR_FAMILY_DEFINE(
    s_family_s29gl512,
    "S29GL512N/P/S",
    BURNER_GBA_PROFILE_AMD_AAA_AA,
    BURNER_NOR_CMDSET_AMD,
    BURNER_NOR_FLAG_NONE,
    64u * 1024u * 1024u,
    128u * 1024u,
    512u,
    32u);
BURNER_NOR_FAMILY_DEFINE(
    s_family_js28f256,
    "JS28F256 / MT28EW256ABA",
    BURNER_GBA_PROFILE_AMD_AAA_AA,
    BURNER_NOR_CMDSET_AMD,
    BURNER_NOR_FLAG_NONE,
    32u * 1024u * 1024u,
    128u * 1024u,
    1024u,
    256u);
BURNER_NOR_FAMILY_DEFINE(
    s_family_s29gl01,
    "S29GL01GP/GS",
    BURNER_GBA_PROFILE_AMD_555_AA,
    BURNER_NOR_CMDSET_AMD,
    BURNER_NOR_FLAG_NONE,
    128u * 1024u * 1024u,
    128u * 1024u,
    512u,
    64u);
BURNER_NOR_FAMILY_DEFINE(
    s_family_s70gl02,
    "S70GL02GS",
    BURNER_GBA_PROFILE_AMD_555_AA,
    BURNER_NOR_CMDSET_AMD,
    BURNER_NOR_FLAG_DUAL_DIE,
    256u * 1024u * 1024u,
    128u * 1024u,
    512u,
    512u);
BURNER_NOR_FAMILY_DEFINE(
    s_family_mx29gl256,
    "MX29GL256E/F",
    BURNER_GBA_PROFILE_AMD_AAA_AA,
    BURNER_NOR_CMDSET_AMD,
    BURNER_NOR_FLAG_NONE,
    32u * 1024u * 1024u,
    128u * 1024u,
    64u,
    64u);

/* GBA/AGB 16-bit identification entries. */

const burner_nor_entry_t g_burner_nor_db_gba[] = {
        BURNER_NOR_GBA_FAMILY_ENTRY(
            s_family_s29gl128,
            BURNER_NOR_ID8_EXACT(0x01u, 0x00u, 0x7Eu, 0x22u, 0x21u, 0x22u, 0x01u, 0x22u)),
        BURNER_NOR_GBA_FAMILY_ENTRY(
            s_family_s29gl256,
            BURNER_NOR_ID8_EXACT(0x01u, 0x00u, 0x7Eu, 0x22u, 0x22u, 0x22u, 0x01u, 0x22u)),
        BURNER_NOR_GBA_FAMILY_ENTRY(
            s_family_s29gl512,
            BURNER_NOR_ID8_EXACT(0x01u, 0x00u, 0x7Eu, 0x22u, 0x23u, 0x22u, 0x01u, 0x22u)),
        BURNER_NOR_GBA_FAMILY_ENTRY(
            s_family_js28f256,
            BURNER_NOR_ID8_EXACT(0x89u, 0x00u, 0x7Eu, 0x22u, 0x22u, 0x22u, 0x01u, 0x22u)),
        BURNER_NOR_GBA_FAMILY_ENTRY(
            s_family_s29gl01,
            BURNER_NOR_ID8_EXACT(0x01u, 0x00u, 0x7Eu, 0x22u, 0x28u, 0x22u, 0x01u, 0x22u)),
        BURNER_NOR_GBA_FAMILY_ENTRY(
            s_family_s70gl02,
            BURNER_NOR_ID8_EXACT(0x01u, 0x00u, 0x7Eu, 0x22u, 0x48u, 0x22u, 0x01u, 0x22u)),
        BURNER_NOR_GBA_FAMILY_ENTRY(
            s_family_mx29gl256,
            BURNER_NOR_ID8_EXACT(0xC2u, 0x00u, 0x7Eu, 0x22u, 0x22u, 0x22u, 0x01u, 0x22u)),
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "26L6420MC-90 family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_AA,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 8u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0xC2u, 0x00u, 0xFCu, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "29LV128DB family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_AA,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0xC2u, 0x00u, 0x7Au, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "29LV128DT family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_A9,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0xC1u, 0x00u, 0x7Du, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "29LV128DT family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_A9,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0xC1u, 0x00u, 0x79u, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "28EW256A alt-vendor family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_AA,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x9Du, 0x00u, 0x7Eu, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "28EW256A alt-vendor family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_AA,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0xEFu, 0x00u, 0x7Eu, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "MSP55LV128 family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_A9,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 64u * 1024u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x04u, 0x00u, 0x7Du, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "MSP55LV128 family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_AA,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 64u * 1024u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x04u, 0x00u, 0x7Eu, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "MX29LV320ET family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_AA,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 4u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0xC2u, 0x00u, 0xA7u, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "MX29LV320ET family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_AA,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 4u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0xC2u, 0x00u, 0xC9u, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "MX29LV320ET family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_AA,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 4u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x04u, 0x00u, 0xF6u, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "S29GL032 family",
            .profile = BURNER_GBA_PROFILE_AMD_AAA_A9,
            .cmdset = BURNER_NOR_CMDSET_AMD,
            .device_size = 4u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x02u, 0x00u, 0xFAu, 0x22u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
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
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 128u * 1024u * 1024u,
            .sector_size = 256u * 1024u,
            .buffer_write_bytes = 1024u,
            .flags = BURNER_NOR_FLAG_INTEL_88B0,
            .id_len = 8u,
            .id = {0x89u, 0x00u, 0xB0u, 0x88u, 0x04u, 0x00u, 0x89u, 0x00u},
            .id_mask = BURNER_NOR_ID_MASK8_EXACT,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "GE28F128W30 / 128W30B",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x8Au, 0x00u, 0x57u, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "28F256L30B family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x8Au, 0x00u, 0x15u, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "4050M0Y0Q0 / 0121M0Y0BE family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x8Au, 0x00u, 0x02u, 0x89u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "4050M0Y0Q0 / 0121M0Y0BE family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x8Au, 0x00u, 0x7Du, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M36L0R705 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_RESET_EVERY_1MB,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x20u, 0x00u, 0xC4u, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M36L0R705 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_RESET_EVERY_1MB,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x20u, 0x00u, 0xC5u, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M36L0R705 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_RESET_EVERY_1MB,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x20u, 0x00u, 0xC6u, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M36L0R705 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_RESET_EVERY_1MB,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x8Au, 0x00u, 0x0Fu, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M36L0R806 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_RESET_EVERY_1MB,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x20u, 0x00u, 0x0Du, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M36L0R806 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_RESET_EVERY_1MB,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x20u, 0x00u, 0x0Eu, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M36L0R806 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_RESET_EVERY_1MB,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x8Au, 0x00u, 0x0Eu, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M36L0R806 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_RESET_EVERY_1MB,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x8Au, 0x00u, 0x10u, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M36L0R806 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_RESET_EVERY_1MB,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x8Au, 0x00u, 0x1Cu, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M36W0R603 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 8u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x20u, 0x00u, 0x10u, 0x88u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "Flash Advance 64M / 28F640J3A",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 8u * 1024u * 1024u,
            .sector_size = 128u * 1024u,
            .buffer_write_bytes = 32u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x89u, 0x00u, 0x17u, 0x00u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "Flash2Advance 128M / dual 28F640J3A",
            .profile = BURNER_GBA_PROFILE_INTEL_DUAL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 256u * 1024u,
            .buffer_write_bytes = 64u,
            .flags = BURNER_NOR_FLAG_DUAL_DIE,
            .id_len = 8u,
            .id = {0x89u, 0x00u, 0x89u, 0x00u, 0x17u, 0x00u, 0x17u, 0x00u},
            .id_mask = BURNER_NOR_ID_MASK8_EXACT,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "Flash2Advance 256M / dual 28F128J3A",
            .profile = BURNER_GBA_PROFILE_INTEL_DUAL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 256u * 1024u,
            .buffer_write_bytes = 64u,
            .flags = BURNER_NOR_FLAG_DUAL_DIE,
            .id_len = 8u,
            .id = {0x89u, 0x00u, 0x89u, 0x00u, 0x18u, 0x00u, 0x18u, 0x00u},
            .id_mask = BURNER_NOR_ID_MASK8_EXACT,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "Flash2Advance Ultra 2G / 4400L0YDQ0 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DUAL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 256u * 1024u * 1024u,
            .sector_size = 256u * 1024u,
            .buffer_write_bytes = 64u,
            .flags = BURNER_NOR_FLAG_DUAL_DIE,
            .id_len = 8u,
            .id = {0x89u, 0x00u, 0x89u, 0x00u, 0x10u, 0x88u, 0x10u, 0x88u},
            .id_mask = BURNER_NOR_ID_MASK8_EXACT,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "TE28F128",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x89u, 0x00u, 0x18u, 0x00u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "TE28F256",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x89u, 0x00u, 0x1Du, 0x00u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "GA-07 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 8u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x1Cu, 0x00u, 0x2Bu, 0x00u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M5M29G130AN family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 16u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x1Cu, 0x00u, 0x3Du, 0x00u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M5M29HD528 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 256u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x1Cu, 0x00u, 0xFAu, 0x00u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "M6MGJ927 family",
            .profile = BURNER_GBA_PROFILE_INTEL_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 8u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0x1Cu, 0x00u, 0xB9u, 0x00u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "Sharp Dev E201629/E201843/E201850 family",
            .profile = BURNER_GBA_PROFILE_SHARP_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 0u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0xB0u, 0x00u, 0xE2u, 0x00u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
        {
            .bus = BURNER_NOR_IDBUS_GBA,
            .name = "Sharp Dev E201868 family",
            .profile = BURNER_GBA_PROFILE_SHARP_DIRECT,
            .cmdset = BURNER_NOR_CMDSET_INTEL,
            .device_size = 32u * 1024u * 1024u,
            .sector_size = 0u,
            .buffer_write_bytes = 0u,
            .flags = BURNER_NOR_FLAG_NONE,
            .id_len = 8u,
            .id = BURNER_NOR_ID8_PREFIX4(0xB0u, 0x00u, 0xB0u, 0x00u),
            .id_mask = BURNER_NOR_ID_MASK8_PREFIX4,
        },
};

const size_t g_burner_nor_db_gba_count =
    sizeof(g_burner_nor_db_gba) / sizeof(g_burner_nor_db_gba[0]);

/* GB/MBC5 8-bit identification entries. */

const burner_nor_entry_t g_burner_nor_db_mbc5[] = {
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
        BURNER_NOR_MBC5_FAMILY_ENTRY(
            s_family_s29gl128,
            BURNER_NOR_FLAG_NONE,
            BURNER_NOR_ID4_EXACT(0x01u, 0x7Eu, 0x21u, 0x01u)),
        BURNER_NOR_MBC5_FAMILY_ENTRY(
            s_family_s29gl256,
            BURNER_NOR_FLAG_NONE,
            BURNER_NOR_ID4_EXACT(0x01u, 0x7Eu, 0x22u, 0x01u)),
        BURNER_NOR_MBC5_FAMILY_ENTRY(
            s_family_s29gl512,
            BURNER_NOR_FLAG_NONE,
            BURNER_NOR_ID4_EXACT(0x01u, 0x7Eu, 0x23u, 0x01u)),
        BURNER_NOR_MBC5_FAMILY_ENTRY(
            s_family_s29gl01,
            BURNER_NOR_FLAG_NONE,
            BURNER_NOR_ID4_EXACT(0x01u, 0x7Eu, 0x28u, 0x01u)),
        BURNER_NOR_MBC5_FAMILY_ENTRY(
            s_family_s70gl02,
            BURNER_NOR_FLAG_NONE,
            BURNER_NOR_ID4_EXACT(0x01u, 0x7Eu, 0x48u, 0x01u)),
        BURNER_NOR_MBC5_FAMILY_ENTRY(
            s_family_mx29gl256,
            BURNER_NOR_FLAG_NONE,
            BURNER_NOR_ID4_EXACT(0xC2u, 0x7Eu, 0x22u, 0x01u)),
        BURNER_NOR_MBC5_FAMILY_ENTRY(
            s_family_js28f256,
            BURNER_NOR_FLAG_LIMIT_BUFFER_TO_ID,
            BURNER_NOR_ID4_EXACT(0x89u, 0x7Eu, 0x22u, 0x01u)),
};

const size_t g_burner_nor_db_mbc5_count =
    sizeof(g_burner_nor_db_mbc5) / sizeof(g_burner_nor_db_mbc5[0]);
