#ifndef _PIR_H
#define _PIR_H

#include <stdbool.h>

#include "csmwrap.h"

/*
 * Build a $PIR (PCI IRQ Routing) table from ACPI _PRT and _PRS evaluations,
 * allocate it below 4 GiB, and write the address/length into the CSM's
 * EFI_COMPATIBILITY16_TABLE so SeaBIOS can hand it out via INT 1Ah AX=B406h.
 *
 * Must be called before ExitBootServices (uses gBS->AllocatePages and uACPI).
 * Returns true on success, false if no _PRT entries were found or allocation
 * failed - in which case the field stays zero and SeaBIOS reports the
 * function as unsupported.
 */
bool pir_init(struct csmwrap_priv *priv);

#endif /* _PIR_H */
