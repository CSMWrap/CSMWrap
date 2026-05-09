#include <efi.h>
#include <printf.h>

#include "csmwrap.h"
#include "io.h"
#include "iommu.h"

#include <uacpi/acpi.h>
#include <uacpi/tables.h>

/*
 * IOMMU Disabling for Legacy OS Handoff
 *
 * Disables Intel VT-d and AMD-Vi IOMMUs before legacy OS boot.
 * This is necessary because UEFI-configured IOMMU translation tables
 * become stale after PCI BAR relocation.
 */

/*
 * Intel VT-d DMAR Table Structures
 * Reference: Intel VT-d Architecture Specification
 */

#define ACPI_DMAR_SIGNATURE "DMAR"

#define DMAR_TYPE_DRHD  0  /* DMA Remapping Hardware Unit Definition */

struct dmar_header {
    uint16_t type;
    uint16_t length;
} __attribute__((packed));

struct dmar_drhd {
    struct dmar_header header;
    uint8_t flags;
    uint8_t reserved;
    uint16_t segment;
    uint64_t register_base;
} __attribute__((packed));

/*
 * Intel VT-d Register Offsets
 */
#define VTD_GCMD_REG    0x18
#define VTD_GSTS_REG    0x1C

#define VTD_GCMD_TE     (1U << 31)
#define VTD_GCMD_IRE    (1U << 25)
#define VTD_GCMD_QIE    (1U << 26)
#define VTD_GSTS_TES    (1U << 31)
#define VTD_GSTS_IRES   (1U << 25)
#define VTD_GSTS_QIES   (1U << 26)

/*
 * Mask to clear one-shot command bits when reading GSTS to write to GCMD.
 * One-shot bits (30, 29, 27, 24) trigger hardware operations when set:
 * - Bit 30: SRTP (Set Root Table Pointer)
 * - Bit 29: SFL (Set Fault Log)
 * - Bit 27: WBF (Write Buffer Flush)
 * - Bit 24: SIRTP (Set Interrupt Remap Table Pointer)
 * Reference: EDK2 IntelSiliconPkg/VTd and Intel VT-d Specification
 */
#define VTD_GSTS_ONESHOT_MASK   0x96FFFFFF

/*
 * AMD IOMMU (AMD-Vi) IVRS Table Structures
 * Reference: AMD I/O Virtualization Technology (IOMMU) Specification
 */

#define ACPI_IVRS_SIGNATURE "IVRS"

#define IVRS_TYPE_IVHD_10   0x10
#define IVRS_TYPE_IVHD_11   0x11
#define IVRS_TYPE_IVHD_40   0x40

struct acpi_ivrs {
    struct acpi_sdt_hdr hdr;
    uint32_t iv_info;
    uint64_t reserved;
} __attribute__((packed));

struct ivrs_header {
    uint8_t type;
    uint8_t flags;
    uint16_t length;
    uint16_t device_id;
    uint16_t capability_offset;
    uint64_t iommu_base;
} __attribute__((packed));

/*
 * AMD IOMMU Register Offsets
 *
 * The control register is 64-bit at offset 0x18, but every bit we touch lives
 * in the low 32 bits, so we read/write the low half only and leave the upper
 * half untouched.
 */
#define AMD_IOMMU_CTRL_REG          0x18
#define AMD_IOMMU_CTRL_EN           (1ULL << 0)
#define AMD_IOMMU_CTRL_EVT_LOG      (1ULL << 2)
#define AMD_IOMMU_CTRL_EVT_INT      (1ULL << 3)
#define AMD_IOMMU_CTRL_CMDBUF       (1ULL << 12)
#define AMD_IOMMU_CTRL_PPR_LOG      (1ULL << 13)
#define AMD_IOMMU_CTRL_PPR_INT      (1ULL << 14)
#define AMD_IOMMU_CTRL_PPR          (1ULL << 15)
#define AMD_IOMMU_CTRL_GA_LOG       (1ULL << 28)
#define AMD_IOMMU_CTRL_GA_INT       (1ULL << 29)

#define AMD_IOMMU_STATUS_REG        0x2020
#define AMD_IOMMU_STATUS_EVTLOG_RUN (1ULL << 3)
#define AMD_IOMMU_STATUS_CMDBUF_RUN (1ULL << 4)
#define AMD_IOMMU_STATUS_PPRLOG_RUN (1ULL << 7)
#define AMD_IOMMU_STATUS_GALOG_RUN  (1ULL << 8)

/*
 * Disable a single Intel VT-d IOMMU unit
 */
static bool vtd_disable_unit(uint64_t reg_base) {
    void *base = (void *)(uintptr_t)reg_base;
    uint32_t gsts, gcmd;

    gsts = readl(base + VTD_GSTS_REG);
    printf("  VT-d unit at 0x%llx: GSTS=0x%08x (TE=%d, IRE=%d, QIE=%d)\n",
           reg_base, gsts,
           !!(gsts & VTD_GSTS_TES),
           !!(gsts & VTD_GSTS_IRES),
           !!(gsts & VTD_GSTS_QIES));

    if (!(gsts & (VTD_GSTS_TES | VTD_GSTS_IRES | VTD_GSTS_QIES))) {
        printf("    Already fully disabled\n");
        return true;
    }

    /* Disable translation (TE) */
    if (gsts & VTD_GSTS_TES) {
        gcmd = gsts & VTD_GSTS_ONESHOT_MASK;
        gcmd &= ~VTD_GCMD_TE;
        writel(base + VTD_GCMD_REG, gcmd);

        while ((gsts = readl(base + VTD_GSTS_REG)) & VTD_GSTS_TES)
            asm volatile("pause");
        printf("    Translation disabled\n");
    }

    /* Disable interrupt remapping (IRE) */
    if (gsts & VTD_GSTS_IRES) {
        gcmd = gsts & VTD_GSTS_ONESHOT_MASK;
        gcmd &= ~(VTD_GCMD_TE | VTD_GCMD_IRE);
        writel(base + VTD_GCMD_REG, gcmd);

        while ((gsts = readl(base + VTD_GSTS_REG)) & VTD_GSTS_IRES)
            asm volatile("pause");
        printf("    Interrupt remapping disabled\n");
    }

    /* Disable queued invalidation (QIE) - must be after TE and IRE */
    if (gsts & VTD_GSTS_QIES) {
        gcmd = gsts & VTD_GSTS_ONESHOT_MASK;
        gcmd &= ~(VTD_GCMD_TE | VTD_GCMD_IRE | VTD_GCMD_QIE);
        writel(base + VTD_GCMD_REG, gcmd);

        while ((gsts = readl(base + VTD_GSTS_REG)) & VTD_GSTS_QIES)
            asm volatile("pause");
        printf("    Queued invalidation disabled\n");
    }

    return true;
}

/*
 * Parse DMAR table and disable all Intel VT-d IOMMUs
 */
static int vtd_disable_all(void) {
    uacpi_table tbl;
    uacpi_status status;
    struct acpi_dmar *dmar;
    uint8_t *ptr, *end;
    int disabled = 0;

    status = uacpi_table_find_by_signature(ACPI_DMAR_SIGNATURE, &tbl);
    if (status != UACPI_STATUS_OK) {
        return 0;  /* No DMAR table, no Intel VT-d */
    }

    dmar = (struct acpi_dmar *)tbl.hdr;
    printf("DMAR: found table, host_address_width=%d, flags=0x%02x\n",
           dmar->haw, dmar->flags);

    ptr = (uint8_t *)dmar + sizeof(struct acpi_dmar);
    end = (uint8_t *)dmar + dmar->hdr.length;

    while (ptr + sizeof(struct dmar_header) <= end) {
        struct dmar_header *hdr = (struct dmar_header *)ptr;

        if (hdr->length < sizeof(struct dmar_header) || ptr + hdr->length > end) {
            printf("DMAR: invalid structure length\n");
            break;
        }

        if (hdr->type == DMAR_TYPE_DRHD) {
            struct dmar_drhd *drhd = (struct dmar_drhd *)ptr;
            printf("DMAR: DRHD segment=%d flags=0x%02x base=0x%llx\n",
                   drhd->segment, drhd->flags, drhd->register_base);

            if (vtd_disable_unit(drhd->register_base)) {
                disabled++;
            }
        }

        ptr += hdr->length;
    }

    uacpi_table_unref(&tbl);
    return disabled;
}

/*
 * Disable a single AMD IOMMU unit
 */
static bool amd_iommu_disable_unit(uint64_t iommu_base) {
    void *base = (void *)(uintptr_t)iommu_base;
    uint32_t ctrl;

    ctrl = readl(base + AMD_IOMMU_CTRL_REG);
    printf("  AMD IOMMU at 0x%llx: CTRL=0x%08x (En=%d)\n",
           iommu_base, ctrl, !!(ctrl & AMD_IOMMU_CTRL_EN));

    if (!(ctrl & AMD_IOMMU_CTRL_EN)) {
        printf("    IOMMU already disabled\n");
        return true;
    }

    /* Disable command buffer, logs and their interrupts before clearing the
     * master enable. Clearing IommuEn while sub-features are still live can
     * leave queued descriptors and in-flight DMA in an undefined state. */
    ctrl &= ~(AMD_IOMMU_CTRL_CMDBUF |
              AMD_IOMMU_CTRL_EVT_LOG | AMD_IOMMU_CTRL_EVT_INT |
              AMD_IOMMU_CTRL_GA_LOG | AMD_IOMMU_CTRL_GA_INT |
              AMD_IOMMU_CTRL_PPR_LOG | AMD_IOMMU_CTRL_PPR_INT |
              AMD_IOMMU_CTRL_PPR);
    writel(base + AMD_IOMMU_CTRL_REG, ctrl);

    /* The *Run bits are level-sensitive and only drop to 0 once the engine
     * has actually drained, so wait for them before continuing. */
    const uint32_t run_mask = AMD_IOMMU_STATUS_CMDBUF_RUN | AMD_IOMMU_STATUS_EVTLOG_RUN |
                              AMD_IOMMU_STATUS_PPRLOG_RUN | AMD_IOMMU_STATUS_GALOG_RUN;
    while (readl(base + AMD_IOMMU_STATUS_REG) & run_mask)
        asm volatile("pause");

    ctrl &= ~AMD_IOMMU_CTRL_EN;
    writel(base + AMD_IOMMU_CTRL_REG, ctrl);

    printf("    IOMMU disabled successfully\n");
    return true;
}

/*
 * Parse IVRS table and disable all AMD IOMMUs
 */
static int amd_iommu_disable_all(void) {
    uacpi_table tbl;
    uacpi_status status;
    struct acpi_ivrs *ivrs;
    uint8_t *ptr, *end;
    int disabled = 0;

    status = uacpi_table_find_by_signature(ACPI_IVRS_SIGNATURE, &tbl);
    if (status != UACPI_STATUS_OK) {
        return 0;  /* No IVRS table, no AMD IOMMU */
    }

    ivrs = (struct acpi_ivrs *)tbl.hdr;
    printf("IVRS: found table, iv_info=0x%08x\n", ivrs->iv_info);

    ptr = (uint8_t *)ivrs + sizeof(struct acpi_ivrs);
    end = (uint8_t *)ivrs + ivrs->hdr.length;

    while (ptr + sizeof(struct ivrs_header) <= end) {
        struct ivrs_header *hdr = (struct ivrs_header *)ptr;

        if (hdr->length < sizeof(struct ivrs_header) || ptr + hdr->length > end) {
            printf("IVRS: invalid block length\n");
            break;
        }

        if (hdr->type == IVRS_TYPE_IVHD_10 ||
            hdr->type == IVRS_TYPE_IVHD_11 ||
            hdr->type == IVRS_TYPE_IVHD_40) {
            printf("IVRS: IVHD type=0x%02x device_id=0x%04x base=0x%llx\n",
                   hdr->type, hdr->device_id, hdr->iommu_base);

            if (amd_iommu_disable_unit(hdr->iommu_base)) {
                disabled++;
            }
        }

        ptr += hdr->length;
    }

    uacpi_table_unref(&tbl);
    return disabled;
}

/*
 * Main entry point: disable all IOMMUs
 */
bool iommu_disable(void) {
    int vtd_count = 0;
    int amd_count = 0;

    printf("Disabling IOMMUs for legacy OS compatibility...\n");

    vtd_count = vtd_disable_all();
    if (vtd_count > 0) {
        printf("Disabled %d Intel VT-d IOMMU unit(s)\n", vtd_count);
    }

    amd_count = amd_iommu_disable_all();
    if (amd_count > 0) {
        printf("Disabled %d AMD IOMMU unit(s)\n", amd_count);
    }

    if (vtd_count == 0 && amd_count == 0) {
        printf("No IOMMUs found or all already disabled\n");
        return false;
    }

    return true;
}
