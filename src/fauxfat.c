#include "fauxfat.h"

#include <limits.h>
#include <string.h>

#define FF_FAT_OFFSET_BLOCKS 128u
#define FF_UPCASE_BYTES      128u
#define FF_ROOT_ENTRIES      2048u
#define FF_FILE_SLOT_FIRST   4u
#define FF_FILE_SLOT_COUNT   681u
#define FF_FAT_EOC           0xffffffffu
#define FF_MAX_CLUSTER_COUNT 0xfffffff5u

#define FF_ENTRY_BITMAP 0x81u
#define FF_ENTRY_UPCASE 0x82u
#define FF_ENTRY_LABEL  0x83u
#define FF_ENTRY_FILE   0x85u
#define FF_ENTRY_GUID   0xa0u
#define FF_ENTRY_PAD    0xa1u
#define FF_ENTRY_STREAM 0xc0u
#define FF_ENTRY_NAME   0xc1u

#define FF_UNIX_1980 315532800LL
#define FF_UNIX_2108 4354819200LL

static const uint8_t ff_oem_map_guid[16] = {
    0xe7, 0xe6, 0x5b, 0x99, 0x45, 0x34, 0xdc, 0x46,
    0xa2, 0x13, 0x74, 0xd9, 0x85, 0xb3, 0x01, 0x34
};

static const uint8_t ff_oem_epoch_guid[16] = {
    0xdb, 0x83, 0x72, 0x8b, 0xea, 0xb9, 0xdb, 0x4a,
    0xa3, 0x27, 0x9b, 0x59, 0x73, 0x63, 0x47, 0x9b
};

static void ff_store16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void ff_store32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void ff_store64(uint8_t *p, uint64_t v)
{
    ff_store32(p, (uint32_t)v);
    ff_store32(p + 4, (uint32_t)(v >> 32));
}

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

static int ff_guid_is_zero(const uint8_t guid[16])
{
    unsigned i;
    uint8_t x = 0;

    for (i = 0; i < 16; ++i)
        x |= guid[i];
    return x == 0;
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

/* CRC-32C/Castagnoli, reflected polynomial, initial/final xor all ones. */
static uint32_t ff_crc32c_update(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t i;
    unsigned bit;

    for (i = 0; i < n; ++i) {
        crc ^= p[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
    }
    return crc;
}

static uint32_t ff_crc32c(const uint8_t *p, size_t n)
{
    return ~ff_crc32c_update(0xffffffffu, p, n);
}

typedef struct ff_sha256 {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
} ff_sha256;

static uint32_t ff_rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void ff_sha256_compress(ff_sha256 *s, const uint8_t block[64])
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned i;

    for (i = 0; i < 16; ++i) {
        const uint8_t *p = block + 4u * i;
        w[i]             = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    for (; i < 64; ++i) {
        uint32_t x  = w[i - 15];
        uint32_t y  = w[i - 2];
        uint32_t s0 = ff_rotr32(x, 7) ^ ff_rotr32(x, 18) ^ (x >> 3);
        uint32_t s1 = ff_rotr32(y, 17) ^ ff_rotr32(y, 19) ^ (y >> 10);
        w[i]        = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    f = s->h[5];
    g = s->h[6];
    h = s->h[7];

    for (i = 0; i < 64; ++i) {
        uint32_t s1  = ff_rotr32(e, 6) ^ ff_rotr32(e, 11) ^ ff_rotr32(e, 25);
        uint32_t ch  = (e & f) ^ (~e & g);
        uint32_t t1  = h + s1 + ch + k[i] + w[i];
        uint32_t s0  = ff_rotr32(a, 2) ^ ff_rotr32(a, 13) ^ ff_rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2  = s0 + maj;
        h            = g;
        g            = f;
        f            = e;
        e            = d + t1;
        d            = c;
        c            = b;
        b            = a;
        a            = t1 + t2;
    }

    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

static void ff_sha256_init(ff_sha256 *s)
{
    static const uint32_t h0[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(s->h, h0, sizeof(h0));
    s->bytes = 0;
    s->used  = 0;
}

static void ff_sha256_update(ff_sha256 *s, const uint8_t *p, size_t n)
{
    s->bytes += n;
    while (n) {
        size_t take = sizeof(s->block) - s->used;
        if (take > n)
            take = n;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == sizeof(s->block)) {
            ff_sha256_compress(s, s->block);
            s->used = 0;
        }
    }
}

static void ff_sha256_final(ff_sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->bytes * 8u;
    unsigned i;

    s->block[s->used++] = 0x80u;
    if (s->used > 56u) {
        memset(s->block + s->used, 0, 64u - s->used);
        ff_sha256_compress(s, s->block);
        s->used = 0;
    }
    memset(s->block + s->used, 0, 56u - s->used);
    for (i = 0; i < 8; ++i)
        s->block[63u - i] = (uint8_t)(bits >> (8u * i));
    ff_sha256_compress(s, s->block);

    for (i = 0; i < 8; ++i) {
        out[4u * i + 0] = (uint8_t)(s->h[i] >> 24);
        out[4u * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4u * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4u * i + 3] = (uint8_t)s->h[i];
    }
}

static uint64_t ff_file_clusters(const fauxfat_file *f)
{
    return ((f->size - 1u) / FAUXFAT_CLUSTER_SIZE) + 1u;
}

static uint32_t ff_file_first_cluster(const fauxfat_view *v, size_t file_index)
{
    uint64_t c = v->data_first_cluster;
    size_t i;

    for (i = 0; i < file_index; ++i)
        c += ff_file_clusters(&v->config->files[i]);
    return (uint32_t)c;
}

static uint32_t ff_fat_entry(const fauxfat_view *v, uint32_t entry)
{
    uint32_t bitmap_first = 2u;
    uint32_t bitmap_last  = bitmap_first + v->bitmap_clusters - 1u;

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

    /* v1 packs every remaining cluster into a visible NoFatChain file. */
    return 0u;
}

static void ff_make_boot_sector(const fauxfat_view *v, uint8_t out[512])
{
    memset(out, 0, 512);
    out[0] = 0xeb;
    out[1] = 0x76;
    out[2] = 0x90;
    memcpy(out + 3, "EXFAT   ", 8);
    ff_store64(out + 64, v->config->partition_lba);
    ff_store64(out + 72, v->volume_blocks);
    ff_store32(out + 80, FF_FAT_OFFSET_BLOCKS);
    ff_store32(out + 84, v->fat_length_blocks);
    ff_store32(out + 88, v->cluster_heap_block);
    ff_store32(out + 92, v->cluster_count);
    ff_store32(out + 96, v->root_cluster);
    ff_store32(out + 100, v->config->volume_serial);
    ff_store16(out + 104, 0x0100u);
    ff_store16(out + 106, 0u);
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
    memcpy(out + 16, v->map_sha256, 32);

    memcpy(out + 48, ff_oem_epoch_guid, 16);
    memset(payload, 0, sizeof(payload));
    memcpy(payload, "FFV1", 4);
    ff_store16(payload + 4, 1u);
    ff_store16(payload + 6, 0u);
    ff_store64(payload + 8, v->config->structural_epoch);
    ff_store32(payload + 16, v->fat_crc32c);
    ff_store32(payload + 20, v->bitmap_crc32c);
    ff_store32(payload + 24, v->root_crc32c);
    ff_store32(payload + 28, ff_crc32c(payload, 28));
    memcpy(out + 64, payload, sizeof(payload));
}

static void ff_make_checksum_sector(const fauxfat_view *v, uint8_t out[512])
{
    unsigned i;
    for (i = 0; i < 128; ++i)
        ff_store32(out + 4u * i, v->boot_checksum);
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

static void ff_make_root_entry(const fauxfat_view *v,
                               uint32_t entry_index,
                               uint8_t out[32])
{
    const fauxfat_config *cfg = v->config;

    memset(out, 0, 32);

    if (entry_index == 0u) {
        uint64_t bitmap_bytes = ((uint64_t)v->cluster_count + 7u) / 8u;
        out[0]                = FF_ENTRY_BITMAP;
        ff_store32(out + 20, 2u);
        ff_store64(out + 24, bitmap_bytes);
        return;
    }

    if (entry_index == 1u) {
        out[0] = FF_ENTRY_UPCASE;
        ff_store32(out + 4, v->upcase_checksum);
        ff_store32(out + 20, v->upcase_cluster);
        ff_store64(out + 24, FF_UPCASE_BYTES);
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
            ff_store16(out + 2u + 2u * i, (uint8_t)label[i]);
        return;
    }

    if (entry_index == 3u) {
        out[0] = FF_ENTRY_GUID;
        memcpy(out + 6, cfg->volume_guid, 16);
        ff_store16(out + 2, ff_entry_set_checksum(out, 32));
        return;
    }

    if (entry_index >= FF_FILE_SLOT_FIRST && entry_index < 2047u) {
        uint32_t rel    = entry_index - FF_FILE_SLOT_FIRST;
        size_t slot     = rel / 3u;
        unsigned member = rel % 3u;

        if (slot < cfg->file_count) {
            const fauxfat_file *f = &cfg->files[slot];
            size_t name_len       = 0;
            uint8_t set[96];
            uint8_t *file_ent   = set;
            uint8_t *stream_ent = set + 32;
            uint8_t *name_ent   = set + 64;
            size_t i;
            uint32_t timestamp;
            uint8_t ten_ms;

            (void)ff_name_valid(f->name, &name_len);
            (void)ff_exfat_timestamp(f->mtime, &timestamp, &ten_ms);
            memset(set, 0, sizeof(set));

            file_ent[0] = FF_ENTRY_FILE;
            file_ent[1] = 2u;
            ff_store32(file_ent + 8, timestamp);
            ff_store32(file_ent + 12, timestamp);
            ff_store32(file_ent + 16, timestamp);
            file_ent[20] = ten_ms;
            file_ent[21] = ten_ms;
            file_ent[22] = 0x80u;
            file_ent[23] = 0x80u;
            file_ent[24] = 0x80u;

            stream_ent[0] = FF_ENTRY_STREAM;
            stream_ent[1] = 0x03u;
            stream_ent[3] = (uint8_t)name_len;
            ff_store16(stream_ent + 4, ff_name_hash(f->name));
            ff_store64(stream_ent + 8, f->size);
            ff_store32(stream_ent + 20, ff_file_first_cluster(v, slot));
            ff_store64(stream_ent + 24, f->size);

            name_ent[0] = FF_ENTRY_NAME;
            for (i = 0; i < name_len; ++i)
                ff_store16(name_ent + 2u + 2u * i, (uint8_t)f->name[i]);

            ff_store16(file_ent + 2, ff_entry_set_checksum(set, sizeof(set)));
            memcpy(out, set + member * 32u, 32);
            return;
        }
    }

    out[0] = FF_ENTRY_PAD;
    ff_store16(out + 2, 0x0508u);
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
        ff_store32(out + 4u * i, ff_fat_entry(v, (uint32_t)entry));
    }
}

static int ff_locate_file_block(const fauxfat_view *v,
                                uint32_t cluster,
                                uint32_t block_in_cluster,
                                size_t *file_index,
                                uint64_t *file_offset,
                                size_t *length);

static int ff_read_file_block(const fauxfat_view *v,
                              uint32_t cluster,
                              uint32_t block_in_cluster,
                              uint8_t out[512])
{
    size_t file_index;
    uint64_t file_offset;
    size_t length;
    int rc = ff_locate_file_block(v, cluster, block_in_cluster,
                                  &file_index, &file_offset, &length);

    if (rc != FAUXFAT_OK)
        return rc;

    memset(out, 0, 512);
    if (length) {
        const fauxfat_file *f = &v->config->files[file_index];
        rc                    = v->config->read(v->config->io_context, f->fd,
                                                file_offset, out, length);
        if (rc != 0)
            return rc;
    }
    return FAUXFAT_OK;
}

static int ff_locate_file_block(const fauxfat_view *v,
                                uint32_t cluster,
                                uint32_t block_in_cluster,
                                size_t *file_index,
                                uint64_t *file_offset,
                                size_t *length)
{
    uint32_t first = v->data_first_cluster;
    size_t i;

    for (i = 0; i < v->config->file_count; ++i) {
        const fauxfat_file *f = &v->config->files[i];
        uint32_t nclusters    = (uint32_t)ff_file_clusters(f);

        if (cluster >= first && cluster < first + nclusters) {
            uint64_t off = ((uint64_t)(cluster - first) * FAUXFAT_CLUSTER_SIZE) +
                           ((uint64_t)block_in_cluster * FAUXFAT_BLOCK_SIZE);
            size_t n = 0;

            if (off < f->size) {
                uint64_t left = f->size - off;
                n             = left > FAUXFAT_BLOCK_SIZE ? FAUXFAT_BLOCK_SIZE : (size_t)left;
            }

            if (file_index)
                *file_index = i;
            if (file_offset)
                *file_offset = off;
            if (length)
                *length = n;
            return FAUXFAT_OK;
        }
        first += nclusters;
    }

    return FAUXFAT_EGEOMETRY;
}

static int ff_translate_data_block(const fauxfat_view *v,
                                   uint64_t block_address,
                                   fauxfat_write_mapping *mapping)
{
    uint64_t heap_rel;
    uint32_t cluster;
    uint32_t block_in_cluster;
    size_t file_index;
    uint64_t file_offset;
    size_t length;
    int rc;

    if (block_address >= v->volume_blocks)
        return FAUXFAT_ERANGE;
    if (block_address < v->cluster_heap_block)
        return FAUXFAT_EUNMAPPED;

    heap_rel         = block_address - v->cluster_heap_block;
    cluster          = 2u + (uint32_t)(heap_rel / FAUXFAT_BLOCKS_PER_CLUSTER);
    block_in_cluster = (uint32_t)(heap_rel % FAUXFAT_BLOCKS_PER_CLUSTER);

    if (cluster < v->data_first_cluster)
        return FAUXFAT_EUNMAPPED;

    rc = ff_locate_file_block(v, cluster, block_in_cluster,
                              &file_index, &file_offset, &length);
    if (rc != FAUXFAT_OK)
        return rc;
    if (length == 0u)
        return FAUXFAT_EUNMAPPED;

    mapping->file_index  = file_index;
    mapping->fd          = v->config->files[file_index].fd;
    mapping->file_offset = file_offset;
    mapping->length      = length;
    return FAUXFAT_OK;
}

static void ff_compute_component_hashes(fauxfat_view *v)
{
    uint8_t block[512];
    uint32_t crc;
    ff_sha256 sha;
    uint8_t boot[512];
    uint8_t prefix[8]             = { 'F', 'F', 'M', 'A', 'P', '1', 0, 0 };
    uint64_t bitmap_bytes         = ((uint64_t)v->cluster_count + 7u) / 8u;
    uint64_t fat_meaningful_bytes = ((uint64_t)v->cluster_count + 2u) * 4u;
    uint32_t fat_blocks           = (uint32_t)((fat_meaningful_bytes + 511u) / 512u);
    uint32_t i;

    crc = 0xffffffffu;
    for (i = 0; i < fat_blocks; ++i) {
        uint64_t left = fat_meaningful_bytes - (uint64_t)i * 512u;
        size_t n      = left > 512u ? 512u : (size_t)left;
        ff_make_fat_block(v, i, block);
        crc = ff_crc32c_update(crc, block, n);
    }
    v->fat_crc32c = ~crc;

    crc = 0xffffffffu;
    for (i = 0; (uint64_t)i * 512u < bitmap_bytes; ++i) {
        uint64_t left = bitmap_bytes - (uint64_t)i * 512u;
        size_t n      = left > 512u ? 512u : (size_t)left;
        ff_make_bitmap_block(v, i, block);
        crc = ff_crc32c_update(crc, block, n);
    }
    v->bitmap_crc32c = ~crc;

    crc = 0xffffffffu;
    for (i = 0; i < FAUXFAT_BLOCKS_PER_CLUSTER; ++i) {
        ff_make_root_block(v, i, block);
        crc = ff_crc32c_update(crc, block, 512);
    }
    v->root_crc32c = ~crc;

    ff_sha256_init(&sha);
    ff_sha256_update(&sha, prefix, sizeof(prefix));
    ff_make_boot_sector(v, boot);
    ff_sha256_update(&sha, boot + 64, 42);
    ff_sha256_update(&sha, boot + 108, 4);

    for (i = 0; i < fat_blocks; ++i) {
        uint64_t left = fat_meaningful_bytes - (uint64_t)i * 512u;
        size_t n      = left > 512u ? 512u : (size_t)left;
        ff_make_fat_block(v, i, block);
        ff_sha256_update(&sha, block, n);
    }
    for (i = 0; (uint64_t)i * 512u < bitmap_bytes; ++i) {
        uint64_t left = bitmap_bytes - (uint64_t)i * 512u;
        size_t n      = left > 512u ? 512u : (size_t)left;
        ff_make_bitmap_block(v, i, block);
        ff_sha256_update(&sha, block, n);
    }
    for (i = 0; i < FAUXFAT_BLOCKS_PER_CLUSTER; ++i) {
        ff_make_root_block(v, i, block);
        ff_sha256_update(&sha, block, 512);
    }
    ff_sha256_final(&sha, v->map_sha256);
}

static uint32_t ff_compute_boot_checksum(const fauxfat_view *v)
{
    uint8_t block[512];
    uint32_t sum = 0;
    uint32_t sector;
    unsigned byte;

    for (sector = 0; sector <= 10; ++sector) {
        if (sector == 0)
            ff_make_boot_sector(v, block);
        else if (sector <= 8)
            ff_make_extended_boot(block);
        else if (sector == 9)
            ff_make_oem_sector(v, block);
        else
            memset(block, 0, 512);

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
    uint64_t file_clusters   = 0;
    uint32_t bitmap_clusters = 1u;
    uint64_t clusters64;
    uint64_t fat_bytes;
    size_t i, j;
    size_t dummy;

    if (!v || !cfg || (cfg->file_count && (!cfg->files || !cfg->read || !cfg->write)) ||
        cfg->file_count > FAUXFAT_MAX_FILES || ff_guid_is_zero(cfg->volume_guid) ||
        !ff_label_valid(cfg->volume_label, &dummy))
        return FAUXFAT_EINVAL;

    memset(v, 0, sizeof(*v));
    v->config = cfg;

    for (i = 0; i < cfg->file_count; ++i) {
        const fauxfat_file *f = &cfg->files[i];
        uint32_t ignored_timestamp;
        uint8_t ignored_ten_ms;

        if (!ff_name_valid(f->name, &dummy) || f->size == 0u ||
            !ff_exfat_timestamp(f->mtime, &ignored_timestamp, &ignored_ten_ms))
            return FAUXFAT_EINVAL;
        for (j = 0; j < i; ++j) {
            if (ff_name_equal_folded(f->name, cfg->files[j].name))
                return FAUXFAT_EINVAL;
        }
        if (ff_file_clusters(f) > UINT32_MAX ||
            file_clusters > UINT64_MAX - ff_file_clusters(f))
            return FAUXFAT_EGEOMETRY;
        file_clusters += ff_file_clusters(f);
    }

    for (;;) {
        uint64_t bitmap_bytes;
        uint64_t need;
        clusters64 = file_clusters + bitmap_clusters + 2u; /* upcase + root */
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

int fauxfat_read_block(const fauxfat_view *v,
                       uint64_t block_address,
                       uint8_t out[FAUXFAT_BLOCK_SIZE])
{
    uint64_t heap_rel;
    uint32_t cluster;
    uint32_t block_in_cluster;

    if (!v || !v->config || !out)
        return FAUXFAT_EINVAL;
    if (block_address >= v->volume_blocks)
        return FAUXFAT_ERANGE;

    if (block_address == 0u || block_address == 12u) {
        ff_make_boot_sector(v, out);
        return FAUXFAT_OK;
    }
    if ((block_address >= 1u && block_address <= 8u) ||
        (block_address >= 13u && block_address <= 20u)) {
        ff_make_extended_boot(out);
        return FAUXFAT_OK;
    }
    if (block_address == 9u || block_address == 21u) {
        ff_make_oem_sector(v, out);
        return FAUXFAT_OK;
    }
    if (block_address == 10u || block_address == 22u ||
        (block_address >= 24u && block_address < FF_FAT_OFFSET_BLOCKS)) {
        memset(out, 0, 512);
        return FAUXFAT_OK;
    }
    if (block_address == 11u || block_address == 23u) {
        ff_make_checksum_sector(v, out);
        return FAUXFAT_OK;
    }

    if (block_address >= FF_FAT_OFFSET_BLOCKS &&
        block_address < FF_FAT_OFFSET_BLOCKS + v->fat_length_blocks) {
        ff_make_fat_block(v, (uint32_t)(block_address - FF_FAT_OFFSET_BLOCKS), out);
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
        ff_make_bitmap_block(v, bitmap_block, out);
        return FAUXFAT_OK;
    }

    if (cluster == v->upcase_cluster) {
        ff_make_upcase_block(block_in_cluster, out);
        return FAUXFAT_OK;
    }

    if (cluster == v->root_cluster) {
        ff_make_root_block(v, block_in_cluster, out);
        return FAUXFAT_OK;
    }

    return ff_read_file_block(v, cluster, block_in_cluster, out);
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
        fauxfat_write_mapping mapping;
        int rc = ff_translate_data_block(v, first_block + i, &mapping);

        if (rc == FAUXFAT_OK) {
            const fauxfat_file *f = &v->config->files[mapping.file_index];
            uint64_t requested    = (uint64_t)(block_count - i) * FAUXFAT_BLOCK_SIZE;
            uint64_t available    = f->size - mapping.file_offset;
            size_t length         = (size_t)(requested < available ? requested : available);
            size_t blocks         = (length + FAUXFAT_BLOCK_SIZE - 1u) / FAUXFAT_BLOCK_SIZE;
            size_t rendered       = blocks * FAUXFAT_BLOCK_SIZE;

            rc = v->config->read(v->config->io_context, mapping.fd,
                                 mapping.file_offset,
                                 out + i * FAUXFAT_BLOCK_SIZE,
                                 length);
            if (rc != 0)
                return rc;
            if (rendered > length)
                memset(out + i * FAUXFAT_BLOCK_SIZE + length, 0,
                       rendered - length);
            i += blocks;
            continue;
        }

        if (rc != FAUXFAT_EUNMAPPED)
            return rc;

        rc = fauxfat_read_block(v, first_block + i,
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

int fauxfat_write_block(const fauxfat_view *v,
                        uint64_t block_address,
                        const uint8_t in[FAUXFAT_BLOCK_SIZE])
{
    fauxfat_write_mapping mapping;
    int rc;

    if (!in)
        return FAUXFAT_EINVAL;

    rc = fauxfat_translate_write(v, block_address, &mapping);
    if (rc != FAUXFAT_OK)
        return rc;

    return v->config->write(v->config->io_context, mapping.fd,
                            mapping.file_offset, in, mapping.length);
}

int fauxfat_write_blocks(const fauxfat_view *v,
                         uint64_t first_block,
                         size_t block_count,
                         const uint8_t *in)
{
    size_t i;

    if (!v || (!in && block_count))
        return FAUXFAT_EINVAL;
    if (block_count > SIZE_MAX / FAUXFAT_BLOCK_SIZE)
        return FAUXFAT_ERANGE;
    if ((uint64_t)block_count > UINT64_MAX - first_block ||
        first_block + (uint64_t)block_count > v->volume_blocks)
        return FAUXFAT_ERANGE;

    /* Validate the complete request before changing any caller-owned bytes. */
    for (i = 0; i < block_count; ++i) {
        fauxfat_write_mapping mapping;
        int rc = fauxfat_translate_write(v, first_block + i, &mapping);
        if (rc != FAUXFAT_OK)
            return rc;
    }

    for (i = 0; i < block_count;) {
        fauxfat_write_mapping mapping;
        const fauxfat_file *f;
        uint64_t requested;
        uint64_t available;
        size_t length;
        size_t blocks;
        int rc = fauxfat_translate_write(v, first_block + i, &mapping);

        if (rc != FAUXFAT_OK)
            return rc; /* preflight above means this indicates internal drift */

        f         = &v->config->files[mapping.file_index];
        requested = (uint64_t)(block_count - i) * FAUXFAT_BLOCK_SIZE;
        available = f->size - mapping.file_offset;
        length    = (size_t)(requested < available ? requested : available);
        blocks    = (length + FAUXFAT_BLOCK_SIZE - 1u) / FAUXFAT_BLOCK_SIZE;

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
