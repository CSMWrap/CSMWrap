#include <efi.h>
#include <csmwrap.h>
#include <oprom.h>
#include <video.h>
#include <config.h>
#include <printf.h>
#include <x86thunk.h>

/* Option ROM alignment - must match SeaBIOS OPTION_ROM_ALIGN (2048) */
#define OPTION_ROM_ALIGN 2048

EFI_STATUS
GetPciLegacyRom(
    IN     UINT16 Csm16Revision,
    IN     UINT16 VendorId,
    IN     UINT16 DeviceId,
    IN OUT VOID   **Rom,
    IN OUT UINTN  *ImageSize,
    OUT    UINTN  *MaxRuntimeImageLength,   OPTIONAL
    OUT    UINT8  *OpRomRevision,           OPTIONAL
    OUT    VOID   **ConfigUtilityCodeHeader OPTIONAL
)
{
    BOOLEAN                 Match;
    UINT16                  *DeviceIdList;
    EFI_PCI_ROM_HEADER      RomHeader;
    PCI_3_0_DATA_STRUCTURE  *Pcir;
    VOID                    *BackupImage;
    VOID                    *BestImage;

    if (*ImageSize < sizeof(EFI_PCI_ROM_HEADER)) {
        return EFI_NOT_FOUND;
    }

    BestImage   = NULL;
    BackupImage = NULL;
    RomHeader.Raw = *Rom;
    while (RomHeader.Generic->Signature == PCI_EXPANSION_ROM_HEADER_SIGNATURE) {
        if (RomHeader.Generic->PcirOffset == 0 ||
            (RomHeader.Generic->PcirOffset & 3) != 0 ||
            *ImageSize < RomHeader.Raw - (UINT8 *)*Rom + RomHeader.Generic->PcirOffset + sizeof(PCI_DATA_STRUCTURE)) {
            break;
        }

        Pcir = (PCI_3_0_DATA_STRUCTURE *)(RomHeader.Raw + RomHeader.Generic->PcirOffset);
        if (Pcir->Signature != PCI_DATA_STRUCTURE_SIGNATURE) {
            break;
        }

        if (((UINTN)RomHeader.Raw - (UINTN)*Rom) + Pcir->ImageLength * 512 > *ImageSize) {
            break;
        }

        if (Pcir->CodeType == PCI_CODE_TYPE_PCAT_IMAGE) {
            Match = FALSE;
            if (Pcir->VendorId == VendorId) {
                if (Pcir->DeviceId == DeviceId) {
                    Match = TRUE;
                } else if ((Pcir->Revision >= 3) && (Pcir->DeviceListOffset != 0)) {
                    DeviceIdList = (UINT16 *)(((UINT8 *)Pcir) + Pcir->DeviceListOffset);
                    while (*DeviceIdList != 0) {
                        if (*DeviceIdList == DeviceId) {
                            Match = TRUE;
                            break;
                        }
                        DeviceIdList++;
                    }
                }
            }

            if (Match) {
                if (Csm16Revision >= 0x0300) {
                    if (Pcir->Revision >= 3) {
                        BestImage = RomHeader.Raw;
                        break;
                    } else {
                        BackupImage = RomHeader.Raw;
                    }
                } else {
                    if (Pcir->Revision >= 3) {
                        BackupImage = RomHeader.Raw;
                    } else {
                        BestImage = RomHeader.Raw;
                        break;
                    }
                }
            }
        }

        if ((Pcir->Indicator & 0x80) == 0x80) {
            break;
        } else {
            RomHeader.Raw += 512 * Pcir->ImageLength;
        }
    }

    if (BestImage == NULL) {
        if (BackupImage == NULL) {
            return EFI_NOT_FOUND;
        }
        BestImage = BackupImage;
    }

    RomHeader.Raw = BestImage;
    Pcir = (PCI_3_0_DATA_STRUCTURE *)(RomHeader.Raw + RomHeader.Generic->PcirOffset);
    *Rom       = BestImage;
    *ImageSize = Pcir->ImageLength * 512;

    if (MaxRuntimeImageLength != NULL) {
        if (Pcir->Revision < 3) {
            *MaxRuntimeImageLength = 0;
        } else {
            *MaxRuntimeImageLength = Pcir->MaxRuntimeImageLength * 512;
        }
    }

    if (OpRomRevision != NULL) {
        if (Pcir->Length >= 0x1C) {
            *OpRomRevision = Pcir->Revision;
        } else {
            *OpRomRevision = 0;
        }
    }

    if (ConfigUtilityCodeHeader != NULL) {
        if ((Pcir->Revision < 3) || (Pcir->ConfigUtilityCodeHeaderOffset == 0)) {
            *ConfigUtilityCodeHeader = NULL;
        } else {
            *ConfigUtilityCodeHeader = RomHeader.Raw + Pcir->ConfigUtilityCodeHeaderOffset;
        }
    }

    return EFI_SUCCESS;
}

void oprom_enumerate(struct csmwrap_priv *priv, struct pci_oprom_list *list)
{
    EFI_STATUS Status;
    EFI_HANDLE *HandleBuffer;
    UINTN HandleCount;
    EFI_GUID PciIoGuid = EFI_PCI_IO_PROTOCOL_GUID;

    list->count = 0;

    Status = gBS->LocateHandleBuffer(ByProtocol, &PciIoGuid, NULL,
                                      &HandleCount, &HandleBuffer);
    if (EFI_ERROR(Status))
        return;

    for (UINTN i = 0; i < HandleCount && list->count < MAX_PCI_OPROMS; i++) {
        EFI_PCI_IO_PROTOCOL *PciIo;
        Status = gBS->HandleProtocol(HandleBuffer[i], &PciIoGuid, (VOID **)&PciIo);
        if (EFI_ERROR(Status))
            continue;

        /* Skip devices without option ROMs */
        if (!PciIo->RomImage || !PciIo->RomSize)
            continue;

        /* Read PCI config header */
        PCI_TYPE00 PciConfig;
        Status = PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0,
                                  sizeof(PciConfig) / sizeof(UINT32), &PciConfig);
        if (EFI_ERROR(Status))
            continue;

        /* Skip VGA devices - handled separately by the video path */
        if (PciConfig.Hdr.ClassCode[2] == PCI_CLASS_DISPLAY)
            continue;

        /* Skip the VGA device explicitly by BDF too, in case class doesn't match */
        UINTN Seg, Bus, Device, Function;
        PciIo->GetLocation(PciIo, &Seg, &Bus, &Device, &Function);
        uint8_t devfn = (uint8_t)((Device << 3) | Function);
        if ((uint8_t)Bus == priv->vga_pci_bus && devfn == priv->vga_pci_devfn)
            continue;

        /* Try to extract an x86 PC-AT legacy ROM image */
        VOID *RomImage = PciIo->RomImage;
        UINTN RomSize = (UINTN)PciIo->RomSize;
        Status = GetPciLegacyRom(0x0300,
                                  PciConfig.Hdr.VendorId,
                                  PciConfig.Hdr.DeviceId,
                                  &RomImage, &RomSize,
                                  NULL, NULL, NULL);
        if (EFI_ERROR(Status))
            continue;

        struct pci_oprom_info *entry = &list->entries[list->count];
        entry->bus        = (uint8_t)Bus;
        entry->devfn      = devfn;
        entry->vendor_id  = PciConfig.Hdr.VendorId;
        entry->device_id  = PciConfig.Hdr.DeviceId;
        entry->class_code = PciConfig.Hdr.ClassCode[2];
        entry->rom_image  = RomImage;
        entry->rom_size   = RomSize;
        list->count++;

        printf("OpROM: %02x:%02x.%x %04x:%04x class %02x (%u bytes)\n",
               (uint8_t)Bus, (uint8_t)Device, (uint8_t)Function,
               PciConfig.Hdr.VendorId, PciConfig.Hdr.DeviceId,
               PciConfig.Hdr.ClassCode[2], (unsigned)RomSize);
    }

    gBS->FreePool(HandleBuffer);

    if (list->count > 0)
        printf("Found %zu non-VGA option ROM(s)\n", list->count);
    else
        printf("No non-VGA option ROMs found\n");
}

void oprom_dispatch_all(struct csmwrap_priv *priv, struct pci_oprom_list *list)
{
    if (list->count == 0)
        return;

    /*
     * ROM placement watermark. VGA ROM (if present) occupies
     * VGABIOS_START..VGABIOS_START+vbios_size. Non-VGA ROMs go after it.
     */
    uintptr_t rom_watermark = VGABIOS_START;
    if (vbios_loc != NULL)
        rom_watermark = ALIGN_UP(VGABIOS_START + vbios_size, OPTION_ROM_ALIGN);

    for (size_t i = 0; i < list->count; i++) {
        struct pci_oprom_info *info = &list->entries[i];
        uintptr_t rom_size_aligned = ALIGN_UP(info->rom_size, OPTION_ROM_ALIGN);

        /* Check available space (must not collide with SeaBIOS CSM binary) */
        if (rom_watermark + rom_size_aligned > priv->csm_bin_base) {
            printf("OpROM: no space for %04x:%04x (need 0x%x, have 0x%x)\n",
                   info->vendor_id, info->device_id,
                   (unsigned)rom_size_aligned,
                   (unsigned)(priv->csm_bin_base - rom_watermark));
            continue;
        }

        /* Copy ROM image to shadow RAM */
        memcpy((void *)rom_watermark, info->rom_image, info->rom_size);
        /* Zero-pad to alignment boundary */
        if (info->rom_size < rom_size_aligned)
            memset((void *)(rom_watermark + info->rom_size), 0,
                   rom_size_aligned - info->rom_size);

        /* Fill the dispatch table */
        EFI_DISPATCH_OPROM_TABLE *table = &priv->low_stub->oprom_table;
        memset(table, 0, sizeof(*table));
        table->OpromSegment       = (UINT16)(rom_watermark >> 4);
        table->PciBus             = info->bus;
        table->PciDeviceFunction  = info->devfn;
        table->NumberBbsEntries   = (UINT8)priv->low_stub->bbs_entry_count;
        table->BbsTablePointer    = (UINT32)(uintptr_t)priv->low_stub->bbs_entries;

        printf("OpROM: dispatching %04x:%04x at seg %04x (%u bytes)\n",
               info->vendor_id, info->device_id,
               table->OpromSegment, (unsigned)info->rom_size);

        EFI_IA32_REGISTER_SET Regs;
        memset(&Regs, 0, sizeof(Regs));
        Regs.X.AX = Legacy16DispatchOprom;
        Regs.X.ES = EFI_SEGMENT(table);
        Regs.X.BX = EFI_OFFSET(table);
        LegacyBiosFarCall86(priv->csm_efi_table->Compatibility16CallSegment,
                            priv->csm_efi_table->Compatibility16CallOffset,
                            &Regs, NULL, 0);

        printf("OpROM: dispatch returned AX=%04x BX=%04x\n",
               Regs.X.AX, Regs.X.BX);

        rom_watermark += rom_size_aligned;
    }
}
