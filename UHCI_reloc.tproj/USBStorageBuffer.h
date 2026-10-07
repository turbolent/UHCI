#ifndef USB_STORAGE_BUFFER_H
#define USB_STORAGE_BUFFER_H
#include "USBMassStorage.h"

#define USB_STORAGE_CLIENT_PAGES (USB_STORAGE_MAX_REQUEST / 4096 + 1)

typedef struct USBStorageBufferOps {
    int (*physical)(void *, uhci_u32, uhci_u32 *);
    int (*map)(void *, uhci_u32, uhci_u32, void **);
    void (*unmap)(void *, void *, uhci_u32);
} USBStorageBufferOps;

typedef struct USBStorageBuffer {
    const USBStorageBufferOps *ops;
    void *context;
    void *mapped[USB_STORAGE_CLIENT_PAGES];
    unsigned offset[USB_STORAGE_CLIENT_PAGES], length[USB_STORAGE_CLIENT_PAGES];
    unsigned count, mappingBytes;
    void *local;
} USBStorageBuffer;

int USBStorageBufferMap(USBStorageBuffer *, const USBStorageBufferOps *, void *,
    unsigned, void *, unsigned, int);
void USBStorageBufferCopy(USBStorageBuffer *, const void *, unsigned);
void USBStorageBufferUnmap(USBStorageBuffer *);
#endif
