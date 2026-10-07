#ifndef UHCI_PCI_H
#define UHCI_PCI_H
#include "UHCIRegs.h"
typedef struct UHCIPCIOps {
    int (*read)(void *, unsigned, uhci_u32 *);
    int (*write)(void *, unsigned, uhci_u32);
} UHCIPCIOps;
int UHCIPCICommand(const UHCIPCIOps *, void *, unsigned set, unsigned clear);
int UHCIPCIDisableMessages(const UHCIPCIOps *, void *);
int UHCIPCIBar(const UHCIPCIOps *, void *, unsigned *address, unsigned *bytes);
/* kind: 1 PCI gate, 2 legacy PIRQ gate, 5 PCI plus implemented PIRQ. */
int UHCIPCISelectGate(const UHCIPCIOps *, void *, unsigned vendor, unsigned *kind);
int UHCIPCIGate(const UHCIPCIOps *, void *, unsigned kind, int closed);
#endif
