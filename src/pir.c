/*
 * $PIR (PCI IRQ Routing) Table Generation
 *
 * Synthesizes a PCI BIOS Specification 2.1 $PIR table from ACPI _PRT
 * (PCI Routing Table) and _PRS (Possible Resource Settings) evaluations,
 * allocates it below 4 GiB, and points the CSM's IrqRoutingTablePointer at
 * it. SeaBIOS picks up the pointer in handle_csm_0002 and serves it back to
 * legacy callers via INT 1Ah AX=B406h ("Get PCI IRQ Routing Options").
 */

#include <efi.h>
#include <printf.h>
#include "csmwrap.h"
#include "pir.h"
#include "io.h"

#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#define PIR_SIGNATURE 0x52495024  /* "$PIR" */
#define PIR_VERSION   0x0100

/*
 * Per-pin "any free legacy IRQ" bitmap used when the actual routing isn't
 * representable in 8259 IRQ space (e.g. a static GSI >= 16, or a link
 * device with no _PRS). Bits 3,4,5,7,9,10,11,12,14,15 - the IRQs
 * traditionally available for PCI assignment on a PC/AT.
 */
#define LEGACY_PCI_IRQ_BITMAP 0xDEB8

#define MAX_PIR_SLOTS    256
#define MAX_PCI_BUSES    32
#define MAX_LINK_DEVICES 32

#pragma pack(1)
struct pir_link_info {
    uint8_t  link;
    uint16_t bitmap;
};

struct pir_slot {
    uint8_t  bus;
    uint8_t  dev;          /* PCI device number << 3 */
    struct pir_link_info pins[4];   /* INTA, INTB, INTC, INTD */
    uint8_t  slot_nr;
    uint8_t  reserved;
};

struct pir_header {
    uint32_t signature;
    uint16_t version;
    uint16_t size;
    uint8_t  router_bus;
    uint8_t  router_devfunc;
    uint16_t exclusive_irqs;
    uint32_t compatible_devid;
    uint32_t miniport_data;
    uint8_t  reserved[11];
    uint8_t  checksum;
};
#pragma pack()

struct pci_bus_info {
    uacpi_namespace_node *node;
    uint8_t bus_num;
};

static struct {
    struct pci_bus_info buses[MAX_PCI_BUSES];
    size_t count;
} pir_buses;

static struct pir_slot slots[MAX_PIR_SLOTS];
static size_t slot_count;

struct link_device_entry {
    uacpi_namespace_node *node;
    uint8_t  link_id;
    uint16_t bitmap;
};

static struct {
    struct link_device_entry entries[MAX_LINK_DEVICES];
    size_t count;
} pir_link_devices;

/* Read PCI vendor ID via legacy PIO; matches the helper in mptable.c. */
static bool pci_device_exists(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t addr = 0x80000000 | (bus << 16) | (dev << 11) | (func << 8);
    outl(0xCF8, addr);
    uint16_t vendor = inw(0xCFC);
    return vendor != 0xFFFF;
}

/* Callback for root bridge discovery (PNP0A03 / PNP0A08). */
static uacpi_iteration_decision discover_root_bus_callback(
    void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user;
    (void)depth;

    if (pir_buses.count >= MAX_PCI_BUSES)
        return UACPI_ITERATION_DECISION_CONTINUE;

    uacpi_u64 bus_num = 0;
    uacpi_eval_integer(node, "_BBN", UACPI_NULL, &bus_num);

    /* Skip duplicates (some firmware lists the same bus more than once). */
    for (size_t i = 0; i < pir_buses.count; i++) {
        if (pir_buses.buses[i].bus_num == (uint8_t)bus_num)
            return UACPI_ITERATION_DECISION_CONTINUE;
    }

    pir_buses.buses[pir_buses.count].node = node;
    pir_buses.buses[pir_buses.count].bus_num = (uint8_t)bus_num;
    pir_buses.count++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* Callback for secondary buses with their own _PRT under a root bridge. */
static uacpi_iteration_decision discover_secondary_callback(
    void *user, uacpi_namespace_node *node, uacpi_u32 node_depth)
{
    (void)node_depth;
    uint8_t parent_bus = *(uint8_t *)user;

    if (pir_buses.count >= MAX_PCI_BUSES)
        return UACPI_ITERATION_DECISION_CONTINUE;

    /* Only interested in nodes with their own _PRT (i.e. PCI-PCI bridges). */
    uacpi_namespace_node *prt_node = UACPI_NULL;
    uacpi_status st = uacpi_namespace_node_find(node, "_PRT", &prt_node);
    if (st != UACPI_STATUS_OK || !prt_node)
        return UACPI_ITERATION_DECISION_CONTINUE;

    uacpi_u64 adr = 0;
    if (uacpi_eval_integer(node, "_ADR", UACPI_NULL, &adr) != UACPI_STATUS_OK)
        return UACPI_ITERATION_DECISION_CONTINUE;

    uint8_t dev = (adr >> 16) & 0x1F;
    uint8_t func = adr & 0x7;
    if (!pci_device_exists(parent_bus, dev, func))
        return UACPI_ITERATION_DECISION_CONTINUE;

    /* Read secondary bus number from PCI bridge config (offset 0x19). */
    uint8_t secondary_bus = pciConfigReadByte(parent_bus, dev, func, 0x19);

    pir_buses.buses[pir_buses.count].node = node;
    pir_buses.buses[pir_buses.count].bus_num = secondary_bus;
    pir_buses.count++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static void discover_secondary_buses(uacpi_namespace_node *root_bridge,
                                     uint8_t parent_bus)
{
    uacpi_namespace_for_each_child(
        root_bridge,
        discover_secondary_callback,
        UACPI_NULL,
        UACPI_OBJECT_DEVICE_BIT,
        3,
        &parent_bus);
}

static void discover_pci_buses(void)
{
    pir_buses.count = 0;

    uacpi_namespace_node *sb_node =
        uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB);
    if (!sb_node) {
        printf("pir: _SB not found, cannot discover PCI buses\n");
        return;
    }

    const uacpi_char *hids[] = { "PNP0A03", "PNP0A08", UACPI_NULL };
    uacpi_find_devices_at(sb_node, hids, discover_root_bus_callback, UACPI_NULL);

    size_t root_count = pir_buses.count;
    for (size_t i = 0; i < root_count; i++) {
        discover_secondary_buses(pir_buses.buses[i].node,
                                  pir_buses.buses[i].bus_num);
    }
}

/* Find the first PCI ISA bridge (class 0x06, subclass 0x01) to use as
 * router. Falls back to (0, 0, 0) - the host bridge - if none found. */
static bool find_isa_bridge(uint8_t *out_bus, uint8_t *out_devfn)
{
    for (size_t b = 0; b < pir_buses.count; b++) {
        uint8_t bus = pir_buses.buses[b].bus_num;
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                if (!pci_device_exists(bus, dev, func)) {
                    if (func == 0) break;
                    continue;
                }
                uint8_t class_code = pciConfigReadByte(bus, dev, func, 0x0B);
                uint8_t subclass = pciConfigReadByte(bus, dev, func, 0x0A);
                if (class_code == 0x06 && subclass == 0x01) {
                    *out_bus = bus;
                    *out_devfn = (uint8_t)((dev << 3) | func);
                    return true;
                }
                if (func == 0) {
                    uint8_t header = pciConfigReadByte(bus, dev, func, 0x0E);
                    if (!(header & 0x80)) break;
                }
            }
        }
    }
    return false;
}

/* Get an existing slot for (bus, dev) or create a new one. dev is already
 * shifted left by 3 to match the $PIR encoding. */
static struct pir_slot *get_or_create_slot(uint8_t bus, uint8_t dev_shifted)
{
    for (size_t i = 0; i < slot_count; i++) {
        if (slots[i].bus == bus && slots[i].dev == dev_shifted)
            return &slots[i];
    }
    if (slot_count >= MAX_PIR_SLOTS)
        return NULL;

    struct pir_slot *s = &slots[slot_count++];
    memset(s, 0, sizeof(*s));
    s->bus = bus;
    s->dev = dev_shifted;
    return s;
}

/* Accumulate possible IRQs (< 16) into a 16-bit bitmap from a link
 * device's _PRS. */
struct prs_ctx {
    uint16_t bitmap;
};

static uacpi_iteration_decision link_prs_callback(void *user,
                                                   uacpi_resource *resource)
{
    struct prs_ctx *ctx = user;

    if (resource->type == UACPI_RESOURCE_TYPE_IRQ) {
        for (uint8_t i = 0; i < resource->irq.num_irqs; i++) {
            if (resource->irq.irqs[i] < 16)
                ctx->bitmap |= (uint16_t)(1u << resource->irq.irqs[i]);
        }
    } else if (resource->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
        for (uint8_t i = 0; i < resource->extended_irq.num_irqs; i++) {
            if (resource->extended_irq.irqs[i] < 16)
                ctx->bitmap |= (uint16_t)(1u << resource->extended_irq.irqs[i]);
        }
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* Look up an existing link device or assign a new link ID and evaluate _PRS
 * for its possible-IRQ bitmap. Link IDs start at 0x80 to leave the lower
 * range for static-GSI link IDs. */
static uint8_t get_link_id(uacpi_namespace_node *link, uint16_t *bitmap_out)
{
    for (size_t i = 0; i < pir_link_devices.count; i++) {
        if (pir_link_devices.entries[i].node == link) {
            *bitmap_out = pir_link_devices.entries[i].bitmap;
            return pir_link_devices.entries[i].link_id;
        }
    }
    if (pir_link_devices.count >= MAX_LINK_DEVICES) {
        *bitmap_out = LEGACY_PCI_IRQ_BITMAP;
        return 0;
    }

    struct prs_ctx ctx = { .bitmap = 0 };
    uacpi_for_each_device_resource(link, "_PRS", link_prs_callback, &ctx);
    if (ctx.bitmap == 0)
        ctx.bitmap = LEGACY_PCI_IRQ_BITMAP;

    uint8_t link_id = (uint8_t)(0x80 + pir_link_devices.count);
    pir_link_devices.entries[pir_link_devices.count].node = link;
    pir_link_devices.entries[pir_link_devices.count].link_id = link_id;
    pir_link_devices.entries[pir_link_devices.count].bitmap = ctx.bitmap;
    pir_link_devices.count++;

    *bitmap_out = ctx.bitmap;
    return link_id;
}

bool pir_init(struct csmwrap_priv *priv)
{
    printf("pir: building $PIR table\n");

    slot_count = 0;
    pir_link_devices.count = 0;

    discover_pci_buses();
    if (pir_buses.count == 0) {
        printf("pir: no PCI buses discovered, skipping $PIR\n");
        return false;
    }

    uint8_t router_bus = 0, router_devfunc = 0;
    if (!find_isa_bridge(&router_bus, &router_devfunc)) {
        printf("pir: no ISA bridge found, using host bridge as router\n");
    }

    /* Walk _PRT for each bus; group entries by (bus, dev) into slots. */
    for (size_t bi = 0; bi < pir_buses.count; bi++) {
        struct pci_bus_info *binfo = &pir_buses.buses[bi];
        uacpi_pci_routing_table *prt = NULL;
        if (uacpi_get_pci_routing_table(binfo->node, &prt) != UACPI_STATUS_OK
            || !prt)
            continue;

        for (size_t i = 0; i < prt->num_entries; i++) {
            uacpi_pci_routing_table_entry *e = &prt->entries[i];
            uint8_t dev = (uint8_t)((e->address >> 16) & 0x1F);
            uint8_t pin = (uint8_t)e->pin;
            if (pin > 3)
                continue;

            struct pir_slot *slot =
                get_or_create_slot(binfo->bus_num, (uint8_t)(dev << 3));
            if (!slot) {
                printf("pir: too many slots, truncating\n");
                uacpi_free_pci_routing_table(prt);
                goto prt_done;
            }

            uint8_t link;
            uint16_t bitmap;
            if (e->source == NULL) {
                /* Static GSI: link ID = GSI (truncated to 8 bits, fine for
                 * the typical < 256 GSI range), bitmap reflects the actual
                 * IRQ when representable. */
                link = (uint8_t)e->index;
                bitmap = (e->index < 16) ? (uint16_t)(1u << e->index)
                                         : LEGACY_PCI_IRQ_BITMAP;
            } else {
                link = get_link_id(e->source, &bitmap);
            }

            slot->pins[pin].link = link;
            slot->pins[pin].bitmap = bitmap;
        }

        uacpi_free_pci_routing_table(prt);
    }
prt_done:

    if (slot_count == 0) {
        printf("pir: no _PRT entries found, skipping $PIR\n");
        return false;
    }

    size_t table_size = sizeof(struct pir_header)
                      + slot_count * sizeof(struct pir_slot);

    /* Allocate below 4 GiB - IrqRoutingTablePointer is 32 bits.
     * EfiRuntimeServicesData becomes E820 RESERVED so the OS won't reclaim
     * it; SeaBIOS dereferences PirAddr on demand from INT 1Ah callers. */
    EFI_PHYSICAL_ADDRESS table_addr = 0xFFFFFFFF;
    EFI_STATUS status = gBS->AllocatePages(
        AllocateMaxAddress, EfiRuntimeServicesData,
        (table_size + 4095) / 4096, &table_addr);
    if (EFI_ERROR(status)) {
        printf("pir: failed to allocate memory for $PIR\n");
        return false;
    }

    memset((void *)(uintptr_t)table_addr, 0, table_size);

    struct pir_header *hdr = (struct pir_header *)(uintptr_t)table_addr;
    hdr->signature = PIR_SIGNATURE;
    hdr->version = PIR_VERSION;
    hdr->size = (uint16_t)table_size;
    hdr->router_bus = router_bus;
    hdr->router_devfunc = router_devfunc;
    hdr->exclusive_irqs = 0;
    hdr->compatible_devid = 0;
    hdr->miniport_data = 0;

    struct pir_slot *out_slots = (struct pir_slot *)(hdr + 1);
    memcpy(out_slots, slots, slot_count * sizeof(struct pir_slot));

    /* Checksum: sum of all bytes (including checksum field) must be 0. */
    uint8_t sum = 0;
    uint8_t *bytes = (uint8_t *)hdr;
    for (size_t i = 0; i < table_size; i++)
        sum += bytes[i];
    hdr->checksum = (uint8_t)(-sum);

    priv->csm_efi_table->IrqRoutingTablePointer = (uint32_t)table_addr;
    priv->csm_efi_table->IrqRoutingTableLength = (uint32_t)table_size;

    printf("pir: $PIR built at 0x%lx, %zu slots, %zu bytes "
           "(router %02x:%02x.%x, %zu link device(s))\n",
           (unsigned long)table_addr, slot_count, table_size,
           router_bus, router_devfunc >> 3, router_devfunc & 7,
           pir_link_devices.count);

    return true;
}
