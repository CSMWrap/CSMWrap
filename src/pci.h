#ifndef PCI_H
#define PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pci_address {
    uint16_t segment;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
};

struct pci_range {
    uint64_t base;
    uint64_t length;
    uint64_t reloc_ptr;
    bool prefetchable;
};

enum pci_device_type {
    PCI_DEVICE_REGULAR,
    PCI_DEVICE_BRIDGE,
};

struct pci_device {
    // Type of the device
    enum pci_device_type type;

    // The root bus that this device is on
    struct pci_bus *root_bus;

    // A bus if a device is a bridge.
    struct pci_bus *bridge_bus;

    // Address of the device on the bus
    uint8_t slot;
    uint8_t function;

    int reallocated_windows;
};

struct pci_bar {
    // The PCI device that this bar belongs to
    struct pci_device *device;

    // The bar number in context of the device
    uint8_t bar_number;

    bool is_64;
    bool prefetchable;

    // If true, firmware originally placed this BAR in a prefetchable region.
    // This allows non-prefetchable BARs to be relocated to prefetchable ranges
    // if the firmware deemed it safe for this particular device.
    bool firmware_in_prefetchable;

    // Base address and size of the bar
    uint64_t base;
    uint64_t length;

    // Range associated with bridge window pseudo-BARs.
    struct pci_range *range;
};

#define PCI_MAX_RANGES_PER_BUS 32
#define PCI_MAX_DEVICES_PER_BUS 256
#define PCI_MAX_BARS_PER_BUS 512

struct pci_bus {
    bool root;

    uint32_t segment;
    uint8_t bus;

    // Sorted list of address ranges this root bus decodes
    size_t range_count;
    struct pci_range ranges[PCI_MAX_RANGES_PER_BUS];

    // List of devices associated with this root bus
    size_t device_count;
    struct pci_device *devices[PCI_MAX_DEVICES_PER_BUS];

    // Sorted list of allocated bars associated with this root bus
    size_t bar_count;
    struct pci_bar *bars[PCI_MAX_BARS_PER_BUS];

    uint64_t required_prefetchable_size;
    uint64_t required_non_prefetchable_size;
};

#define pci_read8(address, offset)  ((uint8_t)pci_read_config_space((address), (offset), 1))
#define pci_read16(address, offset) ((uint16_t)pci_read_config_space((address), (offset), 2))
#define pci_read32(address, offset) pci_read_config_space((address), (offset), 4)

#define pci_write8(address, offset, value)  pci_write_config_space((address), (offset), 1, (value))
#define pci_write16(address, offset, value) pci_write_config_space((address), (offset), 2, (value))
#define pci_write32(address, offset, value) pci_write_config_space((address), (offset), 4, (value))

// Read `size` bytes (1, 2 or 4) from PCI config space. The access goes through
// at the requested width so that dword-only RMW does not silently clobber
// RW1C bits sharing the same dword (e.g. Status next to Command).
uint32_t pci_read_config_space(struct pci_address *address, uint32_t offset, uint32_t size);

// Write `size` bytes (1, 2 or 4) to PCI config space.
void pci_write_config_space(struct pci_address *address, uint32_t offset, uint32_t size, uint32_t value);

// Discover PCI root buses and devices behind them.
bool pci_early_initialize(void);
bool pci_late_initialize(void);

// Force I/O space, memory space, and bus master enable on a device whose
// option ROM is about to be dispatched. Legacy oproms expect their device
// fully usable; UEFI may have left bus master off (or even MEM/IO off if
// the device was inactive in UEFI).
void pci_enable_for_oprom(uint8_t bus, uint8_t devfn);

// Copy the bus numbers of every discovered root bus other than bus 0 into
// `out` (capacity `cap` bytes). Returns the number of entries written; if
// the buffer is too small, returns 0 without writing anything.
size_t pci_get_extra_root_buses(uint8_t *out, size_t cap);

#endif
