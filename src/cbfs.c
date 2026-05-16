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
 * The pointer at 0xFFFFFFFC holds a 32-bit value that, when added to
 * 0x100000000, gives the physical address of the CBFS master header.
 * i.e. header_phys = 0x100000000 + (int32_t)(*(uint32_t*)0xFFFFFFFC)
 *
 * Returns NULL if the header doesn't look valid.
 */
struct cbfs_header *cbfs_find_header(void)
{
    volatile uint32_t *ptr_loc = (volatile uint32_t *)0xFFFFFFFCUL;
    uint32_t raw = *ptr_loc;

    printf("CBFS: header pointer raw=0x%08x\n", raw);

    struct cbfs_header *hdr = (struct cbfs_header *)(uintptr_t)raw;

    if (hdr->magic == CBFS_HEADER_MAGIC_LE || hdr->magic == CBFS_HEADER_MAGIC_BE) {
        printf("CBFS: found header at 0x%08x (direct address)\n", raw);
        goto found;
    }

    uint64_t header_addr = (uint64_t)0x100000000ULL + (int32_t)raw;
    hdr = (struct cbfs_header *)(uintptr_t)header_addr;

    if (hdr->magic == CBFS_HEADER_MAGIC_LE || hdr->magic == CBFS_HEADER_MAGIC_BE) {
        printf("CBFS: found header at 0x%llx (offset method)\n",
               (unsigned long long)header_addr);
        goto found;
    }

    printf("CBFS: no valid header found (magic=0x%08x)\n", hdr->magic);
    return NULL;

found:
    printf("CBFS: romsize=%u bootblocksize=%u offset=%u\n",
           be32(hdr->romsize), be32(hdr->bootblocksize), be32(hdr->offset));
    return hdr;
}

static uintptr_t cbfs_rom_base(struct cbfs_header *hdr)
{
    return (uintptr_t)(0x100000000ULL - (uint64_t)be32(hdr->romsize));
}

void *cbfs_find_file(struct cbfs_header *hdr,
                     const char *name, uint32_t *data_len)
{
    uintptr_t rom_base = cbfs_rom_base(hdr);
    uintptr_t cur = rom_base + be32(hdr->offset);
    uintptr_t rom_end = (uintptr_t)0xFFFFFFFFUL;

    printf("CBFS: walking files from 0x%lx\n", (unsigned long)cur);

    while (cur < rom_end) {
        struct cbfs_file *f = (struct cbfs_file *)cur;

        /* Check file magic */
        bool magic_ok = true;
        for (int i = 0; i < 8; i++) {
            if (f->magic[i] != CBFS_FILE_MAGIC[i]) {
                magic_ok = false;
                break;
            }
        }
        if (!magic_ok) {
            cur += 64;
            continue;
        }

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

        uint32_t entry_size = foffset + flen;
        entry_size = (entry_size + 63) & ~63U;
        if (entry_size == 0) {
            /* Corrupt entry - keep scanning instead of spinning forever */
            cur += 64;
            continue;
        }
        cur += entry_size;
    }

    printf("CBFS: file '%s' not found\n", name);
    return NULL;
}
