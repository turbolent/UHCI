#ifndef UHCI_INTERRUPT_STATE_H
#define UHCI_INTERRUPT_STATE_H
#include "UHCIRegs.h"
#define UHCI_MODE_INVALID 0U
#define UHCI_MODE_INTX 1U
#define UHCI_MODE_POLLING 2U
typedef struct UHCIInterruptOps {
    uhci_u32 (*read)(void *, unsigned);
    int (*write)(void *, unsigned, uhci_u32);
    int (*pciRead)(void *, uhci_u32 *);
    int (*pciWrite)(void *, uhci_u32);
    int (*gate)(void *, int closed);
    int (*rearm)(void *);
    void (*publish)(void *);
} UHCIInterruptOps;
typedef struct UHCIInterruptState {
    unsigned mode, participant, active, generation, resourcesOwned;
    volatile unsigned stopping, admitted;
    unsigned mmioLost, interruptDebt, dmaDebt, rearmDebt;
    unsigned fenced, dmaFenced, pending, work;
    unsigned callbacks, foreign, owned, retries, failures, services;
    unsigned watchdogPending;
    uhci_u64 watchdogSince;
    const UHCIInterruptOps *ops;
    void *context;
} UHCIInterruptState;
unsigned UHCIInterruptCauses(unsigned status, unsigned enabled);
unsigned UHCIInterruptModeParse(const char *);
/* All functions below require the native short MMIO/PCI/rearm lock. */
void UHCIInterruptLost(UHCIInterruptState *);
int UHCIInterruptFence(UHCIInterruptState *, int dma);
void UHCIInterruptCallback(UHCIInterruptState *);
void UHCIInterruptRetry(UHCIInterruptState *);
void UHCIInterruptWatchdog(UHCIInterruptState *, uhci_u64 now);
unsigned UHCIInterruptTakeWork(UHCIInterruptState *);
int UHCIInterruptRestore(UHCIInterruptState *);
void UHCIInterruptPoll(UHCIInterruptState *);
void UHCIInterruptStop(UHCIInterruptState *);
#endif
