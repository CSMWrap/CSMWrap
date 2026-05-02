/*
 * MP Table Generation from ACPI
 *
 * Generates Intel MultiProcessor Specification tables from ACPI MADT
 * for legacy BIOS compatibility. The helper core reserved for BIOS proxy
 * is excluded from the generated tables.
 */

#include <efi.h>
#include <printf.h>
#include "csmwrap.h"
#include "mptable.h"
#include "bios_proxy.h"
#include "io.h"

#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include <uacpi/namespace.h>

/* MP Table Signatures */
#define MPTABLE_SIGNATURE   0x5f504d5f  /* "_MP_" */
#define MPCONFIG_SIGNATURE  0x504d4350  /* "PCMP" */

/* MP Table Entry Types */
#define MPT_TYPE_CPU        0
#define MPT_TYPE_BUS        1
#define MPT_TYPE_IOAPIC     2
#define MPT_TYPE_INTSRC     3
#define MPT_TYPE_LOCAL_INT  4

/* Interrupt types for mpt_intsrc.irqtype */
#define MP_INT_TYPE_INT     0   /* Vectored interrupt */
#define MP_INT_TYPE_NMI     1   /* Non-maskable interrupt */
#define MP_INT_TYPE_SMI     2   /* System management interrupt */
#define MP_INT_TYPE_EXTINT  3   /* External interrupt (8259) */

/* Interrupt flags (polarity and trigger mode) */
#define MP_IRQFLAG_CONFORM      0x0000  /* Conforms to bus specification */
#define MP_IRQFLAG_ACTIVE_HIGH  0x0001  /* Active high polarity */
#define MP_IRQFLAG_ACTIVE_LOW   0x0003  /* Active low polarity */
#define MP_IRQFLAG_EDGE         0x0004  /* Edge triggered */
#define MP_IRQFLAG_LEVEL        0x000C  /* Level triggered */

/* Maximum entries */
#define MAX_CPUS        256
#define MAX_IOAPICS     8
#define MAX_OVERRIDES   24
#define MAX_BUS_ENTRIES 256
#define MAX_PCI_BUSES   32

/* Discovered PCI bus information */
struct pci_bus_info {
    uacpi_namespace_node *node;  /* ACPI namespace node for the bus/bridge */
    uint8_t bus_num;             /* PCI bus number (from _BBN or _ADR) */
    uint8_t mp_bus_id;           /* MP table bus ID (assigned sequentially) */
    bool is_secondary;           /* True if this is a secondary bus behind a bridge */
};

static struct {
    struct pci_bus_info buses[MAX_PCI_BUSES];
    size_t count;
} discovered_pci_buses;

/* Forward declaration for recursive walk */
static void discover_secondary_buses(uacpi_namespace_node *parent, uint8_t parent_bus);

/* Check if a PCI device exists by reading vendor ID via legacy PIO */
static bool pci_device_exists(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t addr = 0x80000000 | (bus << 16) | (dev << 11) | (func << 8);
    outl(0xCF8, addr);
    uint16_t vendor = inw(0xCFC);
    return vendor != 0xFFFF;
}

#pragma pack(1)

struct mptable_floating {
    uint32_t signature;     /* "_MP_" */
    uint32_t physaddr;      /* Physical address of MP config table */
    uint8_t length;         /* Length in 16-byte units (1) */
    uint8_t spec_rev;       /* MP spec revision (4 = 1.4) */
    uint8_t checksum;       /* Checksum */
    uint8_t feature1;       /* Feature byte 1 (0 = config table present) */
    uint8_t feature2;       /* Feature byte 2 */
    uint8_t reserved[3];
};

struct mptable_config {
    uint32_t signature;     /* "PCMP" */
    uint16_t length;        /* Base table length */
    uint8_t spec;           /* MP spec revision */
    uint8_t checksum;       /* Checksum */
    char oemid[8];          /* OEM ID */
    char productid[12];     /* Product ID */
    uint32_t oemptr;        /* OEM table pointer */
    uint16_t oemsize;       /* OEM table size */
    uint16_t entrycount;    /* Number of entries */
    uint32_t lapic;         /* Local APIC address */
    uint16_t exttable_length;
    uint8_t exttable_checksum;
    uint8_t reserved;
};

struct mpt_cpu {
    uint8_t type;           /* MPT_TYPE_CPU (0) */
    uint8_t apicid;         /* Local APIC ID */
    uint8_t apicver;        /* Local APIC version */
    uint8_t cpuflag;        /* CPU flags: bit 0=enabled, bit 1=BSP */
    uint32_t cpusignature;  /* CPU signature (from CPUID) */
    uint32_t featureflag;   /* CPU feature flags (from CPUID) */
    uint32_t reserved[2];
};

struct mpt_bus {
    uint8_t type;           /* MPT_TYPE_BUS (1) */
    uint8_t busid;          /* Bus ID */
    char bustype[6];        /* Bus type string: "PCI   " or "ISA   " */
};

struct mpt_ioapic {
    uint8_t type;           /* MPT_TYPE_IOAPIC (2) */
    uint8_t apicid;         /* I/O APIC ID */
    uint8_t apicver;        /* I/O APIC version */
    uint8_t flags;          /* Flags: bit 0=enabled */
    uint32_t apicaddr;      /* I/O APIC base address */
};

struct mpt_intsrc {
    uint8_t type;           /* MPT_TYPE_INTSRC (3) or MPT_TYPE_LOCAL_INT (4) */
    uint8_t irqtype;        /* Interrupt type */
    uint16_t irqflag;       /* Polarity and trigger mode */
    uint8_t srcbus;         /* Source bus ID */
    uint8_t srcbusirq;      /* Source bus IRQ */
    uint8_t dstapic;        /* Destination I/O APIC ID */
    uint8_t dstirq;         /* Destination I/O APIC input */
};

#pragma pack()

/* Forward declaration */
static uint8_t get_ioapic_input_count(uint32_t ioapic_addr);

/* Parsed MADT data */
struct madt_data {
    struct {
        uint32_t apic_id;
        uint8_t enabled;
        uint8_t is_bsp;
    } cpus[MAX_CPUS];
    size_t cpu_count;

    struct {
        uint8_t id;
        uint32_t address;
        uint32_t gsi_base;
        uint8_t input_count;  /* Number of interrupt inputs (from hardware) */
    } ioapics[MAX_IOAPICS];
    size_t ioapic_count;

    struct {
        uint8_t source_irq;
        uint32_t gsi;
        uint16_t flags;
    } overrides[MAX_OVERRIDES];
    size_t override_count;

    /* Local APIC NMI - we assume all CPUs use the same LINT for NMI */
    uint8_t nmi_lint;       /* Which LINT pin receives NMI (0 or 1) */
    uint16_t nmi_flags;
    uint8_t has_nmi_info;
};

/* Compute checksum (sum of all bytes must be 0) */
static uint8_t compute_checksum(void *data, size_t len)
{
    uint8_t sum = 0;
    uint8_t *p = (uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        sum += p[i];
    }
    return sum;
}

/* Get BSP APIC ID */
static uint32_t get_bsp_apic_id(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (ebx >> 24) & 0xFF;
}

/* Get CPU signature and features from CPUID */
static void get_cpu_info(uint32_t *signature, uint32_t *features)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    *signature = eax;
    *features = edx;
}

/* Get Local APIC version */
static uint8_t get_lapic_version(void)
{
    uint64_t apic_base = rdmsr(0x1b);
    if (apic_base & (1 << 10))  /* x2APIC mode: use MSR 0x803 */
        return rdmsr(0x803) & 0xFF;
    uint32_t lapic_addr = apic_base & 0xFFFFF000;
    volatile uint32_t *ver_reg = (volatile uint32_t *)(uintptr_t)(lapic_addr + 0x30);
    return (*ver_reg) & 0xFF;
}

/* Convert ACPI MADT flags to MP table flags */
static uint16_t madt_flags_to_mp(uint16_t madt_flags)
{
    uint16_t mp_flags = 0;

    /* Polarity */
    switch (madt_flags & ACPI_MADT_POLARITY_MASK) {
        case ACPI_MADT_POLARITY_ACTIVE_HIGH:
            mp_flags |= MP_IRQFLAG_ACTIVE_HIGH;
            break;
        case ACPI_MADT_POLARITY_ACTIVE_LOW:
            mp_flags |= MP_IRQFLAG_ACTIVE_LOW;
            break;
        default:
            /* Conforming - use bus default */
            mp_flags |= MP_IRQFLAG_CONFORM;
            break;
    }

    /* Trigger mode */
    switch (madt_flags & ACPI_MADT_TRIGGERING_MASK) {
        case ACPI_MADT_TRIGGERING_EDGE:
            mp_flags |= MP_IRQFLAG_EDGE;
            break;
        case ACPI_MADT_TRIGGERING_LEVEL:
            mp_flags |= MP_IRQFLAG_LEVEL;
            break;
        default:
            /* Conforming - use bus default */
            break;
    }

    return mp_flags;
}

/* Parse MADT and extract relevant information */
static bool parse_madt(struct madt_data *data, int helper_apic_id)
{
    struct uacpi_table madt_table;

    memset(data, 0, sizeof(*data));
    data->nmi_lint = 1;  /* Default: LINT1 for NMI */

    if (uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &madt_table) != UACPI_STATUS_OK) {
        printf("mptable: MADT not found\n");
        return false;
    }

    struct acpi_madt *madt = (struct acpi_madt *)madt_table.virt_addr;
    uint8_t *entry = (uint8_t *)(madt + 1);
    uint8_t *end = (uint8_t *)madt + madt->hdr.length;
    uint32_t bsp_id = get_bsp_apic_id();

    while (entry < end) {
        struct acpi_entry_hdr *hdr = (struct acpi_entry_hdr *)entry;
        if (hdr->length < 2) break;

        switch (hdr->type) {
            case ACPI_MADT_ENTRY_TYPE_LAPIC: {
                struct acpi_madt_lapic *lapic = (struct acpi_madt_lapic *)entry;

                /* Skip helper core */
                if ((int)lapic->id == helper_apic_id) {
                    entry += hdr->length;
                    continue;
                }

                if (data->cpu_count < MAX_CPUS && (lapic->flags & ACPI_PIC_ENABLED)) {
                    data->cpus[data->cpu_count].apic_id = lapic->id;
                    data->cpus[data->cpu_count].enabled = 1;
                    data->cpus[data->cpu_count].is_bsp = (lapic->id == bsp_id) ? 1 : 0;
                    data->cpu_count++;
                }
                break;
            }

            case ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC: {
                struct acpi_madt_x2apic *x2apic = (struct acpi_madt_x2apic *)entry;

                /* Skip helper core */
                if ((int)x2apic->id == helper_apic_id) {
                    entry += hdr->length;
                    continue;
                }

                /* MP table only supports 8-bit APIC IDs */
                if (x2apic->id > 255) {
                    entry += hdr->length;
                    continue;
                }

                if (data->cpu_count < MAX_CPUS && (x2apic->flags & ACPI_PIC_ENABLED)) {
                    data->cpus[data->cpu_count].apic_id = x2apic->id;
                    data->cpus[data->cpu_count].enabled = 1;
                    data->cpus[data->cpu_count].is_bsp = (x2apic->id == bsp_id) ? 1 : 0;
                    data->cpu_count++;
                }
                break;
            }

            case ACPI_MADT_ENTRY_TYPE_IOAPIC: {
                struct acpi_madt_ioapic *ioapic = (struct acpi_madt_ioapic *)entry;

                if (data->ioapic_count < MAX_IOAPICS) {
                    data->ioapics[data->ioapic_count].id = ioapic->id;
                    data->ioapics[data->ioapic_count].address = ioapic->address;
                    data->ioapics[data->ioapic_count].gsi_base = ioapic->gsi_base;
                    /* Read actual input count from hardware */
                    data->ioapics[data->ioapic_count].input_count =
                        get_ioapic_input_count(ioapic->address);
                    data->ioapic_count++;
                }
                break;
            }

            case ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
                struct acpi_madt_interrupt_source_override *iso =
                    (struct acpi_madt_interrupt_source_override *)entry;

                if (data->override_count < MAX_OVERRIDES) {
                    data->overrides[data->override_count].source_irq = iso->source;
                    data->overrides[data->override_count].gsi = iso->gsi;
                    data->overrides[data->override_count].flags = iso->flags;
                    data->override_count++;
                }
                break;
            }

            case ACPI_MADT_ENTRY_TYPE_LAPIC_NMI: {
                struct acpi_madt_lapic_nmi *nmi = (struct acpi_madt_lapic_nmi *)entry;

                /* Use the first NMI entry we find (usually applies to all CPUs) */
                if (!data->has_nmi_info) {
                    data->nmi_lint = nmi->lint & 1;
                    data->nmi_flags = nmi->flags;
                    data->has_nmi_info = 1;
                }
                break;
            }

            default:
                break;
        }

        entry += hdr->length;
    }

    uacpi_table_unref(&madt_table);

    /* Compute total GSI count */
    uint32_t total_gsis = 0;
    for (size_t i = 0; i < data->ioapic_count; i++) {
        total_gsis += data->ioapics[i].input_count;
    }

    printf("mptable: %zu CPUs, %zu I/O APIC(s) (%u GSIs), %zu overrides\n",
           data->cpu_count, data->ioapic_count, total_gsis, data->override_count);

    return data->cpu_count > 0 && data->ioapic_count > 0;
}

/* Find which I/O APIC handles a given GSI */
static int find_ioapic_for_gsi(struct madt_data *data, uint32_t gsi, uint8_t *dstirq)
{
    for (size_t i = 0; i < data->ioapic_count; i++) {
        /* Use actual input count read from hardware */
        if (gsi >= data->ioapics[i].gsi_base &&
            gsi < data->ioapics[i].gsi_base + data->ioapics[i].input_count) {
            *dstirq = gsi - data->ioapics[i].gsi_base;
            return data->ioapics[i].id;
        }
    }
    return -1;
}

/* Read I/O APIC register */
static uint32_t ioapic_read(uint32_t ioapic_addr, uint8_t reg)
{
    volatile uint32_t *ioregsel = (volatile uint32_t *)(uintptr_t)ioapic_addr;
    volatile uint32_t *iowin = (volatile uint32_t *)(uintptr_t)(ioapic_addr + 0x10);

    *ioregsel = reg;
    return *iowin;
}

/* Get I/O APIC version from its registers */
static uint8_t get_ioapic_version(uint32_t ioapic_addr)
{
    return ioapic_read(ioapic_addr, 0x01) & 0xFF;
}

/* Get I/O APIC input count from its registers */
static uint8_t get_ioapic_input_count(uint32_t ioapic_addr)
{
    /* IOAPICVER register (0x01): bits [23:16] = max redirection entry */
    uint32_t ver = ioapic_read(ioapic_addr, 0x01);
    uint8_t max_redir = (ver >> 16) & 0xFF;
    return max_redir + 1;  /* Input count = max redirection entry + 1 */
}

/* Context for IRQ resource iteration */
struct irq_find_ctx {
    int32_t gsi;        /* Output: found GSI, or -1 if not found */
    uint16_t flags;     /* Output: ACPI interrupt flags */
};

/* Convert uACPI resource flags to ACPI MADT interrupt flags format */
static uint16_t uacpi_flags_to_madt(uint8_t triggering, uint8_t polarity)
{
    uint16_t flags = 0;
    flags |= polarity ? ACPI_MADT_POLARITY_ACTIVE_LOW : ACPI_MADT_POLARITY_ACTIVE_HIGH;
    flags |= triggering ? ACPI_MADT_TRIGGERING_EDGE : ACPI_MADT_TRIGGERING_LEVEL;
    return flags;
}

/* Callback to find IRQ in resource list */
static uacpi_iteration_decision find_irq_callback(void *user, uacpi_resource *resource)
{
    struct irq_find_ctx *ctx = (struct irq_find_ctx *)user;

    if (resource->type == UACPI_RESOURCE_TYPE_IRQ) {
        if (resource->irq.num_irqs > 0) {
            ctx->gsi = resource->irq.irqs[0];
            ctx->flags = uacpi_flags_to_madt(resource->irq.triggering, resource->irq.polarity);
            return UACPI_ITERATION_DECISION_BREAK;
        }
    } else if (resource->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
        if (resource->extended_irq.num_irqs > 0) {
            ctx->gsi = resource->extended_irq.irqs[0];
            ctx->flags = uacpi_flags_to_madt(resource->extended_irq.triggering, resource->extended_irq.polarity);
            return UACPI_ITERATION_DECISION_BREAK;
        }
    }

    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* Get GSI from a PCI interrupt link device by evaluating _CRS */
static int32_t get_link_device_gsi(uacpi_namespace_node *link, uint16_t *out_flags)
{
    struct irq_find_ctx ctx = { .gsi = -1, .flags = 0 };

    uacpi_status status = uacpi_for_each_device_resource(
        link, "_CRS", find_irq_callback, &ctx);

    if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND) {
        return -1;
    }

    if (out_flags && ctx.gsi >= 0) {
        *out_flags = ctx.flags;
    }

    return ctx.gsi;
}

/* Callback for discovering PCI root bridges */
static uacpi_iteration_decision discover_pci_bus_callback(
    void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user;
    (void)depth;

    if (discovered_pci_buses.count >= MAX_PCI_BUSES) {
        printf("mptable: too many PCI buses, skipping additional bridges\n");
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    /* Get bus number from _BBN (Base Bus Number), defaults to 0 if not present */
    uacpi_u64 bus_num = 0;
    uacpi_eval_integer(node, "_BBN", UACPI_NULL, &bus_num);

    /* Check if we already have this bus number (some systems have duplicate entries) */
    for (size_t i = 0; i < discovered_pci_buses.count; i++) {
        if (discovered_pci_buses.buses[i].bus_num == (uint8_t)bus_num) {
            return UACPI_ITERATION_DECISION_CONTINUE;
        }
    }

    discovered_pci_buses.buses[discovered_pci_buses.count].node = node;
    discovered_pci_buses.buses[discovered_pci_buses.count].bus_num = (uint8_t)bus_num;
    /* mp_bus_id will be assigned later when creating bus entries */
    discovered_pci_buses.count++;

    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* Callback for finding secondary buses with _PRT under a bridge */
static uacpi_iteration_decision discover_secondary_callback(
    void *user, uacpi_namespace_node *node, uacpi_u32 node_depth)
{
    (void)node_depth;
    uint8_t parent_bus = *(uint8_t *)user;

    if (discovered_pci_buses.count >= MAX_PCI_BUSES) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    /* Check if this node has a _PRT method (indicates it routes interrupts for a bus) */
    uacpi_namespace_node *prt_node = UACPI_NULL;
    uacpi_status st = uacpi_namespace_node_find(node, "_PRT", &prt_node);
    if (st != UACPI_STATUS_OK || !prt_node) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    /* Get _ADR to check if this is a PCI device */
    uacpi_u64 adr = 0;
    if (uacpi_eval_integer(node, "_ADR", UACPI_NULL, &adr) != UACPI_STATUS_OK) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    /* _ADR format: high word = device, low word = function */
    uint8_t dev = (adr >> 16) & 0x1F;
    uint8_t func = adr & 0x7;

    /* Verify device actually exists in PCI config space */
    if (!pci_device_exists(parent_bus, dev, func)) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    /* This is a PCI bridge with its own _PRT - add as secondary bus */
    discovered_pci_buses.buses[discovered_pci_buses.count].node = node;
    /* Secondary bus number isn't directly available, but we can use a placeholder
     * since what matters for interrupt routing is the MP bus ID mapping */
    discovered_pci_buses.buses[discovered_pci_buses.count].bus_num = 0xFF;  /* Unknown */
    discovered_pci_buses.buses[discovered_pci_buses.count].is_secondary = true;
    discovered_pci_buses.count++;

    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* Walk namespace under a root bridge to find secondary buses */
static void discover_secondary_buses(uacpi_namespace_node *root_bridge, uint8_t parent_bus)
{
    /* Walk children looking for bridges with _PRT */
    uacpi_namespace_for_each_child(
        root_bridge,
        discover_secondary_callback,   /* descending callback */
        UACPI_NULL,                     /* ascending callback (not needed) */
        UACPI_OBJECT_DEVICE_BIT,        /* type mask - only devices */
        3,                              /* Max depth - look 3 levels deep for nested bridges */
        &parent_bus                     /* user data */
    );
}

/* Discover all PCI root bridges via ACPI */
static void discover_pci_buses(void)
{
    discovered_pci_buses.count = 0;

    uacpi_namespace_node *sb_node = uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB);
    if (!sb_node) {
        printf("mptable: _SB not found, cannot discover PCI buses\n");
        return;
    }

    /* Search for PCI and PCIe root bridges */
    const uacpi_char *hids[] = { "PNP0A03", "PNP0A08", UACPI_NULL };
    uacpi_find_devices_at(sb_node, hids, discover_pci_bus_callback, UACPI_NULL);

    size_t root_count = discovered_pci_buses.count;

    /* Walk each root bridge to find secondary buses with their own _PRT */
    for (size_t i = 0; i < root_count; i++) {
        discover_secondary_buses(
            discovered_pci_buses.buses[i].node,
            discovered_pci_buses.buses[i].bus_num
        );
    }

    size_t secondary_count = discovered_pci_buses.count - root_count;
    if (secondary_count > 0) {
        printf("mptable: discovered %zu root + %zu secondary PCI buses\n",
               root_count, secondary_count);
    } else {
        printf("mptable: discovered %zu PCI root bridge(s)\n", root_count);
    }
}

/* Build the MP table */
bool mptable_init(struct csmwrap_priv *priv)
{
    struct madt_data madt;
    int helper_apic_id = bios_proxy_get_helper_apic_id();

    printf("mptable: building MP table (excluding helper core APIC ID %d)\n", helper_apic_id);

    if (!parse_madt(&madt, helper_apic_id)) {
        printf("mptable: failed to parse MADT, skipping MP table\n");
        return false;
    }

    /* Discover PCI root bridges via ACPI */
    discover_pci_buses();

    /* Get CPU info from CPUID */
    uint32_t cpu_signature, cpu_features;
    get_cpu_info(&cpu_signature, &cpu_features);
    uint8_t lapic_version = get_lapic_version();

    /* Ensure at least one PCI bus for backwards compatibility */
    size_t pci_bus_count = discovered_pci_buses.count > 0 ? discovered_pci_buses.count : 1;

    /*
     * Calculate table size:
     * - Config header
     * - CPU entries
     * - Bus entries (N PCI buses + 1 ISA)
     * - I/O APIC entries
     * - ISA interrupt entries (16)
     * - PCI interrupt entries (up to 128 per bus * N buses)
     * - Local interrupt entries (2: ExtINT + NMI)
     */
    size_t table_size = sizeof(struct mptable_config) +
                        madt.cpu_count * sizeof(struct mpt_cpu) +
                        (pci_bus_count + 1) * sizeof(struct mpt_bus) +
                        madt.ioapic_count * sizeof(struct mpt_ioapic) +
                        (16 + 128 * pci_bus_count + 2) * sizeof(struct mpt_intsrc);

    /* Allocate table anywhere below 4GB - SeaBIOS will relocate it to F-segment */
    size_t total_size = sizeof(struct mptable_floating) + table_size;
    EFI_PHYSICAL_ADDRESS table_addr = 0xFFFFFFFF;

    EFI_STATUS status = gBS->AllocatePages(
        AllocateMaxAddress,
        EfiLoaderCode,
        (total_size + 4095) / 4096,
        &table_addr
    );

    if (EFI_ERROR(status)) {
        printf("mptable: failed to allocate memory\n");
        return false;
    }

    /* Pass address to SeaBIOS via boot table */
    priv->low_stub->boot_table.MpTable = (uint32_t)table_addr;

    memset((void *)(uintptr_t)table_addr, 0, total_size);

    /* Build config table first, then floating pointer */
    struct mptable_config *config = (struct mptable_config *)(uintptr_t)(table_addr + sizeof(struct mptable_floating));
    uint8_t *entry_ptr = (uint8_t *)(config + 1);
    uint8_t *entry_end = (uint8_t *)(uintptr_t)(table_addr + total_size);
    uint16_t entry_count = 0;

    /* Config header */
    config->signature = MPCONFIG_SIGNATURE;
    config->spec = 4;  /* MP spec 1.4 */
    memcpy(config->oemid, "CSMWRAP ", 8);
    memcpy(config->productid, "MP TABLE    ", 12);
    config->lapic = rdmsr(0x1b) & 0xFFFFF000;

    /* CPU entries */
    for (size_t i = 0; i < madt.cpu_count; i++) {
        struct mpt_cpu *cpu = (struct mpt_cpu *)entry_ptr;
        cpu->type = MPT_TYPE_CPU;
        cpu->apicid = madt.cpus[i].apic_id;
        cpu->apicver = lapic_version;
        cpu->cpuflag = madt.cpus[i].enabled ? 0x01 : 0x00;
        if (madt.cpus[i].is_bsp) cpu->cpuflag |= 0x02;
        cpu->cpusignature = cpu_signature;
        cpu->featureflag = cpu_features;
        entry_ptr += sizeof(struct mpt_cpu);
        entry_count++;
    }

    /* Bus entries: PCI buses */
    uint8_t next_bus_id = 0;

    if (discovered_pci_buses.count > 0) {
        /* Create bus entries for all discovered PCI buses */
        for (size_t i = 0; i < discovered_pci_buses.count; i++) {
            struct mpt_bus *pci_bus = (struct mpt_bus *)entry_ptr;
            pci_bus->type = MPT_TYPE_BUS;
            pci_bus->busid = next_bus_id;
            memcpy(pci_bus->bustype, "PCI   ", 6);
            entry_ptr += sizeof(struct mpt_bus);
            entry_count++;

            /* Store the MP bus ID for _PRT processing later */
            discovered_pci_buses.buses[i].mp_bus_id = next_bus_id;
            next_bus_id++;
        }
    } else {
        /* Fallback: create at least one PCI bus entry */
        struct mpt_bus *pci_bus = (struct mpt_bus *)entry_ptr;
        pci_bus->type = MPT_TYPE_BUS;
        pci_bus->busid = next_bus_id;
        memcpy(pci_bus->bustype, "PCI   ", 6);
        entry_ptr += sizeof(struct mpt_bus);
        entry_count++;
        next_bus_id++;
    }

    /* Bus entries: ISA bus (always last) */
    uint8_t isa_bus_id = next_bus_id;
    struct mpt_bus *isa_bus = (struct mpt_bus *)entry_ptr;
    isa_bus->type = MPT_TYPE_BUS;
    isa_bus->busid = isa_bus_id;
    memcpy(isa_bus->bustype, "ISA   ", 6);
    entry_ptr += sizeof(struct mpt_bus);
    entry_count++;

    /* I/O APIC entries */
    for (size_t i = 0; i < madt.ioapic_count; i++) {
        struct mpt_ioapic *ioapic = (struct mpt_ioapic *)entry_ptr;
        ioapic->type = MPT_TYPE_IOAPIC;
        ioapic->apicid = madt.ioapics[i].id;
        ioapic->apicver = get_ioapic_version(madt.ioapics[i].address);
        ioapic->flags = 0x01;  /* Enabled */
        ioapic->apicaddr = madt.ioapics[i].address;
        entry_ptr += sizeof(struct mpt_ioapic);
        entry_count++;
    }

    /* ISA interrupt entries (IRQ 0-15) */
    for (int irq = 0; irq < 16; irq++) {
        uint32_t gsi = irq;  /* Default: identity mapping */
        uint16_t flags = MP_IRQFLAG_CONFORM;  /* Default: conforms to bus spec */

        /* Check for override */
        for (size_t j = 0; j < madt.override_count; j++) {
            if (madt.overrides[j].source_irq == irq) {
                gsi = madt.overrides[j].gsi;
                flags = madt_flags_to_mp(madt.overrides[j].flags);
                break;
            }
        }

        uint8_t dstirq;
        int ioapic_id = find_ioapic_for_gsi(&madt, gsi, &dstirq);
        if (ioapic_id < 0) continue;

        struct mpt_intsrc *intsrc = (struct mpt_intsrc *)entry_ptr;
        intsrc->type = MPT_TYPE_INTSRC;
        intsrc->irqtype = MP_INT_TYPE_INT;
        intsrc->irqflag = flags;
        intsrc->srcbus = isa_bus_id;
        intsrc->srcbusirq = irq;
        intsrc->dstapic = ioapic_id;
        intsrc->dstirq = dstirq;
        entry_ptr += sizeof(struct mpt_intsrc);
        entry_count++;
    }

    /* PCI interrupt entries - from ACPI _PRT (PCI Routing Table) */
    /* Process _PRT for each discovered PCI root bridge */
    for (size_t bus_idx = 0; bus_idx < discovered_pci_buses.count; bus_idx++) {
        struct pci_bus_info *bus_info = &discovered_pci_buses.buses[bus_idx];

        uacpi_pci_routing_table *prt = NULL;
        uacpi_status prt_status = uacpi_get_pci_routing_table(bus_info->node, &prt);
        if (prt_status != UACPI_STATUS_OK || !prt) continue;

        for (size_t i = 0; i < prt->num_entries; i++) {
            if (entry_ptr + sizeof(struct mpt_intsrc) > entry_end) {
                printf("mptable: table full, truncating PCI interrupt entries\n");
                goto prt_done;
            }
            uacpi_pci_routing_table_entry *e = &prt->entries[i];
            int32_t gsi;
            uint16_t acpi_flags = 0;

            if (e->source == NULL) {
                /* Static routing: index is the GSI directly */
                gsi = e->index;
            } else {
                /* Dynamic routing: evaluate link device's _CRS */
                gsi = get_link_device_gsi(e->source, &acpi_flags);
                if (gsi < 0) continue;
            }

            /* Find which I/O APIC handles this GSI */
            uint8_t dstirq;
            int ioapic_id = find_ioapic_for_gsi(&madt, gsi, &dstirq);
            if (ioapic_id < 0) continue;

            /* _PRT address format: high word = device, low word = function (0xFFFF = any) */
            uint8_t dev = (e->address >> 16) & 0x1F;

            /* Convert ACPI flags to MP flags, default to PCI standard if not specified */
            uint16_t mp_flags;
            if (acpi_flags) {
                mp_flags = madt_flags_to_mp(acpi_flags);
            } else {
                mp_flags = MP_IRQFLAG_ACTIVE_LOW | MP_IRQFLAG_LEVEL;  /* PCI default */
            }

            struct mpt_intsrc *intsrc = (struct mpt_intsrc *)entry_ptr;
            intsrc->type = MPT_TYPE_INTSRC;
            intsrc->irqtype = MP_INT_TYPE_INT;
            intsrc->irqflag = mp_flags;
            intsrc->srcbus = bus_info->mp_bus_id;  /* Use the correct MP bus ID */
            intsrc->srcbusirq = (dev << 2) | e->pin;  /* MP spec: (dev << 2) | pin */
            intsrc->dstapic = ioapic_id;
            intsrc->dstirq = dstirq;
            entry_ptr += sizeof(struct mpt_intsrc);
            entry_count++;
        }

        uacpi_free_pci_routing_table(prt);
    }
prt_done:

    /* Local interrupt entries */
    /* ExtINT on the LINT pin opposite NMI, BSP only (8259 wired to BSP). */
    uint8_t bsp_apic_id = (uint8_t)get_bsp_apic_id();
    uint8_t extint_lint = madt.nmi_lint ^ 1;
    struct mpt_intsrc *extint = (struct mpt_intsrc *)entry_ptr;
    extint->type = MPT_TYPE_LOCAL_INT;
    extint->irqtype = MP_INT_TYPE_EXTINT;
    extint->irqflag = MP_IRQFLAG_CONFORM;
    extint->srcbus = isa_bus_id;
    extint->srcbusirq = 0;
    extint->dstapic = bsp_apic_id;
    extint->dstirq = extint_lint;
    entry_ptr += sizeof(struct mpt_intsrc);
    entry_count++;

    /* NMI on LINT1 (or whatever MADT specified) */
    struct mpt_intsrc *nmi = (struct mpt_intsrc *)entry_ptr;
    nmi->type = MPT_TYPE_LOCAL_INT;
    nmi->irqtype = MP_INT_TYPE_NMI;
    nmi->irqflag = madt.has_nmi_info ? madt_flags_to_mp(madt.nmi_flags) : MP_IRQFLAG_CONFORM;
    nmi->srcbus = isa_bus_id;
    nmi->srcbusirq = 0;
    nmi->dstapic = 0xFF;  /* All local APICs */
    nmi->dstirq = madt.nmi_lint;
    entry_ptr += sizeof(struct mpt_intsrc);
    entry_count++;

    /* Finalize config table */
    config->entrycount = entry_count;
    config->length = (uint16_t)((uintptr_t)entry_ptr - (uintptr_t)config);
    config->checksum = 0;
    config->checksum = -compute_checksum(config, config->length);

    /* Build floating pointer structure */
    struct mptable_floating *floating = (struct mptable_floating *)(uintptr_t)table_addr;
    floating->signature = MPTABLE_SIGNATURE;
    floating->physaddr = (uint32_t)(uintptr_t)config;
    floating->length = 1;  /* 16 bytes */
    floating->spec_rev = 4;  /* MP spec 1.4 */
    floating->feature1 = 0;  /* Config table present */
    floating->feature2 = 0;
    floating->checksum = 0;
    floating->checksum = -compute_checksum(floating, sizeof(*floating));

    printf("mptable: built at 0x%lx, %u entries, %u bytes\n",
           (unsigned long)table_addr, entry_count, config->length);

    return true;
}
