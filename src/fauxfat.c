#include "fauxfat.h"
#include "fauxbytes.h"

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

#include <limits.h>
#include <string.h>

#define FF_FAT_OFFSET_BLOCKS  128u
#define FF_UPCASE_BYTES       128u
#define FF_ROOT_ENTRIES       2048u
#define FF_FILE_SLOT_FIRST    4u
#define FF_PUBLIC_ENTRY_COUNT 5u
#define FF_OPAQUE_ENTRY_COUNT 5u
#define FF_FAT_EOC            0xffffffffu
#define FF_MAX_CLUSTER_COUNT  0xfffffff5u

#define FF_OEM_FLAG_BENIGN_RESERVE 0x0001u
#define FF_OEM_FLAG_MUTABLE_LENGTH 0x0002u
#define FF_OEM_KNOWN_FLAGS         (FF_OEM_FLAG_BENIGN_RESERVE | FF_OEM_FLAG_MUTABLE_LENGTH)

#define FF_XXH32_MAP_SEED    0x00000000u
#define FF_XXH32_FAT_SEED    0x46415431u /* "FAT1" */
#define FF_XXH32_BITMAP_SEED 0x42495431u /* "BIT1" */
#define FF_XXH32_ROOT_SEED   0x524f4f54u /* "ROOT" */
#define FF_XXH32_UPCASE_SEED 0x55504331u /* "UPC1" */

#define FF_ENTRY_BITMAP          0x81u
#define FF_ENTRY_UPCASE          0x82u
#define FF_ENTRY_LABEL           0x83u
#define FF_ENTRY_FILE            0x85u
#define FF_ENTRY_GUID            0xa0u
#define FF_ENTRY_PAD             0xa1u
#define FF_ENTRY_PRIVATE_RESERVE 0xbfu
#define FF_ENTRY_STREAM          0xc0u
#define FF_ENTRY_NAME            0xc1u
#define FF_ENTRY_VENDOR_EXT      0xe0u
#define FF_ENTRY_VENDOR_ALLOC    0xe1u

#define FF_ENTRY_IN_USE    0x80u
#define FF_ENTRY_SECONDARY 0x40u
#define FF_ENTRY_BENIGN    0x20u

#define FF_ATTR_READONLY  0x0001u
#define FF_ATTR_HIDDEN    0x0002u
#define FF_ATTR_SYSTEM    0x0004u
#define FF_ATTR_DIRECTORY 0x0010u
#define FF_ATTR_ARCHIVE   0x0020u

#define FF_UNIX_1980 315532800LL
#define FF_UNIX_2108 4354819200LL

static int ff_public_file_entry_valid(const uint8_t e[32], time_t *mtime);
static int ff_dev_read_block(const fauxfat_device *device,
                             uint64_t block,
                             uint8_t out[FAUXFAT_BLOCK_SIZE]);

static const uint8_t ff_oem_map_guid[16] = {
    0xe7, 0xe6, 0x5b, 0x99, 0x45, 0x34, 0xdc, 0x46,
    0xa2, 0x13, 0x74, 0xd9, 0x85, 0xb3, 0x01, 0x34
};

static const uint8_t ff_oem_epoch_guid[16] = {
    0xdb, 0x83, 0x72, 0x8b, 0xea, 0xb9, 0xdb, 0x4a,
    0xa3, 0x27, 0x9b, 0x59, 0x73, 0x63, 0x47, 0x9b
};

static const uint8_t ff_private_reserve_magic[8] = {
    'F', 'A', 'U', 'X', 'R', 'S', 'V', '1'
};

static const uint8_t ff_opaque_meta_guid[16] = {
    0x21, 0x72, 0xa6, 0xe2, 0x24, 0x0b, 0xfe, 0x41,
    0xa8, 0xad, 0xa8, 0x30, 0x3d, 0xfa, 0xa8, 0x43
};

static const uint8_t ff_opaque_alloc_guid[16] = {
    0xea, 0xbd, 0x0d, 0x94, 0xf9, 0xce, 0xaa, 0x4c,
    0x85, 0x55, 0x0f, 0x60, 0xe0, 0x5b, 0xa9, 0x3c
};

/*
 * Public files keep their manufactured logical name in two benign Vendor
 * Extension records.  This is deliberately redundant with the namespace
 * name: the namespace may be renamed by a host, while recovery still exposes
 * the stable logical name to firmware.
 */
static const uint8_t ff_public_name_guid0[16] = {
    0x65, 0xc7, 0x13, 0xae, 0x8b, 0x27, 0x2f, 0x4b,
    0x9d, 0x4e, 0x74, 0x26, 0xb0, 0xc6, 0x18, 0x91
};

static const uint8_t ff_public_name_guid1[16] = {
    0x0e, 0x4a, 0xb1, 0x58, 0x9c, 0x89, 0x34, 0x47,
    0xb3, 0xe1, 0x5a, 0x71, 0x7d, 0x0a, 0x2f, 0xcc
};

static uint16_t ff_ror16(uint16_t v)
{
    return (uint16_t)((v >> 1) | (v << 15));
}

static uint32_t ff_ror32(uint32_t v)
{
    return (v >> 1) | (v << 31);
}

static uint32_t ff_align_up_u32(uint32_t v, uint32_t align)
{
    return (v + align - 1u) & ~(align - 1u);
}

/*
 * Convert UTC Unix seconds to exFAT's packed local date/time representation.
 * We deliberately do not call gmtime()/gmtime_r(): the on-disk view is always
 * UTC (UTC offset byte 0x80), so libc timezone/locale machinery buys us
 * nothing except code and state.
 *
 * exFAT stores seconds in two-second units.  The separate 10 ms increment is
 * therefore 100 for an odd Unix second and 0 for an even one.
 */
static int ff_exfat_timestamp(time_t value,
                              uint32_t *timestamp,
                              uint8_t *ten_ms)
{
    int64_t seconds                     = (int64_t)value;
    static const uint8_t month_days[12] = {
        31u, 28u, 31u, 30u, 31u, 30u,
        31u, 31u, 30u, 31u, 30u, 31u
    };

    uint32_t elapsed;
    uint32_t days;
    uint32_t sod;
    uint32_t year;
    uint32_t doy;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
    uint32_t i;

    if (seconds < FF_UNIX_1980 || seconds >= FF_UNIX_2108)
        return 0;

    /* The entire representable exFAT interval from 1980 fits in uint32_t. */
    elapsed = (uint32_t)(seconds - FF_UNIX_1980);
    days    = elapsed / 86400u;
    sod     = elapsed % 86400u;

    /* 1980..2099 is thirty regular four-year cycles beginning on leap years. */
    if (days < 43830u) {
        uint32_t cycle = days / 1461u;
        doy            = days % 1461u;
        year           = 1980u + cycle * 4u;
        if (doy >= 366u) {
            doy -= 366u;
            year += 1u + doy / 365u;
            doy %= 365u;
        }
    } else {
        /* Only 2100..2107 remain; walking at most eight years is cheaper. */
        doy  = days - 43830u;
        year = 2100u;
        for (;;) {
            uint32_t year_days = (year == 2104u) ? 366u : 365u;
            if (doy < year_days)
                break;
            doy -= year_days;
            ++year;
        }
    }

    month = 1u;
    for (i = 0; i < 12u; ++i) {
        uint32_t n = month_days[i];
        if (i == 1u && (year % 4u) == 0u && year != 2100u)
            ++n;
        if (doy < n)
            break;
        doy -= n;
        ++month;
    }
    day = doy + 1u;

    hour   = sod / 3600u;
    minute = (sod % 3600u) / 60u;
    second = sod % 60u;

    *timestamp = ((year - 1980u) << 25) |
                 (month << 21) |
                 (day << 16) |
                 (hour << 11) |
                 (minute << 5) |
                 (second >> 1);
    *ten_ms = (uint8_t)((second & 1u) ? 100u : 0u);
    return 1;
}

static int ff_timestamp_fields(uint32_t timestamp,
                               uint32_t *year_out,
                               uint32_t *month_out,
                               uint32_t *day_out,
                               uint32_t *hour_out,
                               uint32_t *minute_out,
                               uint32_t *second_out)
{
    static const uint8_t month_days[12] = {
        31u, 28u, 31u, 30u, 31u, 30u,
        31u, 31u, 30u, 31u, 30u, 31u
    };

    uint32_t year    = 1980u + ((timestamp >> 25) & 0x7fu);
    uint32_t month   = (timestamp >> 21) & 0x0fu;
    uint32_t day     = (timestamp >> 16) & 0x1fu;
    uint32_t hour    = (timestamp >> 11) & 0x1fu;
    uint32_t minute  = (timestamp >> 5) & 0x3fu;
    uint32_t second2 = timestamp & 0x1fu;
    uint32_t max_day;

    if (month == 0u || month > 12u || day == 0u ||
        hour > 23u || minute > 59u || second2 > 29u)
        return 0;

    max_day = month_days[month - 1u];
    if (month == 2u && (year % 4u) == 0u && year != 2100u)
        ++max_day;
    if (day > max_day)
        return 0;

    if (year_out)
        *year_out = year;
    if (month_out)
        *month_out = month;
    if (day_out)
        *day_out = day;
    if (hour_out)
        *hour_out = hour;
    if (minute_out)
        *minute_out = minute;
    if (second_out)
        *second_out = second2 * 2u;
    return 1;
}

static int ff_utc_offset_valid(uint8_t offset)
{
    /* 0 means unspecified. Otherwise bit 7 marks a signed 7-bit 15-minute offset. */
    return offset == 0u || (offset & 0x80u) != 0u;
}

static int ff_exfat_timestamp_decode(uint32_t timestamp,
                                     uint8_t ten_ms,
                                     int manufactured,
                                     time_t *value_out)
{
    static const uint16_t days_before_month[12] = {
        0u, 31u, 59u, 90u, 120u, 151u,
        181u, 212u, 243u, 273u, 304u, 334u
    };

    uint32_t year, month, day, hour, minute, second;
    uint32_t y;
    uint32_t days = 0u;
    int64_t seconds;
    time_t value;

    if (!ff_timestamp_fields(timestamp, &year, &month, &day,
                             &hour, &minute, &second))
        return 0;
    if (manufactured) {
        if (ten_ms != 0u && ten_ms != 100u)
            return 0;
        if (ten_ms == 100u)
            ++second;
    } else if (ten_ms > 199u) {
        return 0;
    }

    for (y = 1980u; y < year; ++y)
        days += ((y % 4u) == 0u && y != 2100u) ? 366u : 365u;
    days += days_before_month[month - 1u];
    if (month > 2u && (year % 4u) == 0u && year != 2100u)
        ++days;
    days += day - 1u;

    seconds = (int64_t)FF_UNIX_1980 +
              (int64_t)days * 86400LL +
              (int64_t)hour * 3600LL +
              (int64_t)minute * 60LL + second;
    if (seconds < FF_UNIX_1980 || seconds >= FF_UNIX_2108)
        return 0;

    value = (time_t)seconds;
    if ((int64_t)value != seconds)
        return 0;
    if (value_out)
        *value_out = value;
    return 1;
}

/*
 * fauxFAT deliberately defines a tiny custom exFAT up-case table.
 *
 * The first 128 mappings are mandated by exFAT.  For ISO-8859-1 we also
 * fold the lower-case Latin-1 letters whose upper-case form is itself in
 * ISO-8859-1.  Everything from U+0100 through U+FFFF is identity-mapped.
 * The exFAT compression marker (FFFF,count) makes the complete Unicode
 * table only 128 bytes on disk.
 */
static uint16_t ff_upcase_latin1(uint16_t ch)
{
    if (ch >= (uint16_t)'a' && ch <= (uint16_t)'z')
        return (uint16_t)(ch - ((uint16_t)'a' - (uint16_t)'A'));
    if ((ch >= 0x00e0u && ch <= 0x00f6u) ||
        (ch >= 0x00f8u && ch <= 0x00feu))
        return (uint16_t)(ch - 0x20u);
    return ch;
}

/* Return one 16-bit word from the compressed 128-byte up-case table. */
static uint16_t ff_upcase_word(unsigned word)
{
    if (word == 0u)
        return 0xffffu;
    if (word == 1u)
        return 0x0061u; /* U+0000..U+0060 identity */

    if (word >= 2u && word < 28u)
        return (uint16_t)(0x0041u + (word - 2u)); /* a..z -> A..Z */

    if (word == 28u)
        return 0xffffu;
    if (word == 29u)
        return 0x0065u; /* U+007B..U+00DF identity */

    if (word >= 30u && word < 62u)
        return ff_upcase_latin1((uint16_t)(0x00e0u + (word - 30u)));

    if (word == 62u)
        return 0xffffu;
    return 0xff00u; /* U+0100..U+FFFF identity */
}

static uint8_t ff_upcase_byte(size_t offset)
{
    uint16_t word = ff_upcase_word((unsigned)(offset >> 1));
    return (uint8_t)(offset & 1u ? word >> 8 : word);
}

static void ff_make_upcase_block(uint32_t block_in_cluster, uint8_t out[512])
{
    uint64_t first = (uint64_t)block_in_cluster * 512u;
    size_t i;

    memset(out, 0, 512);
    if (first >= FF_UPCASE_BYTES)
        return;

    for (i = 0; i < 512u && first + i < FF_UPCASE_BYTES; ++i)
        out[i] = ff_upcase_byte((size_t)first + i);
}

static uint32_t ff_upcase_checksum(void)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i < FF_UPCASE_BYTES; ++i)
        sum = ff_ror32(sum) + ff_upcase_byte(i);
    return sum;
}

static int ff_name_valid(const char *s, size_t *len_out)
{
    size_t n = 0;

    if (!s || !*s)
        return 0;

    while (s[n]) {
        unsigned char c = (unsigned char)s[n];
        int forbidden   = c < 0x20u || c == '"' || c == '*' || c == '/' ||
                        c == ':' || c == '<' || c == '>' || c == '?' ||
                        c == '\\' || c == '|';
        if (forbidden || n == FAUXFAT_NAME_MAX)
            return 0;
        ++n;
    }

    if ((n == 1u && s[0] == '.') ||
        (n == 2u && s[0] == '.' && s[1] == '.'))
        return 0;

    if (len_out)
        *len_out = n;
    return 1;
}

static int ff_name_equal_folded(const char *a, const char *b)
{
    size_t i = 0;

    for (;;) {
        uint8_t ca = (uint8_t)a[i];
        uint8_t cb = (uint8_t)b[i];
        if (ff_upcase_latin1(ca) != ff_upcase_latin1(cb))
            return 0;
        if (ca == 0u)
            return 1;
        ++i;
    }
}

static int ff_label_valid(const char *s, size_t *len_out)
{
    size_t n = 0;

    if (!s)
        s = "FAUXFAT";

    while (s[n]) {
        unsigned char c = (unsigned char)s[n];
        if (n == 11u || c < 0x20u || c > 0x7eu)
            return 0;
        ++n;
    }

    if (len_out)
        *len_out = n;
    return 1;
}

static uint16_t ff_name_hash(const char *name)
{
    uint16_t h = 0;

    while (*name) {
        uint16_t ch = ff_upcase_latin1((uint8_t)*name++);
        h           = (uint16_t)(ff_ror16(h) + (uint8_t)ch);
        h           = (uint16_t)(ff_ror16(h) + (uint8_t)(ch >> 8));
    }
    return h;
}

static uint16_t ff_entry_set_checksum(const uint8_t *p, size_t bytes)
{
    uint16_t sum = 0;
    size_t i;

    for (i = 0; i < bytes; ++i) {
        if (i == 2u || i == 3u)
            continue;
        sum = (uint16_t)(ff_ror16(sum) + p[i]);
    }
    return sum;
}

/* fauxFAT hashes private structure only; exFAT checksums keep their own rules. */
typedef XXH32_state_t ff_xxh32;

static void ff_xxh32_init(ff_xxh32 *state, uint32_t seed)
{
    (void)XXH32_reset(state, seed);
}

static void ff_xxh32_update(ff_xxh32 *state, const uint8_t *data, size_t size)
{
    (void)XXH32_update(state, data, size);
}

static uint32_t ff_xxh32_digest(const ff_xxh32 *state)
{
    return XXH32_digest(state);
}

typedef struct ff_structural_hash_state {
    ff_xxh32 fat;
    ff_xxh32 bitmap;
    ff_xxh32 root;
    ff_xxh32 upcase;
    ff_xxh32 map;
} ff_structural_hash_state;

typedef struct ff_structural_hash_values {
    uint32_t fat;
    uint32_t bitmap;
    uint32_t root;
    uint32_t upcase;
    uint32_t map;
} ff_structural_hash_values;

static const uint8_t ff_map_hash_prefix[8] = {
    'F', 'F', 'M', 'A', 'P', '1', 0, 0
};

static void ff_structural_hash_init(ff_structural_hash_state *state,
                                    const uint8_t boot[FAUXFAT_BLOCK_SIZE])
{
    ff_xxh32_init(&state->fat, FF_XXH32_FAT_SEED);
    ff_xxh32_init(&state->bitmap, FF_XXH32_BITMAP_SEED);
    ff_xxh32_init(&state->root, FF_XXH32_ROOT_SEED);
    ff_xxh32_init(&state->upcase, FF_XXH32_UPCASE_SEED);
    ff_xxh32_init(&state->map, FF_XXH32_MAP_SEED);
    ff_xxh32_update(&state->map, ff_map_hash_prefix,
                    sizeof(ff_map_hash_prefix));
    if (boot) {
        ff_xxh32_update(&state->map, boot + 64u, 42u);
        ff_xxh32_update(&state->map, boot + 108u, 4u);
    }
}

static void ff_structural_hash_digest(const ff_structural_hash_state *state,
                                      ff_structural_hash_values *values)
{
    values->fat    = ff_xxh32_digest(&state->fat);
    values->bitmap = ff_xxh32_digest(&state->bitmap);
    values->root   = ff_xxh32_digest(&state->root);
    values->upcase = ff_xxh32_digest(&state->upcase);
    values->map    = ff_xxh32_digest(&state->map);
}

static void ff_structural_hash_update(ff_xxh32 *component, ff_xxh32 *map,
                                      const uint8_t *data, size_t bytes)
{
    ff_xxh32_update(component, data, bytes);
    ff_xxh32_update(map, data, bytes);
}

static uint64_t ff_file_clusters(const fauxfat_file *f)
{
    if (f->allocation_size != 0u)
        return f->allocation_size / FAUXFAT_CLUSTER_SIZE;
    return ((f->size - 1u) / FAUXFAT_CLUSTER_SIZE) + 1u;
}

static int ff_file_uses_fat_chain(const fauxfat_file *f)
{
    return f->allocation_size != 0u;
}

static uint64_t ff_file_data_length(const fauxfat_view *v, size_t file_index)
{
    return v->file_data_length[file_index];
}

static uint64_t ff_file_valid_data_length(const fauxfat_view *v,
                                         size_t file_index)
{
    return v->file_valid_data_length[file_index];
}

static uint64_t ff_file_write_limit(const fauxfat_view *v, size_t file_index)
{
    const fauxfat_file *f = &v->config->files[file_index];
    return f->allocation_size != 0u ? f->allocation_size
                                    : ff_file_data_length(v, file_index);
}

static uint64_t ff_opaque_clusters(const fauxfat_opaque_file *f)
{
    return f->size / FAUXFAT_CLUSTER_SIZE;
}

static uint64_t ff_data_cluster_count(const fauxfat_view *v)
{
    return (uint64_t)v->cluster_count + 2u - v->data_first_cluster;
}

/*
 * Configured placement is expressed relative to the first payload cluster,
 * not as an absolute exFAT cluster/LBA.  That avoids a geometry loop: the FAT
 * and bitmap sizes may move the cluster heap, but payload offsets remain the
 * same.  We deliberately recompute these small prefix sums instead of keeping
 * an extent index in RAM.
 */
static uint64_t ff_file_data_cluster(const fauxfat_view *v, size_t file_index)
{
    uint64_t cursor = 0u;
    size_t i;

    for (i = 0u; i <= file_index; ++i) {
        const fauxfat_file *f = &v->config->files[i];
        uint64_t start        = f->data_cluster == FAUXFAT_CLUSTER_AUTO ? cursor : f->data_cluster;
        if (i == file_index)
            return start;
        cursor = start + ff_file_clusters(f);
    }
    return cursor;
}

static uint64_t ff_public_end_data_cluster(const fauxfat_view *v)
{
    uint64_t cursor = 0u;
    size_t i;

    for (i = 0u; i < v->config->file_count; ++i) {
        const fauxfat_file *f = &v->config->files[i];
        uint64_t start        = f->data_cluster == FAUXFAT_CLUSTER_AUTO ? cursor : f->data_cluster;
        cursor                = start + ff_file_clusters(f);
    }
    return cursor;
}

static uint64_t ff_opaque_data_cluster(const fauxfat_view *v,
                                       size_t opaque_index)
{
    uint64_t cursor = ff_public_end_data_cluster(v);
    size_t i;

    for (i = 0u; i <= opaque_index; ++i) {
        const fauxfat_opaque_file *f = &v->config->opaque_files[i];
        uint64_t start               = f->data_cluster == FAUXFAT_CLUSTER_AUTO ? cursor : f->data_cluster;
        if (i == opaque_index)
            return start;
        cursor = start + ff_opaque_clusters(f);
    }
    return cursor;
}

static uint32_t ff_file_first_cluster(const fauxfat_view *v, size_t file_index)
{
    return (uint32_t)((uint64_t)v->data_first_cluster +
                      ff_file_data_cluster(v, file_index));
}

static uint32_t ff_opaque_first_cluster(const fauxfat_view *v,
                                        size_t opaque_index)
{
    return (uint32_t)((uint64_t)v->data_first_cluster +
                      ff_opaque_data_cluster(v, opaque_index));
}

static int ff_data_cluster_is_public(const fauxfat_view *v,
                                     uint64_t data_cluster)
{
    size_t i;

    for (i = 0u; i < v->config->file_count; ++i) {
        uint64_t first = ff_file_data_cluster(v, i);
        uint64_t end   = first + ff_file_clusters(&v->config->files[i]);

        if (data_cluster < first)
            return 0;
        if (data_cluster < end)
            return 1;
    }
    return 0;
}

static void ff_data_extent(const fauxfat_view *v, size_t index,
                           uint64_t *first, uint64_t *end)
{
    if (index < v->config->file_count) {
        *first = ff_file_data_cluster(v, index);
        *end   = *first + ff_file_clusters(&v->config->files[index]);
    } else {
        size_t oi = index - v->config->file_count;
        *first    = ff_opaque_data_cluster(v, oi);
        *end      = *first + ff_opaque_clusters(&v->config->opaque_files[oi]);
    }
}

static size_t ff_anonymous_private_run_count(const fauxfat_view *v)
{
    size_t extent_count = v->config->file_count + v->config->opaque_file_count;
    uint64_t cursor     = 0u;
    uint64_t data_count = ff_data_cluster_count(v);
    size_t count        = 0u;
    size_t i;

    for (i = 0u; i < extent_count; ++i) {
        uint64_t first;
        uint64_t end;

        ff_data_extent(v, i, &first, &end);
        if (cursor < first)
            ++count;
        cursor = end;
    }
    if (cursor < data_count)
        ++count;
    return count;
}

static int ff_anonymous_private_run(const fauxfat_view *v, size_t ordinal,
                                    uint64_t *first_out,
                                    uint64_t *count_out)
{
    size_t extent_count = v->config->file_count + v->config->opaque_file_count;
    uint64_t cursor     = 0u;
    uint64_t data_count = ff_data_cluster_count(v);
    size_t run          = 0u;
    size_t i;

    for (i = 0u; i < extent_count; ++i) {
        uint64_t first;
        uint64_t end;

        ff_data_extent(v, i, &first, &end);
        if (cursor < first) {
            if (run == ordinal) {
                *first_out = cursor;
                *count_out = first - cursor;
                return 1;
            }
            ++run;
        }
        cursor = end;
    }
    if (cursor < data_count && run == ordinal) {
        *first_out = cursor;
        *count_out = data_count - cursor;
        return 1;
    }
    return 0;
}

static int ff_file_length_changes_enabled(const fauxfat_view *v)
{
    size_t i;

    if (v->mutable_file_lengths)
        return 1;
    for (i = 0u; i < v->config->file_count; ++i) {
        if (v->config->files[i].allocation_size != 0u)
            return 1;
    }
    return 0;
}

static uint16_t ff_oem_flags(const fauxfat_view *v)
{
    uint16_t flags = v->config->private_reservation ==
                             FAUXFAT_PRIVATE_BENIGN_PRIMARY_AND_BAD
                         ? FF_OEM_FLAG_BENIGN_RESERVE
                         : 0u;

    if (ff_file_length_changes_enabled(v))
        flags |= FF_OEM_FLAG_MUTABLE_LENGTH;
    return flags;
}

static uint32_t ff_fat_entry(const fauxfat_view *v, uint32_t entry)
{
    uint32_t bitmap_first = 2u;
    uint32_t bitmap_last  = bitmap_first + v->bitmap_clusters - 1u;
    uint64_t data_cluster;

    if (entry == 0u)
        return 0xfffffff8u;
    if (entry == 1u)
        return FF_FAT_EOC;
    if (entry < 2u || entry > v->cluster_count + 1u)
        return 0u;

    if (entry >= bitmap_first && entry <= bitmap_last)
        return entry == bitmap_last ? FF_FAT_EOC : entry + 1u;
    if (entry == v->upcase_cluster || entry == v->root_cluster)
        return FF_FAT_EOC;
    if (entry < v->data_first_cluster)
        return 0u;

    data_cluster = (uint64_t)entry - v->data_first_cluster;
    if (ff_data_cluster_is_public(v, data_cluster)) {
        size_t i;

        for (i = 0u; i < v->config->file_count; ++i) {
            const fauxfat_file *f = &v->config->files[i];
            uint64_t first = ff_file_data_cluster(v, i);
            uint64_t clusters = ff_file_clusters(f);

            if (data_cluster < first)
                break;
            if (data_cluster < first + clusters) {
                if (!ff_file_uses_fat_chain(f))
                    /* NoFatChain payload: FAT contents are invalid. */
                    return 0u;
                if (data_cluster + 1u == first + clusters)
                    return FF_FAT_EOC;
                return entry + 1u;
            }
        }
        return 0u;
    }

    /* Opaque descriptors, explicit holes and tail reserve are blockers. */
    return 0xfffffff7u;
}

static void ff_make_boot_sector(const fauxfat_view *v,
                                uint8_t out[512],
                                int backup)
{
    memset(out, 0, 512);
    out[0] = 0xeb;
    out[1] = 0x76;
    out[2] = 0x90;
    memcpy(out + 3, "EXFAT   ", 8);
    faux_store_le64(out + 64, v->config->partition_lba);
    faux_store_le64(out + 72, v->volume_blocks);
    faux_store_le32(out + 80, FF_FAT_OFFSET_BLOCKS);
    faux_store_le32(out + 84, v->fat_length_blocks);
    faux_store_le32(out + 88, v->cluster_heap_block);
    faux_store_le32(out + 92, v->cluster_count);
    faux_store_le32(out + 96, v->root_cluster);
    faux_store_le32(out + 100, v->config->volume_serial);
    faux_store_le16(out + 104, 0x0100u);
    faux_store_le16(out + 106,
                    backup ? v->backup_volume_flags : v->volume_flags);
    out[108] = 9u;
    out[109] = 7u;
    out[110] = 1u;
    out[111] = 0x80u;
    out[112] = 100u;
    memset(out + 120, 0xf4, 390);
    out[510] = 0x55u;
    out[511] = 0xaau;
}

static void ff_make_extended_boot(uint8_t out[512])
{
    memset(out, 0, 512);
    out[510] = 0x55u;
    out[511] = 0xaau;
}

static void ff_make_oem_sector(const fauxfat_view *v, uint8_t out[512])
{
    uint8_t payload[32];

    memset(out, 0, 512);
    memcpy(out, ff_oem_map_guid, 16);
    faux_store_le32(out + 16, v->map_xxh32);
    faux_store_le32(out + 20, 0u);

    memcpy(out + 48, ff_oem_epoch_guid, 16);
    memset(payload, 0, sizeof(payload));
    memcpy(payload, "FFV1", 4);
    faux_store_le16(payload + 4, 1u);
    faux_store_le16(payload + 6, ff_oem_flags(v));
    faux_store_le64(payload + 8, v->config->structural_epoch);
    faux_store_le32(payload + 16, v->fat_xxh32);
    faux_store_le32(payload + 20, v->bitmap_xxh32);
    faux_store_le32(payload + 24, v->root_xxh32);
    faux_store_le32(payload + 28, v->upcase_xxh32);
    memcpy(out + 64, payload, sizeof(payload));
}

static void ff_make_checksum_sector(const fauxfat_view *v, uint8_t out[512])
{
    unsigned i;
    for (i = 0; i < 128; ++i)
        faux_store_le32(out + 4u * i, v->boot_checksum);
}

static void ff_make_bitmap_block(const fauxfat_view *v,
                                 uint32_t block_in_bitmap,
                                 uint8_t out[512])
{
    uint64_t first_byte   = (uint64_t)block_in_bitmap * 512u;
    uint64_t bitmap_bytes = ((uint64_t)v->cluster_count + 7u) / 8u;
    unsigned i;

    memset(out, 0, 512);
    for (i = 0; i < 512; ++i) {
        uint64_t byte_no = first_byte + i;
        if (byte_no >= bitmap_bytes)
            break;
        out[i] = 0xffu;
    }

    if ((v->cluster_count & 7u) != 0u &&
        bitmap_bytes != 0u &&
        first_byte <= bitmap_bytes - 1u &&
        bitmap_bytes - 1u < first_byte + 512u) {
        unsigned valid = v->cluster_count & 7u;
        out[(size_t)((bitmap_bytes - 1u) - first_byte)] =
            (uint8_t)((1u << valid) - 1u);
    }
}

static void ff_store_file_times(uint8_t file_ent[32], time_t mtime)
{
    uint32_t timestamp;
    uint8_t ten_ms;

    (void)ff_exfat_timestamp(mtime, &timestamp, &ten_ms);
    faux_store_le32(file_ent + 8, timestamp);
    faux_store_le32(file_ent + 12, timestamp);
    faux_store_le32(file_ent + 16, timestamp);
    file_ent[20] = ten_ms;
    file_ent[21] = ten_ms;
    file_ent[22] = 0x80u;
    file_ent[23] = 0x80u;
    file_ent[24] = 0x80u;
}

static void ff_make_public_file_set_at(const fauxfat_view *v,
                                       size_t file_index,
                                       uint64_t valid_data_length,
                                       uint64_t data_length,
                                       uint8_t set[160])
{
    const fauxfat_file *f = &v->config->files[file_index];
    uint8_t *file_ent     = set;
    uint8_t *stream_ent   = set + 32;
    uint8_t *name_ent     = set + 64;
    uint8_t *vendor0      = set + 96;
    uint8_t *vendor1      = set + 128;
    size_t name_len       = 0;
    size_t i;

    (void)ff_name_valid(f->name, &name_len);
    memset(set, 0, 160);

    file_ent[0] = FF_ENTRY_FILE;
    file_ent[1] = 4u;
    ff_store_file_times(file_ent, f->mtime);
    faux_store_le16(file_ent + 4u,
                    faux_load_le16(v->file_primary_fields[file_index]));
    memcpy(file_ent + 12u,
           v->file_primary_fields[file_index] + 2u, 8u);
    file_ent[21] = v->file_primary_fields[file_index][10];
    file_ent[23] = v->file_primary_fields[file_index][11];
    file_ent[24] = v->file_primary_fields[file_index][12];

    stream_ent[0] = FF_ENTRY_STREAM;
    stream_ent[1] = ff_file_uses_fat_chain(f) ? 0x01u : 0x03u;
    stream_ent[3] = (uint8_t)name_len;
    faux_store_le16(stream_ent + 4, ff_name_hash(f->name));
    faux_store_le64(stream_ent + 8, valid_data_length);
    faux_store_le32(stream_ent + 20, ff_file_first_cluster(v, file_index));
    faux_store_le64(stream_ent + 24, data_length);

    name_ent[0] = FF_ENTRY_NAME;
    for (i = 0; i < name_len; ++i)
        faux_store_le16(name_ent + 2u + 2u * i, (uint8_t)f->name[i]);

    /*
     * Persist the logical/original name independently of the host namespace.
     * The first record carries length + bytes 0..12; the second carries bytes
     * 13..14 and repeats the length for a cheap torn/corrupt-record check.
     */
    vendor0[0] = FF_ENTRY_VENDOR_EXT;
    vendor0[1] = 0u;
    memcpy(vendor0 + 2, ff_public_name_guid0, 16u);
    vendor0[18] = (uint8_t)name_len;
    for (i = 0; i < name_len && i < 13u; ++i)
        vendor0[19u + i] = (uint8_t)f->name[i];

    vendor1[0] = FF_ENTRY_VENDOR_EXT;
    vendor1[1] = 0u;
    memcpy(vendor1 + 2, ff_public_name_guid1, 16u);
    if (name_len > 13u)
        vendor1[18] = (uint8_t)f->name[13];
    if (name_len > 14u)
        vendor1[19] = (uint8_t)f->name[14];
    vendor1[20] = (uint8_t)name_len;

    faux_store_le16(file_ent + 2, ff_entry_set_checksum(set, 160));
}

static void ff_make_public_file_set(const fauxfat_view *v,
                                    size_t file_index,
                                    uint8_t set[160])
{
    ff_make_public_file_set_at(v, file_index,
                               ff_file_valid_data_length(v, file_index),
                               ff_file_data_length(v, file_index), set);
    if (v->pending_file_set_checksum_valid[file_index])
        faux_store_le16(set + 2u,
                        v->pending_file_set_checksum[file_index]);
}

static void ff_opaque_stub_name(size_t opaque_index,
                                char out[FAUXFAT_NAME_MAX + 1u])
{
    static const char hex[] = "0123456789ABCDEF";
    uint32_t v              = (uint32_t)opaque_index;
    unsigned i;

    out[0] = '$';
    out[1] = 'F';
    out[2] = 'F';
    for (i = 0; i < 8u; ++i)
        out[3u + i] = hex[(v >> (28u - 4u * i)) & 0x0fu];
    out[11] = '\0';
}

static void ff_make_opaque_file_set(const fauxfat_view *v,
                                    size_t opaque_index,
                                    uint8_t set[160])
{
    const fauxfat_opaque_file *f = &v->config->opaque_files[opaque_index];
    uint8_t *file_ent            = set;
    uint8_t *stream_ent          = set + 32;
    uint8_t *name_ent            = set + 64;
    uint8_t *vendor_ext          = set + 96;
    uint8_t *vendor_alloc        = set + 128;
    char stub[FAUXFAT_NAME_MAX + 1u];
    size_t name_len = 0;
    size_t stub_len = 0;
    size_t i;

    (void)ff_name_valid(f->name, &name_len);
    ff_opaque_stub_name(opaque_index, stub);
    (void)ff_name_valid(stub, &stub_len);
    memset(set, 0, 160);

    file_ent[0] = FF_ENTRY_FILE;
    file_ent[1] = 4u;
    faux_store_le16(file_ent + 4, FF_ATTR_READONLY | FF_ATTR_HIDDEN | FF_ATTR_SYSTEM);
    ff_store_file_times(file_ent, f->mtime);

    /* The visible stream is an inert zero-length descriptor file. */
    stream_ent[0] = FF_ENTRY_STREAM;
    stream_ent[1] = 0x01u; /* AllocationPossible, no allocation => NoFatChain=0 */
    stream_ent[3] = (uint8_t)stub_len;
    faux_store_le16(stream_ent + 4, ff_name_hash(stub));

    name_ent[0] = FF_ENTRY_NAME;
    for (i = 0; i < stub_len; ++i)
        faux_store_le16(name_ent + 2u + 2u * i, (uint8_t)stub[i]);

    /*
     * The metadata GUID is the format/version tag. VendorDefined stores the
     * logical ISO-8859-1 name: length + first 13 bytes.  The final two bytes
     * live in the Vendor Allocation's VendorDefined field, giving us the full
     * fauxFAT 15-byte name without another directory entry.
     */
    vendor_ext[0] = FF_ENTRY_VENDOR_EXT;
    vendor_ext[1] = 0u;
    memcpy(vendor_ext + 2, ff_opaque_meta_guid, 16);
    vendor_ext[18] = (uint8_t)name_len;
    for (i = 0; i < name_len && i < 13u; ++i)
        vendor_ext[19u + i] = (uint8_t)f->name[i];

    vendor_alloc[0] = FF_ENTRY_VENDOR_ALLOC;
    vendor_alloc[1] = 0x03u; /* AllocationPossible | NoFatChain */
    memcpy(vendor_alloc + 2, ff_opaque_alloc_guid, 16);
    if (name_len > 13u)
        vendor_alloc[18] = (uint8_t)f->name[13];
    if (name_len > 14u)
        vendor_alloc[19] = (uint8_t)f->name[14];
    faux_store_le32(vendor_alloc + 20, ff_opaque_first_cluster(v, opaque_index));
    faux_store_le64(vendor_alloc + 24, f->size);

    faux_store_le16(file_ent + 2, ff_entry_set_checksum(set, 160));
}

static void ff_make_private_reserve_entry(const fauxfat_view *v,
                                          size_t ordinal,
                                          uint8_t out[32])
{
    uint64_t first   = 0u;
    uint64_t count   = 0u;
    size_t run_count = ff_anonymous_private_run_count(v);

    if (!ff_anonymous_private_run(v, ordinal, &first, &count)) {
        memset(out, 0, 32u);
        return;
    }
    memset(out, 0, 32u);
    out[0] = FF_ENTRY_PRIVATE_RESERVE;
    out[1] = 0u;
    /* Generic Primary: AllocationPossible | NoFatChain. */
    faux_store_le16(out + 4u, 0x0003u);
    memcpy(out + 6u, ff_private_reserve_magic,
           sizeof(ff_private_reserve_magic));
    faux_store_le32(out + 14u, (uint32_t)ordinal);
    faux_store_le16(out + 18u, (uint16_t)run_count);
    faux_store_le32(out + 20u,
                    (uint32_t)((uint64_t)v->data_first_cluster + first));
    faux_store_le64(out + 24u, count * FAUXFAT_CLUSTER_SIZE);
    faux_store_le16(out + 2u, ff_entry_set_checksum(out, 32u));
}

static void ff_make_root_entry(const fauxfat_view *v,
                               uint32_t entry_index,
                               uint8_t out[32])
{
    const fauxfat_config *cfg = v->config;
    uint32_t public_first     = FF_FILE_SLOT_FIRST;
    uint32_t public_end       = public_first +
                          (uint32_t)(cfg->file_count * FF_PUBLIC_ENTRY_COUNT);
    uint32_t opaque_first = public_end;
    uint32_t opaque_end   = opaque_first +
                          (uint32_t)(cfg->opaque_file_count * FF_OPAQUE_ENTRY_COUNT);
    uint32_t blocker_first = opaque_end;
    uint32_t blocker_end   = blocker_first +
                           (cfg->private_reservation == FAUXFAT_PRIVATE_BENIGN_PRIMARY_AND_BAD ? (uint32_t)ff_anonymous_private_run_count(v) : 0u);

    memset(out, 0, 32);

    if (entry_index == 0u) {
        uint64_t bitmap_bytes = ((uint64_t)v->cluster_count + 7u) / 8u;
        out[0]                = FF_ENTRY_BITMAP;
        faux_store_le32(out + 20, 2u);
        faux_store_le64(out + 24, bitmap_bytes);
        return;
    }

    if (entry_index == 1u) {
        out[0] = FF_ENTRY_UPCASE;
        faux_store_le32(out + 4, v->upcase_checksum);
        faux_store_le32(out + 20, v->upcase_cluster);
        faux_store_le64(out + 24, FF_UPCASE_BYTES);
        return;
    }

    if (entry_index == 2u) {
        const char *label = cfg->volume_label ? cfg->volume_label : "FAUXFAT";
        size_t len        = 0;
        size_t i;
        (void)ff_label_valid(label, &len);
        out[0] = FF_ENTRY_LABEL;
        out[1] = (uint8_t)len;
        for (i = 0; i < len; ++i)
            faux_store_le16(out + 2u + 2u * i, (uint8_t)label[i]);
        return;
    }

    if (entry_index == 3u) {
        out[0] = FF_ENTRY_GUID;
        memcpy(out + 6, cfg->volume_guid, 16);
        faux_store_le16(out + 2, ff_entry_set_checksum(out, 32));
        return;
    }

    if (entry_index >= public_first && entry_index < public_end) {
        uint32_t rel    = entry_index - public_first;
        size_t slot     = rel / FF_PUBLIC_ENTRY_COUNT;
        unsigned member = rel % FF_PUBLIC_ENTRY_COUNT;
        uint8_t set[160];

        ff_make_public_file_set(v, slot, set);
        memcpy(out, set + member * 32u, 32);
        return;
    }

    if (entry_index >= opaque_first && entry_index < opaque_end) {
        uint32_t rel    = entry_index - opaque_first;
        size_t slot     = rel / FF_OPAQUE_ENTRY_COUNT;
        unsigned member = rel % FF_OPAQUE_ENTRY_COUNT;
        uint8_t set[160];

        ff_make_opaque_file_set(v, slot, set);
        memcpy(out, set + member * 32u, 32);
        return;
    }

    if (entry_index >= blocker_first && entry_index < blocker_end) {
        ff_make_private_reserve_entry(v, entry_index - blocker_first, out);
        return;
    }

    out[0] = FF_ENTRY_PAD;
    faux_store_le16(out + 2, 0x0508u);
}

static void ff_make_root_block(const fauxfat_view *v,
                               uint32_t block_in_root,
                               uint8_t out[512])
{
    uint32_t first = block_in_root * 16u;
    unsigned i;

    for (i = 0; i < 16; ++i)
        ff_make_root_entry(v, first + i, out + 32u * i);
}

static void ff_make_fat_block(const fauxfat_view *v,
                              uint32_t block_in_fat,
                              uint8_t out[512])
{
    uint64_t first_entry = (uint64_t)block_in_fat * 128u;
    unsigned i;

    memset(out, 0, 512);
    for (i = 0; i < 128; ++i) {
        uint64_t entry = first_entry + i;
        if (entry > (uint64_t)v->cluster_count + 1u)
            break;
        faux_store_le32(out + 4u * i, ff_fat_entry(v, (uint32_t)entry));
    }
}

/*
 * The synthetic block view, formatter and verifier all need the same answer
 * to the only geometry question that matters: what does this physical block
 * mean?  Keep that answer here rather than letting each consumer rediscover
 * exFAT layout with a slightly different off-by-one.
 *
 * REQUIRED_ZERO is structural zero data.  UNDEFINED is deliberately weaker:
 * the synthetic view returns zero for deterministic reads, but a sparse/in-
 * place formatter is free to skip those blocks entirely.
 */
typedef enum ff_range_kind {
    FF_RANGE_GENERATED,
    FF_RANGE_REQUIRED_ZERO,
    FF_RANGE_PUBLIC_PAYLOAD,
    FF_RANGE_OPAQUE_PRESERVE,
    FF_RANGE_RESERVED_PRESERVE,
    FF_RANGE_UNDEFINED
} ff_range_kind;

typedef enum ff_generated_kind {
    FF_GENERATED_BOOT,
    FF_GENERATED_EXTENDED_BOOT,
    FF_GENERATED_OEM,
    FF_GENERATED_CHECKSUM,
    FF_GENERATED_FAT,
    FF_GENERATED_BITMAP,
    FF_GENERATED_UPCASE,
    FF_GENERATED_ROOT
} ff_generated_kind;

typedef struct ff_block_mapping {
    ff_range_kind kind;
    ff_generated_kind generated_kind;
    uint32_t generated_index;
    size_t object_index;
    uint64_t object_offset;
    size_t length;
} ff_block_mapping;

static int ff_classify_block(const fauxfat_view *v,
                             uint64_t block_address,
                             ff_block_mapping *m)
{
    uint64_t heap_rel;
    uint32_t cluster;
    uint32_t block_in_cluster;
    uint64_t fat_meaningful_bytes;
    uint32_t fat_meaningful_blocks;
    uint64_t bitmap_bytes;
    uint32_t bitmap_data_blocks;
    size_t i;

    if (block_address >= v->volume_blocks)
        return FAUXFAT_ERANGE;

    memset(m, 0, sizeof(*m));

    if (block_address == 0u || block_address == 12u) {
        m->kind           = FF_RANGE_GENERATED;
        m->generated_kind = FF_GENERATED_BOOT;
        m->generated_index = block_address == 12u ? 1u : 0u;
        return FAUXFAT_OK;
    }

    if ((block_address >= 1u && block_address <= 8u) ||
        (block_address >= 13u && block_address <= 20u)) {
        m->kind           = FF_RANGE_GENERATED;
        m->generated_kind = FF_GENERATED_EXTENDED_BOOT;
        return FAUXFAT_OK;
    }

    if (block_address == 9u || block_address == 21u) {
        m->kind           = FF_RANGE_GENERATED;
        m->generated_kind = FF_GENERATED_OEM;
        return FAUXFAT_OK;
    }

    if (block_address == 10u || block_address == 22u) {
        m->kind = FF_RANGE_REQUIRED_ZERO;
        return FAUXFAT_OK;
    }

    if (block_address == 11u || block_address == 23u) {
        m->kind           = FF_RANGE_GENERATED;
        m->generated_kind = FF_GENERATED_CHECKSUM;
        return FAUXFAT_OK;
    }

    if (block_address >= 24u && block_address < FF_FAT_OFFSET_BLOCKS) {
        m->kind = FF_RANGE_UNDEFINED;
        return FAUXFAT_OK;
    }

    if (block_address >= FF_FAT_OFFSET_BLOCKS &&
        block_address < FF_FAT_OFFSET_BLOCKS + v->fat_length_blocks) {
        fat_meaningful_bytes  = ((uint64_t)v->cluster_count + 2u) * 4u;
        fat_meaningful_blocks = (uint32_t)((fat_meaningful_bytes + 511u) / 512u);
        m->generated_index    = (uint32_t)(block_address - FF_FAT_OFFSET_BLOCKS);
        if (m->generated_index < fat_meaningful_blocks) {
            m->kind           = FF_RANGE_GENERATED;
            m->generated_kind = FF_GENERATED_FAT;
        } else {
            m->kind = FF_RANGE_UNDEFINED;
        }
        return FAUXFAT_OK;
    }

    if (block_address < v->cluster_heap_block)
        return FAUXFAT_EGEOMETRY;

    heap_rel         = block_address - v->cluster_heap_block;
    cluster          = 2u + (uint32_t)(heap_rel / FAUXFAT_BLOCKS_PER_CLUSTER);
    block_in_cluster = (uint32_t)(heap_rel % FAUXFAT_BLOCKS_PER_CLUSTER);

    if (cluster >= 2u && cluster < 2u + v->bitmap_clusters) {
        uint32_t bitmap_block = (cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER +
                                block_in_cluster;
        bitmap_bytes       = ((uint64_t)v->cluster_count + 7u) / 8u;
        bitmap_data_blocks = (uint32_t)((bitmap_bytes + 511u) / 512u);
        m->generated_index = bitmap_block;
        if (bitmap_block < bitmap_data_blocks) {
            m->kind           = FF_RANGE_GENERATED;
            m->generated_kind = FF_GENERATED_BITMAP;
        } else {
            m->kind = FF_RANGE_UNDEFINED;
        }
        return FAUXFAT_OK;
    }

    if (cluster == v->upcase_cluster) {
        if (block_in_cluster == 0u) {
            m->kind            = FF_RANGE_GENERATED;
            m->generated_kind  = FF_GENERATED_UPCASE;
            m->generated_index = 0u;
        } else {
            m->kind = FF_RANGE_UNDEFINED;
        }
        return FAUXFAT_OK;
    }

    if (cluster == v->root_cluster) {
        m->kind            = FF_RANGE_GENERATED;
        m->generated_kind  = FF_GENERATED_ROOT;
        m->generated_index = block_in_cluster;
        return FAUXFAT_OK;
    }

    if (cluster < v->data_first_cluster ||
        cluster > v->cluster_count + 1u)
        return FAUXFAT_EGEOMETRY;

    {
        uint64_t data_cluster = (uint64_t)cluster - v->data_first_cluster;

        for (i = 0; i < v->config->file_count; ++i) {
            const fauxfat_file *f = &v->config->files[i];
            uint64_t first_data   = ff_file_data_cluster(v, i);
            uint64_t nclusters    = ff_file_clusters(f);

            if (data_cluster < first_data) {
                m->kind = FF_RANGE_RESERVED_PRESERVE;
                return FAUXFAT_OK;
            }
            if (data_cluster < first_data + nclusters) {
                uint64_t off = (data_cluster - first_data) *
                                   FAUXFAT_CLUSTER_SIZE +
                               (uint64_t)block_in_cluster * FAUXFAT_BLOCK_SIZE;
                uint64_t write_limit = ff_file_write_limit(v, i);
                if (off < write_limit) {
                    uint64_t left    = write_limit - off;
                    m->kind          = FF_RANGE_PUBLIC_PAYLOAD;
                    m->object_index  = i;
                    m->object_offset = off;
                    m->length        = left > FAUXFAT_BLOCK_SIZE ? FAUXFAT_BLOCK_SIZE : (size_t)left;
                } else {
                    m->kind = FF_RANGE_UNDEFINED;
                }
                return FAUXFAT_OK;
            }
        }

        for (i = 0; i < v->config->opaque_file_count; ++i) {
            const fauxfat_opaque_file *f = &v->config->opaque_files[i];
            uint64_t first_data          = ff_opaque_data_cluster(v, i);
            uint64_t nclusters           = ff_opaque_clusters(f);

            if (data_cluster < first_data) {
                m->kind = FF_RANGE_RESERVED_PRESERVE;
                return FAUXFAT_OK;
            }
            if (data_cluster < first_data + nclusters) {
                m->kind         = FF_RANGE_OPAQUE_PRESERVE;
                m->object_index = i;
                m->object_offset =
                    (data_cluster - first_data) * FAUXFAT_CLUSTER_SIZE +
                    (uint64_t)block_in_cluster * FAUXFAT_BLOCK_SIZE;
                m->length = FAUXFAT_BLOCK_SIZE;
                return FAUXFAT_OK;
            }
        }

        /* Explicit holes and any configured tail capacity are private reserve. */
        m->kind = FF_RANGE_RESERVED_PRESERVE;
        return FAUXFAT_OK;
    }

    return FAUXFAT_EGEOMETRY;
}

static int ff_render_classified_block(const fauxfat_view *v,
                                      const ff_block_mapping *m,
                                      uint8_t out[512])
{
    if (m->kind == FF_RANGE_REQUIRED_ZERO || m->kind == FF_RANGE_UNDEFINED ||
        m->kind == FF_RANGE_RESERVED_PRESERVE) {
        memset(out, 0, 512);
        return FAUXFAT_OK;
    }
    if (m->kind != FF_RANGE_GENERATED)
        return FAUXFAT_EUNMAPPED;

    switch (m->generated_kind) {
    case FF_GENERATED_BOOT:
        ff_make_boot_sector(v, out, m->generated_index != 0u);
        return FAUXFAT_OK;
    case FF_GENERATED_EXTENDED_BOOT:
        ff_make_extended_boot(out);
        return FAUXFAT_OK;
    case FF_GENERATED_OEM:
        ff_make_oem_sector(v, out);
        return FAUXFAT_OK;
    case FF_GENERATED_CHECKSUM:
        ff_make_checksum_sector(v, out);
        return FAUXFAT_OK;
    case FF_GENERATED_FAT:
        ff_make_fat_block(v, m->generated_index, out);
        return FAUXFAT_OK;
    case FF_GENERATED_BITMAP:
        ff_make_bitmap_block(v, m->generated_index, out);
        return FAUXFAT_OK;
    case FF_GENERATED_UPCASE:
        ff_make_upcase_block(m->generated_index, out);
        return FAUXFAT_OK;
    case FF_GENERATED_ROOT:
        ff_make_root_block(v, m->generated_index, out);
        return FAUXFAT_OK;
    }

    return FAUXFAT_EGEOMETRY;
}

static int ff_render_structural_block(const fauxfat_view *v,
                                      uint64_t block_address,
                                      uint8_t out[512])
{
    ff_block_mapping m;
    int rc = ff_classify_block(v, block_address, &m);

    if (rc != FAUXFAT_OK)
        return rc;
    return ff_render_classified_block(v, &m, out);
}

static int ff_translate_data_block(const fauxfat_view *v,
                                   uint64_t block_address,
                                   fauxfat_write_mapping *mapping)
{
    ff_block_mapping m;
    int rc;

    rc = ff_classify_block(v, block_address, &m);
    if (rc != FAUXFAT_OK)
        return rc;
    if (m.kind != FF_RANGE_PUBLIC_PAYLOAD || m.length == 0u)
        return FAUXFAT_EUNMAPPED;

    mapping->file_index  = m.object_index;
    mapping->fd          = v->config->files[m.object_index].fd;
    mapping->file_offset = m.object_offset;
    mapping->length      = m.length;
    return FAUXFAT_OK;
}

static int ff_apply_root_block_write(fauxfat_view *v,
                                     uint64_t block_address,
                                     const uint8_t in[FAUXFAT_BLOCK_SIZE])
{
    ff_block_mapping mapping;
    fauxfat_view candidate;
    uint8_t current[FAUXFAT_BLOCK_SIZE];
    uint8_t seen_primary[FAUXFAT_MAX_FILES] = {0};
    uint8_t changed_primary[FAUXFAT_MAX_FILES] = {0};
    uint8_t changed_stream[FAUXFAT_MAX_FILES] = {0};
    uint16_t received_checksum[FAUXFAT_MAX_FILES] = {0};
    uint8_t received_primary_fields[FAUXFAT_MAX_FILES][13] = {{0}};
    uint64_t received_valid_length[FAUXFAT_MAX_FILES] = {0};
    uint64_t received_data_length[FAUXFAT_MAX_FILES] = {0};
    uint32_t first_public = FF_FILE_SLOT_FIRST;
    uint32_t end_public = first_public +
        (uint32_t)(v->config->file_count * FF_PUBLIC_ENTRY_COUNT);
    uint32_t block_index;
    unsigned slot;
    size_t i;
    int rc;

    rc = ff_classify_block(v, block_address, &mapping);
    if (rc != FAUXFAT_OK)
        return rc;
    if (mapping.kind != FF_RANGE_GENERATED ||
        mapping.generated_kind != FF_GENERATED_ROOT)
        return FAUXFAT_EUNMAPPED;

    block_index = mapping.generated_index;
    ff_make_root_block(v, block_index, current);
    if (memcmp(current, in, FAUXFAT_BLOCK_SIZE) == 0)
        return FAUXFAT_OK;

    for (slot = 0u; slot < 16u; ++slot) {
        uint32_t entry = block_index * 16u + slot;
        const uint8_t *old_entry = current + slot * 32u;
        const uint8_t *new_entry = in + slot * 32u;
        size_t file_index;
        unsigned member;

        if (entry < first_public || entry >= end_public) {
            if (memcmp(old_entry, new_entry, 32u) != 0)
                return FAUXFAT_EUNMAPPED;
            continue;
        }

        file_index = (entry - first_public) / FF_PUBLIC_ENTRY_COUNT;
        member = (entry - first_public) % FF_PUBLIC_ENTRY_COUNT;

        if (member == 0u) {
            uint8_t allowed[32];
            uint16_t old_attributes = faux_load_le16(old_entry + 4u);
            uint16_t new_attributes = faux_load_le16(new_entry + 4u);

            memcpy(allowed, old_entry, sizeof(allowed));
            allowed[2] = new_entry[2];
            allowed[3] = new_entry[3];
            allowed[4] = new_entry[4];
            allowed[5] = new_entry[5];
            memcpy(allowed + 12u, new_entry + 12u, 8u);
            allowed[21] = new_entry[21];
            allowed[23] = new_entry[23];
            allowed[24] = new_entry[24];
            if ((old_attributes & (uint16_t)~FF_ATTR_ARCHIVE) !=
                    (new_attributes & (uint16_t)~FF_ATTR_ARCHIVE) ||
                memcmp(allowed, new_entry, sizeof(allowed)) != 0)
                return FAUXFAT_EUNMAPPED;
            if (!ff_public_file_entry_valid(new_entry, NULL))
                return FAUXFAT_ESTRUCTURE;

            seen_primary[file_index] = 1u;
            received_checksum[file_index] = faux_load_le16(new_entry + 2u);
            faux_store_le16(received_primary_fields[file_index],
                            new_attributes);
            memcpy(received_primary_fields[file_index] + 2u,
                   new_entry + 12u, 8u);
            received_primary_fields[file_index][10] = new_entry[21];
            received_primary_fields[file_index][11] = new_entry[23];
            received_primary_fields[file_index][12] = new_entry[24];
            changed_primary[file_index] =
                received_checksum[file_index] != faux_load_le16(old_entry + 2u) ||
                memcmp(received_primary_fields[file_index],
                       v->file_primary_fields[file_index], 13u) != 0;
        } else if (member == 1u) {
            if (v->config->files[file_index].allocation_size == 0u) {
                if (memcmp(old_entry, new_entry, 32u) != 0)
                    return FAUXFAT_EUNMAPPED;
                continue;
            }
            if (memcmp(old_entry, new_entry, 8u) != 0 ||
                memcmp(old_entry + 16u, new_entry + 16u, 8u) != 0)
                return FAUXFAT_EUNMAPPED;
            received_valid_length[file_index] = faux_load_le64(new_entry + 8u);
            received_data_length[file_index] = faux_load_le64(new_entry + 24u);
            changed_stream[file_index] =
                memcmp(old_entry + 8u, new_entry + 8u, 8u) != 0 ||
                memcmp(old_entry + 24u, new_entry + 24u, 8u) != 0;
        } else if (memcmp(old_entry, new_entry, 32u) != 0) {
            return FAUXFAT_EUNMAPPED;
        }
    }

    candidate = *v;
    for (i = 0u; i < v->config->file_count; ++i) {
        uint8_t set[160];
        uint16_t expected_checksum;

        if (!changed_primary[i] && !changed_stream[i])
            continue;

        if (changed_primary[i])
            memcpy(candidate.file_primary_fields[i],
                   received_primary_fields[i], 13u);

        if (changed_stream[i]) {
            uint64_t data_length = received_data_length[i];
            uint64_t valid_length = received_valid_length[i];
            if (data_length == 0u || valid_length > data_length ||
                data_length > v->config->files[i].allocation_size)
                return FAUXFAT_ESTRUCTURE;
            candidate.file_data_length[i] = data_length;
            candidate.file_valid_data_length[i] = valid_length;
        }

        ff_make_public_file_set_at(&candidate, i,
                                   candidate.file_valid_data_length[i],
                                   candidate.file_data_length[i], set);
        expected_checksum = faux_load_le16(set + 2u);

        if (changed_stream[i]) {
            if ((seen_primary[i] &&
                 received_checksum[i] != expected_checksum) ||
                (!seen_primary[i] &&
                 candidate.pending_file_set_checksum_valid[i] &&
                 candidate.pending_file_set_checksum[i] != expected_checksum))
                return FAUXFAT_ESTRUCTURE;
            candidate.pending_file_set_checksum_valid[i] = 0u;
        } else if (received_checksum[i] == expected_checksum) {
            candidate.pending_file_set_checksum_valid[i] = 0u;
        } else if (v->config->files[i].allocation_size == 0u) {
            return FAUXFAT_ESTRUCTURE;
        } else {
            candidate.pending_file_set_checksum[i] = received_checksum[i];
            candidate.pending_file_set_checksum_valid[i] = 1u;
        }
    }

    memcpy(v->file_data_length, candidate.file_data_length,
           sizeof(v->file_data_length));
    memcpy(v->file_valid_data_length, candidate.file_valid_data_length,
           sizeof(v->file_valid_data_length));
    memcpy(v->file_primary_fields, candidate.file_primary_fields,
           sizeof(v->file_primary_fields));
    memcpy(v->pending_file_set_checksum,
           candidate.pending_file_set_checksum,
           sizeof(v->pending_file_set_checksum));
    memcpy(v->pending_file_set_checksum_valid,
           candidate.pending_file_set_checksum_valid,
           sizeof(v->pending_file_set_checksum_valid));
    return FAUXFAT_OK;
}

static int ff_apply_boot_block_write(fauxfat_view *v,
                                     uint64_t block_address,
                                     const uint8_t in[FAUXFAT_BLOCK_SIZE])
{
    ff_block_mapping mapping;
    uint8_t current[FAUXFAT_BLOCK_SIZE];
    uint16_t flags;
    unsigned i;
    int rc;

    rc = ff_classify_block(v, block_address, &mapping);
    if (rc != FAUXFAT_OK)
        return rc;
    if (mapping.kind != FF_RANGE_GENERATED ||
        mapping.generated_kind != FF_GENERATED_BOOT)
        return FAUXFAT_EUNMAPPED;

    ff_make_boot_sector(v, current, block_address == 12u);
    if (memcmp(current, in, 106u) != 0 ||
        memcmp(current + 108u, in + 108u, FAUXFAT_BLOCK_SIZE - 108u) != 0)
        return FAUXFAT_EUNMAPPED;

    flags = faux_load_le16(in + 106u);
    if ((flags & (uint16_t)~0x0002u) != 0u)
        return FAUXFAT_ESTRUCTURE;
    for (i = 0u; i < 2u; ++i) {
        if (current[106u + i] != in[106u + i]) {
            if (block_address == 12u)
                v->backup_volume_flags = flags;
            else
                v->volume_flags = flags;
            return FAUXFAT_OK;
        }
    }
    return FAUXFAT_OK;
}

static int ff_preflight_block_write(fauxfat_view *v,
                                    uint64_t block_address,
                                    const uint8_t in[FAUXFAT_BLOCK_SIZE],
                                    fauxfat_write_mapping *payload_mapping,
                                    int *is_metadata)
{
    ff_block_mapping mapping;
    int rc;

    rc = ff_classify_block(v, block_address, &mapping);
    if (rc != FAUXFAT_OK)
        return rc;
    if (mapping.kind == FF_RANGE_PUBLIC_PAYLOAD) {
        *is_metadata = 0;
        return ff_translate_data_block(v, block_address, payload_mapping);
    }
    if (mapping.kind == FF_RANGE_GENERATED &&
        mapping.generated_kind == FF_GENERATED_ROOT) {
        *is_metadata = 1;
        return ff_apply_root_block_write(v, block_address, in);
    }
    if (mapping.kind == FF_RANGE_GENERATED &&
        mapping.generated_kind == FF_GENERATED_BOOT) {
        *is_metadata = 1;
        return ff_apply_boot_block_write(v, block_address, in);
    }
    if (mapping.kind == FF_RANGE_GENERATED ||
        mapping.kind == FF_RANGE_REQUIRED_ZERO) {
        uint8_t current[FAUXFAT_BLOCK_SIZE];
        rc = ff_render_classified_block(v, &mapping, current);
        if (rc != FAUXFAT_OK)
            return rc;
        if (memcmp(current, in, FAUXFAT_BLOCK_SIZE) == 0) {
            *is_metadata = 1;
            return FAUXFAT_OK;
        }
    }
    return FAUXFAT_EUNMAPPED;
}

static void ff_root_hash_normalize_public_metadata(
    const fauxfat_view *v,
    uint32_t block_index,
    uint8_t block[FAUXFAT_BLOCK_SIZE])
{
    size_t i;

    for (i = 0u; i < v->config->file_count; ++i) {
        uint64_t file_entry = FF_FILE_SLOT_FIRST +
                              (uint64_t)i * FF_PUBLIC_ENTRY_COUNT;
        uint64_t stream_entry = file_entry + 1u;
        uint8_t set[160];

        ff_make_public_file_set_at(v, i,
                                   ff_file_valid_data_length(v, i),
                                   ff_file_data_length(v, i), set);
        faux_store_le16(set + 4u, 0u);
        ff_store_file_times(set, v->config->files[i].mtime);

        if (ff_file_uses_fat_chain(&v->config->files[i])) {
            faux_store_le64(set + 40u, 0u);
            faux_store_le64(set + 56u, 0u);
        }
        faux_store_le16(set + 2u, ff_entry_set_checksum(set, sizeof(set)));

        if (file_entry / 16u == block_index)
            memcpy(block + (file_entry % 16u) * 32u, set, 32u);
        if (stream_entry / 16u == block_index) {
            if (ff_file_uses_fat_chain(&v->config->files[i])) {
                size_t offset = (size_t)(stream_entry % 16u) * 32u;
                memset(block + offset + 8u, 0, 8u);
                memset(block + offset + 24u, 0, 8u);
            }
        }
    }
}

static void ff_compute_component_hashes(fauxfat_view *v)
{
    uint8_t block[512];
    ff_structural_hash_state hashes;
    ff_structural_hash_values values;
    uint64_t bitmap_bytes         = ((uint64_t)v->cluster_count + 7u) / 8u;
    uint64_t fat_meaningful_bytes = ((uint64_t)v->cluster_count + 2u) * 4u;
    uint32_t fat_blocks           = (uint32_t)((fat_meaningful_bytes + 511u) / 512u);
    uint32_t i;

    (void)ff_render_structural_block(v, 0u, block);
    ff_structural_hash_init(&hashes, block);

    for (i = 0; i < fat_blocks; ++i) {
        uint64_t left = fat_meaningful_bytes - (uint64_t)i * 512u;
        size_t n      = left > 512u ? 512u : (size_t)left;
        (void)ff_render_structural_block(v, FF_FAT_OFFSET_BLOCKS + i, block);
        ff_structural_hash_update(&hashes.fat, &hashes.map, block, n);
    }

    for (i = 0; (uint64_t)i * 512u < bitmap_bytes; ++i) {
        uint64_t left = bitmap_bytes - (uint64_t)i * 512u;
        size_t n      = left > 512u ? 512u : (size_t)left;
        (void)ff_render_structural_block(v,
                                         (uint64_t)v->cluster_heap_block + i, block);
        ff_structural_hash_update(&hashes.bitmap, &hashes.map, block, n);
    }

    {
        uint64_t upcase_block = (uint64_t)v->cluster_heap_block +
                                (uint64_t)(v->upcase_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
        (void)ff_render_structural_block(v, upcase_block, block);
        ff_structural_hash_update(&hashes.upcase, &hashes.map, block,
                                  FF_UPCASE_BYTES);
    }

    for (i = 0; i < FAUXFAT_BLOCKS_PER_CLUSTER; ++i) {
        uint64_t root_block = (uint64_t)v->cluster_heap_block +
                              (uint64_t)(v->root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER + i;
        (void)ff_render_structural_block(v, root_block, block);
        ff_root_hash_normalize_public_metadata(v, i, block);
        ff_structural_hash_update(&hashes.root, &hashes.map, block,
                                  FAUXFAT_BLOCK_SIZE);
    }

    ff_structural_hash_digest(&hashes, &values);
    v->fat_xxh32    = values.fat;
    v->bitmap_xxh32 = values.bitmap;
    v->root_xxh32   = values.root;
    v->upcase_xxh32 = values.upcase;
    v->map_xxh32    = values.map;
}

static uint32_t ff_compute_boot_checksum(const fauxfat_view *v)
{
    uint8_t block[512];
    uint32_t sum = 0;
    uint32_t sector;
    unsigned byte;

    for (sector = 0; sector <= 10; ++sector) {
        (void)ff_render_structural_block(v, sector, block);

        for (byte = 0; byte < 512; ++byte) {
            if (sector == 0 && (byte == 106u || byte == 107u || byte == 112u))
                continue;
            sum = ff_ror32(sum) + block[byte];
        }
    }
    return sum;
}

int fauxfat_init(fauxfat_view *v, const fauxfat_config *cfg)
{
    uint64_t cursor = 0u;
    uint64_t data_clusters;
    uint32_t bitmap_clusters = 1u;
    uint64_t clusters64;
    uint64_t fat_bytes;
    size_t i, j;
    size_t dummy;
    uint64_t root_entries;

    if (!v || !cfg ||
        (cfg->file_count && !cfg->files) ||
        (cfg->opaque_file_count && !cfg->opaque_files) ||
        ((cfg->file_count || cfg->opaque_file_count) && !cfg->read) ||
        (cfg->file_count && !cfg->write) ||
        cfg->file_count > FAUXFAT_MAX_FILES ||
        cfg->opaque_file_count > FAUXFAT_MAX_OPAQUE_FILES ||
        (cfg->private_reservation != FAUXFAT_PRIVATE_BAD_CLUSTERS &&
         cfg->private_reservation !=
             FAUXFAT_PRIVATE_BENIGN_PRIMARY_AND_BAD) ||
        faux_guid_is_zero(cfg->volume_guid) ||
        !ff_label_valid(cfg->volume_label, &dummy))
        return FAUXFAT_EINVAL;

    root_entries = FF_FILE_SLOT_FIRST +
                   (uint64_t)cfg->file_count * FF_PUBLIC_ENTRY_COUNT +
                   (uint64_t)cfg->opaque_file_count * FF_OPAQUE_ENTRY_COUNT;
    if (root_entries > FF_ROOT_ENTRIES)
        return FAUXFAT_EGEOMETRY;

    memset(v, 0, sizeof(*v));
    v->config = cfg;

    for (i = 0; i < cfg->file_count; ++i) {
        const fauxfat_file *f = &cfg->files[i];
        uint8_t initial_entry[32] = {0};
        uint32_t ignored_timestamp;
        uint8_t ignored_ten_ms;
        uint64_t nclusters;
        uint64_t first;

        if (!ff_name_valid(f->name, &dummy) || f->size == 0u ||
            (f->allocation_size != 0u &&
             (f->allocation_size < f->size ||
              (f->allocation_size % FAUXFAT_CLUSTER_SIZE) != 0u)) ||
            !ff_exfat_timestamp(f->mtime, &ignored_timestamp, &ignored_ten_ms))
            return FAUXFAT_EINVAL;
        v->file_data_length[i] = f->size;
        v->file_valid_data_length[i] = f->size;
        ff_store_file_times(initial_entry, f->mtime);
        faux_store_le16(v->file_primary_fields[i], 0u);
        memcpy(v->file_primary_fields[i] + 2u, initial_entry + 12u, 8u);
        v->file_primary_fields[i][10] = initial_entry[21];
        v->file_primary_fields[i][11] = initial_entry[23];
        v->file_primary_fields[i][12] = initial_entry[24];
        for (j = 0; j < i; ++j) {
            if (ff_name_equal_folded(f->name, cfg->files[j].name))
                return FAUXFAT_EINVAL;
        }

        nclusters = ff_file_clusters(f);
        first     = f->data_cluster == FAUXFAT_CLUSTER_AUTO ? cursor : f->data_cluster;
        if (first < cursor || nclusters > FF_MAX_CLUSTER_COUNT ||
            first > FF_MAX_CLUSTER_COUNT ||
            nclusters > FF_MAX_CLUSTER_COUNT - first)
            return FAUXFAT_EGEOMETRY;
        cursor = first + nclusters;
    }

    for (i = 0u; i < cfg->file_count; ++i) {
        if (cfg->files[i].allocation_size != 0u) {
            v->mutable_file_lengths = 1u;
            break;
        }
    }

    for (i = 0; i < cfg->opaque_file_count; ++i) {
        const fauxfat_opaque_file *f = &cfg->opaque_files[i];
        uint32_t ignored_timestamp;
        uint8_t ignored_ten_ms;
        uint64_t nclusters;
        uint64_t first;
        char stub[FAUXFAT_NAME_MAX + 1u];

        if (!ff_name_valid(f->name, &dummy) || f->size == 0u ||
            (f->size % FAUXFAT_CLUSTER_SIZE) != 0u ||
            !ff_exfat_timestamp(f->mtime, &ignored_timestamp, &ignored_ten_ms))
            return FAUXFAT_EINVAL;

        nclusters = ff_opaque_clusters(f);
        first     = f->data_cluster == FAUXFAT_CLUSTER_AUTO ? cursor : f->data_cluster;
        if (first < cursor || nclusters > FF_MAX_CLUSTER_COUNT ||
            first > FF_MAX_CLUSTER_COUNT ||
            nclusters > FF_MAX_CLUSTER_COUNT - first)
            return FAUXFAT_EGEOMETRY;
        cursor = first + nclusters;

        /* The hidden descriptor's real namespace name must not collide. */
        ff_opaque_stub_name(i, stub);
        for (j = 0; j < cfg->file_count; ++j) {
            if (ff_name_equal_folded(stub, cfg->files[j].name))
                return FAUXFAT_EINVAL;
        }
    }

    data_clusters = cfg->data_cluster_count ? cfg->data_cluster_count : cursor;
    if (data_clusters < cursor || data_clusters > FF_MAX_CLUSTER_COUNT)
        return FAUXFAT_EGEOMETRY;

    for (;;) {
        uint64_t bitmap_bytes;
        uint64_t need;
        if (data_clusters > UINT64_MAX - bitmap_clusters - 2u)
            return FAUXFAT_EGEOMETRY;
        clusters64 = data_clusters + bitmap_clusters + 2u;
        if (clusters64 > FF_MAX_CLUSTER_COUNT)
            return FAUXFAT_EGEOMETRY;
        bitmap_bytes = (clusters64 + 7u) / 8u;
        need         = (bitmap_bytes + FAUXFAT_CLUSTER_SIZE - 1u) / FAUXFAT_CLUSTER_SIZE;
        if (need == bitmap_clusters)
            break;
        if (need == 0u || need > UINT32_MAX)
            return FAUXFAT_EGEOMETRY;
        bitmap_clusters = (uint32_t)need;
    }

    v->bitmap_clusters    = bitmap_clusters;
    v->upcase_cluster     = 2u + bitmap_clusters;
    v->root_cluster       = v->upcase_cluster + 1u;
    v->data_first_cluster = v->root_cluster + 1u;
    v->cluster_count      = (uint32_t)clusters64;

    if (cfg->private_reservation == FAUXFAT_PRIVATE_BENIGN_PRIMARY_AND_BAD) {
        root_entries += ff_anonymous_private_run_count(v);
        if (root_entries > FF_ROOT_ENTRIES ||
            ff_anonymous_private_run_count(v) > UINT16_MAX)
            return FAUXFAT_EGEOMETRY;
    }

    fat_bytes = ((uint64_t)v->cluster_count + 2u) * 4u;
    if (fat_bytes > UINT32_MAX * (uint64_t)FAUXFAT_BLOCK_SIZE)
        return FAUXFAT_EGEOMETRY;
    v->fat_length_blocks  = ff_align_up_u32((uint32_t)((fat_bytes + 511u) / 512u),
                                            FAUXFAT_BLOCKS_PER_CLUSTER);
    v->cluster_heap_block = FF_FAT_OFFSET_BLOCKS + v->fat_length_blocks;
    v->volume_blocks      = (uint64_t)v->cluster_heap_block +
                       (uint64_t)v->cluster_count * FAUXFAT_BLOCKS_PER_CLUSTER;

    v->upcase_checksum = ff_upcase_checksum();
    ff_compute_component_hashes(v);
    v->boot_checksum = ff_compute_boot_checksum(v);
    return FAUXFAT_OK;
}

uint64_t fauxfat_block_count(const fauxfat_view *v)
{
    return v ? v->volume_blocks : 0u;
}

size_t fauxfat_disk_file_count(const fauxfat_view *v)
{
    if (!v || !v->config)
        return 0u;
    return v->config->file_count + v->config->opaque_file_count;
}

int fauxfat_describe_disk_file(const fauxfat_view *v,
                               size_t index,
                               fauxfat_disk_file *out)
{
    uint32_t first_cluster;
    uint64_t clusters;
    const char *name;
    time_t mtime;
    size_t len;

    if (!v || !v->config || !out)
        return FAUXFAT_EINVAL;
    if (index >= fauxfat_disk_file_count(v))
        return FAUXFAT_ERANGE;

    memset(out, 0, sizeof(*out));
    if (index < v->config->file_count) {
        const fauxfat_file *f = &v->config->files[index];
        first_cluster         = ff_file_first_cluster(v, index);
        clusters              = ff_file_clusters(f);
        name                  = f->name;
        mtime                 = f->mtime;
        (void)ff_exfat_timestamp_decode(
            faux_load_le32(v->file_primary_fields[index] + 2u),
            v->file_primary_fields[index][10], 0, &mtime);
        out->kind             = FAUXFAT_DISK_FILE_PUBLIC;
        out->data_length      = ff_file_data_length(v, index);
    } else {
        size_t oi                    = index - v->config->file_count;
        const fauxfat_opaque_file *f = &v->config->opaque_files[oi];
        first_cluster                = ff_opaque_first_cluster(v, oi);
        clusters                     = ff_opaque_clusters(f);
        name                         = f->name;
        mtime                        = f->mtime;
        out->kind                    = FAUXFAT_DISK_FILE_OPAQUE;
        out->data_length             = f->size;
    }

    len = strlen(name);
    memcpy(out->name, name, len);
    out->name[len]   = '\0';
    out->first_block = (uint64_t)v->cluster_heap_block +
                       (uint64_t)(first_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    out->allocation_blocks = clusters * FAUXFAT_BLOCKS_PER_CLUSTER;
    out->mtime             = mtime;
    return FAUXFAT_OK;
}

int fauxfat_get_file_lengths(const fauxfat_view *v,
                             size_t file_index,
                             uint64_t *data_length,
                             uint64_t *valid_data_length)
{
    if (!v || !v->config || !data_length || !valid_data_length)
        return FAUXFAT_EINVAL;
    if (file_index >= v->config->file_count)
        return FAUXFAT_ERANGE;
    *data_length       = ff_file_data_length(v, file_index);
    *valid_data_length = ff_file_valid_data_length(v, file_index);
    return FAUXFAT_OK;
}

int fauxfat_read_block(const fauxfat_view *v,
                       uint64_t block_address,
                       uint8_t out[FAUXFAT_BLOCK_SIZE])
{
    ff_block_mapping m;
    int rc;

    if (!v || !v->config || !out)
        return FAUXFAT_EINVAL;
    rc = ff_classify_block(v, block_address, &m);
    if (rc != FAUXFAT_OK)
        return rc;

    if (m.kind == FF_RANGE_GENERATED ||
        m.kind == FF_RANGE_REQUIRED_ZERO ||
        m.kind == FF_RANGE_RESERVED_PRESERVE ||
        m.kind == FF_RANGE_UNDEFINED)
        return ff_render_classified_block(v, &m, out);

    if (m.kind == FF_RANGE_PUBLIC_PAYLOAD) {
        const fauxfat_file *f = &v->config->files[m.object_index];
        uint64_t valid_length = ff_file_valid_data_length(v, m.object_index);
        size_t read_length = 0u;

        memset(out, 0, FAUXFAT_BLOCK_SIZE);
        if (m.object_offset < valid_length) {
            uint64_t left = valid_length - m.object_offset;
            read_length = left < m.length ? (size_t)left : m.length;
        }
        if (read_length == 0u)
            return FAUXFAT_OK;
        rc = v->config->read(v->config->io_context, f->fd,
                             m.object_offset, out, read_length);
        return rc == 0 ? FAUXFAT_OK : rc;
    }

    if (m.kind == FF_RANGE_OPAQUE_PRESERVE) {
        const fauxfat_opaque_file *f = &v->config->opaque_files[m.object_index];
        rc                           = v->config->read(v->config->io_context, f->fd,
                                                       m.object_offset, out, FAUXFAT_BLOCK_SIZE);
        return rc == 0 ? FAUXFAT_OK : rc;
    }

    return FAUXFAT_EGEOMETRY;
}

int fauxfat_read_blocks(const fauxfat_view *v,
                        uint64_t first_block,
                        size_t block_count,
                        uint8_t *out)
{
    size_t i;

    if (!out && block_count)
        return FAUXFAT_EINVAL;
    if (!v)
        return FAUXFAT_EINVAL;
    if (block_count > SIZE_MAX / FAUXFAT_BLOCK_SIZE)
        return FAUXFAT_ERANGE;
    if ((uint64_t)block_count > UINT64_MAX - first_block ||
        first_block + (uint64_t)block_count > v->volume_blocks)
        return FAUXFAT_ERANGE;

    for (i = 0; i < block_count;) {
        ff_block_mapping m;
        int rc = ff_classify_block(v, first_block + i, &m);

        if (rc != FAUXFAT_OK)
            return rc;

        if (m.kind == FF_RANGE_PUBLIC_PAYLOAD) {
            const fauxfat_file *f = &v->config->files[m.object_index];
            uint64_t requested    = (uint64_t)(block_count - i) * FAUXFAT_BLOCK_SIZE;
            uint64_t available    = ff_file_write_limit(v, m.object_index) -
                                    m.object_offset;
            size_t length         = (size_t)(requested < available ? requested : available);
            size_t blocks         = (length + FAUXFAT_BLOCK_SIZE - 1u) / FAUXFAT_BLOCK_SIZE;
            size_t rendered       = blocks * FAUXFAT_BLOCK_SIZE;
            uint64_t valid_length = ff_file_valid_data_length(v, m.object_index);
            size_t read_length    = 0u;

            memset(out + i * FAUXFAT_BLOCK_SIZE, 0, rendered);
            if (m.object_offset < valid_length) {
                uint64_t valid_available = valid_length - m.object_offset;
                read_length = valid_available < length
                                  ? (size_t)valid_available
                                  : length;
            }
            if (read_length != 0u) {
                rc = v->config->read(v->config->io_context, f->fd,
                                     m.object_offset,
                                     out + i * FAUXFAT_BLOCK_SIZE,
                                     read_length);
                if (rc != 0)
                    return rc;
            }
            i += blocks;
            continue;
        }

        if (m.kind == FF_RANGE_OPAQUE_PRESERVE) {
            const fauxfat_opaque_file *f = &v->config->opaque_files[m.object_index];
            uint64_t requested           = (uint64_t)(block_count - i) * FAUXFAT_BLOCK_SIZE;
            uint64_t available           = f->size - m.object_offset;
            size_t length                = (size_t)(requested < available ? requested : available);
            size_t blocks                = length / FAUXFAT_BLOCK_SIZE;

            rc = v->config->read(v->config->io_context, f->fd,
                                 m.object_offset,
                                 out + i * FAUXFAT_BLOCK_SIZE,
                                 length);
            if (rc != 0)
                return rc;
            i += blocks;
            continue;
        }

        if (m.kind == FF_RANGE_RESERVED_PRESERVE) {
            size_t blocks = 1u;
            while (blocks < block_count - i) {
                ff_block_mapping next;
                rc = ff_classify_block(v, first_block + i + blocks, &next);
                if (rc != FAUXFAT_OK)
                    return rc;
                if (next.kind != FF_RANGE_RESERVED_PRESERVE)
                    break;
                ++blocks;
            }
            memset(out + i * FAUXFAT_BLOCK_SIZE, 0,
                   blocks * FAUXFAT_BLOCK_SIZE);
            i += blocks;
            continue;
        }

        rc = ff_render_classified_block(v, &m,
                                        out + i * FAUXFAT_BLOCK_SIZE);
        if (rc != FAUXFAT_OK)
            return rc;
        ++i;
    }
    return FAUXFAT_OK;
}

int fauxfat_translate_write(const fauxfat_view *v,
                            uint64_t block_address,
                            fauxfat_write_mapping *mapping)
{
    if (!v || !v->config || !mapping)
        return FAUXFAT_EINVAL;
    return ff_translate_data_block(v, block_address, mapping);
}

int fauxfat_write_block(fauxfat_view *v,
                        uint64_t block_address,
                        const uint8_t in[FAUXFAT_BLOCK_SIZE])
{
    fauxfat_write_mapping mapping;
    int is_metadata;
    int rc;

    if (!v || !v->config || !in)
        return FAUXFAT_EINVAL;

    rc = ff_preflight_block_write(v, block_address, in, &mapping,
                                  &is_metadata);
    if (rc != FAUXFAT_OK)
        return rc;
    if (is_metadata)
        return FAUXFAT_OK;

    return v->config->write(v->config->io_context, mapping.fd,
                            mapping.file_offset, in, mapping.length);
}

int fauxfat_write_blocks(fauxfat_view *v,
                         uint64_t first_block,
                         size_t block_count,
                         const uint8_t *in)
{
    fauxfat_view preflight_view;
    size_t i;

    if (!v || (!in && block_count))
        return FAUXFAT_EINVAL;
    if (block_count > SIZE_MAX / FAUXFAT_BLOCK_SIZE)
        return FAUXFAT_ERANGE;
    if ((uint64_t)block_count > UINT64_MAX - first_block ||
        first_block + (uint64_t)block_count > v->volume_blocks)
        return FAUXFAT_ERANGE;

    /* Length-sector writes can expose reserved payload blocks later in a request. */
    preflight_view = *v;
    for (i = 0; i < block_count; ++i) {
        fauxfat_write_mapping mapping;
        int is_metadata;
        int rc = ff_preflight_block_write(
            &preflight_view, first_block + i,
            in + i * FAUXFAT_BLOCK_SIZE, &mapping, &is_metadata);
        if (rc != FAUXFAT_OK)
            return rc;
    }

    for (i = 0; i < block_count;) {
        ff_block_mapping block_mapping;
        fauxfat_write_mapping mapping;
        uint64_t requested;
        uint64_t available;
        size_t length;
        size_t blocks;
        int rc = ff_classify_block(v, first_block + i, &block_mapping);

        if (rc != FAUXFAT_OK)
            return rc;

        if (block_mapping.kind != FF_RANGE_PUBLIC_PAYLOAD) {
            int is_metadata;
            rc = ff_preflight_block_write(v, first_block + i,
                                          in + i * FAUXFAT_BLOCK_SIZE,
                                          &mapping, &is_metadata);
            if (rc != FAUXFAT_OK)
                return rc; /* validated above; this indicates internal drift */
            if (!is_metadata)
                return FAUXFAT_EGEOMETRY;
            ++i;
            continue;
        }

        rc = fauxfat_translate_write(v, first_block + i, &mapping);
        if (rc != FAUXFAT_OK)
            return rc;

        blocks = 1u;
        while (blocks < block_count - i) {
            fauxfat_write_mapping next;
            rc = fauxfat_translate_write(v, first_block + i + blocks,
                                         &next);
            if (rc != FAUXFAT_OK || next.file_index != mapping.file_index ||
                next.file_offset != mapping.file_offset +
                                        (uint64_t)blocks * FAUXFAT_BLOCK_SIZE)
                break;
            ++blocks;
        }
        requested = (uint64_t)blocks * FAUXFAT_BLOCK_SIZE;
        available = ff_file_write_limit(v, mapping.file_index) -
                    mapping.file_offset;
        length = (size_t)(requested < available ? requested : available);

        rc = v->config->write(v->config->io_context, mapping.fd,
                              mapping.file_offset,
                              in + i * FAUXFAT_BLOCK_SIZE,
                              length);
        if (rc != 0)
            return rc;
        i += blocks;
    }

    return FAUXFAT_OK;
}

static int ff_device_skip(const fauxfat_device *device,
                          uint64_t first_block,
                          uint64_t block_count,
                          fauxfat_skip_kind kind)
{
    if (block_count == 0u || !device->skip)
        return FAUXFAT_OK;
    return device->skip(device->context, first_block, block_count, kind);
}

int fauxfat_format(const fauxfat_view *v,
                   const fauxfat_device *device,
                   fauxfat_preserve_fn preserve,
                   void *preserve_context,
                   unsigned flags)
{
    uint8_t block_data[FAUXFAT_BLOCK_SIZE];
    uint64_t block = 0u;

    if (!v || !v->config || !device || !device->write || !device->zero)
        return FAUXFAT_EINVAL;
    if (flags & ~FAUXFAT_FORMAT_ZERO_UNDEFINED)
        return FAUXFAT_EINVAL;

    while (block < v->volume_blocks) {
        ff_block_mapping m;
        int rc = ff_classify_block(v, block, &m);

        if (rc != FAUXFAT_OK)
            return rc;

        if (m.kind == FF_RANGE_GENERATED) {
            rc = ff_render_classified_block(v, &m, block_data);
            if (rc != FAUXFAT_OK)
                return rc;
            rc = device->write(device->context, block, 1u, block_data);
            if (rc != 0)
                return rc;
            ++block;
            continue;
        }

        if (m.kind == FF_RANGE_REQUIRED_ZERO) {
            rc = device->zero(device->context, block, 1u);
            if (rc != 0)
                return rc;
            ++block;
            continue;
        }

        if (m.kind == FF_RANGE_PUBLIC_PAYLOAD) {
            fauxfat_disk_file d;
            uint64_t data_blocks;
            uint64_t slack_blocks;
            int keep = 0;

            if (m.object_offset != 0u)
                return FAUXFAT_EGEOMETRY;
            rc = fauxfat_describe_disk_file(v, m.object_index, &d);
            if (rc != FAUXFAT_OK)
                return rc;
            if (d.first_block != block || d.kind != FAUXFAT_DISK_FILE_PUBLIC)
                return FAUXFAT_EGEOMETRY;

            if (preserve) {
                keep = preserve(preserve_context, &d);
                if (keep < 0)
                    return keep;
            }

            if (keep > 0) {
                rc = ff_device_skip(device, block, d.allocation_blocks,
                                    FAUXFAT_SKIP_PRESERVE);
                if (rc != FAUXFAT_OK)
                    return rc;
                block += d.allocation_blocks;
                continue;
            }

            if (v->config->files[m.object_index].allocation_size != 0u) {
                /* Reserved growth space must not expose stale cluster data. */
                rc = device->zero(device->context, block, d.allocation_blocks);
                if (rc != 0)
                    return rc;
                block += d.allocation_blocks;
                continue;
            }

            data_blocks = (d.data_length + FAUXFAT_BLOCK_SIZE - 1u) /
                          FAUXFAT_BLOCK_SIZE;
            if (data_blocks > d.allocation_blocks)
                return FAUXFAT_EGEOMETRY;
            rc = device->zero(device->context, block, data_blocks);
            if (rc != 0)
                return rc;
            block += data_blocks;

            slack_blocks = d.allocation_blocks - data_blocks;
            if (slack_blocks != 0u) {
                if (flags & FAUXFAT_FORMAT_ZERO_UNDEFINED)
                    rc = device->zero(device->context, block, slack_blocks);
                else
                    rc = ff_device_skip(device, block, slack_blocks,
                                        FAUXFAT_SKIP_UNDEFINED);
                if (rc != FAUXFAT_OK)
                    return rc;
                block += slack_blocks;
            }
            continue;
        }

        if (m.kind == FF_RANGE_OPAQUE_PRESERVE) {
            fauxfat_disk_file d;
            size_t disk_index = v->config->file_count + m.object_index;

            if (m.object_offset != 0u)
                return FAUXFAT_EGEOMETRY;
            rc = fauxfat_describe_disk_file(v, disk_index, &d);
            if (rc != FAUXFAT_OK)
                return rc;
            if (d.first_block != block || d.kind != FAUXFAT_DISK_FILE_OPAQUE)
                return FAUXFAT_EGEOMETRY;
            rc = ff_device_skip(device, block, d.allocation_blocks,
                                FAUXFAT_SKIP_PRESERVE);
            if (rc != FAUXFAT_OK)
                return rc;
            block += d.allocation_blocks;
            continue;
        }

        if (m.kind == FF_RANGE_RESERVED_PRESERVE) {
            uint64_t first = block;

            do {
                ++block;
                if (block >= v->volume_blocks)
                    break;
                rc = ff_classify_block(v, block, &m);
                if (rc != FAUXFAT_OK)
                    return rc;
            } while (m.kind == FF_RANGE_RESERVED_PRESERVE);

            rc = ff_device_skip(device, first, block - first,
                                FAUXFAT_SKIP_PRESERVE);
            if (rc != FAUXFAT_OK)
                return rc;
            continue;
        }

        if (m.kind == FF_RANGE_UNDEFINED) {
            uint64_t first = block;

            do {
                ++block;
                if (block >= v->volume_blocks)
                    break;
                rc = ff_classify_block(v, block, &m);
                if (rc != FAUXFAT_OK)
                    return rc;
            } while (m.kind == FF_RANGE_UNDEFINED);

            if (flags & FAUXFAT_FORMAT_ZERO_UNDEFINED)
                rc = device->zero(device->context, first, block - first);
            else
                rc = ff_device_skip(device, first, block - first,
                                    FAUXFAT_SKIP_UNDEFINED);
            if (rc != FAUXFAT_OK)
                return rc;
            continue;
        }

        return FAUXFAT_EGEOMETRY;
    }

    return FAUXFAT_OK;
}

typedef struct ff_root_reader {
    const fauxfat_device *device;
    uint64_t first_block;
    uint32_t cached_block;
    uint8_t cache[FAUXFAT_BLOCK_SIZE];
    int valid;
} ff_root_reader;

static int ff_all_zero(const uint8_t *p, size_t n)
{
    size_t i;
    uint8_t x = 0u;

    for (i = 0; i < n; ++i)
        x |= p[i];
    return x == 0u;
}

static int ff_root_entry(ff_root_reader *r, uint32_t entry, uint8_t out[32])
{
    uint32_t block = entry / 16u;
    uint32_t slot  = entry % 16u;
    int rc;

    if (entry >= FF_ROOT_ENTRIES)
        return FAUXFAT_ESTRUCTURE;
    if (!r->valid || r->cached_block != block) {
        rc = r->device->read(r->device->context, r->first_block + block,
                             1u, r->cache);
        if (rc != 0)
            return rc;
        r->cached_block = block;
        r->valid        = 1;
    }
    memcpy(out, r->cache + slot * 32u, 32u);
    return FAUXFAT_OK;
}

static int ff_root_set(ff_root_reader *r, uint32_t first,
                       unsigned count, uint8_t *out)
{
    unsigned i;
    int rc;

    if (count == 0u || first > FF_ROOT_ENTRIES - count)
        return FAUXFAT_ESTRUCTURE;
    for (i = 0; i < count; ++i) {
        rc = ff_root_entry(r, first + i, out + i * 32u);
        if (rc != FAUXFAT_OK)
            return rc;
    }
    return FAUXFAT_OK;
}

static int ff_public_file_entry_valid(const uint8_t e[32], time_t *mtime)
{
    uint16_t attrs;

    if (e[0] != FF_ENTRY_FILE || e[1] != 4u)
        return 0;
    attrs = faux_load_le16(e + 4);
    if ((attrs & (uint16_t)~FF_ATTR_ARCHIVE) != 0u)
        return 0;
    if (!ff_all_zero(e + 6, 2u) || !ff_all_zero(e + 25, 7u))
        return 0;

    /* Creation metadata is part of the fauxFAT structure and is never host-mutable. */
    if (e[22] != 0x80u ||
        !ff_exfat_timestamp_decode(faux_load_le32(e + 8), e[20], 1, mtime))
        return 0;

    /* Modify/access metadata may legitimately change during a host write cycle. */
    if (!ff_exfat_timestamp_decode(faux_load_le32(e + 12), e[21], 0, NULL) ||
        !ff_timestamp_fields(faux_load_le32(e + 16), NULL, NULL, NULL,
                             NULL, NULL, NULL) ||
        !ff_utc_offset_valid(e[23]) || !ff_utc_offset_valid(e[24]))
        return 0;
    return 1;
}

static int ff_opaque_file_entry_valid(const uint8_t e[32], time_t *mtime)
{
    uint16_t attrs;
    uint16_t fixed = FF_ATTR_READONLY | FF_ATTR_HIDDEN | FF_ATTR_SYSTEM;

    if (e[0] != FF_ENTRY_FILE || e[1] != 4u)
        return 0;
    attrs = faux_load_le16(e + 4);
    if ((attrs & (uint16_t)~FF_ATTR_ARCHIVE) != fixed)
        return 0;
    if (!ff_all_zero(e + 6, 2u) || !ff_all_zero(e + 25, 7u))
        return 0;
    if (e[22] != 0x80u ||
        !ff_exfat_timestamp_decode(faux_load_le32(e + 8), e[20], 1, mtime))
        return 0;
    if (!ff_exfat_timestamp_decode(faux_load_le32(e + 12), e[21], 0, NULL) ||
        !ff_timestamp_fields(faux_load_le32(e + 16), NULL, NULL, NULL,
                             NULL, NULL, NULL) ||
        !ff_utc_offset_valid(e[23]) || !ff_utc_offset_valid(e[24]))
        return 0;
    return 1;
}

static int ff_decode_name_entry(const uint8_t name_ent[32], uint8_t name_len,
                                char out[FAUXFAT_NAME_MAX + 1u])
{
    unsigned i;

    if (name_ent[0] != FF_ENTRY_NAME || name_ent[1] != 0u ||
        name_len == 0u || name_len > FAUXFAT_NAME_MAX)
        return 0;

    for (i = 0; i < FAUXFAT_NAME_MAX; ++i) {
        uint16_t ch = faux_load_le16(name_ent + 2u + 2u * i);
        if (i < name_len) {
            if (ch == 0u || ch > 0x00ffu)
                return 0;
            out[i] = (char)(uint8_t)ch;
        } else if (ch != 0u) {
            return 0;
        }
    }
    out[name_len] = '\0';
    return ff_name_valid(out, NULL);
}

static int ff_extent_to_descriptor(const fauxfat_view *v,
                                   uint32_t first_cluster,
                                   uint64_t data_length,
                                   int opaque,
                                   uint64_t allocation_clusters,
                                   fauxfat_disk_file *d,
                                   uint64_t *end_cluster)
{
    uint64_t clusters;
    uint64_t first = first_cluster;
    uint64_t limit = (uint64_t)v->cluster_count + 2u;

    if (data_length == 0u || first_cluster < v->data_first_cluster ||
        first >= limit)
        return 0;

    if (allocation_clusters != 0u) {
        clusters = allocation_clusters;
        if (!opaque && clusters <
                           ((data_length - 1u) / FAUXFAT_CLUSTER_SIZE) + 1u)
            return 0;
    } else if (opaque) {
        if ((data_length % FAUXFAT_CLUSTER_SIZE) != 0u)
            return 0;
        clusters = data_length / FAUXFAT_CLUSTER_SIZE;
    } else {
        clusters = ((data_length - 1u) / FAUXFAT_CLUSTER_SIZE) + 1u;
    }
    if (clusters == 0u || clusters > limit - first)
        return 0;

    d->first_block = (uint64_t)v->cluster_heap_block +
                     (first - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    d->data_length       = data_length;
    d->allocation_blocks = clusters * FAUXFAT_BLOCKS_PER_CLUSTER;
    *end_cluster         = first + clusters;
    return 1;
}

/*
 * Return the complete public allocation.  NoFatChain extents are sized from
 * DataLength by exFAT; FAT-chain extents can retain a larger reserved tail.
 */
static int ff_public_allocation_clusters(const fauxfat_view *v,
                                         const fauxfat_device *device,
                                         const uint8_t *stream,
                                         uint64_t data_length,
                                         uint64_t *clusters_out,
                                         int *error_out)
{
    uint32_t first = faux_load_le32(stream + 20u);
    uint64_t minimum;
    uint64_t count = 0u;
    uint32_t current = first;
    uint64_t cached_fat_block = UINT64_MAX;
    uint8_t fat_sector[FAUXFAT_BLOCK_SIZE];

    if (error_out)
        *error_out = FAUXFAT_OK;
    if (data_length == 0u ||
        (stream[1] != 0x01u && stream[1] != 0x03u))
        return 0;
    minimum = ((data_length - 1u) / FAUXFAT_CLUSTER_SIZE) + 1u;

    if (stream[1] == 0x03u) {
        *clusters_out = minimum;
        return 1;
    }

    for (;;) {
        uint32_t next;
        uint64_t byte_offset;
        uint64_t fat_block;
        unsigned offset;
        int rc;

        if (current < v->data_first_cluster ||
            current > v->cluster_count + 1u ||
            ++count > (uint64_t)v->cluster_count + 1u)
            return 0;
        byte_offset = (uint64_t)current * 4u;
        fat_block = FF_FAT_OFFSET_BLOCKS +
                    byte_offset / FAUXFAT_BLOCK_SIZE;
        offset = (unsigned)(byte_offset % FAUXFAT_BLOCK_SIZE);
        if (fat_block != cached_fat_block) {
            rc = ff_dev_read_block(device, fat_block, fat_sector);
            if (rc != FAUXFAT_OK) {
                if (error_out)
                    *error_out = rc;
                return 0;
            }
            cached_fat_block = fat_block;
        }
        next = faux_load_le32(fat_sector + offset);
        if (next == FF_FAT_EOC) {
            if (count < minimum)
                return 0;
            *clusters_out = count;
            return 1;
        }
        if (next != current + 1u)
            return 0;
        current = next;
    }
}

static int ff_decode_split_name(const uint8_t head[13],
                                uint8_t tail0, uint8_t tail1, uint8_t len,
                                char out[FAUXFAT_NAME_MAX + 1u])
{
    unsigned i;

    if (len == 0u || len > FAUXFAT_NAME_MAX)
        return 0;
    for (i = 0u; i < 13u; ++i) {
        if (i < len)
            out[i] = (char)head[i];
        else if (head[i] != 0u)
            return 0;
    }
    if (len > 13u)
        out[13] = (char)tail0;
    else if (tail0 != 0u)
        return 0;
    if (len > 14u)
        out[14] = (char)tail1;
    else if (tail1 != 0u)
        return 0;
    out[len] = '\0';
    return ff_name_valid(out, NULL);
}

static int ff_decode_public_origin(const uint8_t vendor0[32],
                                   const uint8_t vendor1[32],
                                   char out[FAUXFAT_NAME_MAX + 1u])
{
    uint8_t len;

    if (vendor0[0] != FF_ENTRY_VENDOR_EXT || vendor0[1] != 0u ||
        memcmp(vendor0 + 2u, ff_public_name_guid0, 16u) != 0 ||
        vendor1[0] != FF_ENTRY_VENDOR_EXT || vendor1[1] != 0u ||
        memcmp(vendor1 + 2u, ff_public_name_guid1, 16u) != 0)
        return 0;

    len = vendor0[18];
    if (vendor1[20] != len || !ff_all_zero(vendor1 + 21u, 11u))
        return 0;
    return ff_decode_split_name(vendor0 + 19u, vendor1[18], vendor1[19],
                                len, out);
}

static int ff_parse_public_set(const fauxfat_view *v,
                               const fauxfat_device *device,
                               const uint8_t set[160],
                               fauxfat_disk_file *d,
                               uint64_t *end_cluster,
                               int *error_out)
{
    const uint8_t *file_ent = set;
    const uint8_t *stream   = set + 32u;
    const uint8_t *name_ent = set + 64u;
    const uint8_t *vendor0  = set + 96u;
    const uint8_t *vendor1  = set + 128u;
    char visible_name[FAUXFAT_NAME_MAX + 1u];
    uint64_t data_length;
    uint64_t allocation_clusters;
    uint8_t name_len;
    int parse_error = FAUXFAT_OK;

    if (error_out)
        *error_out = FAUXFAT_OK;

    memset(d, 0, sizeof(*d));
    d->kind = FAUXFAT_DISK_FILE_PUBLIC;

    if (!ff_public_file_entry_valid(file_ent, &d->mtime) ||
        faux_load_le16(file_ent + 2) != ff_entry_set_checksum(set, 160u))
        return 0;

    if (stream[0] != FF_ENTRY_STREAM ||
        (stream[1] != 0x01u && stream[1] != 0x03u) ||
        stream[2] != 0u || !ff_all_zero(stream + 6, 2u) ||
        !ff_all_zero(stream + 16, 4u))
        return 0;
    name_len = stream[3];
    if (!ff_decode_name_entry(name_ent, name_len, visible_name) ||
        faux_load_le16(stream + 4) != ff_name_hash(visible_name) ||
        !ff_decode_public_origin(vendor0, vendor1, d->name))
        return 0;
    if (strcmp(visible_name, d->name) != 0)
        d->flags |= FAUXFAT_DISK_FILE_NAME_CHANGED;

    data_length = faux_load_le64(stream + 24);
    if (faux_load_le64(stream + 8) > data_length ||
        !ff_public_allocation_clusters(v, device, stream, data_length,
                                       &allocation_clusters, &parse_error) ||
        !ff_extent_to_descriptor(v, faux_load_le32(stream + 20), data_length,
                                 0, allocation_clusters, d, end_cluster)) {
        if (error_out && parse_error != FAUXFAT_OK)
            *error_out = parse_error;
        return 0;
    }
    return 1;
}

static int ff_opaque_stub_valid(const char *name)
{
    unsigned i;

    if (!name || strlen(name) != 11u || name[0] != '$' ||
        name[1] != 'F' || name[2] != 'F')
        return 0;
    for (i = 3u; i < 11u; ++i) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

static int ff_parse_opaque_set_common(const fauxfat_view *v,
                                      const uint8_t set[160],
                                      const char *expected_stub,
                                      fauxfat_disk_file *d,
                                      uint64_t *end_cluster)
{
    const uint8_t *file_ent     = set;
    const uint8_t *stream       = set + 32u;
    const uint8_t *name_ent     = set + 64u;
    const uint8_t *vendor_ext   = set + 96u;
    const uint8_t *vendor_alloc = set + 128u;
    char stub[FAUXFAT_NAME_MAX + 1u];
    uint8_t logical_len;

    memset(d, 0, sizeof(*d));
    d->kind = FAUXFAT_DISK_FILE_OPAQUE;

    if (!ff_opaque_file_entry_valid(file_ent, &d->mtime) ||
        faux_load_le16(file_ent + 2u) != ff_entry_set_checksum(set, 160u) ||
        stream[0] != FF_ENTRY_STREAM || stream[1] != 0x01u ||
        stream[2] != 0u || !ff_all_zero(stream + 6u, 14u) ||
        !ff_all_zero(stream + 20u, 12u) || stream[3] != 11u ||
        !ff_decode_name_entry(name_ent, stream[3], stub) ||
        faux_load_le16(stream + 4u) != ff_name_hash(stub))
        return 0;
    if (expected_stub ? strcmp(stub, expected_stub) != 0 : !ff_opaque_stub_valid(stub))
        return 0;

    if (vendor_ext[0] != FF_ENTRY_VENDOR_EXT || vendor_ext[1] != 0u ||
        memcmp(vendor_ext + 2u, ff_opaque_meta_guid, 16u) != 0 ||
        vendor_alloc[0] != FF_ENTRY_VENDOR_ALLOC || vendor_alloc[1] != 0x03u ||
        memcmp(vendor_alloc + 2u, ff_opaque_alloc_guid, 16u) != 0)
        return 0;
    logical_len = vendor_ext[18];
    if (!ff_decode_split_name(vendor_ext + 19u, vendor_alloc[18],
                              vendor_alloc[19], logical_len, d->name))
        return 0;

    return ff_extent_to_descriptor(v, faux_load_le32(vendor_alloc + 20u),
                                   faux_load_le64(vendor_alloc + 24u), 1,
                                   0u, d, end_cluster);
}

static int ff_parse_opaque_set(const fauxfat_view *v,
                               const uint8_t set[160],
                               unsigned opaque_ordinal,
                               fauxfat_disk_file *d,
                               uint64_t *end_cluster)
{
    char expected_stub[FAUXFAT_NAME_MAX + 1u];

    ff_opaque_stub_name(opaque_ordinal, expected_stub);
    return ff_parse_opaque_set_common(v, set, expected_stub, d, end_cluster);
}

static int ff_parse_opaque_set_loose(const fauxfat_view *v,
                                     const uint8_t set[160],
                                     fauxfat_disk_file *d,
                                     uint64_t *end_cluster)
{
    return ff_parse_opaque_set_common(v, set, NULL, d, end_cluster);
}

static int ff_private_reserve_entry_valid(const fauxfat_view *v,
                                          size_t ordinal,
                                          const uint8_t entry[32],
                                          uint16_t *run_count_out,
                                          uint64_t *end_cluster_out)
{
    uint16_t run_count;
    uint32_t first_cluster;
    uint64_t data_length;
    uint64_t clusters;
    uint64_t end_cluster;

    if (entry[0] != FF_ENTRY_PRIVATE_RESERVE || entry[1] != 0u ||
        faux_load_le16(entry + 2u) != ff_entry_set_checksum(entry, 32u) ||
        faux_load_le16(entry + 4u) != 0x0003u ||
        memcmp(entry + 6u, ff_private_reserve_magic,
               sizeof(ff_private_reserve_magic)) != 0 ||
        faux_load_le32(entry + 14u) != (uint32_t)ordinal)
        return 0;

    run_count     = faux_load_le16(entry + 18u);
    first_cluster = faux_load_le32(entry + 20u);
    data_length   = faux_load_le64(entry + 24u);
    if (run_count == 0u || ordinal >= run_count ||
        first_cluster < v->data_first_cluster ||
        data_length == 0u ||
        (data_length % FAUXFAT_CLUSTER_SIZE) != 0u)
        return 0;
    clusters    = data_length / FAUXFAT_CLUSTER_SIZE;
    end_cluster = (uint64_t)first_cluster + clusters;
    if (end_cluster > (uint64_t)v->cluster_count + 2u)
        return 0;
    if (run_count_out)
        *run_count_out = run_count;
    if (end_cluster_out)
        *end_cluster_out = end_cluster;
    return 1;
}

static int ff_parse_regular_set_loose(const fauxfat_view *v,
                                      const fauxfat_device *device,
                                      const uint8_t set[96],
                                      fauxfat_disk_file *d,
                                      uint64_t *end_cluster,
                                      int *error_out)
{
    const uint8_t *file_ent = set;
    const uint8_t *stream   = set + 32u;
    const uint8_t *name_ent = set + 64u;
    uint16_t attrs;
    uint64_t valid_length;
    uint64_t data_length;
    uint64_t allocation_clusters;
    int parse_error = FAUXFAT_OK;

    if (error_out)
        *error_out = FAUXFAT_OK;

    memset(d, 0, sizeof(*d));
    d->kind = FAUXFAT_DISK_FILE_PUBLIC;

    if (file_ent[0] != FF_ENTRY_FILE || file_ent[1] != 2u ||
        faux_load_le16(file_ent + 2u) != ff_entry_set_checksum(set, 96u))
        return 0;
    attrs = faux_load_le16(file_ent + 4u);
    if ((attrs & FF_ATTR_DIRECTORY) != 0u ||
        (attrs & (uint16_t)~(FF_ATTR_READONLY | FF_ATTR_HIDDEN |
                             FF_ATTR_SYSTEM | FF_ATTR_ARCHIVE)) != 0u)
        return 0;
    if (stream[0] != FF_ENTRY_STREAM ||
        (stream[1] != 0x01u && stream[1] != 0x03u) ||
        stream[2] != 0u || !ff_all_zero(stream + 6u, 2u) ||
        !ff_all_zero(stream + 16u, 4u) ||
        !ff_decode_name_entry(name_ent, stream[3], d->name) ||
        faux_load_le16(stream + 4u) != ff_name_hash(d->name))
        return 0;

    valid_length = faux_load_le64(stream + 8u);
    data_length  = faux_load_le64(stream + 24u);
    if (data_length == 0u || valid_length > data_length ||
        !ff_public_allocation_clusters(v, device, stream, data_length,
                                       &allocation_clusters, &parse_error) ||
        !ff_extent_to_descriptor(v, faux_load_le32(stream + 20u), data_length,
                                 0, allocation_clusters, d, end_cluster)) {
        if (error_out && parse_error != FAUXFAT_OK)
            *error_out = parse_error;
        return 0;
    }

    if (!ff_exfat_timestamp_decode(faux_load_le32(file_ent + 12u), file_ent[21],
                                   0, &d->mtime))
        d->mtime = (time_t)0;
    return 1;
}

static int ff_validate_root_system_entries(const fauxfat_view *v,
                                           ff_root_reader *r)
{
    uint8_t e[32];
    uint64_t bitmap_bytes    = ((uint64_t)v->cluster_count + 7u) / 8u;
    uint32_t bitmap_clusters = (uint32_t)((bitmap_bytes + FAUXFAT_CLUSTER_SIZE - 1u) /
                                          FAUXFAT_CLUSTER_SIZE);
    size_t i;
    int rc;

    if (bitmap_clusters == 0u || v->bitmap_clusters != bitmap_clusters ||
        v->upcase_cluster != 2u + bitmap_clusters ||
        v->root_cluster != v->upcase_cluster + 1u ||
        v->data_first_cluster != v->root_cluster + 1u)
        return FAUXFAT_EGEOMETRY;

    rc = ff_root_entry(r, 0u, e);
    if (rc != FAUXFAT_OK)
        return rc;
    if (e[0] != FF_ENTRY_BITMAP || e[1] != 0u || !ff_all_zero(e + 2, 18u) ||
        faux_load_le32(e + 20) != 2u || faux_load_le64(e + 24) != bitmap_bytes)
        return FAUXFAT_ESTRUCTURE;

    rc = ff_root_entry(r, 1u, e);
    if (rc != FAUXFAT_OK)
        return rc;
    if (e[0] != FF_ENTRY_UPCASE || !ff_all_zero(e + 1, 3u) ||
        faux_load_le32(e + 4) != ff_upcase_checksum() || !ff_all_zero(e + 8, 12u) ||
        faux_load_le32(e + 20) != v->upcase_cluster ||
        faux_load_le64(e + 24) != FF_UPCASE_BYTES)
        return FAUXFAT_ESTRUCTURE;

    rc = ff_root_entry(r, 2u, e);
    if (rc != FAUXFAT_OK)
        return rc;
    if (e[0] != FF_ENTRY_LABEL || e[1] > 11u)
        return FAUXFAT_ESTRUCTURE;
    for (i = 0; i < 11u; ++i) {
        uint16_t ch = faux_load_le16(e + 2u + 2u * i);
        if (i < e[1]) {
            if (ch < 0x20u || ch > 0x7eu)
                return FAUXFAT_ESTRUCTURE;
        } else if (ch != 0u) {
            return FAUXFAT_ESTRUCTURE;
        }
    }
    if (!ff_all_zero(e + 24, 8u))
        return FAUXFAT_ESTRUCTURE;

    rc = ff_root_entry(r, 3u, e);
    if (rc != FAUXFAT_OK)
        return rc;
    if (e[0] != FF_ENTRY_GUID || e[1] != 0u ||
        faux_load_le16(e + 2) != ff_entry_set_checksum(e, 32u) ||
        !ff_all_zero(e + 4, 2u) || faux_guid_is_zero(e + 6) ||
        !ff_all_zero(e + 22, 10u))
        return FAUXFAT_ESTRUCTURE;

    return FAUXFAT_OK;
}

static int ff_root_public_name_exists(const fauxfat_device *device,
                                      uint64_t root_first_block,
                                      uint32_t public_end_entry,
                                      const char *name,
                                      int logical,
                                      int *found)
{
    ff_root_reader r;
    uint32_t entry;

    *found = 0;
    memset(&r, 0, sizeof(r));
    r.device      = device;
    r.first_block = root_first_block;

    for (entry = FF_FILE_SLOT_FIRST; entry < public_end_entry;
         entry += FF_PUBLIC_ENTRY_COUNT) {
        uint8_t set[160];
        char other[FAUXFAT_NAME_MAX + 1u];
        int rc = ff_root_set(&r, entry, FF_PUBLIC_ENTRY_COUNT, set);
        if (rc != FAUXFAT_OK)
            return rc;
        if (set[0] != FF_ENTRY_FILE || set[1] != 4u ||
            set[32] != FF_ENTRY_STREAM ||
            !(logical ? ff_decode_public_origin(set + 96u, set + 128u, other) : ff_decode_name_entry(set + 64u, set[35], other)))
            return FAUXFAT_ESTRUCTURE;
        if (ff_name_equal_folded(name, other)) {
            *found = 1;
            return FAUXFAT_OK;
        }
    }
    return FAUXFAT_OK;
}

static int ff_root_reader_init(const fauxfat_view *v,
                               const fauxfat_device *device,
                               ff_root_reader *reader,
                               uint64_t *root_first_block)
{
    uint64_t first;

    if (!v || !device || !device->read || !reader)
        return FAUXFAT_EINVAL;
    if (v->root_cluster < 2u || v->root_cluster > v->cluster_count + 1u)
        return FAUXFAT_EGEOMETRY;
    first = (uint64_t)v->cluster_heap_block +
            (uint64_t)(v->root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    if (first > v->volume_blocks ||
        FAUXFAT_BLOCKS_PER_CLUSTER > v->volume_blocks - first)
        return FAUXFAT_EGEOMETRY;

    memset(reader, 0, sizeof(*reader));
    reader->device      = device;
    reader->first_block = first;
    if (root_first_block)
        *root_first_block = first;
    return FAUXFAT_OK;
}

static int ff_emit_descriptor(fauxfat_file_emit_fn emit,
                              void *emit_context,
                              unsigned *descriptor_index,
                              uint64_t *previous_end_cluster,
                              uint64_t end_cluster,
                              uint32_t first_cluster,
                              const fauxfat_disk_file *d)
{
    int rc;

    if (end_cluster <= (uint64_t)first_cluster ||
        (uint64_t)first_cluster < *previous_end_cluster)
        return FAUXFAT_ESTRUCTURE;

    *previous_end_cluster = end_cluster;
    if (emit) {
        rc = emit(emit_context, *descriptor_index, d);
        if (rc != 0)
            return rc;
    }
    ++*descriptor_index;
    return FAUXFAT_OK;
}

int fauxfat_parse_root_strict(const fauxfat_view *v,
                              const fauxfat_device *device,
                              fauxfat_file_emit_fn emit,
                              void *emit_context,
                              size_t *descriptor_count)
{
    ff_root_reader r;
    uint64_t root_first_block;
    uint64_t previous_end_cluster;
    uint32_t entry               = FF_FILE_SLOT_FIRST;
    uint32_t public_end_entry    = FF_FILE_SLOT_FIRST;
    unsigned public_count        = 0u;
    unsigned opaque_count        = 0u;
    unsigned blocker_count       = 0u;
    uint16_t blocker_total       = 0u;
    uint64_t blocker_end_cluster = 0u;
    unsigned descriptor_index    = 0u;

    enum {
        FF_ROOT_PUBLIC,
        FF_ROOT_OPAQUE,
        FF_ROOT_BLOCKER,
        FF_ROOT_PADDING
    } phase = FF_ROOT_PUBLIC;

    int rc;

    rc = ff_root_reader_init(v, device, &r, &root_first_block);
    if (rc != FAUXFAT_OK)
        return rc;
    previous_end_cluster = v->data_first_cluster;

    rc = ff_validate_root_system_entries(v, &r);
    if (rc != FAUXFAT_OK)
        return rc;

    while (entry < FF_ROOT_ENTRIES) {
        uint8_t first[32];

        rc = ff_root_entry(&r, entry, first);
        if (rc != FAUXFAT_OK)
            return rc;

        if (first[0] == FF_ENTRY_PAD) {
            if (first[1] != 0u || faux_load_le16(first + 2) != 0x0508u ||
                !ff_all_zero(first + 4, 28u))
                return FAUXFAT_ESTRUCTURE;
            phase = FF_ROOT_PADDING;
            ++entry;
            continue;
        }

        if (first[0] == FF_ENTRY_PRIVATE_RESERVE &&
            v->config->private_reservation ==
                FAUXFAT_PRIVATE_BENIGN_PRIMARY_AND_BAD) {
            uint16_t total;
            uint64_t end_cluster;
            uint32_t first_cluster = faux_load_le32(first + 20u);

            if (phase == FF_ROOT_PADDING ||
                !ff_private_reserve_entry_valid(v, blocker_count, first,
                                                &total, &end_cluster) ||
                (blocker_count != 0u && total != blocker_total) ||
                (blocker_count != 0u &&
                 (uint64_t)first_cluster < blocker_end_cluster))
                return FAUXFAT_ESTRUCTURE;
            blocker_total       = total;
            blocker_end_cluster = end_cluster;
            phase               = FF_ROOT_BLOCKER;
            ++blocker_count;
            ++entry;
            continue;
        }

        if (phase == FF_ROOT_PADDING || phase == FF_ROOT_BLOCKER ||
            first[0] != FF_ENTRY_FILE)
            return FAUXFAT_ESTRUCTURE;

        if (first[1] == 4u) {
            uint8_t set[160];
            fauxfat_disk_file d;
            uint64_t end_cluster;
            char visible[FAUXFAT_NAME_MAX + 1u];
            int seen_logical;
            int seen_visible;
            int parse_error;

            if (entry > FF_ROOT_ENTRIES - FF_PUBLIC_ENTRY_COUNT)
                return FAUXFAT_ESTRUCTURE;
            rc = ff_root_set(&r, entry, FF_PUBLIC_ENTRY_COUNT, set);
            if (rc != FAUXFAT_OK)
                return rc;

            if (set[128u] == FF_ENTRY_VENDOR_EXT &&
                memcmp(set + 130u, ff_public_name_guid1, 16u) == 0) {
                if (phase != FF_ROOT_PUBLIC)
                    return FAUXFAT_ESTRUCTURE;
                if (!ff_parse_public_set(v, device, set, &d, &end_cluster,
                                         &parse_error))
                    return parse_error != FAUXFAT_OK ? parse_error :
                                                       FAUXFAT_ESTRUCTURE;
                if (v->config->file_count != 0u &&
                    (public_count >= v->config->file_count ||
                     d.allocation_blocks !=
                         ff_file_clusters(&v->config->files[public_count]) *
                             FAUXFAT_BLOCKS_PER_CLUSTER))
                    return FAUXFAT_ESTRUCTURE;
                if (!ff_decode_name_entry(set + 64u, set[35], visible) ||
                    end_cluster <= previous_end_cluster ||
                    faux_load_le32(set + 32u + 20u) < previous_end_cluster)
                    return FAUXFAT_ESTRUCTURE;

                rc = ff_root_public_name_exists(device, root_first_block,
                                                entry, d.name, 1,
                                                &seen_logical);
                if (rc != FAUXFAT_OK)
                    return rc;
                rc = ff_root_public_name_exists(device, root_first_block,
                                                entry, visible, 0,
                                                &seen_visible);
                if (rc != FAUXFAT_OK)
                    return rc;
                if (seen_logical || seen_visible)
                    return FAUXFAT_ESTRUCTURE;

                rc = ff_emit_descriptor(emit, emit_context,
                                        &descriptor_index,
                                        &previous_end_cluster, end_cluster,
                                        faux_load_le32(set + 32u + 20u), &d);
                if (rc != FAUXFAT_OK)
                    return rc;
                ++public_count;
                entry += FF_PUBLIC_ENTRY_COUNT;
                continue;
            }

            /* The only other four-secondary fauxFAT set is an opaque one. */
            if (set[128u] != FF_ENTRY_VENDOR_ALLOC ||
                memcmp(set + 130u, ff_opaque_alloc_guid, 16u) != 0)
                return FAUXFAT_ESTRUCTURE;

            {
                char stub[FAUXFAT_NAME_MAX + 1u];
                int collision;

                if (phase == FF_ROOT_PUBLIC) {
                    phase            = FF_ROOT_OPAQUE;
                    public_end_entry = entry;
                }
                if (phase != FF_ROOT_OPAQUE)
                    return FAUXFAT_ESTRUCTURE;

                if (!ff_parse_opaque_set(v, set, opaque_count, &d, &end_cluster) ||
                    end_cluster <= previous_end_cluster ||
                    faux_load_le32(set + 128u + 20u) < previous_end_cluster)
                    return FAUXFAT_ESTRUCTURE;

                ff_opaque_stub_name(opaque_count, stub);
                rc = ff_root_public_name_exists(device, root_first_block,
                                                public_end_entry, stub, 0,
                                                &collision);
                if (rc != FAUXFAT_OK)
                    return rc;
                if (collision)
                    return FAUXFAT_ESTRUCTURE;

                rc = ff_emit_descriptor(emit, emit_context,
                                        &descriptor_index,
                                        &previous_end_cluster, end_cluster,
                                        faux_load_le32(set + 128u + 20u), &d);
                if (rc != FAUXFAT_OK)
                    return rc;
                ++opaque_count;
                entry += FF_OPAQUE_ENTRY_COUNT;
                continue;
            }
        }

        return FAUXFAT_ESTRUCTURE;
    }

    if (public_count > FAUXFAT_MAX_FILES || opaque_count > FAUXFAT_MAX_OPAQUE_FILES)
        return FAUXFAT_ESTRUCTURE;
    if (v->config->private_reservation ==
            FAUXFAT_PRIVATE_BENIGN_PRIMARY_AND_BAD &&
        blocker_count != blocker_total)
        return FAUXFAT_ESTRUCTURE;
    if (descriptor_count)
        *descriptor_count = descriptor_index;
    return FAUXFAT_OK;
}

static int ff_dev_read_block(const fauxfat_device *device,
                             uint64_t block,
                             uint8_t out[FAUXFAT_BLOCK_SIZE])
{
    return device->read(device->context, block, 1u, out);
}

static int ff_oem_recognizable(const uint8_t sector[FAUXFAT_BLOCK_SIZE])
{
    return memcmp(sector, ff_oem_map_guid, 16u) == 0 &&
           memcmp(sector + 48u, ff_oem_epoch_guid, 16u) == 0 &&
           memcmp(sector + 64u, "FFV1", 4u) == 0 &&
           faux_load_le16(sector + 68u) == 1u &&
           (faux_load_le16(sector + 70u) &
            (uint16_t)~FF_OEM_KNOWN_FLAGS) == 0u;
}

static int ff_boot_sector_fixed_equal(const uint8_t actual[FAUXFAT_BLOCK_SIZE],
                                      const uint8_t expected[FAUXFAT_BLOCK_SIZE],
                                      int backup)
{
    uint16_t flags;
    unsigned i;

    for (i = 0u; i < FAUXFAT_BLOCK_SIZE; ++i) {
        if (i == 106u || i == 107u || i == 112u)
            continue;
        if (actual[i] != expected[i])
            return 0;
    }

    flags = faux_load_le16(actual + 106u);
    if (!backup) {
        /* Only VolumeDirty may differ on the current Main Boot Sector. */
        if ((flags & (uint16_t)~0x0002u) != 0u || actual[112] != 100u)
            return 0;
    } else {
        /*
         * Backup VolumeFlags/PercentInUse are explicitly stale in exFAT.
         * NumberOfFats is one, so ActiveFat remains nonsensical; otherwise
         * accept only the defined low flag bits and a sane percentage.
         */
        if ((flags & 0xfff1u) != 0u || actual[112] > 100u)
            return 0;
    }
    return 1;
}

static int ff_boot_checksum_disk(const fauxfat_device *device,
                                 uint64_t base,
                                 uint32_t *checksum)
{
    uint8_t block[FAUXFAT_BLOCK_SIZE];
    uint32_t sum = 0u;
    uint32_t sector;
    unsigned byte;
    int rc;

    for (sector = 0u; sector <= 10u; ++sector) {
        rc = ff_dev_read_block(device, base + sector, block);
        if (rc != 0)
            return rc;
        for (byte = 0u; byte < FAUXFAT_BLOCK_SIZE; ++byte) {
            if (sector == 0u &&
                (byte == 106u || byte == 107u || byte == 112u))
                continue;
            sum = ff_ror32(sum) + block[byte];
        }
    }
    *checksum = sum;
    return FAUXFAT_OK;
}

static int ff_boot_checksum_sector_valid(const fauxfat_device *device,
                                         uint64_t block,
                                         uint32_t checksum)
{
    uint8_t bytes[FAUXFAT_BLOCK_SIZE];
    unsigned i;
    int rc = ff_dev_read_block(device, block, bytes);

    if (rc != 0)
        return rc;
    for (i = 0u; i < 128u; ++i) {
        if (faux_load_le32(bytes + 4u * i) != checksum)
            return 0;
    }
    return 1;
}

static int ff_verify_boot_region(const fauxfat_view *v,
                                 const fauxfat_device *device,
                                 uint64_t base,
                                 int backup,
                                 uint8_t boot_out[FAUXFAT_BLOCK_SIZE])
{
    uint8_t actual[FAUXFAT_BLOCK_SIZE];
    uint8_t expected[FAUXFAT_BLOCK_SIZE];
    uint32_t checksum;
    uint32_t sector;
    int rc;

    rc = ff_dev_read_block(device, base, actual);
    if (rc != 0)
        return rc;
    ff_make_boot_sector(v, expected, backup);
    if (!ff_boot_sector_fixed_equal(actual, expected, backup))
        return 0;
    if (boot_out)
        memcpy(boot_out, actual, FAUXFAT_BLOCK_SIZE);

    ff_make_extended_boot(expected);
    for (sector = 1u; sector <= 8u; ++sector) {
        rc = ff_dev_read_block(device, base + sector, actual);
        if (rc != 0)
            return rc;
        if (memcmp(actual, expected, FAUXFAT_BLOCK_SIZE) != 0)
            return 0;
    }

    rc = ff_dev_read_block(device, base + 10u, actual);
    if (rc != 0)
        return rc;
    if (!ff_all_zero(actual, FAUXFAT_BLOCK_SIZE))
        return 0;

    rc = ff_boot_checksum_disk(device, base, &checksum);
    if (rc != FAUXFAT_OK)
        return rc;
    return ff_boot_checksum_sector_valid(device, base + 11u, checksum);
}

static int ff_verify_rendered_region(const fauxfat_view *v,
                                     const fauxfat_device *device,
                                     uint64_t first_block, uint64_t bytes,
                                     ff_xxh32 *component, ff_xxh32 *map)
{
    uint8_t actual[FAUXFAT_BLOCK_SIZE];
    uint8_t expected[FAUXFAT_BLOCK_SIZE];
    uint32_t block_index = 0u;

    while (bytes != 0u) {
        size_t n = bytes > FAUXFAT_BLOCK_SIZE ? FAUXFAT_BLOCK_SIZE : (size_t)bytes;
        int rc   = ff_dev_read_block(device, first_block + block_index, actual);

        if (rc != 0)
            return rc;
        rc = ff_render_structural_block(v, first_block + block_index, expected);
        if (rc != FAUXFAT_OK)
            return rc;
        if (memcmp(actual, expected, n) != 0)
            return 0;
        ff_structural_hash_update(component, map, actual, n);
        bytes -= n;
        ++block_index;
    }
    return 1;
}

static int ff_verify_upcase_and_hash(const fauxfat_view *v,
                                     const fauxfat_device *device,
                                     ff_xxh32 *component,
                                     ff_xxh32 *map)
{
    uint64_t block = (uint64_t)v->cluster_heap_block +
                     (uint64_t)(v->upcase_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;

    return ff_verify_rendered_region(v, device, block, FF_UPCASE_BYTES,
                                     component, map);
}

static int ff_verify_bitmap_and_hash(const fauxfat_view *v,
                                     const fauxfat_device *device,
                                     ff_xxh32 *component,
                                     ff_xxh32 *map)
{
    uint64_t bytes = ((uint64_t)v->cluster_count + 7u) / 8u;

    return ff_verify_rendered_region(v, device, v->cluster_heap_block, bytes,
                                     component, map);
}

static int ff_verify_fat_and_hash(const fauxfat_view *v,
                                  const fauxfat_device *device,
                                  ff_xxh32 *component,
                                  ff_xxh32 *map)
{
    uint64_t bytes = ((uint64_t)v->cluster_count + 2u) * 4u;

    return ff_verify_rendered_region(v, device, FF_FAT_OFFSET_BLOCKS, bytes,
                                     component, map);
}

static int ff_root_block_canonicalize(const fauxfat_view *v,
                                      uint32_t block_index,
                                      uint8_t actual[FAUXFAT_BLOCK_SIZE])
{
    uint8_t expected[FAUXFAT_BLOCK_SIZE];
    unsigned slot;

    ff_make_root_block(v, block_index, expected);
    ff_root_hash_normalize_public_metadata(v, block_index, expected);
    for (slot = 0u; slot < 16u; ++slot) {
        uint8_t *a       = actual + slot * 32u;
        const uint8_t *e = expected + slot * 32u;
        uint64_t root_entry = (uint64_t)block_index * 16u + slot;
        uint64_t first_public_stream = FF_FILE_SLOT_FIRST + 1u;

        if (ff_file_length_changes_enabled(v) && e[0] == FF_ENTRY_STREAM &&
            e[1] == 0x01u &&
            root_entry >= first_public_stream &&
            ((root_entry - first_public_stream) % FF_PUBLIC_ENTRY_COUNT) == 0u &&
            ((root_entry - first_public_stream) / FF_PUBLIC_ENTRY_COUNT) <
                v->config->file_count) {
            if (a[0] != FF_ENTRY_STREAM || a[1] != e[1])
                return 0;
            memcpy(a + 8u, e + 8u, 8u);
            memcpy(a + 24u, e + 24u, 8u);
            continue;
        }

        if (e[0] != FF_ENTRY_FILE)
            continue;
        if (a[0] != FF_ENTRY_FILE || a[1] != e[1] ||
            (faux_load_le16(a + 4u) & (uint16_t)~FF_ATTR_ARCHIVE) !=
                faux_load_le16(e + 4u))
            return 0;

        /* SetChecksum and the permitted host-volatile File fields. */
        a[2] = e[2];
        a[3] = e[3];
        faux_store_le16(a + 4u, faux_load_le16(e + 4u));
        memcpy(a + 12u, e + 12u, 8u);
        a[21] = e[21];
        a[23] = e[23];
        a[24] = e[24];
    }
    return memcmp(actual, expected, FAUXFAT_BLOCK_SIZE) == 0;
}

static int ff_verify_root_and_hash(const fauxfat_view *v,
                                   const fauxfat_device *device,
                                   ff_xxh32 *component,
                                   ff_xxh32 *map)
{
    uint8_t block[FAUXFAT_BLOCK_SIZE];
    uint64_t first = (uint64_t)v->cluster_heap_block +
                     (uint64_t)(v->root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    uint32_t i;
    int rc = fauxfat_parse_root_strict(v, device, NULL, NULL, NULL);

    if (rc != FAUXFAT_OK)
        return rc == FAUXFAT_ESTRUCTURE || rc == FAUXFAT_EGEOMETRY ? 0 : rc;

    for (i = 0u; i < FAUXFAT_BLOCKS_PER_CLUSTER; ++i) {
        rc = ff_dev_read_block(device, first + i, block);
        if (rc != 0)
            return rc;
        if (!ff_root_block_canonicalize(v, i, block))
            return 0;
        ff_structural_hash_update(component, map, block,
                                  FAUXFAT_BLOCK_SIZE);
    }
    return 1;
}

typedef int (*ff_hash_verify_fn)(const fauxfat_view *v,
                                 const fauxfat_device *device,
                                 ff_xxh32 *component, ff_xxh32 *map);

static int ff_verify_structural_hashes(const fauxfat_view *v,
                                       const fauxfat_device *device,
                                       ff_structural_hash_state *hashes,
                                       ff_hash_verify_fn verify_fat,
                                       ff_hash_verify_fn verify_root)
{
    int rc;

    rc = verify_fat(v, device, &hashes->fat, &hashes->map);
    if (rc != 1)
        return rc;
    rc = ff_verify_bitmap_and_hash(v, device, &hashes->bitmap, &hashes->map);
    if (rc != 1)
        return rc;
    rc = ff_verify_upcase_and_hash(v, device, &hashes->upcase, &hashes->map);
    if (rc != 1)
        return rc;
    return verify_root(v, device, &hashes->root, &hashes->map);
}

static int ff_oem_hash_fields_valid(const uint8_t sector[FAUXFAT_BLOCK_SIZE],
                                    const ff_structural_hash_values *hashes)
{
    return ff_oem_recognizable(sector) &&
           faux_load_le32(sector + 16u) == hashes->map &&
           ff_all_zero(sector + 20u, 28u) &&
           faux_load_le32(sector + 80u) == hashes->fat &&
           faux_load_le32(sector + 84u) == hashes->bitmap &&
           faux_load_le32(sector + 88u) == hashes->root &&
           faux_load_le32(sector + 92u) == hashes->upcase &&
           ff_all_zero(sector + 96u, FAUXFAT_BLOCK_SIZE - 96u);
}

static int ff_oem_sector_valid(const fauxfat_view *v,
                               const uint8_t sector[FAUXFAT_BLOCK_SIZE],
                               const ff_structural_hash_values *hashes)
{
    return ff_oem_hash_fields_valid(sector, hashes) &&
           faux_load_le16(sector + 70u) == ff_oem_flags(v) &&
           faux_load_le64(sector + 72u) == v->config->structural_epoch &&
           hashes->map == v->map_xxh32 &&
           hashes->fat == v->fat_xxh32 &&
           hashes->bitmap == v->bitmap_xxh32 &&
           hashes->root == v->root_xxh32 &&
           hashes->upcase == v->upcase_xxh32;
}

int fauxfat_validate_strict(const fauxfat_view *v,
                            const fauxfat_device *device,
                            fauxfat_volume_class *classification)
{
    uint8_t main_boot[FAUXFAT_BLOCK_SIZE];
    uint8_t main_oem[FAUXFAT_BLOCK_SIZE];
    uint8_t backup_oem[FAUXFAT_BLOCK_SIZE];
    ff_structural_hash_state hashes;
    ff_structural_hash_values values;
    int recognized;
    int rc;

    if (!v || !v->config || !device || !device->read || !classification)
        return FAUXFAT_EINVAL;

    *classification = FAUXFAT_VOLUME_INVALID;

    /* Establish fauxFAT identity first so later mismatches can be classified. */
    rc = ff_dev_read_block(device, 9u, main_oem);
    if (rc != 0)
        return rc;
    recognized = ff_oem_recognizable(main_oem);
    if (recognized)
        *classification = FAUXFAT_VOLUME_FAUXFAT_CHANGED;

    rc = ff_verify_boot_region(v, device, 0u, 0, main_boot);
    if (rc < 0)
        return rc;
    if (rc == 0)
        return FAUXFAT_OK;
    rc = ff_verify_boot_region(v, device, 12u, 1, NULL);
    if (rc < 0)
        return rc;
    if (rc == 0)
        return FAUXFAT_OK;

    rc = ff_dev_read_block(device, 21u, backup_oem);
    if (rc != 0)
        return rc;
    if (!recognized || memcmp(main_oem, backup_oem, FAUXFAT_BLOCK_SIZE) != 0)
        return FAUXFAT_OK;

    ff_structural_hash_init(&hashes, main_boot);

    rc = ff_verify_structural_hashes(v, device, &hashes,
                                     ff_verify_fat_and_hash,
                                     ff_verify_root_and_hash);
    if (rc < 0)
        return rc;
    if (rc == 0)
        return FAUXFAT_OK;

    ff_structural_hash_digest(&hashes, &values);
    if (!ff_oem_sector_valid(v, main_oem, &values))
        return FAUXFAT_OK;

    *classification = FAUXFAT_VOLUME_FAUXFAT_VALID;
    return FAUXFAT_OK;
}

static int ff_scan_loose_geometry(const fauxfat_view *v,
                                  const fauxfat_device *device,
                                  fauxfat_file_emit_fn emit,
                                  void *emit_context,
                                  size_t *descriptor_count)
{
    ff_root_reader r;
    uint64_t root_first_block;
    uint64_t previous_end_cluster;
    uint32_t entry            = 0u;
    unsigned descriptor_index = 0u;
    int rc;

    rc = ff_root_reader_init(v, device, &r, &root_first_block);
    if (rc != FAUXFAT_OK)
        return rc;
    previous_end_cluster = v->data_first_cluster;

    while (entry < FF_ROOT_ENTRIES) {
        uint8_t first[32];
        uint8_t type;

        rc = ff_root_entry(&r, entry, first);
        if (rc != FAUXFAT_OK)
            return rc;
        type = first[0];

        /* Normal exFAT end marker. fauxFAT instead pads to the cluster end. */
        if (type == 0u)
            break;

        /* Deleted/non-live directory entries are irrelevant to salvage. */
        if ((type & FF_ENTRY_IN_USE) == 0u) {
            ++entry;
            continue;
        }

        if (type == FF_ENTRY_BITMAP || type == FF_ENTRY_UPCASE ||
            type == FF_ENTRY_LABEL || type == FF_ENTRY_GUID ||
            type == FF_ENTRY_PAD) {
            ++entry;
            continue;
        }

        if (type == FF_ENTRY_FILE) {
            unsigned secondary_count = first[1];
            unsigned total_count;
            uint8_t set[19u * 32u];
            fauxfat_disk_file d;
            uint64_t end_cluster;
            uint32_t first_cluster;
            int public_hint = 0;
            int opaque_hint = 0;
            int parse_error;

            /* exFAT File sets have 2..18 secondaries; cap scratch accordingly. */
            if (secondary_count < 2u || secondary_count > 18u)
                return FAUXFAT_ESTRUCTURE;
            total_count = secondary_count + 1u;
            if (entry > FF_ROOT_ENTRIES - total_count)
                return FAUXFAT_ESTRUCTURE;
            rc = ff_root_set(&r, entry, total_count, set);
            if (rc != FAUXFAT_OK)
                return rc;
            if (faux_load_le16(set + 2u) !=
                ff_entry_set_checksum(set, (size_t)total_count * 32u))
                return FAUXFAT_ESTRUCTURE;

            if (secondary_count >= 4u) {
                public_hint =
                    (set[96u] == FF_ENTRY_VENDOR_EXT &&
                     memcmp(set + 98u, ff_public_name_guid0, 16u) == 0) ||
                    (set[128u] == FF_ENTRY_VENDOR_EXT &&
                     memcmp(set + 130u, ff_public_name_guid1, 16u) == 0);
                opaque_hint =
                    (set[96u] == FF_ENTRY_VENDOR_EXT &&
                     memcmp(set + 98u, ff_opaque_meta_guid, 16u) == 0) ||
                    (set[128u] == FF_ENTRY_VENDOR_ALLOC &&
                     memcmp(set + 130u, ff_opaque_alloc_guid, 16u) == 0);
            }

            if (secondary_count == 4u &&
                set[96u] == FF_ENTRY_VENDOR_EXT &&
                set[128u] == FF_ENTRY_VENDOR_EXT &&
                memcmp(set + 98u, ff_public_name_guid0, 16u) == 0 &&
                memcmp(set + 130u, ff_public_name_guid1, 16u) == 0) {
                if (!ff_parse_public_set(v, device, set, &d, &end_cluster,
                                         &parse_error))
                    return parse_error != FAUXFAT_OK ? parse_error :
                                                       FAUXFAT_ESTRUCTURE;
                first_cluster = faux_load_le32(set + 32u + 20u);
                rc            = ff_emit_descriptor(emit, emit_context,
                                                   &descriptor_index,
                                                   &previous_end_cluster,
                                                   end_cluster, first_cluster, &d);
                if (rc != FAUXFAT_OK)
                    return rc;
            } else if (secondary_count == 4u &&
                       set[96u] == FF_ENTRY_VENDOR_EXT &&
                       set[128u] == FF_ENTRY_VENDOR_ALLOC &&
                       memcmp(set + 98u, ff_opaque_meta_guid, 16u) == 0 &&
                       memcmp(set + 130u, ff_opaque_alloc_guid, 16u) == 0) {
                if (!ff_parse_opaque_set_loose(v, set, &d, &end_cluster))
                    return FAUXFAT_ESTRUCTURE;
                first_cluster = faux_load_le32(set + 128u + 20u);
                rc            = ff_emit_descriptor(emit, emit_context,
                                                   &descriptor_index,
                                                   &previous_end_cluster,
                                                   end_cluster, first_cluster, &d);
                if (rc != FAUXFAT_OK)
                    return rc;
            } else if (public_hint || opaque_hint) {
                /* A torn/corrupted fauxFAT descriptor is not a foreign object. */
                return FAUXFAT_ESTRUCTURE;
            } else if (secondary_count == 2u) {
                uint16_t attrs        = faux_load_le16(set + 4u);
                const uint8_t *stream = set + 32u;

                /*
                 * Directories and fragmented files are valid exFAT but are
                 * outside the fauxFAT salvage profile. Skip, don't learn to
                 * traverse them.
                 */
                if ((attrs & FF_ATTR_DIRECTORY) == 0u &&
                    stream[0] == FF_ENTRY_STREAM &&
                    stream[1] == 0x03u) {
                    if (!ff_parse_regular_set_loose(v, device, set, &d,
                                                    &end_cluster,
                                                    &parse_error))
                        return parse_error != FAUXFAT_OK ? parse_error :
                                                           FAUXFAT_ESTRUCTURE;
                    first_cluster = faux_load_le32(stream + 20u);
                    rc            = ff_emit_descriptor(emit, emit_context,
                                                       &descriptor_index,
                                                       &previous_end_cluster,
                                                       end_cluster, first_cluster,
                                                       &d);
                    if (rc != FAUXFAT_OK)
                        return rc;
                }
            }

            entry += total_count;
            continue;
        }

        /* A secondary outside its File set is ambiguous corruption. */
        if ((type & FF_ENTRY_SECONDARY) != 0u)
            return FAUXFAT_ESTRUCTURE;

        /* Unknown benign primaries are explicitly ignorable. */
        if ((type & FF_ENTRY_BENIGN) != 0u) {
            ++entry;
            continue;
        }

        return FAUXFAT_ESTRUCTURE;
    }

    if (descriptor_count)
        *descriptor_count = descriptor_index;
    return FAUXFAT_OK;
}

int fauxfat_scan_loose(const fauxfat_view *v,
                       const fauxfat_device *device,
                       fauxfat_file_emit_fn emit,
                       void *emit_context,
                       size_t *descriptor_count,
                       fauxfat_volume_class *classification)
{
    fauxfat_volume_class strict_class;
    int rc;

    if (!v || !device || !device->read || !classification)
        return FAUXFAT_EINVAL;

    rc = fauxfat_validate_strict(v, device, &strict_class);
    if (rc != FAUXFAT_OK)
        return rc;
    rc = ff_scan_loose_geometry(v, device, emit, emit_context,
                                descriptor_count);
    if (rc != FAUXFAT_OK)
        return rc;

    if (strict_class == FAUXFAT_VOLUME_FAUXFAT_VALID)
        *classification = FAUXFAT_VOLUME_FAUXFAT_VALID;
    else if (strict_class == FAUXFAT_VOLUME_FAUXFAT_CHANGED)
        *classification = FAUXFAT_VOLUME_FAUXFAT_CHANGED;
    else
        *classification = FAUXFAT_VOLUME_EXFAT_BEST_EFFORT;
    return FAUXFAT_OK;
}

typedef struct ff_reopen_layout_summary {
    const fauxfat_view *view;
    uint64_t next_cluster;
    int seen_opaque;
    int name_changed;
} ff_reopen_layout_summary;

static int ff_reopen_layout_emit(void *context, unsigned index,
                                 const fauxfat_disk_file *file)
{
    ff_reopen_layout_summary *s = (ff_reopen_layout_summary *)context;
    uint64_t heap               = s->view->cluster_heap_block;
    uint64_t first_cluster;
    uint64_t clusters;
    uint64_t limit = (uint64_t)s->view->cluster_count + 2u;

    (void)index;
    if (file->first_block < heap ||
        ((file->first_block - heap) % FAUXFAT_BLOCKS_PER_CLUSTER) != 0u ||
        file->allocation_blocks == 0u ||
        (file->allocation_blocks % FAUXFAT_BLOCKS_PER_CLUSTER) != 0u)
        return FAUXFAT_ESTRUCTURE;

    first_cluster = 2u +
                    (file->first_block - heap) / FAUXFAT_BLOCKS_PER_CLUSTER;
    clusters = file->allocation_blocks / FAUXFAT_BLOCKS_PER_CLUSTER;
    if (first_cluster < s->next_cluster || clusters > limit - first_cluster)
        return FAUXFAT_ESTRUCTURE;

    if (file->kind == FAUXFAT_DISK_FILE_OPAQUE) {
        s->seen_opaque = 1;
    } else if (s->seen_opaque) {
        return FAUXFAT_ESTRUCTURE;
    }

    if ((file->flags & FAUXFAT_DISK_FILE_NAME_CHANGED) != 0u)
        s->name_changed = 1;
    s->next_cluster = first_cluster + clusters;
    return FAUXFAT_OK;
}

typedef struct ff_reopen_geometry_fields {
    uint64_t partition_lba;
    uint64_t volume_blocks;
    uint32_t volume_serial;
    uint32_t fat_length_blocks;
    uint32_t cluster_heap_block;
    uint32_t cluster_count;
    uint32_t root_cluster;
} ff_reopen_geometry_fields;

static int ff_build_reopen_geometry(const ff_reopen_geometry_fields *g,
                                    fauxfat_config *cfg, fauxfat_view *v)
{
    uint64_t fat_bytes;
    uint64_t bitmap_bytes;
    uint64_t bitmap_clusters;
    uint64_t expected_volume_blocks;
    uint32_t expected_fat_length;

    if (!g || !cfg || !v || g->cluster_count == 0u ||
        g->cluster_count > FF_MAX_CLUSTER_COUNT ||
        g->fat_length_blocks == 0u ||
        (g->fat_length_blocks % FAUXFAT_BLOCKS_PER_CLUSTER) != 0u)
        return 0;

    fat_bytes = ((uint64_t)g->cluster_count + 2u) * 4u;
    if (fat_bytes > UINT32_MAX * (uint64_t)FAUXFAT_BLOCK_SIZE)
        return 0;
    expected_fat_length = ff_align_up_u32(
        (uint32_t)((fat_bytes + FAUXFAT_BLOCK_SIZE - 1u) /
                   FAUXFAT_BLOCK_SIZE),
        FAUXFAT_BLOCKS_PER_CLUSTER);
    if (g->fat_length_blocks != expected_fat_length ||
        g->cluster_heap_block != FF_FAT_OFFSET_BLOCKS +
                                     g->fat_length_blocks)
        return 0;

    expected_volume_blocks = (uint64_t)g->cluster_heap_block +
                             (uint64_t)g->cluster_count * FAUXFAT_BLOCKS_PER_CLUSTER;
    if (g->volume_blocks != expected_volume_blocks)
        return 0;

    bitmap_bytes    = ((uint64_t)g->cluster_count + 7u) / 8u;
    bitmap_clusters = (bitmap_bytes + FAUXFAT_CLUSTER_SIZE - 1u) /
                      FAUXFAT_CLUSTER_SIZE;
    if (bitmap_clusters == 0u || bitmap_clusters > UINT32_MAX)
        return 0;

    memset(cfg, 0, sizeof(*cfg));
    cfg->partition_lba = g->partition_lba;
    cfg->volume_serial = g->volume_serial;

    memset(v, 0, sizeof(*v));
    v->config             = cfg;
    v->bitmap_clusters    = (uint32_t)bitmap_clusters;
    v->upcase_cluster     = 2u + v->bitmap_clusters;
    v->root_cluster       = g->root_cluster;
    v->data_first_cluster = v->root_cluster + 1u;
    v->cluster_count      = g->cluster_count;
    v->fat_length_blocks  = g->fat_length_blocks;
    v->cluster_heap_block = g->cluster_heap_block;
    v->volume_blocks      = g->volume_blocks;

    return v->root_cluster == v->upcase_cluster + 1u &&
           v->data_first_cluster <= v->cluster_count + 2u;
}

static int ff_reopen_geometry(const fauxfat_device *device,
                              fauxfat_view *v,
                              fauxfat_config *cfg,
                              uint8_t main_boot[FAUXFAT_BLOCK_SIZE],
                              int *boot_strict)
{
    ff_reopen_geometry_fields g;
    int rc;

    rc = ff_dev_read_block(device, 0u, main_boot);
    if (rc != 0)
        return rc;

    if (main_boot[0] != 0xebu || main_boot[1] != 0x76u ||
        main_boot[2] != 0x90u ||
        memcmp(main_boot + 3u, "EXFAT   ", 8u) != 0 ||
        faux_load_le16(main_boot + 104u) != 0x0100u ||
        main_boot[108u] != 9u || main_boot[109u] != 7u ||
        main_boot[110u] != 1u ||
        faux_load_le32(main_boot + 80u) != FF_FAT_OFFSET_BLOCKS)
        return 0;

    memset(&g, 0, sizeof(g));
    g.partition_lba      = faux_load_le64(main_boot + 64u);
    g.volume_blocks      = faux_load_le64(main_boot + 72u);
    g.fat_length_blocks  = faux_load_le32(main_boot + 84u);
    g.cluster_heap_block = faux_load_le32(main_boot + 88u);
    g.cluster_count      = faux_load_le32(main_boot + 92u);
    g.root_cluster       = faux_load_le32(main_boot + 96u);
    g.volume_serial      = faux_load_le32(main_boot + 100u);
    if (!ff_build_reopen_geometry(&g, cfg, v))
        return 0;

    rc = ff_verify_boot_region(v, device, 0u, 0, NULL);
    if (rc < 0)
        return rc;
    *boot_strict = (rc == 1);
    rc           = ff_verify_boot_region(v, device, 12u, 1, NULL);
    if (rc < 0)
        return rc;
    if (rc != 1)
        *boot_strict = 0;
    return 1;
}

static int ff_reopen_root_identity(const fauxfat_view *v,
                                   const fauxfat_device *device,
                                   fauxfat_reopen_info *info)
{
    ff_root_reader r;
    uint8_t e[32];
    uint64_t root_first;
    size_t i;
    int rc;

    root_first = (uint64_t)v->cluster_heap_block +
                 (uint64_t)(v->root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    memset(&r, 0, sizeof(r));
    r.device      = device;
    r.first_block = root_first;

    rc = ff_validate_root_system_entries(v, &r);
    if (rc != FAUXFAT_OK)
        return rc;
    if (!info)
        return FAUXFAT_OK;

    rc = ff_root_entry(&r, 2u, e);
    if (rc != FAUXFAT_OK)
        return rc;
    memset(info->volume_label, 0, sizeof(info->volume_label));
    for (i = 0u; i < e[1]; ++i)
        info->volume_label[i] = (char)(uint8_t)faux_load_le16(e + 2u + 2u * i);

    rc = ff_root_entry(&r, 3u, e);
    if (rc != FAUXFAT_OK)
        return rc;
    memcpy(info->volume_guid, e + 6u, sizeof(info->volume_guid));
    return FAUXFAT_OK;
}

static void ff_reopen_canonicalize_file_set(uint8_t *set, size_t bytes,
                                            int mutable_lengths)
{
    uint8_t *file_ent = set;
    uint16_t attrs    = faux_load_le16(file_ent + 4u);

    attrs &= (uint16_t)~FF_ATTR_ARCHIVE;
    faux_store_le16(file_ent + 4u, attrs);
    memcpy(file_ent + 12u, file_ent + 8u, 4u);
    memcpy(file_ent + 16u, file_ent + 8u, 4u);
    file_ent[21u] = file_ent[20u];
    file_ent[23u] = file_ent[22u];
    file_ent[24u] = file_ent[22u];
    if (bytes == 160u && set[96u] == FF_ENTRY_VENDOR_EXT &&
        set[128u] == FF_ENTRY_VENDOR_EXT &&
        memcmp(set + 98u, ff_public_name_guid0, 16u) == 0 &&
        memcmp(set + 130u, ff_public_name_guid1, 16u) == 0) {
        if (mutable_lengths && set[33u] == 0x01u) {
            memset(set + 40u, 0, 8u);
            memset(set + 56u, 0, 8u);
        }
    }
    faux_store_le16(file_ent + 2u, ff_entry_set_checksum(set, bytes));
}

static int ff_reopen_hash_root(const fauxfat_view *v,
                               const fauxfat_device *device,
                               ff_xxh32 *component,
                               ff_xxh32 *map)
{
    ff_root_reader r;
    uint64_t root_first = (uint64_t)v->cluster_heap_block +
                          (uint64_t)(v->root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    uint32_t entry = 0u;
    int rc;

    memset(&r, 0, sizeof(r));
    r.device      = device;
    r.first_block = root_first;

    while (entry < FF_ROOT_ENTRIES) {
        uint8_t first[32];

        rc = ff_root_entry(&r, entry, first);
        if (rc != FAUXFAT_OK)
            return rc;
        if (first[0] == FF_ENTRY_FILE) {
            uint8_t set[160];
            unsigned count = (unsigned)first[1] + 1u;
            size_t bytes;

            if ((first[1] != 2u && first[1] != 4u) ||
                entry > FF_ROOT_ENTRIES - count)
                return 0;
            bytes = (size_t)count * 32u;
            rc    = ff_root_set(&r, entry, count, set);
            if (rc != FAUXFAT_OK)
                return rc;
            if (faux_load_le16(set + 2u) != ff_entry_set_checksum(set, bytes))
                return 0;
            ff_reopen_canonicalize_file_set(
                set, bytes, ff_file_length_changes_enabled(v));
            ff_structural_hash_update(component, map, set, bytes);
            entry += count;
        } else {
            ff_structural_hash_update(component, map, first,
                                      sizeof(first));
            ++entry;
        }
    }
    return 1;
}

typedef struct ff_reopen_public_cursor {
    ff_root_reader reader;
    uint32_t entry;
    uint32_t first_cluster;
    uint64_t end_cluster;
    int no_fat_chain;
    int have;
    int done;
} ff_reopen_public_cursor;

static int ff_reopen_public_next(const fauxfat_view *v,
                                 ff_reopen_public_cursor *c)
{
    uint8_t first[32];
    uint8_t set[160];
    fauxfat_disk_file d;
    uint64_t end_cluster;
    int parse_error;
    int rc;

    if (c->done) {
        c->have = 0;
        return 0;
    }
    if (c->entry >= FF_ROOT_ENTRIES) {
        c->done = 1;
        c->have = 0;
        return 0;
    }

    rc = ff_root_entry(&c->reader, c->entry, first);
    if (rc != FAUXFAT_OK)
        return rc;
    if (first[0] == FF_ENTRY_PAD) {
        c->done = 1;
        c->have = 0;
        return 0;
    }

    if (first[0] == FF_ENTRY_PRIVATE_RESERVE &&
        v->config->private_reservation ==
            FAUXFAT_PRIVATE_BENIGN_PRIMARY_AND_BAD) {
        c->done = 1;
        c->have = 0;
        return 0;
    }

    if (first[0] != FF_ENTRY_FILE || first[1] != 4u ||
        c->entry > FF_ROOT_ENTRIES - 5u)
        return FAUXFAT_ESTRUCTURE;

    rc = ff_root_set(&c->reader, c->entry, 5u, set);
    if (rc != FAUXFAT_OK)
        return rc;
    if (faux_load_le16(set + 2u) != ff_entry_set_checksum(set, sizeof(set)))
        return FAUXFAT_ESTRUCTURE;

    if (set[128u] == FF_ENTRY_VENDOR_EXT &&
        memcmp(set + 130u, ff_public_name_guid1, 16u) == 0) {
        if (!ff_parse_public_set(v, c->reader.device, set, &d,
                                 &end_cluster, &parse_error))
            return parse_error != FAUXFAT_OK ? parse_error :
                                               FAUXFAT_ESTRUCTURE;
        c->first_cluster = faux_load_le32(set + 32u + 20u);
        c->end_cluster   = end_cluster;
        c->no_fat_chain  = set[33u] == 0x03u;
        c->entry += 5u;
        c->have = 1;
        return 1;
    }

    if (set[128u] == FF_ENTRY_VENDOR_ALLOC &&
        memcmp(set + 130u, ff_opaque_alloc_guid, 16u) == 0) {
        c->done = 1;
        c->have = 0;
        return 0;
    }

    return FAUXFAT_ESTRUCTURE;
}

static uint32_t ff_reopen_fixed_fat_entry(const fauxfat_view *v,
                                          uint32_t entry)
{
    uint32_t bitmap_last = 2u + v->bitmap_clusters - 1u;

    if (entry == 0u)
        return 0xfffffff8u;
    if (entry == 1u)
        return FF_FAT_EOC;
    if (entry >= 2u && entry <= bitmap_last)
        return entry == bitmap_last ? FF_FAT_EOC : entry + 1u;
    if (entry == v->upcase_cluster || entry == v->root_cluster)
        return FF_FAT_EOC;
    return 0u;
}

static int ff_reopen_verify_fat(const fauxfat_view *v,
                                const fauxfat_device *device,
                                ff_xxh32 *component,
                                ff_xxh32 *map)
{
    ff_reopen_public_cursor cursor;
    uint8_t actual[FAUXFAT_BLOCK_SIZE];
    uint8_t expected[FAUXFAT_BLOCK_SIZE];
    uint64_t bytes       = ((uint64_t)v->cluster_count + 2u) * 4u;
    uint32_t block_index = 0u;
    uint64_t root_first  = (uint64_t)v->cluster_heap_block +
                          (uint64_t)(v->root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    int rc;

    memset(&cursor, 0, sizeof(cursor));
    cursor.reader.device      = device;
    cursor.reader.first_block = root_first;
    cursor.entry              = FF_FILE_SLOT_FIRST;
    rc                        = ff_reopen_public_next(v, &cursor);
    if (rc < 0)
        return rc;

    while (bytes != 0u) {
        uint64_t first_entry = (uint64_t)block_index * 128u;
        size_t n             = bytes > FAUXFAT_BLOCK_SIZE ? FAUXFAT_BLOCK_SIZE : (size_t)bytes;
        unsigned i;

        rc = ff_dev_read_block(device,
                               (uint64_t)FF_FAT_OFFSET_BLOCKS + block_index,
                               actual);
        if (rc != 0)
            return rc;
        memset(expected, 0, sizeof(expected));

        for (i = 0u; i < 128u; ++i) {
            uint64_t e64 = first_entry + i;
            uint32_t value;

            if (e64 > (uint64_t)v->cluster_count + 1u)
                break;
            if (e64 < v->data_first_cluster) {
                value = ff_reopen_fixed_fat_entry(v, (uint32_t)e64);
            } else {
                uint32_t e = (uint32_t)e64;

                while (cursor.have && e >= cursor.end_cluster) {
                    rc = ff_reopen_public_next(v, &cursor);
                    if (rc < 0)
                        return rc;
                }
                if (cursor.have && e >= cursor.first_cluster &&
                    e < cursor.end_cluster) {
                    if (cursor.no_fat_chain)
                        value = 0u;
                    else if ((uint64_t)e + 1u == cursor.end_cluster)
                        value = FF_FAT_EOC;
                    else
                        value = e + 1u;
                }
                else
                    value = 0xfffffff7u;
            }
            faux_store_le32(expected + 4u * i, value);
        }

        if (memcmp(actual, expected, n) != 0)
            return 0;
        ff_structural_hash_update(component, map, actual, n);
        bytes -= n;
        ++block_index;
    }
    return 1;
}

static int ff_reopen_validate_self(const fauxfat_view *v,
                                   const fauxfat_device *device,
                                   const uint8_t main_boot[FAUXFAT_BLOCK_SIZE],
                                   const uint8_t main_oem[FAUXFAT_BLOCK_SIZE],
                                   int boot_strict,
                                   int *valid)
{
    ff_reopen_layout_summary summary;
    uint8_t backup_oem[FAUXFAT_BLOCK_SIZE];
    ff_structural_hash_state hashes;
    ff_structural_hash_values values;
    size_t count = 0u;
    int rc;

    *valid = 0;
    if (!boot_strict || !ff_oem_recognizable(main_oem))
        return FAUXFAT_OK;

    rc = ff_dev_read_block(device, 21u, backup_oem);
    if (rc != 0)
        return rc;
    if (memcmp(main_oem, backup_oem, sizeof(backup_oem)) != 0)
        return FAUXFAT_OK;

    memset(&summary, 0, sizeof(summary));
    summary.view         = v;
    summary.next_cluster = v->data_first_cluster;
    rc                   = fauxfat_parse_root_strict(v, device, ff_reopen_layout_emit,
                                                     &summary, &count);
    if (rc != FAUXFAT_OK) {
        if (rc == FAUXFAT_ESTRUCTURE || rc == FAUXFAT_EGEOMETRY)
            return FAUXFAT_OK;
        return rc;
    }
    if (summary.name_changed)
        return FAUXFAT_OK;

    ff_structural_hash_init(&hashes, main_boot);

    rc = ff_verify_structural_hashes(v, device, &hashes,
                                     ff_reopen_verify_fat,
                                     ff_reopen_hash_root);
    if (rc < 0)
        return rc;
    if (rc == 0)
        return FAUXFAT_OK;

    ff_structural_hash_digest(&hashes, &values);
    if (!ff_oem_hash_fields_valid(main_oem, &values))
        return FAUXFAT_OK;

    *valid = 1;
    return FAUXFAT_OK;
}

int fauxfat_reopen_probe(const fauxfat_device *device,
                         fauxfat_volume_class *classification,
                         fauxfat_reopen_info *info)
{
    fauxfat_config cfg;
    fauxfat_view v;
    fauxfat_reopen_info local_info;
    fauxfat_reopen_info *out = info ? info : &local_info;
    uint8_t main_boot[FAUXFAT_BLOCK_SIZE];
    uint8_t main_oem[FAUXFAT_BLOCK_SIZE];
    uint8_t backup_oem[FAUXFAT_BLOCK_SIZE];
    const uint8_t *identity_oem = NULL;
    int recognized;
    int boot_strict = 0;
    int self_valid  = 0;
    int geometry;
    int rc;

    if (!device || !device->read || !classification)
        return FAUXFAT_EINVAL;
    memset(out, 0, sizeof(*out));
    *classification = FAUXFAT_VOLUME_INVALID;

    rc = ff_dev_read_block(device, 9u, main_oem);
    if (rc != 0)
        return rc;

    geometry = ff_reopen_geometry(device, &v, &cfg, main_boot, &boot_strict);
    if (geometry < 0)
        return geometry;
    if (geometry == 0)
        return FAUXFAT_OK;

    out->partition_lba      = cfg.partition_lba;
    out->volume_blocks      = v.volume_blocks;
    out->volume_serial      = cfg.volume_serial;
    out->fat_length_blocks  = v.fat_length_blocks;
    out->cluster_heap_block = v.cluster_heap_block;
    out->cluster_count      = v.cluster_count;
    out->root_cluster       = v.root_cluster;
    out->flags |= FAUXFAT_REOPEN_GEOMETRY_VALID;

    rc = ff_dev_read_block(device, 21u, backup_oem);
    if (rc != 0)
        return rc;
    if (ff_oem_recognizable(main_oem))
        identity_oem = main_oem;
    else if (ff_oem_recognizable(backup_oem))
        identity_oem = backup_oem;
    recognized = identity_oem != NULL;
    if (recognized) {
        uint16_t flags = faux_load_le16(identity_oem + 70u);
        cfg.private_reservation =
            (flags & FF_OEM_FLAG_BENIGN_RESERVE) != 0u ? FAUXFAT_PRIVATE_BENIGN_PRIMARY_AND_BAD : FAUXFAT_PRIVATE_BAD_CLUSTERS;
        v.mutable_file_lengths =
            (flags & FF_OEM_FLAG_MUTABLE_LENGTH) != 0u;
        out->private_reservation = cfg.private_reservation;
        out->structural_epoch    = faux_load_le64(identity_oem + 72u);
    }

    rc = ff_reopen_root_identity(&v, device, out);
    if (rc != FAUXFAT_OK) {
        /*
         * OEM parameter sectors are not ownership by themselves.  A host
         * quick-format can replace the exFAT boot/root structures while
         * leaving old OEM parameter bytes behind.  Do not promote such stale
         * identity to fauxFAT-changed unless the bounded fauxFAT root profile
         * is still coherent enough for descriptor recovery.
         */
        if (rc == FAUXFAT_ESTRUCTURE || rc == FAUXFAT_EGEOMETRY)
            return FAUXFAT_OK;
        return rc;
    }
    out->flags |= FAUXFAT_REOPEN_ROOT_VALID;
    if (recognized)
        *classification = FAUXFAT_VOLUME_FAUXFAT_CHANGED;

    rc = ff_reopen_validate_self(&v, device, main_boot, main_oem,
                                 boot_strict, &self_valid);
    if (rc != FAUXFAT_OK)
        return rc;
    if (self_valid)
        *classification = FAUXFAT_VOLUME_FAUXFAT_VALID;
    else if (recognized)
        *classification = FAUXFAT_VOLUME_FAUXFAT_CHANGED;
    else
        *classification = FAUXFAT_VOLUME_EXFAT_BEST_EFFORT;

    out->flags |= FAUXFAT_REOPEN_SCAN_READY;
    return FAUXFAT_OK;
}

static int ff_reopen_view_from_info(const fauxfat_reopen_info *info,
                                    fauxfat_config *cfg,
                                    fauxfat_view *v)
{
    ff_reopen_geometry_fields g;

    if (!info || !cfg || !v ||
        (info->flags & FAUXFAT_REOPEN_SCAN_READY) == 0u ||
        (info->private_reservation != FAUXFAT_PRIVATE_BAD_CLUSTERS &&
         info->private_reservation !=
             FAUXFAT_PRIVATE_BENIGN_PRIMARY_AND_BAD) ||
        info->cluster_count == 0u || info->cluster_count > FF_MAX_CLUSTER_COUNT ||
        info->fat_length_blocks == 0u ||
        (info->fat_length_blocks % FAUXFAT_BLOCKS_PER_CLUSTER) != 0u)
        return FAUXFAT_EINVAL;

    memset(&g, 0, sizeof(g));
    g.partition_lba      = info->partition_lba;
    g.volume_blocks      = info->volume_blocks;
    g.volume_serial      = info->volume_serial;
    g.fat_length_blocks  = info->fat_length_blocks;
    g.cluster_heap_block = info->cluster_heap_block;
    g.cluster_count      = info->cluster_count;
    g.root_cluster       = info->root_cluster;
    if (!ff_build_reopen_geometry(&g, cfg, v))
        return FAUXFAT_EGEOMETRY;
    cfg->private_reservation = info->private_reservation;
    return FAUXFAT_OK;
}

int fauxfat_reopen_scan(const fauxfat_device *device,
                        const fauxfat_reopen_info *info,
                        fauxfat_file_emit_fn emit,
                        void *emit_context,
                        size_t *descriptor_count)
{
    fauxfat_config cfg;
    fauxfat_view v;
    int rc;

    if (!device || !device->read || !info)
        return FAUXFAT_EINVAL;
    if (descriptor_count)
        *descriptor_count = 0u;

    rc = ff_reopen_view_from_info(info, &cfg, &v);
    if (rc != FAUXFAT_OK)
        return rc;
    return ff_scan_loose_geometry(&v, device, emit, emit_context,
                                  descriptor_count);
}

int fauxfat_reopen(const fauxfat_device *device,
                   fauxfat_file_emit_fn emit,
                   void *emit_context,
                   size_t *descriptor_count,
                   fauxfat_volume_class *classification,
                   fauxfat_reopen_info *info)
{
    fauxfat_reopen_info local_info;
    fauxfat_reopen_info *out = info ? info : &local_info;
    int rc;

    if (descriptor_count)
        *descriptor_count = 0u;
    rc = fauxfat_reopen_probe(device, classification, out);
    if (rc != FAUXFAT_OK)
        return rc;
    if ((out->flags & FAUXFAT_REOPEN_SCAN_READY) == 0u)
        return FAUXFAT_OK;
    return fauxfat_reopen_scan(device, out, emit, emit_context,
                               descriptor_count);
}

static int ff_disk_file_range_valid(const fauxfat_disk_file *file)
{
    uint64_t allocation_bytes;

    if (!file ||
        (file->kind != FAUXFAT_DISK_FILE_PUBLIC &&
         file->kind != FAUXFAT_DISK_FILE_OPAQUE) ||
        file->data_length == 0u || file->allocation_blocks == 0u ||
        file->allocation_blocks > UINT64_MAX / FAUXFAT_BLOCK_SIZE)
        return 0;
    allocation_bytes = file->allocation_blocks * FAUXFAT_BLOCK_SIZE;
    if (file->data_length > allocation_bytes ||
        file->allocation_blocks > UINT64_MAX - file->first_block)
        return 0;
    return 1;
}

static int ff_disk_file_access_valid(const fauxfat_disk_file *file,
                                     uint64_t offset, size_t length)
{
    if (!ff_disk_file_range_valid(file))
        return FAUXFAT_ESTRUCTURE;
    if (offset > file->data_length ||
        (uint64_t)length > file->data_length - offset)
        return FAUXFAT_ERANGE;
    return FAUXFAT_OK;
}

int fauxfat_disk_file_read(const fauxfat_device *device,
                           const fauxfat_disk_file *file,
                           uint64_t offset,
                           void *data,
                           size_t length)
{
    uint8_t scratch[FAUXFAT_BLOCK_SIZE];
    uint8_t *dst = (uint8_t *)data;
    uint64_t block;
    size_t done = 0u;
    int rc;

    if (!device || !device->read || (!data && length != 0u))
        return FAUXFAT_EINVAL;
    rc = ff_disk_file_access_valid(file, offset, length);
    if (rc != FAUXFAT_OK || length == 0u)
        return rc;

    block = file->first_block + offset / FAUXFAT_BLOCK_SIZE;

    if ((offset % FAUXFAT_BLOCK_SIZE) != 0u) {
        size_t in_block = (size_t)(offset % FAUXFAT_BLOCK_SIZE);
        size_t n        = FAUXFAT_BLOCK_SIZE - in_block;
        if (n > length)
            n = length;
        rc = device->read(device->context, block, 1u, scratch);
        if (rc != 0)
            return rc;
        memcpy(dst, scratch + in_block, n);
        done = n;
        ++block;
    }

    if (length - done >= FAUXFAT_BLOCK_SIZE) {
        size_t blocks = (length - done) / FAUXFAT_BLOCK_SIZE;
        rc            = device->read(device->context, block, blocks, dst + done);
        if (rc != 0)
            return rc;
        done += blocks * FAUXFAT_BLOCK_SIZE;
        block += blocks;
    }

    if (done < length) {
        rc = device->read(device->context, block, 1u, scratch);
        if (rc != 0)
            return rc;
        memcpy(dst + done, scratch, length - done);
    }

    return FAUXFAT_OK;
}

int fauxfat_disk_file_write(const fauxfat_device *device,
                            const fauxfat_disk_file *file,
                            uint64_t offset,
                            const void *data,
                            size_t length)
{
    uint8_t scratch[FAUXFAT_BLOCK_SIZE];
    const uint8_t *src = (const uint8_t *)data;
    uint64_t block;
    size_t done = 0u;
    int rc;

    if (!device || !device->write || (!data && length != 0u))
        return FAUXFAT_EINVAL;
    rc = ff_disk_file_access_valid(file, offset, length);
    if (rc != FAUXFAT_OK || length == 0u)
        return rc;

    block = file->first_block + offset / FAUXFAT_BLOCK_SIZE;

    if ((offset % FAUXFAT_BLOCK_SIZE) != 0u) {
        size_t in_block = (size_t)(offset % FAUXFAT_BLOCK_SIZE);
        size_t n        = FAUXFAT_BLOCK_SIZE - in_block;
        if (n > length)
            n = length;
        if (!device->read)
            return FAUXFAT_EINVAL;
        rc = device->read(device->context, block, 1u, scratch);
        if (rc != 0)
            return rc;
        memcpy(scratch + in_block, src, n);
        rc = device->write(device->context, block, 1u, scratch);
        if (rc != 0)
            return rc;
        done = n;
        ++block;
    }

    if (length - done >= FAUXFAT_BLOCK_SIZE) {
        size_t blocks = (length - done) / FAUXFAT_BLOCK_SIZE;
        rc            = device->write(device->context, block, blocks, src + done);
        if (rc != 0)
            return rc;
        done += blocks * FAUXFAT_BLOCK_SIZE;
        block += blocks;
    }

    if (done < length) {
        if (!device->read)
            return FAUXFAT_EINVAL;
        rc = device->read(device->context, block, 1u, scratch);
        if (rc != 0)
            return rc;
        memcpy(scratch, src + done, length - done);
        rc = device->write(device->context, block, 1u, scratch);
        if (rc != 0)
            return rc;
    }

    return FAUXFAT_OK;
}
