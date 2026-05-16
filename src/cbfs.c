#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <printf.h>

#include <cbfs.h>

#define CBFS_HEADER_MAGIC_LE 0x4F524243UL
#define CBFS_HEADER_MAGIC_BE 0x4342524FUL
#define CBFS_FILE_MAGIC "LARCHIVE"

static inline uint32_t be32(uint32_t v)
{
    uint8_t *p = (uint8_t *)&v;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/*
 * Locate the coreboot CBFS master header.
 *
 * On memory-mapped x86 flash the 32-bit word at 0xFFFFFFFC is the absolute
 * address of the master header - this is exactly what the SeaBIOS
 * coreboot_cbfs_init() we ship relies on. Some coreboot builds instead store
 * it as a signed offset relative to the 4 GiB top, so if the direct address
 * does not carry the CBFS magic we retry with 0x100000000 + (int32_t)raw.
 * Both candidates are validated against the magic before use.
 *
 * Returns NULL if no valid header is found (e.g. not booted via coreboot, or
 * an FMAP-only image with no legacy master header).
 */
struct cbfs_header *cbfs_find_header(void)
{
    volatile uint32_t *ptr_loc = (volatile uint32_t *)0xFFFFFFFCUL;
    uint32_t raw = *ptr_loc;

    printf("CBFS: header pointer raw=0x%08x\n", raw);

    /*
     * The master header pointer is always 4-byte aligned. A misaligned value
     * (notably the 0xFFFFFFFF read back from erased/non-coreboot flash) means
     * there is no CBFS - bail before dereferencing a wild address. SeaBIOS
     * performs the same check.
     */
    if (raw & 0x3) {
        printf("CBFS: no master header pointer (raw misaligned)\n");
        return NULL;
    }

    struct cbfs_header *hdr = (struct cbfs_header *)(uintptr_t)raw;
    if (hdr->magic == CBFS_HEADER_MAGIC_LE || hdr->magic == CBFS_HEADER_MAGIC_BE) {
        printf("CBFS: found header at 0x%08x (direct address)\n", raw);
        goto found;
    }

    uint64_t header_addr = (uint64_t)0x100000000ULL + (int32_t)raw;
    hdr = (struct cbfs_header *)(uintptr_t)header_addr;
    if (hdr->magic == CBFS_HEADER_MAGIC_LE || hdr->magic == CBFS_HEADER_MAGIC_BE) {
        printf("CBFS: found header at 0x%llx (4GiB-relative)\n",
               (unsigned long long)header_addr);
        goto found;
    }

    printf("CBFS: no valid header found (magic=0x%08x)\n", hdr->magic);
    return NULL;

found:
    printf("CBFS: romsize=%u bootblocksize=%u offset=%u align=%u\n",
           be32(hdr->romsize), be32(hdr->bootblocksize),
           be32(hdr->offset), be32(hdr->align));
    return hdr;
}

void *cbfs_find_file(struct cbfs_header *hdr,
                     const char *name, uint32_t *data_len)
{
    uint32_t romsize = be32(hdr->romsize);
    uint32_t align   = be32(hdr->align);
    if (align == 0)
        align = 64;                 /* coreboot mandates a 64-byte alignment */

    uintptr_t rom_base = (uintptr_t)(0x100000000ULL - (uint64_t)romsize);
    uintptr_t cur      = rom_base + be32(hdr->offset);

    printf("CBFS: walking files from 0x%lx (romsize=%u align=%u)\n",
           (unsigned long)cur, romsize, align);

    /* Bound the walk by the ROM extent the header itself describes. */
    while (cur >= rom_base && cur - rom_base <= romsize) {
        struct cbfs_file *f = (struct cbfs_file *)cur;

        /*
         * CBFS components are contiguous starting at hdr->offset; the first
         * entry without the "LARCHIVE" magic terminates the archive. This is
         * what SeaBIOS' coreboot_cbfs_init() does - scanning ahead for the
         * next magic instead can false-match magic bytes inside file data.
         */
        bool magic_ok = true;
        for (int i = 0; i < 8; i++) {
            if (f->magic[i] != CBFS_FILE_MAGIC[i]) {
                magic_ok = false;
                break;
            }
        }
        if (!magic_ok)
            break;

        uint32_t flen    = be32(f->len);
        uint32_t foffset = be32(f->offset);
        char    *fname   = (char *)(cur + sizeof(struct cbfs_file));

        printf("CBFS: file '%s' len=%u type=0x%x\n", fname, flen, be32(f->type));

        /* Compare name */
        bool match = true;
        const char *a = fname, *b = name;
        while (*a && *b) {
            if (*a != *b) { match = false; break; }
            a++; b++;
        }
        if (match && *a == *b) {
            /* Found it */
            *data_len = flen;
            void *data = (void *)(cur + foffset);
            printf("CBFS: found '%s' at 0x%lx len=%u\n",
                   name, (unsigned long)(cur + foffset), flen);
            return data;
        }

        /*
         * Next entry sits at the next align-aligned boundary past this
         * file's data, mirroring SeaBIOS' ALIGN(data + len, hdr->align).
         */
        uintptr_t next = cur + foffset + flen;
        next = (next + (align - 1)) & ~((uintptr_t)align - 1);
        if (next <= cur)            /* corrupt entry - stop, don't spin */
            break;
        cur = next;
    }

    printf("CBFS: file '%s' not found\n", name);
    return NULL;
}
