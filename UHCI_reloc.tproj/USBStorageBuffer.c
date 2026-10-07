#include "USBStorageBuffer.h"
#include "UHCIMemory.h"

void USBStorageBufferUnmap(USBStorageBuffer *p)
{
    unsigned i;
    for (i = 0; i < p->count; i++)
        p->ops->unmap(p->context, p->mapped[i], p->mappingBytes);
    p->count = 0;
}

int USBStorageBufferMap(USBStorageBuffer *p, const USBStorageBufferOps *ops,
    void *context, unsigned mappingBytes, void *buffer, unsigned length, int local)
{
    uhci_u32 current = (uhci_u32)buffer, physical, base, lastBase = 0;
    unsigned n, index, offset;
    p->ops = ops; p->context = context; p->count = 0; p->local = 0;
    p->mappingBytes = mappingBytes;
    if (length > USB_STORAGE_MAX_REQUEST ||
        (length && (!buffer || current > 0xffffffffUL - (length - 1)))) return 0;
    if (!length) return 1;
    if (local) { p->local = buffer; return 1; }
    if (mappingBytes < 4096 || (mappingBytes & (mappingBytes - 1))) return 0;
    while (length) {
        if (!ops->physical(context, current, &physical)) goto fail;
        /* DriverKit maps Mach pages (8192 bytes on OPENSTEP Intel), while
         * x86 physical translations and the USB DMA slices use 4096 bytes.
         * Preserve the offset within the larger mapping to avoid aliasing
         * adjacent client pages onto the first half of the same Mach page. */
        offset = physical & (mappingBytes - 1);
        base = physical & ~(mappingBytes - 1);
        n = 4096 - (physical & 4095);
        if (n > length) n = length;
        /* Still translate every 4 KiB slice: physical adjacency must be
         * established, not inferred from an 8 KiB virtual Mach page. Reuse
         * the prior mapping only for contiguous bytes of that same page. */
        if (p->count && base == lastBase &&
            offset == p->offset[p->count - 1] + p->length[p->count - 1]) {
            p->length[p->count - 1] += n;
        } else {
            if (p->count == USB_STORAGE_CLIENT_PAGES) goto fail;
            index = p->count;
            if (!ops->map(context, base, mappingBytes, &p->mapped[index])) goto fail;
            p->offset[index] = offset; p->length[index] = n;
            p->count++; lastBase = base;
        }
        current += n; length -= n;
    }
    return 1;
fail:
    USBStorageBufferUnmap(p);
    return 0;
}

void USBStorageBufferCopy(USBStorageBuffer *p, const void *buffer, unsigned length)
{
    const uhci_u8 *source = (const uhci_u8 *)buffer;
    unsigned i;
    if (p->local) {
        bcopy(source, p->local, length);
        return;
    }
    for (i = 0; i < p->count && length; i++) {
        unsigned n = p->length[i];
        uhci_u8 *to = (uhci_u8 *)p->mapped[i] + p->offset[i];
        if (n > length) n = length;
        bcopy(source, to, n);
        source += n; length -= n;
    }
}
