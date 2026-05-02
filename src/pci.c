#define FLANTERM_IN_FLANTERM

#include <csmwrap.h>
#include <io.h>
#include <pci.h>
#include <printf.h>
#include <qsort.h>
#include <flanterm_backends/fb.h>

extern struct flanterm_context *flanterm_ctx;

#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

// ECAM (Enhanced Configuration Access Mechanism) support
// ECAM maps PCI config space to memory: base + (bus << 20) + (dev << 15) + (func << 12) + offset

#define ECAM_MAX_REGIONS 16

struct ecam_region {
    uint64_t base;      // MMIO base address
    uint16_t segment;   // PCI segment
    uint8_t start_bus;  // First bus number covered
    uint8_t end_bus;    // Last bus number covered
};

static struct ecam_region ecam_regions[ECAM_MAX_REGIONS];
static size_t ecam_region_count = 0;

// Find ECAM region for a given segment and bus
static struct ecam_region *find_ecam_region(uint16_t segment, uint8_t bus) {
    for (size_t i = 0; i < ecam_region_count; i++) {
        struct ecam_region *region = &ecam_regions[i];
        if (region->segment == segment &&
            bus >= region->start_bus &&
            bus <= region->end_bus) {
            return region;
        }
    }
    return NULL;
}

// Calculate ECAM address for a given PCI address and offset
// Note: Per ACPI spec, the MCFG base address always corresponds to bus 0,
// regardless of what start_bus is. The start_bus is only for validation.
static inline void *ecam_address(struct ecam_region *region, struct pci_address *address, uint32_t offset) {
    uint64_t addr = region->base;
    addr += (uint64_t)(address->bus) << 20;
    addr += (uint64_t)(address->slot) << 15;
    addr += (uint64_t)(address->function) << 12;
    addr += offset;
    return (void *)(uintptr_t)addr;
}

static uint32_t pci_read_pio(struct pci_address *address, uint32_t offset) {
    if (offset >= 0x100)
        return 0xFFFFFFFF;
    return pciConfigReadDWord(address->bus, address->slot, address->function, offset);
}

static void pci_write_pio(struct pci_address *address, uint32_t offset, uint32_t value) {
    if (offset >= 0x100)
        return;
    pciConfigWriteDWord(address->bus, address->slot, address->function, offset, value);
}

static uint32_t pci_read_ecam(struct pci_address *address, uint32_t offset) {
    struct ecam_region *region = find_ecam_region(address->segment, address->bus);
    if (region == NULL) {
        // Fall back to PIO if no ECAM region covers this bus
        // Note: PIO can only access segment 0, so this will silently access
        // the wrong device if address->segment != 0
        return pci_read_pio(address, offset);
    }
    return readl(ecam_address(region, address, offset));
}

static void pci_write_ecam(struct pci_address *address, uint32_t offset, uint32_t value) {
    struct ecam_region *region = find_ecam_region(address->segment, address->bus);
    if (region == NULL) {
        // Fall back to PIO if no ECAM region covers this bus
        // Note: PIO can only access segment 0, so this will silently access
        // the wrong device if address->segment != 0
        pci_write_pio(address, offset, value);
        return;
    }
    writel(ecam_address(region, address, offset), value);
}

typedef uint32_t(*pci_read_t)(struct pci_address *address, uint32_t offset);
typedef void    (*pci_write_t)(struct pci_address *address, uint32_t offset, uint32_t value);

static pci_read_t pci_read = pci_read_pio;
static pci_write_t pci_write = pci_write_pio;

uint32_t pci_read_config_space(struct pci_address *address, uint32_t offset) {
    return pci_read(address, offset & PCI_OFFSET_MASK);
}

void pci_write_config_space(struct pci_address *address, uint32_t offset, uint32_t value) {
    pci_write(address, offset & PCI_OFFSET_MASK, value);
}

// Parse MCFG table and populate ECAM regions
static bool parse_mcfg_table(void) {
    uacpi_table tbl;
    uacpi_status status;

    status = uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &tbl);
    if (status != UACPI_STATUS_OK) {
        printf("MCFG table not found, using legacy PIO for config access\n");
        return false;
    }

    struct acpi_mcfg *mcfg = (struct acpi_mcfg *)tbl.hdr;

    // Calculate number of entries: (table_length - header_size) / entry_size
    size_t entries_size = mcfg->hdr.length - sizeof(struct acpi_mcfg);
    size_t num_entries = entries_size / sizeof(struct acpi_mcfg_allocation);

    printf("MCFG: found %zu ECAM region(s)\n", num_entries);

    for (size_t i = 0; i < num_entries && ecam_region_count < ECAM_MAX_REGIONS; i++) {
        struct acpi_mcfg_allocation *alloc = &mcfg->entries[i];

        ecam_regions[ecam_region_count].base = alloc->address;
        ecam_regions[ecam_region_count].segment = alloc->segment;
        ecam_regions[ecam_region_count].start_bus = alloc->start_bus;
        ecam_regions[ecam_region_count].end_bus = alloc->end_bus;

        printf("  ECAM region %zu: base=0x%llx segment=%u buses=%u-%u\n",
               ecam_region_count, alloc->address, alloc->segment,
               alloc->start_bus, alloc->end_bus);

        ecam_region_count++;
    }

    if (ecam_region_count > 0) {
        // Switch to ECAM access (with PIO fallback built into the functions)
        pci_read = pci_read_ecam;
        pci_write = pci_write_ecam;
        printf("ECAM: enabled memory-mapped config access\n");
        return true;
    }

    return false;
}

// ============================================================================
// PCIe Resizable BAR Support
// ============================================================================

#define PCI_EXT_CAP_ID_REBAR        0x15    // Resizable BAR capability ID

// Find a PCIe extended capability by ID
// Returns the offset in config space, or 0 if not found
static uint16_t pci_find_ext_capability(struct pci_address *address, uint16_t cap_id) {
    // Extended capabilities start at offset 0x100
    uint16_t offset = 0x100;

    // Walk the extended capability list
    for (int i = 0; i < 48 && offset != 0; i++) {  // Max 48 iterations to prevent infinite loop
        uint32_t header = pci_read32(address, offset);

        // Check for invalid header (all 1s or all 0s)
        if (header == 0xFFFFFFFF || header == 0) {
            return 0;
        }

        uint16_t id = header & 0xFFFF;
        uint16_t next = (header >> 20) & 0xFFC;  // Next pointer is bits [31:20], 4-byte aligned

        if (id == cap_id) {
            return offset;
        }

        offset = next;
    }

    return 0;
}

// Try to resize a BAR to fit within max_size bytes
// Returns the new size if successful, or 0 if resize not possible
static uint64_t pci_try_resize_bar(struct pci_address *address, uint8_t bar_index, uint64_t max_size) {
    uint16_t rebar_offset = pci_find_ext_capability(address, PCI_EXT_CAP_ID_REBAR);
    if (rebar_offset == 0) {
        return 0;  // No Resizable BAR capability
    }

    // Read number of resizable BARs from first control register
    uint32_t ctrl0 = pci_read32(address, rebar_offset + 0x08);
    uint8_t num_bars = (ctrl0 >> 5) & 0x7;

    if (num_bars == 0) {
        return 0;
    }


    // Search for the entry corresponding to our BAR
    for (uint8_t i = 0; i < num_bars; i++) {
        uint16_t entry_offset = rebar_offset + 0x04 + (i * 8);
        uint32_t cap = pci_read32(address, entry_offset);
        uint32_t ctrl = pci_read32(address, entry_offset + 4);

        uint8_t this_bar = ctrl & 0x7;  // BAR index in bits [2:0]

        if (this_bar != bar_index) {
            continue;
        }

        // Found the entry for our BAR
        // Supported sizes are in cap bits [31:4], each bit N represents size 2^(N+20)
        uint32_t supported_sizes = (cap >> 4) & 0x0FFFFFFF;


        // Find the largest size that fits within max_size
        // Sizes: bit 0 = 1MB (2^20), bit 1 = 2MB (2^21), etc.
        int best_size_bit = -1;
        for (int bit = 27; bit >= 0; bit--) {  // Check from largest to smallest
            if (!(supported_sizes & (1 << bit))) {
                continue;
            }

            uint64_t size = 1ULL << (bit + 20);
            if (size <= max_size) {
                best_size_bit = bit;
                break;
            }
        }

        if (best_size_bit < 0) {
            return 0;
        }

        uint64_t new_size = 1ULL << (best_size_bit + 20);

        // Save original BAR(s) - PCIe spec: contents are unspecified after resize
        uint32_t bar_offset = 0x10 + bar_index * 4;
        uint32_t orig_bar_lo = pci_read32(address, bar_offset);
        bool bar_is_64 = (orig_bar_lo & 0x6) == 0x4;
        uint32_t orig_bar_hi = bar_is_64 ? pci_read32(address, bar_offset + 4) : 0;

        // Disable memory decode before resizing
        uint16_t cmd = pci_read16(address, 0x04);
        pci_write16(address, 0x04, cmd & ~0x06);  // Clear memory space + bus master

        // Write new size to control register (size in bits [12:8])
        // Preserve BAR index in bits [2:0]
        uint32_t new_ctrl = (ctrl & 0x7) | ((best_size_bit & 0x1F) << 8);
        pci_write32(address, entry_offset + 4, new_ctrl);

        // Restore BAR address clobbered by resize
        pci_write32(address, bar_offset, orig_bar_lo);
        if (bar_is_64) {
            pci_write32(address, bar_offset + 4, orig_bar_hi);
        }

        // Re-enable memory decode
        pci_write16(address, 0x04, cmd);

        return new_size;
    }

    return 0;
}

#define ROOT_BUSES_MAX 64

static struct pci_bus *root_buses[ROOT_BUSES_MAX];
static size_t root_bus_count = 0;

#define BUS_STRUCT_POOL_COUNT 64

static struct pci_bus *bus_struct_pool = NULL;
static size_t bus_struct_pool_ptr = 0;

static struct pci_bus *allocate_bus(void) {
    if (bus_struct_pool_ptr == BUS_STRUCT_POOL_COUNT) {
        return NULL;
    }
    return &bus_struct_pool[bus_struct_pool_ptr++];
}

#define DEVICE_STRUCT_POOL_COUNT 256

static struct pci_device *device_struct_pool = NULL;
static size_t device_struct_pool_ptr = 0;

static struct pci_device *allocate_device(void) {
    if (device_struct_pool_ptr == DEVICE_STRUCT_POOL_COUNT) {
        return NULL;
    }
    return &device_struct_pool[device_struct_pool_ptr++];
}

#define BAR_STRUCT_POOL_COUNT 512

static struct pci_bar *bar_struct_pool = NULL;
static size_t bar_struct_pool_ptr = 0;

static struct pci_bar *allocate_bar(void) {
    if (bar_struct_pool_ptr == BAR_STRUCT_POOL_COUNT) {
        return NULL;
    }
    return &bar_struct_pool[bar_struct_pool_ptr++];
}

static bool add_root_bus(struct pci_bus *bus) {
    if (root_bus_count == ROOT_BUSES_MAX) {
        return false;
    }

    root_buses[root_bus_count++] = bus;

    return true;
}

static struct pci_range *add_range(struct pci_bus *bus, uint64_t base, uint64_t length, bool prefetchable) {
    // For root buses, filter out ranges above 4GB (can't be used by legacy BIOS)
    // For secondary buses, keep >4GB ranges so we can create bridge window pseudo-BARs
    // (the bridge windows will be relocated to <4GB, updating the range)
    if (bus->root) {
        if (base + length > 0x100000000) {
            if (base >= 0x100000000) {
                return NULL;
            }
            length -= (base + length) - 0x100000000;
        }
    }

    // Low memory ranges are special
    if (base < 0x100000) {
        return NULL;
    }

    if (bus->range_count == PCI_MAX_RANGES_PER_BUS) {
        return NULL;
    }

    bus->ranges[bus->range_count].base = base;
    bus->ranges[bus->range_count].length = length;
    bus->ranges[bus->range_count].prefetchable = prefetchable;

    bus->range_count++;

    return &bus->ranges[bus->range_count - 1];
}

static bool drop_range(struct pci_bus *bus, struct pci_range *range) {
    size_t i = ((uintptr_t)range - (uintptr_t)bus->ranges) / sizeof(struct pci_range);
    if (i + 1 < bus->range_count) {
        memmove(range, &bus->ranges[i + 1], (bus->range_count - i - 1) * sizeof(struct pci_range));
    }
    bus->range_count--;
    return true;
}

static bool add_device(struct pci_bus *bus, struct pci_device *device) {
    if (bus->device_count == PCI_MAX_DEVICES_PER_BUS) {
        return false;
    }

    bus->devices[bus->device_count++] = device;

    return true;
}

static bool add_bar(struct pci_bus *bus, struct pci_bar *bar) {
    // Non-bridge BARs >4GB are hopeless
    if (bar->range == NULL && bar->length >= 0x100000000) {
        return true;
    }

    // Likewise for BARs originally below 1MiB
    if (bar->base < 0x100000) {
        return true;
    }

    if (bus->bar_count == PCI_MAX_BARS_PER_BUS) {
        return false;
    }

    bus->bars[bus->bar_count++] = bar;

    return true;
}

static bool drop_bar(struct pci_bus *bus, struct pci_bar *bar) {
    for (size_t i = 0; i < bus->bar_count; i++) {
        if (bus->bars[i] != bar) {
            continue;
        }
        if (i + 1 < bus->bar_count) {
            memmove(&bus->bars[i], &bus->bars[i + 1], (bus->bar_count - i - 1) * sizeof(struct pci_bar *));
        }
        bus->bar_count--;
        break;
    }

    return true;
}

// Check if firmware placed this address within a prefetchable range.
// This is used to determine if a non-prefetchable BAR can safely be
// relocated to a prefetchable range (firmware already deemed it safe).
static bool is_address_in_prefetchable_range(struct pci_bus *bus, uint64_t address) {
    for (size_t i = 0; i < bus->range_count; i++) {
        struct pci_range *range = &bus->ranges[i];
        if (address >= range->base && address < range->base + range->length) {
            return range->prefetchable;
        }
    }
    return false;
}

static int compare_bars(const void *a, const void *b) {
    const struct pci_bar *bar_a = *(const struct pci_bar **)a;
    const struct pci_bar *bar_b = *(const struct pci_bar **)b;

    // Sort by length descending (largest first)
    if (bar_a->length > bar_b->length) return -1;
    if (bar_a->length < bar_b->length) return 1;
    return 0;
}

static void sort_bars(struct pci_bus *bus) {
    qsort(bus->bars, bus->bar_count, sizeof(struct pci_bar *), compare_bars);
    for (size_t i = 0; i < bus->bar_count; i++) {
        if (bus->bars[i]->range == NULL) {
            continue;
        }

        struct pci_bar *bar = bus->bars[i];
        struct pci_device *device = bar->device;
        struct pci_bus *bridge_bus = device->bridge_bus;

        sort_bars(bridge_bus);
    }
}

static void reallocate_bars(struct pci_bus *bus);

static bool fb_relocated = false;

static void reallocate_single_bar(struct pci_bus *bus, struct pci_bar *bar) {
    bool tried_all_prefetchable = false;

again:
    for (size_t i = 0; i < bus->range_count; i++) {
        struct pci_range *range = &bus->ranges[i];

        if (tried_all_prefetchable == false && bar->prefetchable != range->prefetchable) {
            continue;
        }

        // Skip ranges above 4GB - they can't be used by legacy BIOS
        // (Bridge window pseudo-BARs will relocate these ranges to <4GB first)
        if (range->base >= 0x100000000ULL) {
            continue;
        }

        // BARs must be naturally aligned to their size.
        // Compute alignment on the absolute address since the range base
        // (from ACPI for root buses) may not be aligned to large BAR sizes.
        // For bridge windows, align to largest child BAR, not window size.
        uint64_t alignment = bar->length;
        if (bar->bar_number == 0xff && bar->device->bridge_bus != NULL) {
            // Find largest child BAR with matching prefetchability
            uint64_t largest_child = 0x100000; // 1MB minimum for bridge windows
            struct pci_bus *child_bus = bar->device->bridge_bus;
            for (size_t j = 0; j < child_bus->bar_count; j++) {
                struct pci_bar *child = child_bus->bars[j];
                if (child->prefetchable == bar->prefetchable && child->length > largest_child) {
                    largest_child = child->length;
                }
            }
            alignment = largest_child;
        }
        uint64_t current_absolute = range->base + range->reloc_ptr;
        uint64_t aligned_absolute = ALIGN_UP(current_absolute, alignment);

        if (aligned_absolute + bar->length > range->base + range->length) {
            continue;
        }

        uint64_t orig_base = bar->base;
        bar->base = aligned_absolute;
        range->reloc_ptr = (aligned_absolute - range->base) + bar->length;

        struct pci_device *device = bar->device;

        struct pci_address address;
        address.segment = device->root_bus->segment;
        address.bus = device->root_bus->bus;
        address.slot = device->slot;
        address.function = device->function;

        printf("reallocating BAR %u of device %04x:%02x:%02x.%02x from 0x%llx to 0x%llx\n",
               bar->bar_number, bus->segment, bus->bus, bar->device->slot, bar->device->function,
               orig_base, bar->base);

        // Track framebuffer through BAR relocation. Must only match once:
        // a bridge has separate prefetchable and non-prefetchable windows that
        // can alias after relocation, causing a double-adjustment to a wrong
        // address (e.g. GPU MMIO instead of VRAM), corrupting GPU state.
        if (!fb_relocated
         && priv.cb_fb.physical_address >= orig_base
         && priv.cb_fb.physical_address < orig_base + bar->length) {
            printf("BAR contains the EFI framebuffer. Modifying cb_fb.physical_address accordingly...\n");
            printf("  0x%llx => ", priv.cb_fb.physical_address);
            priv.cb_fb.physical_address -= orig_base;
            priv.cb_fb.physical_address += bar->base;
            printf("0x%llx\n", priv.cb_fb.physical_address);
            fb_relocated = true;
        }

        // Disable memory decode while updating BAR to prevent device from
        // responding to partially-updated or stale addresses
        uint16_t cmd = pci_read16(&address, 0x04);
        pci_write16(&address, 0x04, cmd & ~(1 << 1));

        if (bar->bar_number != 0xff) {
            uint64_t new_base = bar->base | (pci_read32(&address, 0x10 + bar->bar_number * 4) & 0xf);

            pci_write32(&address, 0x10 + bar->bar_number * 4, new_base);

            if (bar->is_64) {
                pci_write32(&address, 0x10 + bar->bar_number * 4 + 4, new_base >> 32);
            }
        } else {
            // Bridge window - need to update BOTH base AND limit registers
            // Limit is an absolute address: base + length - 1
            uint8_t base_reg = bar->prefetchable ? 0x24 : 0x20;
            uint8_t limit_reg = bar->prefetchable ? 0x26 : 0x22;

            // Calculate new limit address
            uint64_t new_limit = bar->base + bar->length - 1;

            // Write base register (preserve type bits in low nibble)
            pci_write16(&address, base_reg,
                ((bar->base >> 16) & 0xfff0) | (pci_read16(&address, base_reg) & 0xf));

            // Write limit register (preserve type bits in low nibble)
            pci_write16(&address, limit_reg,
                ((new_limit >> 16) & 0xfff0) | (pci_read16(&address, limit_reg) & 0xf));

            if (bar->prefetchable && bar->is_64) {
                pci_write32(&address, 0x28, bar->base >> 32);
                pci_write32(&address, 0x2c, new_limit >> 32);
            }

            bar->range->base = bar->base;

            bar->device->reallocated_windows++;

            // Count total bridge window bars for this device
            int total_bridge_windows = 0;
            for (size_t j = 0; j < bus->bar_count; j++) {
                if (bus->bars[j]->device == bar->device && bus->bars[j]->bar_number == 0xff) {
                    total_bridge_windows++;
                }
            }

            // Re-enable memory decode BEFORE recursing - child devices need the bridge to forward transactions
            cmd |= (1 << 0) | (1 << 1) | (1 << 2);
            pci_write16(&address, 0x04, cmd);

            // Only recurse once all bridge windows for this device are done
            if (bar->device->reallocated_windows == total_bridge_windows) {
                reallocate_bars(bar->device->bridge_bus);
            }

            // bar_contains_framebuffer is always false for bridge windows (bar_number == 0xff)
            return;
        }

        // Re-enable memory/IO decode and bus master (for regular BARs)
        cmd |= (1 << 0) | (1 << 1) | (1 << 2);
        pci_write16(&address, 0x04, cmd);

        return;
    }

    // Allow fallback to mismatched prefetchability if:
    // 1. BAR is prefetchable (can always use non-prefetchable range safely), or
    // 2. Firmware originally placed this non-prefetchable BAR in a prefetchable range
    //    (trusting that firmware knew this device is safe in prefetchable memory)
    if ((bar->prefetchable || bar->firmware_in_prefetchable) && tried_all_prefetchable == false) {
        tried_all_prefetchable = true;
        goto again;
    }

    printf("failed to reallocate BAR %u for device %04x:%02x:%02x.%02x\n",
           bar->bar_number, bus->segment, bus->bus, bar->device->slot, bar->device->function);

    // Even if this bridge window failed, we still need to track it for recursion purposes.
    // Otherwise, if one window fails, we'll never recurse into the secondary bus.
    if (bar->bar_number == 0xff && bar->device->bridge_bus != NULL) {
        bar->device->reallocated_windows++;

        int total_bridge_windows = 0;
        for (size_t j = 0; j < bus->bar_count; j++) {
            if (bus->bars[j]->device == bar->device && bus->bars[j]->bar_number == 0xff) {
                total_bridge_windows++;
            }
        }

        if (bar->device->reallocated_windows == total_bridge_windows) {
            reallocate_bars(bar->device->bridge_bus);
        }
    }
}

static void reallocate_bars(struct pci_bus *bus) {
    for (size_t i = 0; i < bus->bar_count; i++) {
        reallocate_single_bar(bus, bus->bars[i]);
    }
}

static bool scan_bars(struct pci_device *device) {
    uint8_t max_bars = device->type == PCI_DEVICE_BRIDGE ? 2 : 6;

    struct pci_address address;
    address.segment = device->root_bus->segment;
    address.bus = device->root_bus->bus;
    address.slot = device->slot;
    address.function = device->function;

    if (device->type == PCI_DEVICE_BRIDGE) {
        uint64_t non_prefetchable_base = pci_read16(&address, 0x20);
        uint64_t non_prefetchable_length = pci_read16(&address, 0x22);

        if (non_prefetchable_base != 0) {
            non_prefetchable_base <<= 16;

            non_prefetchable_length <<= 16;
            non_prefetchable_length |= 0xfffff;

            if (non_prefetchable_length < non_prefetchable_base) {
                goto no_non_prefetch_range;
            }

            non_prefetchable_length -= non_prefetchable_base;
            non_prefetchable_length++;

            struct pci_range *range = add_range(device->bridge_bus, non_prefetchable_base, non_prefetchable_length, false);
            if (range == NULL) {
                // Range not usable (above 4G, below 1M, or pool exhausted) - skip
                goto no_non_prefetch_range;
            }

            struct pci_bar *bar = allocate_bar();
            if (bar == NULL) {
                printf("allocate_bar() failed for bridge non-prefetchable window\n");
                goto no_non_prefetch_range;
            }
            bar->range = range;
            bar->base = non_prefetchable_base;
            bar->length = non_prefetchable_length;
            bar->bar_number = 0xff;
            bar->device = device;
            bar->prefetchable = false;
            bar->is_64 = false;
            // Check if firmware placed this non-prefetchable window in a prefetchable region
            bar->firmware_in_prefetchable = is_address_in_prefetchable_range(device->root_bus, non_prefetchable_base);

            if (!add_bar(device->root_bus, bar)) {
                printf("add_bar() failed for bridge non-prefetchable window\n");
                goto no_non_prefetch_range;
            }
        }
no_non_prefetch_range:

        uint64_t prefetchable_base = pci_read16(&address, 0x24);
        uint64_t prefetchable_length = pci_read16(&address, 0x26);

        if (prefetchable_base != 0) {
            bool is_64 = (prefetchable_base & 0xf) == 0x1;

            prefetchable_base &= ~((uint64_t)0xf);
            prefetchable_base <<= 16;

            prefetchable_length &= ~((uint64_t)0xf);
            prefetchable_length <<= 16;
            prefetchable_length |= 0xfffff;

            if (!is_64 && prefetchable_length < prefetchable_base) {
                goto no_prefetch_range;
            }

            if (is_64) {
                prefetchable_base |= (uint64_t)pci_read32(&address, 0x28) << 32;
                prefetchable_length |= (uint64_t)pci_read32(&address, 0x2c) << 32;

                if (prefetchable_length < prefetchable_base) {
                    goto no_prefetch_range;
                }
            }

            prefetchable_length -= prefetchable_base;
            prefetchable_length++;

            struct pci_range *range = add_range(device->bridge_bus, prefetchable_base, prefetchable_length, true);
            if (range == NULL) {
                // Range not usable (above 4G, below 1M, or pool exhausted) - skip
                goto no_prefetch_range;
            }

            struct pci_bar *bar = allocate_bar();
            if (bar == NULL) {
                printf("allocate_bar() failed for bridge prefetchable window\n");
                goto no_prefetch_range;
            }
            bar->range = range;
            bar->base = prefetchable_base;
            bar->length = prefetchable_length;
            bar->bar_number = 0xff;
            bar->device = device;
            bar->prefetchable = true;
            bar->is_64 = is_64;
            // Check if firmware placed this window in a prefetchable region (should always be true for prefetchable windows)
            bar->firmware_in_prefetchable = is_address_in_prefetchable_range(device->root_bus, prefetchable_base);

            if (!add_bar(device->root_bus, bar)) {
                printf("add_bar() failed for bridge prefetchable window\n");
                goto no_prefetch_range;
            }
        }
no_prefetch_range:
    }

    for (uint8_t bar = 0; bar < max_bars; ) {
        uint32_t bar_offset = 0x10 + bar * 4;
        uint32_t bar_value = pci_read32(&address, bar_offset);

        // Memory bar layout is as follows:
        // - bit 0: always 0
        // - bit 1-2: bar type (0 is 32-bit, 1 is reserved, 2 is 64-bit)
        // - bit 3: prefetchable
        // - bit 4-31: base address

        bool is_64bit = false;
        bool prefetchable = false;

        if ((bar_value & (1 << 0)) != 0) {
            bar += 1;
            continue;
        }

        // Check the bar type to figure out whether it's a 64-bit bar
        is_64bit = (bar_value & 0x6) == 0x4;
        prefetchable = (bar_value & (1 << 3)) != 0;

        // Mask out the flag bits to get the bar address
        uint64_t base = bar_value & 0xFFFFFFF0;

        // If the bar is 64-bit then read the next bar's base address
        // and OR that into our current bar's base address - 64-bit bars
        // are made up of two consecutive bars to form a 64-bit address
        if (bar != max_bars - 1 && is_64bit) {
            uint32_t next_bar = pci_read32(&address, bar_offset + 0x4);
            base |= (uint64_t)next_bar << 32;
        }

        // Disable bus master, memory and IO decoding to prevent the device
        // from mistakenly responding to our PCI config space accesses
        uint8_t cmd = pci_read8(&address, 0x4);
        uint8_t new_cmd = cmd;

        new_cmd &= ~(1 << 0); // IO space decoding
        new_cmd &= ~(1 << 1); // Memory space decoding
        new_cmd &= ~(1 << 2); // Bus master

        pci_write8(&address, 0x4, new_cmd);

        // Discover the bar length
        pci_write32(&address, bar_offset, 0xFFFFFFFF);
        uint32_t response = pci_read32(&address, bar_offset);
        pci_write32(&address, bar_offset, bar_value);
        uint64_t length = response & 0xFFFFFFF0;

        if (bar != max_bars - 1 && is_64bit) {
            uint32_t next_bar = pci_read32(&address, bar_offset + 0x4);
            pci_write32(&address, bar_offset + 0x4, 0xFFFFFFFF);
            uint32_t response = pci_read32(&address, bar_offset + 0x4);
            pci_write32(&address, bar_offset + 0x4, next_bar);
            length |= (uint64_t)response << 32;
        } else {
            length |= 0xffffffff00000000;
        }

        length = ~length + 1;

        // Restore command register
        pci_write8(&address, 0x4, cmd);

        // Skip unimplemented BARs. For 64-bit BARs, lower-half response==0
        // is legitimate when size >= 4GB (size bits live in the upper half).
        if (length == 0 || (!is_64bit && response == 0)) {
            goto next_bar;
        }

        // If BAR is larger than the classic 256MB limit, try to resize it
        // 256MB was the standard GPU framebuffer BAR size before Resizable BAR
        if (length > 256 * 1024 * 1024) {
            uint64_t max_resize = 256 * 1024 * 1024;  // 256 MB - classic pre-ReBAR size

            uint64_t new_length = pci_try_resize_bar(&address, bar, max_resize);

            if (new_length > 0 && new_length < length) {
                printf("Resized BAR%d from %llu MB to %llu MB\n", bar,
                       length / (1024 * 1024), new_length / (1024 * 1024));
                length = new_length;
            } else {
                printf("BAR%d too large (%llu MB) and resize failed, skipping\n",
                       bar, length / (1024 * 1024));
                goto next_bar;
            }
        }

        if (base != 0) {
            struct pci_bar *bar_info = allocate_bar();
            if (bar_info == NULL) {
                printf("allocate_bar() failed for device %04x:%02x:%02x.%02x BAR%d\n",
                       device->root_bus->segment, device->root_bus->bus, device->slot, device->function, bar);
                goto next_bar;
            }

            bar_info->device = device;
            bar_info->bar_number = bar;
            bar_info->base = base;
            bar_info->length = length;
            bar_info->prefetchable = prefetchable;
            bar_info->is_64 = is_64bit;
            // Check if firmware placed this BAR in a prefetchable region
            bar_info->firmware_in_prefetchable = is_address_in_prefetchable_range(device->root_bus, base);

            if (!add_bar(device->root_bus, bar_info)) {
                printf("add_bar() failure\n");
            }

            if (prefetchable) {
                device->root_bus->required_prefetchable_size += length;
            } else {
                device->root_bus->required_non_prefetchable_size += length;
            }
        }
next_bar:

        if (is_64bit) {
            bar += 2;
        } else {
            bar += 1;
        }
    }

    return true;
}

static bool scan_bus(struct pci_bus *bus);

static bool scan_function(struct pci_bus *bus, struct pci_address *address) {
    uint8_t subclass = pci_read8(address, 0xA);
    uint8_t class = pci_read8(address, 0xB);

    struct pci_device *device = allocate_device();
    if (device == NULL) {
        printf("allocate_device() failed for %04x:%02x:%02x.%02x\n",
               address->segment, address->bus, address->slot, address->function);
        return false;
    }

    device->root_bus = bus;
    device->slot = address->slot;
    device->function = address->function;

    struct pci_bus *bridge_bus = NULL;

    if (class == 0x6 && subclass == 0x4) {
        bridge_bus = allocate_bus();
        if (bridge_bus == NULL) {
            printf("allocate_bus() failed for bridge %04x:%02x:%02x.%02x\n",
                   address->segment, address->bus, address->slot, address->function);
            // Continue without scanning behind this bridge
            goto not_a_bridge;
        }

        uint8_t secondary_bus = pci_read8(address, 0x19);

        bridge_bus->segment = address->segment;
        bridge_bus->bus = secondary_bus;

        if (!scan_bus(bridge_bus)) {
            printf("scan_bus() failure\n");
        }

        // Propagate required sizes from child bus to parent bus
        // This ensures nested bridge windows are sized correctly
        bus->required_prefetchable_size += bridge_bus->required_prefetchable_size;
        bus->required_non_prefetchable_size += bridge_bus->required_non_prefetchable_size;

        device->type = PCI_DEVICE_BRIDGE;
        device->bridge_bus = bridge_bus;
    } else {
not_a_bridge:
        device->type = PCI_DEVICE_REGULAR;
        device->bridge_bus = NULL;
    }

    if (!scan_bars(device)) {
        printf("scan_bars() failure\n");
    }

    if (bridge_bus != NULL && bridge_bus->range_count == 0) {
        device->bridge_bus = NULL;
    }

    if (!add_device(bus, device)) {
        printf("add_device() failure\n");
    }

    return true;
}

static bool scan_slot(struct pci_bus *bus, struct pci_address *address) {
    uint16_t vendor_id = pci_read16(address, 0x0);

    // No device on this slot, return
    if (vendor_id == 0xFFFF) {
        return true;
    }

    if (!scan_function(bus, address)) {
        printf("scan_function() failure\n");
    }

    // Check if device is multi-function
    uint8_t header_type = pci_read8(address, 0xE);
    if (!(header_type & 0x80)) {
        return true;
    }

    for (uint8_t func = 1; func < 8; func++) {
        struct pci_address func_addr = *address;
        func_addr.function = func;

        vendor_id = pci_read16(&func_addr, 0x0);
        if (vendor_id == 0xFFFF){
            continue;
        }

        if (!scan_function(bus, &func_addr)) {
            printf("scan_function() failure\n");
        }
    }

    return true;
}

static bool scan_bus(struct pci_bus *bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        struct pci_address address;
        address.segment = bus->segment;
        address.bus = bus->bus;
        address.slot = slot;
        address.function = 0;

        if (!scan_slot(bus, &address)) {
            printf("scan_slot() failure\n");
        }
    }

    return true;
}

static void pretty_print_bus(struct pci_bus *bus, int indent) {
    printf("%-*s%s, segment=%d, bus=%d, range_count=%zu, device_count=%zu, bar_count=%zu\n",
        (int)(indent * 2), "", bus->root ? "root bus" : "bridge bus",
        bus->segment, bus->bus, bus->range_count, bus->device_count, bus->bar_count);

    printf("%-*srequired prefetchable size=0x%llx\n", (int)(indent * 2), "", bus->required_prefetchable_size);
    printf("%-*srequired non-prefetchable size=0x%llx\n", (int)(indent * 2), "", bus->required_non_prefetchable_size);

    for (size_t i = 0; i < bus->range_count; i++) {
        struct pci_range *range = &bus->ranges[i];

        printf("%-*srange %zu: base=0x%llx, length=0x%llx [%llx-%llx] (%sprefetchable)\n",
            (int)((indent + 1) * 2), "", i, range->base, range->length, range->base, range->base + range->length - 1,
            range->prefetchable ? "" : "non-");
    }

    for (size_t i = 0; i < bus->device_count; i++) {
        struct pci_device *device = bus->devices[i];

        struct pci_address address;
        address.segment = device->root_bus->segment;
        address.bus = device->root_bus->bus;
        address.slot = device->slot;
        address.function = device->function;

        uint16_t vendor = pci_read16(&address, 0x0);
        uint16_t product = pci_read16(&address, 0x2);

        uint8_t subclass = pci_read8(&address, 0xA);
        uint8_t class = pci_read8(&address, 0xB);

        printf("%-*sdevice %zu: type=%s, address=%04x:%02x:%02x.%02x, vendor=%04x, product=%04x, subclass=%d, class=%d\n",
            (int)((indent + 1) * 2), "", i, device->type == PCI_DEVICE_BRIDGE ? "bridge" : "device",
            bus->segment, bus->bus, device->slot, device->function, vendor, product, subclass, class);

        if (device->bridge_bus != NULL) {
            pretty_print_bus(device->bridge_bus, indent + 2);
        }
    }

    for (size_t j = 0; j < bus->bar_count; j++) {
        struct pci_bar *bar = bus->bars[j];

        printf("%-*sbar%d: device_address=%04x:%02x:%02x.%02x, base=0x%llx, length=0x%llx\n",
            (int)((indent + 1) * 2), "", bar->bar_number, bus->segment, bus->bus, bar->device->slot, bar->device->function, bar->base, bar->length);
        printf("%-*s\t [%llx-%llx] (%sprefetchable, %s-bit)\n",
            (int)((indent + 1) * 2), "", bar->base, bar->base + bar->length - 1, bar->prefetchable ? "" : "non-", bar->is_64 ? "64" : "32");
    }
}

static uacpi_iteration_decision uacpi_discover_root_bus(void *user, uacpi_namespace_node *node, uacpi_u32 node_depth) {
    (void)node_depth;

    uacpi_resources *resources = NULL;
    uacpi_iteration_decision decision = UACPI_ITERATION_DECISION_CONTINUE;
    uacpi_status status = UACPI_STATUS_OK;

    status = uacpi_get_current_resources(node, &resources);
    if (status != UACPI_STATUS_OK) {
        printf("Failed to get node resources: %s\n", uacpi_status_to_string(status));
        goto cleanup;
    }

    struct pci_bus *root_bus = allocate_bus();
    if (root_bus == NULL) {
        printf("allocate_bus() failure\n");
        goto cleanup;
    }

    uint64_t segment = 0, bus_number = 0;

    uacpi_eval_simple_integer(node, "_SEG", &segment);
    uacpi_eval_simple_integer(node, "_BBN", &bus_number);

    root_bus->root = true;
    root_bus->segment = segment;
    root_bus->bus = bus_number;

    uacpi_resource *res = resources->entries;
    while ((void *)res < (void *)resources->entries + resources->length) {
        if (res->type == UACPI_RESOURCE_TYPE_END_TAG) {
            break;
        }

        switch (res->type) {
        case UACPI_RESOURCE_TYPE_IO:
        case UACPI_RESOURCE_TYPE_FIXED_IO:
            // We don't care about IO regions
            break;
        case UACPI_RESOURCE_TYPE_ADDRESS16:
            if (res->address16.common.type != UACPI_RANGE_MEMORY || res->address16.address_length < 0x1000)
                break;
            if (!add_range(root_bus, res->address16.minimum, res->address16.address_length,
                    res->address16.common.attribute.memory.caching != UACPI_NON_CACHEABLE)) {
                printf("add_range() failure\n");
            }
            break;
        case UACPI_RESOURCE_TYPE_ADDRESS32:
            if (res->address32.common.type != UACPI_RANGE_MEMORY || res->address32.address_length < 0x1000)
                break;
            if (!add_range(root_bus, res->address32.minimum, res->address32.address_length,
                    res->address32.common.attribute.memory.caching != UACPI_NON_CACHEABLE)) {
                printf("add_range() failure\n");
            }
            break;
        case UACPI_RESOURCE_TYPE_ADDRESS64:
            if (res->address64.common.type != UACPI_RANGE_MEMORY || res->address64.address_length < 0x1000)
                break;
            if (!add_range(root_bus, res->address64.minimum, res->address64.address_length,
                    res->address64.common.attribute.memory.caching != UACPI_NON_CACHEABLE)) {
                printf("add_range() failure\n");
            }
            break;
        default:
            printf("Unknown PCI root bus resource type %u\n", res->type);
            break;
        }

        res = UACPI_NEXT_RESOURCE(res);
    }

    if (!add_root_bus(root_bus)) {
        goto cleanup;
    }

    goto out;

cleanup:
    decision = UACPI_ITERATION_DECISION_BREAK;

out:
    if (resources != NULL) {
        uacpi_free_resources(resources);
    }

    *(uacpi_status *)user = status;
    return decision;
}

static bool uacpi_discover_root_bridges(void) {
    uacpi_status status;
    uacpi_status iter_status = UACPI_STATUS_OK;

    status = uacpi_find_devices_at(
        uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB),
        (const char *[]){"PNP0A03", "PNP0A08", NULL}, uacpi_discover_root_bus, &iter_status);

    if (iter_status != UACPI_STATUS_OK) {
        status = iter_status;
    }

    if (status != UACPI_STATUS_OK) {
        printf("uACPI find devices failed: %s\n", uacpi_status_to_string(status));
        return false;
    }

    return true;
}

bool pci_early_initialize(void) {
    if (gBS->AllocatePool(EfiLoaderData, sizeof(struct pci_bus) * BUS_STRUCT_POOL_COUNT, (void *)&bus_struct_pool) != EFI_SUCCESS) {
        return false;
    }
    memset(bus_struct_pool, 0, sizeof(struct pci_bus) * BUS_STRUCT_POOL_COUNT);

    if (gBS->AllocatePool(EfiLoaderData, sizeof(struct pci_device) * DEVICE_STRUCT_POOL_COUNT, (void *)&device_struct_pool) != EFI_SUCCESS) {
        return false;
    }
    memset(device_struct_pool, 0, sizeof(struct pci_device) * DEVICE_STRUCT_POOL_COUNT);

    if (gBS->AllocatePool(EfiLoaderData, sizeof(struct pci_bar) * BAR_STRUCT_POOL_COUNT, (void *)&bar_struct_pool) != EFI_SUCCESS) {
        return false;
    }
    memset(bar_struct_pool, 0, sizeof(struct pci_bar) * BAR_STRUCT_POOL_COUNT);

    if (!acpi_namespace_init()) {
        return false;
    }

    // Parse MCFG table to enable ECAM config access (optional, falls back to PIO)
    parse_mcfg_table();

    if (!uacpi_discover_root_bridges()) {
        return false;
    }

    printf("discovered %zu root buses\n", root_bus_count);

    return true;
}

// Check if any BAR on this bus or its child buses is above 4GB
static bool has_bars_above_4g(struct pci_bus *bus) {
    // Check BARs on this bus
    for (size_t i = 0; i < bus->bar_count; i++) {
        if (bus->bars[i]->base >= 0x100000000ULL) {
            return true;
        }
    }

    // Recursively check child buses (bridges)
    for (size_t i = 0; i < bus->device_count; i++) {
        struct pci_device *device = bus->devices[i];
        if (device->bridge_bus != NULL) {
            if (has_bars_above_4g(device->bridge_bus)) {
                return true;
            }
        }
    }

    return false;
}

static bool resize_bridge_windows(struct pci_bus *bus) {
    // First, recursively resize all child bridge windows (bottom-up order).
    // This ensures that when we size this level's windows, child windows
    // have already been resized and we can use their actual sizes.
    for (size_t i = 0; i < bus->bar_count; i++) {
        if (bus->bars[i]->range == NULL) {
            continue;
        }
        struct pci_device *device = bus->bars[i]->device;
        if (device->bridge_bus != NULL) {
            resize_bridge_windows(device->bridge_bus);
        }
    }

    // Now resize this level's bridge windows
again:
    for (size_t i = 0; i < bus->bar_count; i++) {
        if (bus->bars[i]->range == NULL) {
            continue;
        }

        struct pci_bar *bar = bus->bars[i];
        struct pci_device *device = bar->device;
        struct pci_range *range = bar->range;
        struct pci_bus *bridge_bus = device->bridge_bus;

        // Compute required size: sum of regular BARs + sum of (already resized) child bridge windows
        uint64_t new_size = 0;
        for (size_t j = 0; j < bridge_bus->bar_count; j++) {
            struct pci_bar *child_bar = bridge_bus->bars[j];
            if (child_bar->prefetchable != range->prefetchable) {
                continue;
            }
            new_size += child_bar->length;
        }

        struct pci_address address;
        address.segment = device->root_bus->segment;
        address.bus = device->root_bus->bus;
        address.slot = device->slot;
        address.function = device->function;

        // Read from correct registers based on window type
        uint8_t base_reg = range->prefetchable ? 0x24 : 0x20;
        uint8_t limit_reg = range->prefetchable ? 0x26 : 0x22;
        uint64_t raw_base = pci_read16(&address, base_reg);
        uint64_t raw_limit = pci_read16(&address, limit_reg);

        // Only prefetchable windows have the 64-bit type field (bits 0-3)
        bool is_64 = range->prefetchable && ((raw_base & 0xf) == 0x1);

        printf("new_size=%llx\n", new_size);

        if (new_size == 0) {
            printf("dropping %sprefetchable window of bridge device %04x:%02x:%02x.%02x\n",
                   range->prefetchable ? "" : "non-", bus->segment, bus->bus, device->slot, device->function);
            pci_write16(&address, base_reg, 0x10 | (raw_base & 0xf));
            pci_write16(&address, limit_reg, raw_limit & 0xf);
            if (is_64) {
                pci_write32(&address, 0x28, 0);
                pci_write32(&address, 0x2c, 0);
            }
            drop_range(bridge_bus, range);
            if (bridge_bus->range_count == 0) {
                device->bridge_bus = NULL;
            }
            drop_bar(bus, bar);
            goto again;
        }

        // Round up window size to 1MB (bridge window granularity).
        // The window will be aligned to largest child BAR during relocation,
        // so children will naturally land on aligned addresses.
        new_size = ALIGN_UP(new_size, 0x100000);

        printf("resizing %sprefetchable window of bridge device %04x:%02x:%02x.%02x from %llx to %llx\n",
               range->prefetchable ? "" : "non-", bus->segment, bus->bus, device->slot, device->function, bar->length, new_size);

        uint64_t new_limit = range->base + new_size - 1;

        pci_write16(&address, limit_reg,
            ((new_limit >> 16) & 0xfff0) | (raw_limit & 0x000f));

        if (is_64) {
            pci_write32(&address, 0x2c, new_limit >> 32);
        }

        range->length = new_size;
        bar->length = new_size;
    }

    return true;
}

bool pci_late_initialize(void) {
    for (size_t i = 0; i < root_bus_count; i++) {
        if (!scan_bus(root_buses[i])) {
            printf("scan_bus() failure\n");
        }
    }

    // Save and disable Flanterm during relocation.
    // When we relocate bridge windows, the address range the bridge forwards changes,
    // so accessing the framebuffer at the old address will fail even before we
    // relocate the VGA BAR itself. Disabling Flanterm prevents crashes during this phase.
    uint64_t old_fb_addr = priv.cb_fb.physical_address;
    struct flanterm_context *saved_flanterm_ctx = flanterm_ctx;
    flanterm_ctx = NULL;

    for (size_t i = 0; i < root_bus_count; i++) {
        struct pci_bus *bus = root_buses[i];

        // Check if this root bus needs relocation
        if (!has_bars_above_4g(bus)) {
            printf("Root bus %u: all BARs below 4GB, skipping relocation\n", bus->bus);
            continue;
        }

        printf("Root bus %u: found BARs above 4GB, performing relocation...\n", bus->bus);

        pretty_print_bus(bus, 0);
        printf("---------------\n");

        resize_bridge_windows(bus);
        sort_bars(bus);
        reallocate_bars(bus);

        printf("---------------\n");
        pretty_print_bus(bus, 0);
    }

    // Update Flanterm's framebuffer pointer if it was relocated
    if (saved_flanterm_ctx != NULL && priv.cb_fb.physical_address != old_fb_addr) {
        ((struct flanterm_fb_context *)saved_flanterm_ctx)->framebuffer =
            (void *)(uintptr_t)priv.cb_fb.physical_address;
    }

    // Re-enable Flanterm
    flanterm_ctx = saved_flanterm_ctx;

    if (priv.cb_fb.physical_address >= 0x100000000ULL) {
        panic("Framebuffer at 0x%llx is above 4GB and could not be relocated\n",
              priv.cb_fb.physical_address);
    }

    return true;
}
