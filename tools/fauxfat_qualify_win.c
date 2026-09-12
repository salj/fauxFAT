/*
 * Windows host-qualification utility for fauxFAT whole-disk media.
 *
 * This file intentionally carries the small Win32/Virtdisk declaration subset
 * it uses instead of depending on an installed Windows SDK.  The supported
 * build is Zig's x86_64-windows-gnu target; keeping the declarations local also
 * lets ordinary host compilers perform -fsyntax-only checks in CI.
 */

#include "fauxfat_block.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal Win32 ABI declarations.  This tool is x86-64 only. */
typedef void *HANDLE;
typedef uint32_t DWORD;
typedef int32_t BOOL;
typedef uint16_t WCHAR;
typedef int32_t HRESULT;
typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;

typedef struct ffq_guid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} GUID;

typedef union ffq_large_integer {
    struct {
        uint32_t LowPart;
        int32_t HighPart;
    } parts;

    int64_t QuadPart;
} LARGE_INTEGER;

typedef struct ffq_disk_geometry {
    LARGE_INTEGER Cylinders;
    int32_t MediaType;
    DWORD TracksPerCylinder;
    DWORD SectorsPerTrack;
    DWORD BytesPerSector;
} DISK_GEOMETRY;

typedef struct ffq_get_length_information {
    LARGE_INTEGER Length;
} GET_LENGTH_INFORMATION;

typedef struct ffq_virtual_storage_type {
    DWORD DeviceId;
    GUID VendorId;
} VIRTUAL_STORAGE_TYPE;

typedef struct ffq_create_virtual_disk_parameters {
    DWORD Version;

    union {
        struct {
            GUID UniqueId;
            ULONGLONG MaximumSize;
            ULONG BlockSizeInBytes;
            ULONG SectorSizeInBytes;
            ULONG PhysicalSectorSizeInBytes;
            const WCHAR *ParentPath;
            const WCHAR *SourcePath;
            DWORD OpenFlags;
            VIRTUAL_STORAGE_TYPE ParentVirtualStorageType;
            VIRTUAL_STORAGE_TYPE SourceVirtualStorageType;
            GUID ResiliencyGuid;
        } Version2;
    } u;
} CREATE_VIRTUAL_DISK_PARAMETERS;

typedef struct ffq_open_virtual_disk_parameters {
    DWORD Version;

    union {
        struct {
            ULONG RWDepth;
        } Version1;
    } u;
} OPEN_VIRTUAL_DISK_PARAMETERS;

typedef struct ffq_attach_virtual_disk_parameters {
    DWORD Version;

    union {
        struct {
            ULONG Reserved;
        } Version1;
    } u;
} ATTACH_VIRTUAL_DISK_PARAMETERS;

/* The local declarations above intentionally match the x86-64 Win32 ABI. */
typedef char ffq_assert_guid_size[(sizeof(GUID) == 16u) ? 1 : -1];
typedef char ffq_assert_vst_size[(sizeof(VIRTUAL_STORAGE_TYPE) == 20u) ? 1 : -1];
typedef char ffq_assert_create_params_size[(sizeof(CREATE_VIRTUAL_DISK_PARAMETERS) == 128u) ? 1 : -1];
typedef char ffq_assert_open_params_size[(sizeof(OPEN_VIRTUAL_DISK_PARAMETERS) == 8u) ? 1 : -1];
typedef char ffq_assert_attach_params_size[(sizeof(ATTACH_VIRTUAL_DISK_PARAMETERS) == 8u) ? 1 : -1];
typedef char ffq_assert_disk_geometry_size[(sizeof(DISK_GEOMETRY) == 24u) ? 1 : -1];

extern HANDLE CreateFileW(const WCHAR *name, DWORD desired_access,
                          DWORD share_mode, void *security_attributes,
                          DWORD creation_disposition,
                          DWORD flags_and_attributes, HANDLE template_file);
extern BOOL CloseHandle(HANDLE handle);
extern BOOL ReadFile(HANDLE file, void *buffer, DWORD bytes_to_read,
                     DWORD *bytes_read, void *overlapped);
extern BOOL WriteFile(HANDLE file, const void *buffer, DWORD bytes_to_write,
                      DWORD *bytes_written, void *overlapped);
extern BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance,
                             LARGE_INTEGER *new_pointer, DWORD move_method);
extern BOOL FlushFileBuffers(HANDLE file);
extern BOOL DeviceIoControl(HANDLE device, DWORD control_code,
                            void *in_buffer, DWORD in_size,
                            void *out_buffer, DWORD out_size,
                            DWORD *bytes_returned, void *overlapped);
extern DWORD GetLastError(void);
extern DWORD FormatMessageA(DWORD flags, const void *source, DWORD message_id,
                            DWORD language_id, char *buffer, DWORD size,
                            void *arguments);
extern void *LocalFree(void *memory);
extern int MultiByteToWideChar(uint32_t code_page, DWORD flags,
                               const char *multi_byte, int multi_byte_count,
                               WCHAR *wide, int wide_count);
extern int WideCharToMultiByte(uint32_t code_page, DWORD flags,
                               const WCHAR *wide, int wide_count,
                               char *multi_byte, int multi_byte_count,
                               const char *default_char,
                               BOOL *used_default_char);
extern DWORD GetFullPathNameW(const WCHAR *path, DWORD buffer_length,
                              WCHAR *buffer, WCHAR **file_part);
extern HRESULT CoCreateGuid(GUID *guid);

extern DWORD CreateVirtualDisk(const VIRTUAL_STORAGE_TYPE *storage_type,
                               const WCHAR *path, DWORD access_mask,
                               void *security_descriptor, DWORD flags,
                               ULONG provider_specific_flags,
                               CREATE_VIRTUAL_DISK_PARAMETERS *parameters,
                               void *overlapped, HANDLE *handle);
extern DWORD OpenVirtualDisk(const VIRTUAL_STORAGE_TYPE *storage_type,
                             const WCHAR *path, DWORD access_mask,
                             DWORD flags,
                             OPEN_VIRTUAL_DISK_PARAMETERS *parameters,
                             HANDLE *handle);
extern DWORD AttachVirtualDisk(HANDLE handle, void *security_descriptor,
                               DWORD flags, ULONG provider_specific_flags,
                               ATTACH_VIRTUAL_DISK_PARAMETERS *parameters,
                               void *overlapped);
extern DWORD DetachVirtualDisk(HANDLE handle, DWORD flags,
                               ULONG provider_specific_flags);
extern DWORD GetVirtualDiskPhysicalPath(HANDLE handle,
                                        ULONG *disk_path_size_in_bytes,
                                        WCHAR *disk_path);

#define FFQ_INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

#define FFQ_GENERIC_READ          UINT32_C(0x80000000)
#define FFQ_GENERIC_WRITE         UINT32_C(0x40000000)
#define FFQ_FILE_SHARE_READ       UINT32_C(0x00000001)
#define FFQ_FILE_SHARE_WRITE      UINT32_C(0x00000002)
#define FFQ_FILE_SHARE_DELETE     UINT32_C(0x00000004)
#define FFQ_OPEN_EXISTING         UINT32_C(3)
#define FFQ_FILE_ATTRIBUTE_NORMAL UINT32_C(0x00000080)
#define FFQ_FILE_BEGIN            UINT32_C(0)

#define FFQ_CP_ACP               UINT32_C(0)
#define FFQ_CP_UTF8              UINT32_C(65001)
#define FFQ_MB_ERR_INVALID_CHARS UINT32_C(0x00000008)

#define FFQ_FORMAT_MESSAGE_ALLOCATE_BUFFER UINT32_C(0x00000100)
#define FFQ_FORMAT_MESSAGE_IGNORE_INSERTS  UINT32_C(0x00000200)
#define FFQ_FORMAT_MESSAGE_FROM_SYSTEM     UINT32_C(0x00001000)

#define FFQ_IOCTL_DISK_GET_DRIVE_GEOMETRY UINT32_C(0x00070000)
#define FFQ_IOCTL_DISK_GET_LENGTH_INFO    UINT32_C(0x0007405c)
#define FFQ_IOCTL_DISK_UPDATE_PROPERTIES  UINT32_C(0x00070140)

#define FFQ_VIRTUAL_STORAGE_TYPE_DEVICE_VHDX UINT32_C(3)
#define FFQ_VIRTUAL_DISK_ACCESS_NONE         UINT32_C(0x00000000)
#define FFQ_VIRTUAL_DISK_ACCESS_ATTACH_RO    UINT32_C(0x00010000)
#define FFQ_VIRTUAL_DISK_ACCESS_ATTACH_RW    UINT32_C(0x00020000)
#define FFQ_VIRTUAL_DISK_ACCESS_DETACH       UINT32_C(0x00040000)
#define FFQ_VIRTUAL_DISK_ACCESS_GET_INFO     UINT32_C(0x00080000)
#define FFQ_VIRTUAL_DISK_ACCESS_READ         UINT32_C(0x000d0000)

#define FFQ_CREATE_VIRTUAL_DISK_VERSION_2               UINT32_C(2)
#define FFQ_CREATE_VIRTUAL_DISK_FLAG_NONE               UINT32_C(0)
#define FFQ_OPEN_VIRTUAL_DISK_VERSION_1                 UINT32_C(1)
#define FFQ_OPEN_VIRTUAL_DISK_FLAG_NONE                 UINT32_C(0)
#define FFQ_ATTACH_VIRTUAL_DISK_VERSION_1               UINT32_C(1)
#define FFQ_ATTACH_VIRTUAL_DISK_FLAG_NONE               UINT32_C(0)
#define FFQ_ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY          UINT32_C(1)
#define FFQ_ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER    UINT32_C(2)
#define FFQ_ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME UINT32_C(4)
#define FFQ_DETACH_VIRTUAL_DISK_FLAG_NONE               UINT32_C(0)

#define FFQ_DEFAULT_VHDX_MIB         UINT64_C(512)
#define FFQ_DEFAULT_FAUXFAT_DATA_MIB UINT64_C(64)
#define FFQ_QUALIFY_FILE_BYTES       (UINT64_C(8) * 1024u * 1024u)
#define FFQ_MIB                      (UINT64_C(1024) * 1024u)
#define FFQ_ZERO_CHUNK_BYTES         (64u * 1024u)
#define FFQ_PHYSICAL_PATH_WCHARS     256u

static const GUID ffq_vendor_microsoft = {
    UINT32_C(0xec984aec), UINT16_C(0xa0f9), UINT16_C(0x47e9), { UINT8_C(0x90), UINT8_C(0x1f), UINT8_C(0x71), UINT8_C(0x41), UINT8_C(0x5a), UINT8_C(0x66), UINT8_C(0x34), UINT8_C(0x5b) }
};

static uint8_t ffq_zeroes[FFQ_ZERO_CHUNK_BYTES];

typedef struct ffq_raw_device {
    HANDLE handle;
    uint64_t blocks;
    int writable;
} ffq_raw_device;

typedef struct ffq_format_options {
    uint64_t vhdx_mib;
    uint64_t fauxfat_data_mib;
} ffq_format_options;

typedef struct ffq_verify_context {
    int qualify_file_found;
    int qualify_file_bad;
    size_t descriptor_count;
} ffq_verify_context;

static void ffq_usage(FILE *out)
{
    fprintf(out,
            "usage:\n"
            "  fauxfat-qualify.exe create-vhdx <file.vhdx> [--size-mib N] [--fauxfat-data-mib N]\n"
            "  fauxfat-qualify.exe attach-vhdx <file.vhdx>\n"
            "  fauxfat-qualify.exe detach-vhdx <file.vhdx>\n"
            "  fauxfat-qualify.exe verify-vhdx <file.vhdx>\n"
            "  fauxfat-qualify.exe format-raw <\\\\.\\PhysicalDriveN> --destroy-user-data [--fauxfat-data-mib N]\n"
            "  fauxfat-qualify.exe verify-raw <\\\\.\\PhysicalDriveN>\n"
            "\n"
            "create-vhdx creates a dynamic VHDX, formats partition 1 as fauxFAT,\n"
            "leaves partition 2 raw, then leaves the VHDX attached normally so\n"
            "Windows can mount/mutate it. format-raw refuses to run without the\n"
            "literal --destroy-user-data flag.\n");
}

static void ffq_print_win_error(const char *what, DWORD code)
{
    char *message = NULL;
    DWORD flags   = FFQ_FORMAT_MESSAGE_ALLOCATE_BUFFER |
                  FFQ_FORMAT_MESSAGE_FROM_SYSTEM |
                  FFQ_FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD n;

    n = FormatMessageA(flags, NULL, code, 0u, (char *)&message, 0u, NULL);
    if (n != 0u && message) {
        while (n != 0u && (message[n - 1u] == '\r' || message[n - 1u] == '\n'))
            message[--n] = '\0';
        fprintf(stderr, "%s: Windows error %" PRIu32 ": %s\n",
                what, code, message);
        LocalFree(message);
    } else {
        fprintf(stderr, "%s: Windows error %" PRIu32 "\n", what, code);
    }
}

static WCHAR *ffq_wide_from_multibyte(const char *s)
{
    WCHAR *out;
    int count;

    count = MultiByteToWideChar(FFQ_CP_UTF8, FFQ_MB_ERR_INVALID_CHARS,
                                s, -1, NULL, 0);
    if (count == 0)
        count = MultiByteToWideChar(FFQ_CP_ACP, 0u, s, -1, NULL, 0);
    if (count <= 0)
        return NULL;
    out = (WCHAR *)calloc((size_t)count, sizeof(*out));
    if (!out)
        return NULL;
    if (MultiByteToWideChar(FFQ_CP_UTF8, FFQ_MB_ERR_INVALID_CHARS,
                            s, -1, out, count) == 0 &&
        MultiByteToWideChar(FFQ_CP_ACP, 0u, s, -1, out, count) == 0) {
        free(out);
        return NULL;
    }
    return out;
}

static WCHAR *ffq_absolute_wide_path(const char *s)
{
    WCHAR *input = ffq_wide_from_multibyte(s);
    WCHAR *absolute;
    DWORD need;

    if (!input)
        return NULL;
    need = GetFullPathNameW(input, 0u, NULL, NULL);
    if (need == 0u) {
        free(input);
        return NULL;
    }
    absolute = (WCHAR *)calloc((size_t)need + 1u, sizeof(*absolute));
    if (!absolute) {
        free(input);
        return NULL;
    }
    if (GetFullPathNameW(input, need + 1u, absolute, NULL) == 0u) {
        free(input);
        free(absolute);
        return NULL;
    }
    free(input);
    return absolute;
}

static char *ffq_utf8_from_wide(const WCHAR *s)
{
    char *out;
    int count = WideCharToMultiByte(FFQ_CP_UTF8, 0u, s, -1,
                                    NULL, 0, NULL, NULL);
    if (count <= 0)
        return NULL;
    out = (char *)malloc((size_t)count);
    if (!out)
        return NULL;
    if (WideCharToMultiByte(FFQ_CP_UTF8, 0u, s, -1,
                            out, count, NULL, NULL) == 0) {
        free(out);
        return NULL;
    }
    return out;
}

static int ffq_parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long v;

    errno = 0;
    v     = strtoull(text, &end, 10);
    if (errno != 0 || !end || end == text || *end != '\0')
        return -1;
    *value = (uint64_t)v;
    return 0;
}

static int ffq_parse_create_options(int argc, char **argv, int first,
                                    ffq_format_options *options)
{
    int i;

    options->vhdx_mib         = FFQ_DEFAULT_VHDX_MIB;
    options->fauxfat_data_mib = FFQ_DEFAULT_FAUXFAT_DATA_MIB;
    for (i = first; i < argc; ++i) {
        if (strcmp(argv[i], "--size-mib") == 0) {
            if (++i >= argc || ffq_parse_u64(argv[i], &options->vhdx_mib) != 0)
                return -1;
        } else if (strcmp(argv[i], "--fauxfat-data-mib") == 0) {
            if (++i >= argc ||
                ffq_parse_u64(argv[i], &options->fauxfat_data_mib) != 0)
                return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

static int ffq_parse_raw_format_options(int argc, char **argv, int first,
                                        ffq_format_options *options,
                                        int *destroy)
{
    int i;

    options->vhdx_mib         = 0u;
    options->fauxfat_data_mib = FFQ_DEFAULT_FAUXFAT_DATA_MIB;
    *destroy                  = 0;
    for (i = first; i < argc; ++i) {
        if (strcmp(argv[i], "--destroy-user-data") == 0) {
            *destroy = 1;
        } else if (strcmp(argv[i], "--fauxfat-data-mib") == 0) {
            if (++i >= argc ||
                ffq_parse_u64(argv[i], &options->fauxfat_data_mib) != 0)
                return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

static int ffq_is_physical_drive_path(const char *s)
{
    static const char prefix[] = "\\\\.\\PhysicalDrive";
    size_t i                   = sizeof(prefix) - 1u;

    if (strncmp(s, prefix, i) != 0 || s[i] == '\0')
        return 0;
    for (; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
    }
    return 1;
}

static int ffq_seek(HANDLE handle, uint64_t byte_offset)
{
    LARGE_INTEGER pos;

    if (byte_offset > INT64_MAX)
        return -1;
    pos.QuadPart = (int64_t)byte_offset;
    return SetFilePointerEx(handle, pos, NULL, FFQ_FILE_BEGIN) ? 0 : -1;
}

static int ffq_transfer(HANDLE handle, uint64_t byte_offset,
                        void *buffer, size_t length, int write)
{
    uint8_t *p = (uint8_t *)buffer;

    while (length != 0u) {
        DWORD done  = 0u;
        DWORD chunk = length > UINT32_C(0x40000000) ? UINT32_C(0x40000000) : (DWORD)length;
        BOOL ok;

        if (ffq_seek(handle, byte_offset) != 0)
            return -1;
        if (write)
            ok = WriteFile(handle, p, chunk, &done, NULL);
        else
            ok = ReadFile(handle, p, chunk, &done, NULL);
        if (!ok || done != chunk)
            return -1;
        p += chunk;
        length -= chunk;
        byte_offset += chunk;
    }
    return 0;
}

static int ffq_range_bytes(const ffq_raw_device *raw, uint64_t first_block,
                           uint64_t block_count, uint64_t *offset,
                           uint64_t *byte_count)
{
    if (first_block > raw->blocks || block_count > raw->blocks - first_block ||
        first_block > UINT64_MAX / FAUXFAT_BLOCK_SIZE ||
        block_count > UINT64_MAX / FAUXFAT_BLOCK_SIZE)
        return -1;
    *offset     = first_block * FAUXFAT_BLOCK_SIZE;
    *byte_count = block_count * FAUXFAT_BLOCK_SIZE;
    if (*offset > INT64_MAX || *byte_count > (uint64_t)INT64_MAX ||
        *byte_count > (uint64_t)INT64_MAX - *offset)
        return -1;
    return 0;
}

static int ffq_raw_read(void *context, uint64_t first_block,
                        size_t block_count, void *data)
{
    ffq_raw_device *raw = (ffq_raw_device *)context;
    uint64_t offset;
    uint64_t bytes;

    if (ffq_range_bytes(raw, first_block, (uint64_t)block_count,
                        &offset, &bytes) != 0 ||
        bytes > SIZE_MAX)
        return -1001;
    return ffq_transfer(raw->handle, offset, data, (size_t)bytes, 0) == 0 ? 0 : -1002;
}

static int ffq_raw_write(void *context, uint64_t first_block,
                         size_t block_count, const void *data)
{
    ffq_raw_device *raw = (ffq_raw_device *)context;
    uint64_t offset;
    uint64_t bytes;

    if (!raw->writable)
        return -1010;
    if (ffq_range_bytes(raw, first_block, (uint64_t)block_count,
                        &offset, &bytes) != 0 ||
        bytes > SIZE_MAX)
        return -1011;
    return ffq_transfer(raw->handle, offset, (void *)data,
                        (size_t)bytes, 1) == 0
               ? 0
               : -1012;
}

static int ffq_raw_zero(void *context, uint64_t first_block,
                        uint64_t block_count)
{
    ffq_raw_device *raw = (ffq_raw_device *)context;
    uint64_t offset;
    uint64_t bytes;

    if (!raw->writable)
        return -1020;
    if (ffq_range_bytes(raw, first_block, block_count, &offset, &bytes) != 0)
        return -1021;
    while (bytes != 0u) {
        size_t chunk = bytes > sizeof(ffq_zeroes) ? sizeof(ffq_zeroes) : (size_t)bytes;
        if (ffq_transfer(raw->handle, offset, ffq_zeroes, chunk, 1) != 0)
            return -1022;
        offset += chunk;
        bytes -= chunk;
    }
    return 0;
}

static int ffq_raw_skip(void *context, uint64_t first_block,
                        uint64_t block_count, fauxfat_skip_kind kind)
{
    ffq_raw_device *raw = (ffq_raw_device *)context;
    uint64_t offset;
    uint64_t bytes;

    (void)kind;
    return ffq_range_bytes(raw, first_block, block_count, &offset, &bytes) == 0 ? 0 : -1030;
}

static int ffq_raw_flush(void *context)
{
    ffq_raw_device *raw = (ffq_raw_device *)context;
    return FlushFileBuffers(raw->handle) ? 0 : -1040;
}

static int ffq_open_raw_wide(const WCHAR *path, int writable,
                             ffq_raw_device *raw)
{
    HANDLE handle;
    DWORD access   = FFQ_GENERIC_READ | (writable ? FFQ_GENERIC_WRITE : 0u);
    DWORD returned = 0u;
    GET_LENGTH_INFORMATION length;
    DISK_GEOMETRY geometry;

    memset(raw, 0, sizeof(*raw));
    handle = CreateFileW(path, access,
                         FFQ_FILE_SHARE_READ | FFQ_FILE_SHARE_WRITE |
                             FFQ_FILE_SHARE_DELETE,
                         NULL, FFQ_OPEN_EXISTING, FFQ_FILE_ATTRIBUTE_NORMAL,
                         NULL);
    if (handle == FFQ_INVALID_HANDLE_VALUE) {
        ffq_print_win_error("open raw disk", GetLastError());
        return -1;
    }

    memset(&geometry, 0, sizeof(geometry));
    if (!DeviceIoControl(handle, FFQ_IOCTL_DISK_GET_DRIVE_GEOMETRY,
                         NULL, 0u, &geometry, (DWORD)sizeof(geometry),
                         &returned, NULL)) {
        ffq_print_win_error("query disk geometry", GetLastError());
        CloseHandle(handle);
        return -1;
    }
    if (geometry.BytesPerSector != FAUXFAT_BLOCK_SIZE) {
        fprintf(stderr, "unsupported logical sector size: %" PRIu32 " (fauxFAT requires 512)\n",
                geometry.BytesPerSector);
        CloseHandle(handle);
        return -1;
    }

    memset(&length, 0, sizeof(length));
    if (!DeviceIoControl(handle, FFQ_IOCTL_DISK_GET_LENGTH_INFO,
                         NULL, 0u, &length, (DWORD)sizeof(length),
                         &returned, NULL)) {
        ffq_print_win_error("query disk length", GetLastError());
        CloseHandle(handle);
        return -1;
    }
    if (length.Length.QuadPart <= 0 ||
        ((uint64_t)length.Length.QuadPart % FAUXFAT_BLOCK_SIZE) != 0u) {
        fprintf(stderr, "raw disk has invalid byte length\n");
        CloseHandle(handle);
        return -1;
    }

    raw->handle   = handle;
    raw->blocks   = (uint64_t)length.Length.QuadPart / FAUXFAT_BLOCK_SIZE;
    raw->writable = writable;
    return 0;
}

static int ffq_open_raw_ascii(const char *path, int writable,
                              ffq_raw_device *raw)
{
    WCHAR *wide = ffq_wide_from_multibyte(path);
    int rc;

    if (!wide) {
        fprintf(stderr, "cannot convert raw-device path\n");
        return -1;
    }
    rc = ffq_open_raw_wide(wide, writable, raw);
    free(wide);
    return rc;
}

static void ffq_close_raw(ffq_raw_device *raw)
{
    if (raw->handle && raw->handle != FFQ_INVALID_HANDLE_VALUE)
        CloseHandle(raw->handle);
    memset(raw, 0, sizeof(*raw));
}

static fauxfat_block_device ffq_block_device(ffq_raw_device *raw)
{
    fauxfat_block_device dev;

    memset(&dev, 0, sizeof(dev));
    dev.block_count = raw->blocks;
    dev.io.read     = ffq_raw_read;
    dev.io.write    = ffq_raw_write;
    dev.io.zero     = ffq_raw_zero;
    dev.io.skip     = ffq_raw_skip;
    dev.io.context  = raw;
    dev.flush       = ffq_raw_flush;
    return dev;
}

static int ffq_payload_read(void *context, int fd, uint64_t offset,
                            void *data, size_t length)
{
    (void)context;
    (void)fd;
    (void)offset;
    memset(data, 0, length);
    return 0;
}

static int ffq_payload_write(void *context, int fd, uint64_t offset,
                             const void *data, size_t length)
{
    (void)context;
    (void)fd;
    (void)offset;
    (void)data;
    (void)length;
    return 0;
}

static int ffq_new_guid_bytes(uint8_t out[16])
{
    GUID guid;

    if (CoCreateGuid(&guid) != 0)
        return -1;
    memcpy(out, &guid, 16u);
    return 0;
}

static void ffq_derive_guid(uint8_t out[16], const uint8_t volume_guid[16],
                            uint8_t tag)
{
    memcpy(out, volume_guid, 16u);
    out[15] ^= tag;
}

static int ffq_build_format(uint64_t disk_blocks, uint64_t fauxfat_data_mib,
                            fauxfat_file *file, fauxfat_config *cfg,
                            fauxfat_view *view,
                            fauxfat_block_gpt_plan *plan,
                            fauxgpt_partition parts[2],
                            fauxgpt_layout *layout, fauxgpt_view *gpt)
{
    uint64_t clusters;
    uint8_t disk_guid[16];
    uint8_t p1_guid[16];
    uint8_t p2_guid[16];
    int rc;

    if (fauxfat_data_mib == 0u ||
        fauxfat_data_mib > UINT32_MAX / (FFQ_MIB / FAUXFAT_CLUSTER_SIZE)) {
        fprintf(stderr, "fauxFAT data size is out of range\n");
        return -1;
    }
    clusters = fauxfat_data_mib * (FFQ_MIB / FAUXFAT_CLUSTER_SIZE);
    if (clusters > UINT32_MAX ||
        clusters * FAUXFAT_CLUSTER_SIZE < FFQ_QUALIFY_FILE_BYTES) {
        fprintf(stderr, "fauxFAT data area must be at least 8 MiB\n");
        return -1;
    }

    memset(file, 0, sizeof(*file));
    file->name         = "QUALIFY.BIN";
    file->fd           = 1;
    file->size         = FFQ_QUALIFY_FILE_BYTES;
    file->mtime        = (time_t)UINT64_C(1704067200); /* 2024-01-01 UTC */
    file->data_cluster = FAUXFAT_CLUSTER_AUTO;

    memset(cfg, 0, sizeof(*cfg));
    cfg->files              = file;
    cfg->file_count         = 1u;
    cfg->data_cluster_count = (uint32_t)clusters;
    cfg->read               = ffq_payload_read;
    cfg->write              = ffq_payload_write;
    cfg->partition_lba      = 0u;
    cfg->structural_epoch   = 1u;
    cfg->volume_label       = "FAUXQUAL";
    if (ffq_new_guid_bytes(cfg->volume_guid) != 0) {
        fprintf(stderr, "CoCreateGuid failed\n");
        return -1;
    }

    /* Keep qualification GPT identity derivable from the fauxFAT Volume GUID.
     * That lets verify-vhdx detect a host/tool rewriting disk or partition
     * GUIDs without needing a sidecar file. */
    ffq_derive_guid(disk_guid, cfg->volume_guid, UINT8_C(0x41));
    ffq_derive_guid(p1_guid, cfg->volume_guid, UINT8_C(0x82));
    ffq_derive_guid(p2_guid, cfg->volume_guid, UINT8_C(0xc3));
    cfg->volume_serial = ((uint32_t)cfg->volume_guid[0] << 24) |
                         ((uint32_t)cfg->volume_guid[5] << 16) |
                         ((uint32_t)cfg->volume_guid[10] << 8) |
                         (uint32_t)cfg->volume_guid[15];
    if (cfg->volume_serial == 0u)
        cfg->volume_serial = UINT32_C(0x51464c54);

    rc = fauxfat_init(view, cfg);
    if (rc != FAUXFAT_OK) {
        fprintf(stderr, "fauxfat_init failed: %d\n", rc);
        return -1;
    }
    rc = fauxfat_block_plan_gpt(plan, disk_blocks, view->volume_blocks,
                                0u, 1);
    if (rc != FAUXFAT_BLOCK_OK) {
        fprintf(stderr, "disk is too small for requested fauxFAT layout: %d\n",
                rc);
        return -1;
    }

    cfg->partition_lba = plan->fauxfat_first_lba;
    rc                 = fauxfat_init(view, cfg);
    if (rc != FAUXFAT_OK || view->volume_blocks != plan->fauxfat_block_count) {
        fprintf(stderr, "fauxFAT geometry changed after placement: %d\n", rc);
        return -1;
    }

    memset(parts, 0, 2u * sizeof(*parts));
    memcpy(parts[0].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    memcpy(parts[0].unique_guid, p1_guid, 16u);
    parts[0].first_lba   = plan->fauxfat_first_lba;
    parts[0].block_count = plan->fauxfat_block_count;
    parts[0].name        = "FAUXFAT QUAL";

    memcpy(parts[1].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    memcpy(parts[1].unique_guid, p2_guid, 16u);
    parts[1].first_lba   = plan->user_first_lba;
    parts[1].block_count = plan->user_block_count;
    parts[1].name        = "USER DATA";

    memset(layout, 0, sizeof(*layout));
    layout->disk_blocks = disk_blocks;
    memcpy(layout->disk_guid, disk_guid, 16u);
    layout->partitions      = parts;
    layout->partition_count = 2u;
    rc                      = fauxgpt_init(gpt, layout);
    if (rc != FAUXGPT_OK) {
        fprintf(stderr, "fauxgpt_init failed: %d\n", rc);
        return -1;
    }
    return 0;
}

static int ffq_refresh_disk(HANDLE handle)
{
    DWORD returned = 0u;

    if (!DeviceIoControl(handle, FFQ_IOCTL_DISK_UPDATE_PROPERTIES,
                         NULL, 0u, NULL, 0u, &returned, NULL)) {
        ffq_print_win_error("refresh Windows disk properties", GetLastError());
        return -1;
    }
    return 0;
}

static int ffq_format_raw_device(ffq_raw_device *raw, uint64_t data_mib)
{
    fauxfat_file file;
    fauxfat_config cfg;
    fauxfat_view view;
    fauxfat_block_gpt_plan plan;
    fauxgpt_partition parts[2];
    fauxgpt_layout layout;
    fauxgpt_view gpt;
    fauxfat_block_device dev;
    int rc;

    if (ffq_build_format(raw->blocks, data_mib, &file, &cfg, &view, &plan,
                         parts, &layout, &gpt) != 0)
        return -1;
    dev = ffq_block_device(raw);

    printf("formatting %" PRIu64 " blocks (%" PRIu64 " MiB):\n",
           raw->blocks,
           raw->blocks / (FFQ_MIB / FAUXFAT_BLOCK_SIZE));
    printf("  fauxFAT p1: LBA %" PRIu64 " + %" PRIu64 " blocks\n",
           plan.fauxfat_first_lba, plan.fauxfat_block_count);
    printf("  raw user p2: LBA %" PRIu64 " + %" PRIu64 " blocks\n",
           plan.user_first_lba, plan.user_block_count);

    rc = fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_WRAPPER_GPT, &gpt,
                              NULL, NULL,
                              FAUXFAT_BLOCK_FORMAT_ZERO_UNDEFINED |
                                  FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA);
    if (rc != FAUXFAT_BLOCK_OK) {
        fprintf(stderr, "fauxfat_block_format failed: %d\n", rc);
        return -1;
    }
    if (ffq_refresh_disk(raw->handle) != 0)
        fprintf(stderr, "warning: disk is formatted but Windows cache refresh failed\n");
    return 0;
}

static const char *ffq_class_name(fauxfat_volume_class classification)
{
    switch (classification) {
    case FAUXFAT_VOLUME_EXFAT_BEST_EFFORT:
        return "exfat-best-effort";
    case FAUXFAT_VOLUME_FAUXFAT_CHANGED:
        return "fauxfat-changed";
    case FAUXFAT_VOLUME_FAUXFAT_VALID:
        return "fauxfat-valid";
    default:
        return "invalid";
    }
}

static int ffq_emit_descriptor(void *context, unsigned index,
                               const fauxfat_disk_file *file)
{
    ffq_verify_context *verify = (ffq_verify_context *)context;
    const char *kind           = file->kind == FAUXFAT_DISK_FILE_PUBLIC ? "public" : "opaque";

    ++verify->descriptor_count;
    printf("  file[%u] %-6s %-15s first=%" PRIu64
           " length=%" PRIu64 " alloc_blocks=%" PRIu64 "%s\n",
           index, kind, file->name, file->first_block, file->data_length,
           file->allocation_blocks,
           (file->flags & FAUXFAT_DISK_FILE_NAME_CHANGED) != 0u ? " name-changed" : "");
    if (file->kind == FAUXFAT_DISK_FILE_PUBLIC &&
        strcmp(file->name, "QUALIFY.BIN") == 0) {
        verify->qualify_file_found = 1;
        if (file->data_length != FFQ_QUALIFY_FILE_BYTES)
            verify->qualify_file_bad = 1;
    }
    return 0;
}

static int ffq_verify_raw_device(ffq_raw_device *raw, int require_qualification)
{
    fauxfat_block_device dev = ffq_block_device(raw);
    fauxfat_block_opened opened;
    ffq_verify_context verify;
    size_t descriptor_count = 0u;
    unsigned required_gpt   = FAUXGPT_INFO_PRIMARY_VALID |
                            FAUXGPT_INFO_BACKUP_VALID |
                            FAUXGPT_INFO_PMBR_VALID;
    int rc;

    memset(&verify, 0, sizeof(verify));
    rc = fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_WRAPPER_AUTO, NULL,
                            ffq_emit_descriptor, &verify, &descriptor_count);
    if (rc != FAUXFAT_BLOCK_OK) {
        fprintf(stderr, "fauxfat_block_open failed: %d", rc);
        if (opened.backend_error != 0)
            fprintf(stderr, " (backend %d)", opened.backend_error);
        fputc('\n', stderr);
        return -1;
    }

    printf("wrapper: %s\n",
           opened.wrapper == FAUXFAT_BLOCK_WRAPPER_GPT ? "GPT" : "bare");
    printf("classification: %s\n", ffq_class_name(opened.classification));
    printf("fauxFAT: LBA %" PRIu64 " + %" PRIu64
           " blocks, serial %08" PRIx32 ", epoch %" PRIu64 "\n",
           opened.volume_first_block, opened.volume_blocks,
           opened.fauxfat.volume_serial, opened.fauxfat.structural_epoch);

    if (opened.wrapper == FAUXFAT_BLOCK_WRAPPER_GPT) {
        size_t i;
        printf("GPT: %zu partitions, flags 0x%x\n",
               opened.gpt.partition_count, opened.gpt.flags);
        for (i = 0u; i < opened.gpt.partition_count; ++i) {
            printf("  p%zu: LBA %" PRIu64 " + %" PRIu64 " blocks\n",
                   i + 1u, opened.gpt.partitions[i].first_lba,
                   opened.gpt.partitions[i].block_count);
        }
        if ((opened.gpt.flags & required_gpt) != required_gpt) {
            fprintf(stderr, "qualification verify requires PMBR and both GPT copies valid\n");
            return -1;
        }
    }

    if (require_qualification) {
        fauxfat_block_gpt_plan plan;
        uint8_t expected_disk_guid[16];
        uint8_t expected_p1_guid[16];
        uint8_t expected_p2_guid[16];

        if (opened.wrapper != FAUXFAT_BLOCK_WRAPPER_GPT ||
            opened.gpt.partition_count != 2u) {
            fprintf(stderr, "qualification VHDX must retain the canonical two-partition GPT\n");
            return -1;
        }
        rc = fauxfat_block_plan_gpt(&plan, raw->blocks, opened.volume_blocks,
                                    0u, 1);
        if (rc != FAUXFAT_BLOCK_OK ||
            opened.gpt.partitions[0].first_lba != plan.fauxfat_first_lba ||
            opened.gpt.partitions[0].block_count != plan.fauxfat_block_count ||
            opened.gpt.partitions[1].first_lba != plan.user_first_lba ||
            opened.gpt.partitions[1].block_count != plan.user_block_count) {
            fprintf(stderr, "qualification VHDX partition geometry changed\n");
            return -1;
        }
        ffq_derive_guid(expected_disk_guid, opened.fauxfat.volume_guid,
                        UINT8_C(0x41));
        ffq_derive_guid(expected_p1_guid, opened.fauxfat.volume_guid,
                        UINT8_C(0x82));
        ffq_derive_guid(expected_p2_guid, opened.fauxfat.volume_guid,
                        UINT8_C(0xc3));
        if (memcmp(opened.gpt.disk_guid, expected_disk_guid, 16u) != 0 ||
            memcmp(opened.gpt.partitions[0].unique_guid,
                   expected_p1_guid, 16u) != 0 ||
            memcmp(opened.gpt.partitions[1].unique_guid,
                   expected_p2_guid, 16u) != 0) {
            fprintf(stderr, "qualification VHDX GPT identity changed\n");
            return -1;
        }
    }

    if (descriptor_count != verify.descriptor_count) {
        fprintf(stderr, "descriptor count mismatch\n");
        return -1;
    }
    if (require_qualification &&
        (!verify.qualify_file_found || verify.qualify_file_bad)) {
        fprintf(stderr, "QUALIFY.BIN is missing or has the wrong logical size\n");
        return -1;
    }
    return 0;
}

static VIRTUAL_STORAGE_TYPE ffq_vhdx_storage_type(void)
{
    VIRTUAL_STORAGE_TYPE type;
    type.DeviceId = FFQ_VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    type.VendorId = ffq_vendor_microsoft;
    return type;
}

static int ffq_open_vhdx(const WCHAR *path, DWORD access, ULONG rw_depth,
                         HANDLE *handle)
{
    VIRTUAL_STORAGE_TYPE type = ffq_vhdx_storage_type();
    OPEN_VIRTUAL_DISK_PARAMETERS params;
    DWORD rc;

    memset(&params, 0, sizeof(params));
    params.Version            = FFQ_OPEN_VIRTUAL_DISK_VERSION_1;
    params.u.Version1.RWDepth = rw_depth;
    rc                        = OpenVirtualDisk(&type, path, access,
                                                FFQ_OPEN_VIRTUAL_DISK_FLAG_NONE,
                                                &params, handle);
    if (rc != 0u) {
        ffq_print_win_error("OpenVirtualDisk", rc);
        return -1;
    }
    return 0;
}

static int ffq_attach_vhdx_handle(HANDLE handle, DWORD flags)
{
    ATTACH_VIRTUAL_DISK_PARAMETERS params;
    DWORD rc;

    memset(&params, 0, sizeof(params));
    params.Version = FFQ_ATTACH_VIRTUAL_DISK_VERSION_1;
    rc             = AttachVirtualDisk(handle, NULL, flags, 0u, &params, NULL);
    if (rc != 0u) {
        ffq_print_win_error("AttachVirtualDisk", rc);
        return -1;
    }
    return 0;
}

static int ffq_get_physical_path(HANDLE handle,
                                 WCHAR path[FFQ_PHYSICAL_PATH_WCHARS])
{
    ULONG bytes = (ULONG)(FFQ_PHYSICAL_PATH_WCHARS * sizeof(WCHAR));
    DWORD rc    = GetVirtualDiskPhysicalPath(handle, &bytes, path);

    if (rc != 0u)
        return -1;
    path[FFQ_PHYSICAL_PATH_WCHARS - 1u] = 0u;
    return 0;
}

static void ffq_print_physical_path(const WCHAR *path)
{
    char *utf8 = ffq_utf8_from_wide(path);
    if (utf8) {
        printf("physical disk: %s\n", utf8);
        free(utf8);
    } else {
        printf("physical disk attached (path conversion failed)\n");
    }
}

static int ffq_create_vhdx_file(const WCHAR *path, uint64_t size_bytes)
{
    VIRTUAL_STORAGE_TYPE type = ffq_vhdx_storage_type();
    CREATE_VIRTUAL_DISK_PARAMETERS params;
    HANDLE handle = NULL;
    DWORD rc;

    memset(&params, 0, sizeof(params));
    params.Version = FFQ_CREATE_VIRTUAL_DISK_VERSION_2;
    if (CoCreateGuid(&params.u.Version2.UniqueId) != 0) {
        fprintf(stderr, "CoCreateGuid failed for VHDX identity\n");
        return -1;
    }
    params.u.Version2.MaximumSize               = size_bytes;
    params.u.Version2.BlockSizeInBytes          = 0u;
    params.u.Version2.SectorSizeInBytes         = FAUXFAT_BLOCK_SIZE;
    params.u.Version2.PhysicalSectorSizeInBytes = 4096u;
    params.u.Version2.ParentPath                = NULL;
    params.u.Version2.SourcePath                = NULL;
    params.u.Version2.OpenFlags                 = FFQ_OPEN_VIRTUAL_DISK_FLAG_NONE;

    rc = CreateVirtualDisk(&type, path, FFQ_VIRTUAL_DISK_ACCESS_NONE,
                           NULL, FFQ_CREATE_VIRTUAL_DISK_FLAG_NONE,
                           0u, &params, NULL, &handle);
    if (rc != 0u) {
        ffq_print_win_error("CreateVirtualDisk", rc);
        return -1;
    }
    if (handle)
        CloseHandle(handle);
    return 0;
}

static int ffq_command_create_vhdx(const char *path,
                                   const ffq_format_options *options)
{
    WCHAR *wide = NULL;
    HANDLE vhd  = NULL;
    WCHAR physical[FFQ_PHYSICAL_PATH_WCHARS];
    ffq_raw_device raw;
    uint64_t size_bytes;
    int temp_attached = 0;
    int rc            = -1;

    if (options->vhdx_mib == 0u || options->vhdx_mib > UINT64_MAX / FFQ_MIB) {
        fprintf(stderr, "VHDX size is out of range\n");
        return -1;
    }
    size_bytes = options->vhdx_mib * FFQ_MIB;
    wide       = ffq_absolute_wide_path(path);
    if (!wide) {
        fprintf(stderr, "cannot resolve VHDX path\n");
        goto done;
    }
    if (ffq_create_vhdx_file(wide, size_bytes) != 0)
        goto done;
    if (ffq_open_vhdx(wide,
                      FFQ_VIRTUAL_DISK_ACCESS_ATTACH_RW |
                          FFQ_VIRTUAL_DISK_ACCESS_DETACH |
                          FFQ_VIRTUAL_DISK_ACCESS_GET_INFO,
                      1u, &vhd) != 0)
        goto done;
    if (ffq_attach_vhdx_handle(vhd,
                               FFQ_ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER) != 0)
        goto done;
    temp_attached = 1;
    if (ffq_get_physical_path(vhd, physical) != 0) {
        fprintf(stderr, "GetVirtualDiskPhysicalPath failed after attach\n");
        goto done;
    }
    if (ffq_open_raw_wide(physical, 1, &raw) != 0)
        goto done;
    if (ffq_format_raw_device(&raw, options->fauxfat_data_mib) != 0) {
        ffq_close_raw(&raw);
        goto done;
    }
    ffq_close_raw(&raw);

    if (DetachVirtualDisk(vhd, FFQ_DETACH_VIRTUAL_DISK_FLAG_NONE, 0u) != 0u) {
        fprintf(stderr, "failed to detach temporary VHDX attachment\n");
        goto done;
    }
    temp_attached = 0;
    if (ffq_attach_vhdx_handle(vhd,
                               FFQ_ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME) != 0)
        goto done;
    if (ffq_get_physical_path(vhd, physical) != 0) {
        fprintf(stderr, "GetVirtualDiskPhysicalPath failed after final attach\n");
        goto done;
    }

    printf("VHDX created and left attached. Windows may assign FAUXQUAL a drive letter.\n");
    printf("Partition 2 is intentionally raw; format it with ordinary Windows tooling if desired.\n");
    printf("After host mutations, run verify-vhdx against the same file.\n");
    ffq_print_physical_path(physical);
    rc = 0;

done:
    if (temp_attached && vhd)
        DetachVirtualDisk(vhd, FFQ_DETACH_VIRTUAL_DISK_FLAG_NONE, 0u);
    if (vhd)
        CloseHandle(vhd);
    free(wide);
    return rc;
}

static int ffq_command_attach_vhdx(const char *path)
{
    WCHAR *wide = ffq_absolute_wide_path(path);
    HANDLE vhd  = NULL;
    WCHAR physical[FFQ_PHYSICAL_PATH_WCHARS];
    int rc = -1;

    if (!wide) {
        fprintf(stderr, "cannot resolve VHDX path\n");
        return -1;
    }

    /* A permanently attached VHDX rejects a second handle requesting attach
     * access. Probe it first with GET_INFO|DETACH, then reopen with attach
     * rights only when it is actually detached. */
    if (ffq_open_vhdx(wide,
                      FFQ_VIRTUAL_DISK_ACCESS_DETACH |
                          FFQ_VIRTUAL_DISK_ACCESS_GET_INFO,
                      0u, &vhd) != 0)
        goto done;
    if (ffq_get_physical_path(vhd, physical) == 0) {
        printf("VHDX is already attached.\n");
        ffq_print_physical_path(physical);
        rc = 0;
        goto done;
    }
    CloseHandle(vhd);
    vhd = NULL;
    if (ffq_open_vhdx(wide,
                      FFQ_VIRTUAL_DISK_ACCESS_ATTACH_RW |
                          FFQ_VIRTUAL_DISK_ACCESS_DETACH |
                          FFQ_VIRTUAL_DISK_ACCESS_GET_INFO,
                      1u, &vhd) != 0)
        goto done;
    if (ffq_attach_vhdx_handle(vhd,
                               FFQ_ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME) != 0)
        goto done;
    if (ffq_get_physical_path(vhd, physical) != 0) {
        fprintf(stderr, "GetVirtualDiskPhysicalPath failed after attach\n");
        goto done;
    }
    ffq_print_physical_path(physical);
    rc = 0;

done:
    if (vhd)
        CloseHandle(vhd);
    free(wide);
    return rc;
}

static int ffq_command_detach_vhdx(const char *path)
{
    WCHAR *wide = ffq_absolute_wide_path(path);
    HANDLE vhd  = NULL;
    DWORD detach_rc;
    int rc = -1;

    if (!wide) {
        fprintf(stderr, "cannot resolve VHDX path\n");
        return -1;
    }
    if (ffq_open_vhdx(wide, FFQ_VIRTUAL_DISK_ACCESS_DETACH, 0u, &vhd) != 0)
        goto done;
    detach_rc = DetachVirtualDisk(vhd, FFQ_DETACH_VIRTUAL_DISK_FLAG_NONE, 0u);
    if (detach_rc != 0u) {
        ffq_print_win_error("DetachVirtualDisk", detach_rc);
        goto done;
    }
    printf("VHDX detached.\n");
    rc = 0;

done:
    if (vhd)
        CloseHandle(vhd);
    free(wide);
    return rc;
}

static int ffq_command_verify_vhdx(const char *path)
{
    WCHAR *wide = ffq_absolute_wide_path(path);
    HANDLE vhd  = NULL;
    WCHAR physical[FFQ_PHYSICAL_PATH_WCHARS];
    ffq_raw_device raw;
    int temporary_attach = 0;
    int rc               = -1;

    if (!wide) {
        fprintf(stderr, "cannot resolve VHDX path\n");
        return -1;
    }

    /* GET_INFO|DETACH can reopen an already-permanently-attached disk; READ
     * includes ATTACH_RO and is rejected in that case. Probe first, then only
     * request attach access if no physical device exists yet. */
    if (ffq_open_vhdx(wide,
                      FFQ_VIRTUAL_DISK_ACCESS_DETACH |
                          FFQ_VIRTUAL_DISK_ACCESS_GET_INFO,
                      0u, &vhd) != 0)
        goto done;
    if (ffq_get_physical_path(vhd, physical) != 0) {
        CloseHandle(vhd);
        vhd = NULL;
        if (ffq_open_vhdx(wide, FFQ_VIRTUAL_DISK_ACCESS_READ, 0u, &vhd) != 0)
            goto done;
        if (ffq_attach_vhdx_handle(vhd,
                                   FFQ_ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY |
                                       FFQ_ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER) != 0)
            goto done;
        temporary_attach = 1;
        if (ffq_get_physical_path(vhd, physical) != 0) {
            fprintf(stderr, "GetVirtualDiskPhysicalPath failed after verify attach\n");
            goto done;
        }
    }
    ffq_print_physical_path(physical);
    if (ffq_open_raw_wide(physical, 0, &raw) != 0)
        goto done;
    rc = ffq_verify_raw_device(&raw, 1);
    ffq_close_raw(&raw);

done:
    if (temporary_attach && vhd)
        DetachVirtualDisk(vhd, FFQ_DETACH_VIRTUAL_DISK_FLAG_NONE, 0u);
    if (vhd)
        CloseHandle(vhd);
    free(wide);
    return rc;
}

static int ffq_command_format_raw(const char *path, uint64_t data_mib)
{
    ffq_raw_device raw;
    int rc;

    if (!ffq_is_physical_drive_path(path)) {
        fprintf(stderr, "format-raw only accepts \\\\.\\PhysicalDriveN paths\n");
        return -1;
    }
    if (ffq_open_raw_ascii(path, 1, &raw) != 0)
        return -1;
    rc = ffq_format_raw_device(&raw, data_mib);
    ffq_close_raw(&raw);
    return rc;
}

static int ffq_command_verify_raw(const char *path)
{
    ffq_raw_device raw;
    int rc;

    if (!ffq_is_physical_drive_path(path)) {
        fprintf(stderr, "verify-raw only accepts \\\\.\\PhysicalDriveN paths\n");
        return -1;
    }
    if (ffq_open_raw_ascii(path, 0, &raw) != 0)
        return -1;
    rc = ffq_verify_raw_device(&raw, 0);
    ffq_close_raw(&raw);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        ffq_usage(stderr);
        return 2;
    }

    if (strcmp(argv[1], "create-vhdx") == 0) {
        ffq_format_options options;
        if (ffq_parse_create_options(argc, argv, 3, &options) != 0) {
            ffq_usage(stderr);
            return 2;
        }
        return ffq_command_create_vhdx(argv[2], &options) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "attach-vhdx") == 0) {
        if (argc != 3) {
            ffq_usage(stderr);
            return 2;
        }
        return ffq_command_attach_vhdx(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "detach-vhdx") == 0) {
        if (argc != 3) {
            ffq_usage(stderr);
            return 2;
        }
        return ffq_command_detach_vhdx(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "verify-vhdx") == 0) {
        if (argc != 3) {
            ffq_usage(stderr);
            return 2;
        }
        return ffq_command_verify_vhdx(argv[2]) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "format-raw") == 0) {
        ffq_format_options options;
        int destroy = 0;
        if (ffq_parse_raw_format_options(argc, argv, 3, &options, &destroy) != 0 ||
            !destroy) {
            fprintf(stderr, "format-raw requires --destroy-user-data\n");
            ffq_usage(stderr);
            return 2;
        }
        return ffq_command_format_raw(argv[2], options.fauxfat_data_mib) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "verify-raw") == 0) {
        if (argc != 3) {
            ffq_usage(stderr);
            return 2;
        }
        return ffq_command_verify_raw(argv[2]) == 0 ? 0 : 1;
    }

    ffq_usage(stderr);
    return 2;
}
