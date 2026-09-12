#include "fauxgpt.h"

#include <limits.h>
#include <string.h>

const uint8_t fauxgpt_type_microsoft_basic_data[16] = {
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
    0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
};

static void fg_store16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void fg_store32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void fg_store64(uint8_t *p, uint64_t v)
{
    fg_store32(p, (uint32_t)v);
    fg_store32(p + 4, (uint32_t)(v >> 32));
}

static int fg_guid_zero(const uint8_t guid[16])
{
    unsigned i;

    for (i = 0; i < 16u; ++i) {
        if (guid[i] != 0u)
            return 0;
    }
    return 1;
}

static uint32_t fg_crc32_update(uint32_t crc, const void *data, size_t length)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;

    for (i = 0; i < length; ++i) {
        unsigned bit;
        crc ^= p[i];
        for (bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return crc;
}

static uint32_t fg_crc32(const void *data, size_t length)
{
    return ~fg_crc32_update(UINT32_MAX, data, length);
}

static int fg_name_valid(const char *name)
{
    size_t n = 0u;

    if (!name)
        return 1;
    while (name[n] != '\0') {
        unsigned char c = (unsigned char)name[n];
        if (n >= FAUXGPT_NAME_MAX || c < 0x20u || c > 0x7eu)
            return 0;
        ++n;
    }
    return 1;
}

static void fg_render_entry(const fauxgpt_view *view, size_t index,
                            uint8_t out[FAUXGPT_ENTRY_SIZE])
{
    const fauxgpt_partition *p;
    size_t i;

    memset(out, 0, FAUXGPT_ENTRY_SIZE);
    if (index >= view->layout->partition_count)
        return;

    p = &view->layout->partitions[index];
    memcpy(out, p->type_guid, 16u);
    memcpy(out + 16u, p->unique_guid, 16u);
    fg_store64(out + 32u, p->first_lba);
    fg_store64(out + 40u, p->first_lba + p->block_count - 1u);
    fg_store64(out + 48u, p->attributes);

    if (!p->name)
        return;
    for (i = 0u; i < FAUXGPT_NAME_MAX && p->name[i] != '\0'; ++i)
        fg_store16(out + 56u + 2u * i, (uint16_t)(uint8_t)p->name[i]);
}

static void fg_render_array_block(const fauxgpt_view *view, unsigned array_block,
                                  uint8_t out[FAUXGPT_BLOCK_SIZE])
{
    size_t first_entry = (size_t)array_block *
                         (FAUXGPT_BLOCK_SIZE / FAUXGPT_ENTRY_SIZE);
    unsigned slot;

    memset(out, 0, FAUXGPT_BLOCK_SIZE);
    for (slot = 0u; slot < FAUXGPT_BLOCK_SIZE / FAUXGPT_ENTRY_SIZE; ++slot)
        fg_render_entry(view, first_entry + slot,
                        out + slot * FAUXGPT_ENTRY_SIZE);
}

static void fg_render_header(const fauxgpt_view *view, int backup,
                             uint8_t out[FAUXGPT_BLOCK_SIZE])
{
    uint64_t my_lba        = backup ? view->backup_header_lba : FAUXGPT_PRIMARY_HEADER_LBA;
    uint64_t alternate_lba = backup ? FAUXGPT_PRIMARY_HEADER_LBA : view->backup_header_lba;
    uint64_t array_lba     = backup ? view->backup_array_lba : FAUXGPT_PRIMARY_ARRAY_LBA;
    uint32_t crc;

    memset(out, 0, FAUXGPT_BLOCK_SIZE);
    memcpy(out, "EFI PART", 8u);
    fg_store32(out + 8u, 0x00010000u);
    fg_store32(out + 12u, 92u);
    fg_store64(out + 24u, my_lba);
    fg_store64(out + 32u, alternate_lba);
    fg_store64(out + 40u, FAUXGPT_FIRST_USABLE_LBA);
    fg_store64(out + 48u, view->last_usable_lba);
    memcpy(out + 56u, view->layout->disk_guid, 16u);
    fg_store64(out + 72u, array_lba);
    fg_store32(out + 80u, FAUXGPT_ENTRY_COUNT);
    fg_store32(out + 84u, FAUXGPT_ENTRY_SIZE);
    fg_store32(out + 88u, view->partition_array_crc32);

    crc = fg_crc32(out, 92u);
    fg_store32(out + 16u, crc);
}

static void fg_render_pmbr(const fauxgpt_view *view,
                           uint8_t out[FAUXGPT_BLOCK_SIZE])
{
    uint64_t covered  = view->layout->disk_blocks - 1u;
    uint32_t size_lba = covered > UINT32_MAX ? UINT32_MAX : (uint32_t)covered;
    uint8_t *entry;

    memset(out, 0, FAUXGPT_BLOCK_SIZE);
    entry    = out + 446u;
    entry[0] = 0x00u;
    entry[1] = 0x00u;
    entry[2] = 0x02u;
    entry[3] = 0x00u;
    entry[4] = 0xeeu;
    entry[5] = 0xffu;
    entry[6] = 0xffu;
    entry[7] = 0xffu;
    fg_store32(entry + 8u, 1u);
    fg_store32(entry + 12u, size_lba);
    out[510] = 0x55u;
    out[511] = 0xaau;
}

int fauxgpt_init(fauxgpt_view *view, const fauxgpt_layout *layout)
{
    uint8_t block[FAUXGPT_BLOCK_SIZE];
    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned b;

    if (!view || !layout ||
        (layout->partition_count != 0u && !layout->partitions) ||
        fg_guid_zero(layout->disk_guid))
        return FAUXGPT_EINVAL;
    if (layout->partition_count > FAUXGPT_ENTRY_COUNT)
        return FAUXGPT_ERANGE;
    if (layout->disk_blocks < 68u)
        return FAUXGPT_EGEOMETRY;

    memset(view, 0, sizeof(*view));
    view->layout            = layout;
    view->backup_header_lba = layout->disk_blocks - 1u;
    view->backup_array_lba  = layout->disk_blocks - 1u -
                             FAUXGPT_ENTRY_ARRAY_BLOCKS;
    view->last_usable_lba = view->backup_array_lba - 1u;

    if (view->last_usable_lba < FAUXGPT_FIRST_USABLE_LBA)
        return FAUXGPT_EGEOMETRY;

    for (i = 0u; i < layout->partition_count; ++i) {
        const fauxgpt_partition *p = &layout->partitions[i];
        uint64_t last;
        size_t j;

        if (fg_guid_zero(p->type_guid) || fg_guid_zero(p->unique_guid) ||
            p->block_count == 0u || !fg_name_valid(p->name))
            return FAUXGPT_EINVAL;
        if (p->first_lba < FAUXGPT_FIRST_USABLE_LBA ||
            p->block_count - 1u > UINT64_MAX - p->first_lba)
            return FAUXGPT_EGEOMETRY;
        last = p->first_lba + p->block_count - 1u;
        if (last > view->last_usable_lba)
            return FAUXGPT_EGEOMETRY;

        for (j = 0u; j < i; ++j) {
            const fauxgpt_partition *q = &layout->partitions[j];
            uint64_t qlast             = q->first_lba + q->block_count - 1u;

            if (memcmp(p->unique_guid, q->unique_guid, 16u) == 0)
                return FAUXGPT_EINVAL;
            if (!(last < q->first_lba || qlast < p->first_lba))
                return FAUXGPT_EGEOMETRY;
        }
    }

    for (b = 0u; b < FAUXGPT_ENTRY_ARRAY_BLOCKS; ++b) {
        fg_render_array_block(view, b, block);
        crc = fg_crc32_update(crc, block, sizeof(block));
    }
    view->partition_array_crc32 = ~crc;
    return FAUXGPT_OK;
}

int fauxgpt_render_block(const fauxgpt_view *view,
                         uint64_t block_address,
                         uint8_t out[FAUXGPT_BLOCK_SIZE])
{
    if (!view || !view->layout || !out)
        return FAUXGPT_EINVAL;
    if (block_address >= view->layout->disk_blocks)
        return FAUXGPT_ERANGE;

    if (block_address == 0u) {
        fg_render_pmbr(view, out);
        return FAUXGPT_OK;
    }

    if (block_address == FAUXGPT_PRIMARY_HEADER_LBA) {
        fg_render_header(view, 0, out);
        return FAUXGPT_OK;
    }

    if (block_address >= FAUXGPT_PRIMARY_ARRAY_LBA &&
        block_address < FAUXGPT_PRIMARY_ARRAY_LBA +
                            FAUXGPT_ENTRY_ARRAY_BLOCKS) {
        fg_render_array_block(view,
                              (unsigned)(block_address -
                                         FAUXGPT_PRIMARY_ARRAY_LBA),
                              out);
        return FAUXGPT_OK;
    }

    if (block_address >= view->backup_array_lba &&
        block_address < view->backup_header_lba) {
        fg_render_array_block(view,
                              (unsigned)(block_address -
                                         view->backup_array_lba),
                              out);
        return FAUXGPT_OK;
    }

    if (block_address == view->backup_header_lba) {
        fg_render_header(view, 1, out);
        return FAUXGPT_OK;
    }
    return FAUXGPT_EUNMAPPED;
}

static int fg_write_block(const fauxgpt_view *view,
                          const fauxgpt_device *device,
                          uint64_t lba,
                          uint8_t block[FAUXGPT_BLOCK_SIZE])
{
    int rc = fauxgpt_render_block(view, lba, block);

    if (rc != FAUXGPT_OK)
        return rc;
    return device->write(device->context, lba, 1u, block);
}

int fauxgpt_format(const fauxgpt_view *view, const fauxgpt_device *device)
{
    uint8_t block[FAUXGPT_BLOCK_SIZE];
    uint64_t lba;
    int rc;

    if (!view || !view->layout || !device || !device->write || !device->flush)
        return FAUXGPT_EINVAL;

    for (lba = view->backup_array_lba; lba < view->backup_header_lba; ++lba) {
        rc = fg_write_block(view, device, lba, block);
        if (rc != 0)
            return rc;
    }
    rc = fg_write_block(view, device, view->backup_header_lba, block);
    if (rc != 0)
        return rc;
    rc = device->flush(device->context);
    if (rc != 0)
        return rc;

    for (lba = FAUXGPT_PRIMARY_ARRAY_LBA;
         lba < FAUXGPT_PRIMARY_ARRAY_LBA + FAUXGPT_ENTRY_ARRAY_BLOCKS;
         ++lba) {
        rc = fg_write_block(view, device, lba, block);
        if (rc != 0)
            return rc;
    }
    rc = fg_write_block(view, device, FAUXGPT_PRIMARY_HEADER_LBA, block);
    if (rc != 0)
        return rc;
    rc = fg_write_block(view, device, 0u, block);
    if (rc != 0)
        return rc;
    rc = device->flush(device->context);
    if (rc != 0)
        return rc;

    return FAUXGPT_OK;
}
