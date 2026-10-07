#ifndef USB_STORAGE_WAIT_H
#define USB_STORAGE_WAIT_H

#include "USBMassStorage.h"

/* The result check and wait registration share the event-service lock.
 * prepare must register a timed kernel wait without blocking; block must
 * tolerate a completion arriving after unlock but before block. */
typedef struct USBStorageWaitOps {
    void (*lock)(void *);
    void (*unlock)(void *);
    int (*result)(void *);
    uhci_u64 (*milliseconds)(void *);
    void (*prepare)(void *, int ticks);
    void (*block)(void *);
} USBStorageWaitOps;

int USBStorageWaitTicks(uhci_u64 now, uhci_u64 deadline, unsigned hz);
int USBStorageWait(void *, const USBStorageWaitOps *, uhci_u64 deadline,
                   unsigned hz);

#endif
