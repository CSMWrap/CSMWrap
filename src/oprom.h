#ifndef OPROM_H
#define OPROM_H

#include <stdint.h>
#include <stddef.h>
#include <efi.h>
#include <csmwrap.h>

#define MAX_PCI_OPROMS 16

struct pci_oprom_info {
    uint8_t   bus;
    uint8_t   devfn;       /* (device << 3) | function */
    uint16_t  vendor_id;
    uint16_t  device_id;
    uint8_t   class_code;  /* PCI base class */
    void     *rom_image;   /* Pointer to x86 PC-AT ROM image */
    size_t    rom_size;    /* Size of the x86 image in bytes */
};

struct pci_oprom_list {
    size_t count;
    struct pci_oprom_info entries[MAX_PCI_OPROMS];
};

/*
 * Enumerate all PCI devices with legacy x86 option ROMs (excluding VGA).
 * Must be called before ExitBootServices (requires EFI_PCI_IO_PROTOCOL).
 */
void oprom_enumerate(struct csmwrap_priv *priv, struct pci_oprom_list *list);

/*
 * Dispatch all enumerated option ROMs via Legacy16DispatchOprom.
 * Must be called after Legacy16InitializeYourself and after the VGA oprom
 * dispatch, but before Legacy16UpdateBbs.
 */
void oprom_dispatch_all(struct csmwrap_priv *priv, struct pci_oprom_list *list);

/*
 * Extract the x86 PC-AT legacy ROM image from a PCI ROM bundle.
 * The ROM bundle may contain multiple images (x86, EFI, OpenFirmware).
 * On success, *Rom and *ImageSize are updated to point to the x86 image.
 */
EFI_STATUS GetPciLegacyRom(
    UINT16 Csm16Revision,
    UINT16 VendorId,
    UINT16 DeviceId,
    VOID   **Rom,
    UINTN  *ImageSize,
    UINTN  *MaxRuntimeImageLength,
    UINT8  *OpRomRevision,
    VOID   **ConfigUtilityCodeHeader
);

#endif
