#include <efi.h>

#include <printf.h>
#include "csmwrap.h"
#include "io.h"

/* This is not in E820.h */
#define EfiAcpiAddressRangeHole     (-1ULL)

static const char *
e820_type_name(uint32_t type)
{
    switch (type) {
    case EfiAcpiAddressRangeMemory:      return "RAM";
    case EfiAcpiAddressRangeReserved:    return "RESERVED";
    case EfiAcpiAddressRangeACPI:        return "ACPI";
    case EfiAcpiAddressRangeNVS:         return "NVS";
    default:                             return "UNKNOWN";
    }
}

// Remove an entry from the e820_map.
static void
remove_e820(struct csmwrap_priv *priv, int i)
{
    EFI_E820_ENTRY64 *e820_map = priv->low_stub->e820_map;
    int *e820_count = &priv->low_stub->e820_entries;

    if (i < 0 || i >= *e820_count) {
        printf("e820_map remove index out of range\n");
        return;
    }

    (*e820_count)--;
    memmove(&e820_map[i], &e820_map[i+1],
            sizeof(EFI_E820_ENTRY64) * (*e820_count - i));
}

// Insert an entry in the e820_map at the given position.
static void
insert_e820(struct csmwrap_priv *priv,
            int i, uint64_t start, uint64_t size, uint64_t type)
{
    EFI_E820_ENTRY64 *e820_map = priv->low_stub->e820_map;
    int *e820_count = &priv->low_stub->e820_entries;

    if (*e820_count >= E820_MAX_ENTRIES) {
        printf("e820_map overflow\n");
        return;
    }

    memmove(&e820_map[i + 1], &e820_map[i],
            sizeof(EFI_E820_ENTRY64) * (*e820_count - i));

    (*e820_count)++;
    EFI_E820_ENTRY64 *e = &e820_map[i];
    e->BaseAddr = start;
    e->Length = size;
    e->Type = type;
}

// Show the current e820_map.
static void
dump_map(struct csmwrap_priv *priv)
{
    EFI_E820_ENTRY64 *e820_map = priv->low_stub->e820_map;
    int e820_count = priv->low_stub->e820_entries;

    printf("csmwrap e820 map has %d items:\n", e820_count);
    int i;
    for (i = 0; i < e820_count; i++) {
        EFI_E820_ENTRY64 *e = &e820_map[i];
        uint64_t e_end = e->BaseAddr + e->Length;

        printf("  %d: %016llx - %016llx = %d %s\n", i,
               e->BaseAddr, e_end, e->Type, e820_type_name(e->Type));
    }
}

static const char *
efi_memory_type_name(uint32_t type)
{
    switch (type) {
    case EfiReservedMemoryType:      return "Reserved";
    case EfiLoaderCode:              return "LoaderCode";
    case EfiLoaderData:              return "LoaderData";
    case EfiBootServicesCode:        return "BSCode";
    case EfiBootServicesData:        return "BSData";
    case EfiRuntimeServicesCode:     return "RTCode";
    case EfiRuntimeServicesData:     return "RTData";
    case EfiConventionalMemory:      return "Conventional";
    case EfiUnusableMemory:          return "Unusable";
    case EfiACPIReclaimMemory:       return "ACPIReclaim";
    case EfiACPIMemoryNVS:           return "ACPINVS";
    case EfiMemoryMappedIO:          return "MMIO";
    case EfiMemoryMappedIOPortSpace: return "MMIOPort";
    case EfiPalCode:                 return "PalCode";
    case EfiPersistentMemory:        return "Persistent";
    case EfiUnacceptedMemoryType:    return "Unaccepted";
    default:                         return "UNKNOWN";
    }
}

// Show the raw UEFI memory map that build_e820_map() consumes.
static void
dump_efi_memory_map(EFI_MEMORY_DESCRIPTOR *memory_map,
                    UINTN memory_map_size, UINTN descriptor_size)
{
    EFI_MEMORY_DESCRIPTOR *end = (EFI_MEMORY_DESCRIPTOR *)
                                 ((uint8_t *)memory_map + memory_map_size);
    int count = descriptor_size ? (int)(memory_map_size / descriptor_size) : 0;

    printf("EFI memory map has %d entries (descriptor_size=%u):\n",
           count, (unsigned)descriptor_size);

    int i = 0;
    for (EFI_MEMORY_DESCRIPTOR *d = memory_map; d < end;
         d = NextMemoryDescriptor(d, descriptor_size), i++) {
        uint64_t bytes = d->NumberOfPages * EFI_PAGE_SIZE;
        uint64_t end_addr = d->PhysicalStart + bytes;

        printf("  %3d: %016llx - %016llx (%6llu pages) %2u %-13s attr=%016llx%s\n",
               i, d->PhysicalStart, end_addr,
               (unsigned long long)d->NumberOfPages,
               d->Type, efi_memory_type_name(d->Type),
               (unsigned long long)d->Attribute,
               (d->Attribute & EFI_MEMORY_RUNTIME) ? " RT" : "");
    }
}

void e820_add(struct csmwrap_priv *priv, uint64_t start,
              uint64_t size, uint64_t type)
{
    EFI_E820_ENTRY64 *e820_map = priv->low_stub->e820_map;
    int *e820_count = &priv->low_stub->e820_entries;

    if (!size)
        return;

    // Find position of new item (splitting existing item if needed).
    uint64_t end = start + size;
    int i;
    for (i = 0; i < *e820_count; i++) {
        EFI_E820_ENTRY64 *e = &e820_map[i];
        uint64_t e_end = e->BaseAddr + e->Length;
        if (start > e_end)
            continue;
        // Found position - check if an existing item needs to be split.
        if (start > e->BaseAddr) {
            if (type == e->Type) {
                // Same type - merge them.
                size += start - e->BaseAddr;
                start = e->BaseAddr;
            } else {
                // Split existing item.
                e->Length = start - e->BaseAddr;
                i++;
                if (e_end > end)
                    insert_e820(priv, i, end, e_end - end, e->Type);
            }
        }
        break;
    }
    // Remove/adjust existing items that are overlapping.
    while (i < *e820_count) {
        EFI_E820_ENTRY64 *e = &e820_map[i];
        if (end < e->BaseAddr)
            // No overlap - done.
            break;
        uint64_t e_end = e->BaseAddr + e->Length;
        if (end >= e_end) {
            // Existing item completely overlapped - remove it.
            remove_e820(priv, i);
            continue;
        }
        // Not completely overlapped - adjust its start.
        e->BaseAddr = end;
        e->Length = e_end - end;
        if (type == e->Type) {
            // Same type - merge them.
            size += e->Length;
            remove_e820(priv, i);
        }
        break;
    }
    // Insert new item.
    if (type != EfiAcpiAddressRangeHole)
        insert_e820(priv, i, start, size, type);
}

// Remove any definitions in a memory range (make a memory hole).
void
e820_remove(struct csmwrap_priv *priv, uint64_t start, uint64_t size)
{
    e820_add(priv, start, size, EfiAcpiAddressRangeHole);
}

/*
 * Convert UEFI memory types to E820 types
 * See https://uefi.org/sites/default/files/resources/ACPI_4_Errata_A.pdf Table 14-6
 */
static uint32_t convert_memory_type(EFI_MEMORY_TYPE type)
{
    switch (type) {
        case EfiConventionalMemory:
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
            return EfiAcpiAddressRangeMemory;
        case EfiACPIReclaimMemory:
            return EfiAcpiAddressRangeACPI;
        case EfiACPIMemoryNVS:
            return EfiAcpiAddressRangeNVS;
        case EfiUnusableMemory:
        case EfiReservedMemoryType:
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:
        case EfiMemoryMappedIO:
        case EfiMemoryMappedIOPortSpace:
        case EfiPalCode:
        default:
            return EfiAcpiAddressRangeReserved;
    }
}

static inline void cmos_write(uint8_t reg, uint8_t val)
{
    outb(0x70, 0x80 | reg);
    outb(0x80, 0);
    outb(0x71, val);
}

/*
 * Write CMOS memory size registers to match the E820 map.
 *
 * UEFI firmware does not initialize legacy CMOS memory registers, leaving them
 * with garbage values. Some legacy software (notably Windows 95 setup) reads
 * CMOS directly to detect extended memory size, bypassing INT 15h and XMS.
 *
 * CMOS layout:
 *   0x15/0x16: Base memory in KB (should be 640)
 *   0x17/0x18: Total extended memory in KB, saturated to 65535
 *   0x30/0x31: Total extended memory in KB, saturated to 65535 (mirror of 0x17/0x18)
 *   0x34/0x35: Extended memory above 16MB, in 64KB blocks (max 65535)
 */
static void
e820_update_cmos(struct csmwrap_priv *priv)
{
    EFI_E820_ENTRY64 *e820_map = priv->low_stub->e820_map;
    int e820_count = priv->low_stub->e820_entries;

    /* Find end of contiguous RAM starting from 1MB */
    uint64_t ram_end = 0x100000;
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < e820_count; i++) {
            EFI_E820_ENTRY64 *e = &e820_map[i];
            uint64_t e_end = e->BaseAddr + e->Length;
            if (e->Type != EfiAcpiAddressRangeMemory &&
                e->Type != EfiAcpiAddressRangeACPI)
                continue;
            if (e_end > 0xFFFFFFFFULL)
                continue;
            if (e->BaseAddr <= ram_end && e_end > ram_end) {
                ram_end = e_end;
                progress = 1;
            }
        }
    }

    uint64_t ext_kb = (ram_end > 0x100000) ? (ram_end - 0x100000) / 1024 : 0;
    uint64_t above_16m_64k = (ram_end > 0x1000000) ?
                             (ram_end - 0x1000000) / 65536 : 0;

    /* Cap to register widths */
    uint16_t cmos_17_18 = (ext_kb > 65535) ? 65535 : (uint16_t)ext_kb;
    uint16_t cmos_30_31 = (ext_kb > 65535) ? 65535 : (uint16_t)ext_kb;
    uint16_t cmos_34_35 = (above_16m_64k > 65535) ? 65535 :
                          (uint16_t)above_16m_64k;

    printf("CMOS: base=640 ext=%u KB >16M=%u x64KB (ram_end=0x%llx)\n",
           cmos_30_31, cmos_34_35, ram_end);

    /* Base memory: 640 KB */
    cmos_write(0x15, 640 & 0xFF);
    cmos_write(0x16, 640 >> 8);

    /* Extended memory 1-16MB range in KB */
    cmos_write(0x17, cmos_17_18 & 0xFF);
    cmos_write(0x18, cmos_17_18 >> 8);

    /* Extended memory 1-64MB range in KB */
    cmos_write(0x30, cmos_30_31 & 0xFF);
    cmos_write(0x31, cmos_30_31 >> 8);

    /* Extended memory above 16MB in 64KB blocks */
    cmos_write(0x34, cmos_34_35 & 0xFF);
    cmos_write(0x35, cmos_34_35 >> 8);

    /* Bit 7 of port 0x70 is the chipset NMI mask; clear it so we don't
     * hand off to legacy BIOS with NMI delivery gated. */
    outb(0x70, 0);
}

/*
 * Build E820 memory map based on UEFI GetMemoryMap
 * Return the number of entries in the E820 map
 */
int build_e820_map(struct csmwrap_priv *priv, EFI_MEMORY_DESCRIPTOR *memory_map, UINTN memory_map_size, UINTN descriptor_size)
{
    EFI_MEMORY_DESCRIPTOR *memory_map_end;
    EFI_MEMORY_DESCRIPTOR *memory_map_ptr;

    memory_map_end = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)memory_map + memory_map_size);

    dump_efi_memory_map(memory_map, memory_map_size, descriptor_size);

    /* Process each memory descriptor and convert to E820 format */
    for (memory_map_ptr = memory_map; 
         memory_map_ptr < memory_map_end;
         memory_map_ptr = NextMemoryDescriptor(memory_map_ptr, descriptor_size)) {

        uint64_t start = memory_map_ptr->PhysicalStart;
        uint64_t end = start + (memory_map_ptr->NumberOfPages * EFI_PAGE_SIZE);
        uint32_t type = convert_memory_type(memory_map_ptr->Type);
        
        /* Skip zero-length regions */
        if (start == end)
            continue;

        e820_add(priv, start, end - start, type);
    }

    /* Remove whole 1MB, we are going to fix it later */
    e820_remove(priv, 0, 0x100000);
    /*
     * Reproduce SeaBIOS's initial low-memory layout: 639KB usable + 1KB EBDA.
     * SeaBIOS recomputes its own e820 from endlow/ebda_seg during init and
     * may grow the EBDA further (option ROMs allocating BBS/ATA/MPT space),
     * so this map is a starting snapshot, not the final OS-visible map.
     * Reporting only 512KB here previously confused HIMEM.SYS / EMM386.
     */
    e820_add(priv, 0, 0x9FC00, EfiAcpiAddressRangeMemory);
    e820_add(priv, 0x9FC00, 0x400, EfiAcpiAddressRangeReserved);
    /* Reserve Expansion BIOS (video RAM, option ROMs, system ROM) */
    e820_add(priv, 0xa0000, 0x100000 - 0xa0000, EfiAcpiAddressRangeReserved);

    dump_map(priv);

    e820_update_cmos(priv);

    return 0;
}
