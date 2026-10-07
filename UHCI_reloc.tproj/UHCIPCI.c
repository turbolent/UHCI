#include "UHCIPCI.h"
#include "UHCIMemory.h"
static int readpci(const UHCIPCIOps *o, void *c, unsigned r, uhci_u32 *v)
{ return o->read(c, r, v) && *v != 0xffffffffU; }
int UHCIPCICommand(const UHCIPCIOps *o, void *c, unsigned set, unsigned clear)
{
    uhci_u32 v, after;
    if (!readpci(o, c, 4, &v) || (v & 65535) == 65535) return 0;
    v = ((v & 65535) | set) & ~clear;
    /* Upper half is W1C PCI Status: never echo it. */
    return o->write(c, 4, v) && readpci(o, c, 4, &after) && (after & 65535) == v;
}
int UHCIPCIDisableMessages(const UHCIPCIOps *o, void *c)
{
    uhci_u32 v, after;
    unsigned seen[8], offset, count = 0, msi = 0, msix = 0;
    bzero(seen, sizeof(seen));
    if (!readpci(o, c, 4, &v)) return 0;
    if (!(v & (1U << 20))) return 1;
    if (!readpci(o, c, 0x34, &v)) return 0;
    offset = v & 255;
    while (offset) {
        unsigned next, mask = 0, set = 0;
        if (offset < 0x40 || offset > 0xfc || (offset & 3) || ++count > 48 ||
            (seen[offset / 32] & (1U << (offset % 32)))) return 0;
        seen[offset / 32] |= 1U << (offset % 32);
        if (!readpci(o, c, offset, &v)) return 0;
        next = (v >> 8) & 255;
        if ((v & 255) == 5) { if (msi++) return 0; mask = 1U << 16; }
        if ((v & 255) == 0x11) { if (msix++) return 0; mask = 1U << 31; set = 1U << 30; }
        if ((v & 255) == 1) {
            if (offset > 0xf8 || !readpci(o, c, offset + 4, &after) || (after & 3)) return 0;
        }
        if (mask && (!o->write(c, offset, (v & ~mask) | set) ||
            !readpci(o, c, offset, &after) || (after & mask) || (after & set) != set)) return 0;
        offset = next;
    }
    return 1;
}
static int legacy(const UHCIPCIOps *o, void *c, unsigned route)
{
    uhci_u32 before,after;
    if (!readpci(o,c,UHCI_LEGSUP,&before)) return 0;
    /* Lower half contains RO and W1C status. Never echo observed status. */
    if (!o->write(c,UHCI_LEGSUP,(before&0xffff0000U)|route) ||
        !readpci(o,c,UHCI_LEGSUP,&after)) return 0;
    return (after&(UHCI_LEGACY_ENABLES|UHCI_PIRQ))==route;
}
int UHCIPCISelectGate(const UHCIPCIOps *o, void *c, unsigned vendor, unsigned *kind)
{
    uhci_u32 before,after,desired;
    *kind=0;
    if (!readpci(o,c,4,&before) || (before&65535)==65535) return 0;
    desired=((before&65535)|UHCI_PCI_INT_DISABLE)&~UHCI_PCI_MASTER;
    if (!o->write(c,4,desired) || !readpci(o,c,4,&after) ||
        ((after&65535)!=(desired&~UHCI_PCI_INT_DISABLE) && (after&65535)!=desired)) return 0;
    if (after&UHCI_PCI_INT_DISABLE) {
        *kind=1;
        if (!legacy(o,c,0)) return 0;
        /* QEMU implements the PCI gate but not LEGSUP. Probe this optional
         * gate only while the verified PCI fence contains all sources. */
        if (legacy(o,c,UHCI_PIRQ)) *kind=5;
        return legacy(o,c,0);
    }
    if (vendor!=0x8086 && vendor!=0x1106) return 0;
    *kind=2;
    return legacy(o,c,0);
}
int UHCIPCIGate(const UHCIPCIOps *o, void *c, unsigned kind, int closed)
{
    uhci_u32 command;
    if (kind!=1 && kind!=2 && kind!=5) return 0;
    if (closed) {
        if (kind&1) return UHCIPCICommand(o,c,UHCI_PCI_INT_DISABLE,0);
        return legacy(o,c,0);
    }
    if (!readpci(o,c,4,&command) || !(command&UHCI_PCI_MASTER) || !(command&UHCI_PCI_IO)) return 0;
    if ((kind==2 || kind==5) && !legacy(o,c,UHCI_PIRQ)) return 0;
    return !(kind&1) || UHCIPCICommand(o,c,0,UHCI_PCI_INT_DISABLE);
}
int UHCIPCIBar(const UHCIPCIOps *o, void *c, unsigned *address, unsigned *bytes)
{
    uhci_u32 bar;
    /* UHCI USBBASE defines a 32-byte, 32-byte-aligned I/O register range.
     * Decode it without sizing writes: DriverKit has not yet reserved it,
     * and a duplicate probe must leave an existing owner completely alone. */
    if (!readpci(o,c,0x20,&bar) || (bar&31)!=1 || !(bar&0xffe0) ||
        (bar&0xffff0000U)) return 0;
    *address=bar&0xffe0; *bytes=32; return 1;
}
