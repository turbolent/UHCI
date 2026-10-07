#ifndef UHCI_CORE_H
#define UHCI_CORE_H
#include "UHCIRegs.h"
#include "USBMassStorage.h"

#define UHCI_TD_COUNT 2050U
#define UHCI_DESC_PAGES 9U
#define UHCI_DATA_PAGES 4U
#define UHCI_FRAME_COUNT 1024U
#define UHCI_ENDPOINTS 3U
#define UHCI_EP_CONTROL 0U
#define UHCI_EP_BULK_IN 1U
#define UHCI_EP_BULK_OUT 2U
typedef struct UHCIEndpoint {
    UHCIDMA descriptors, tdPages[UHCI_DESC_PAGES], data[UHCI_DATA_PAGES];
    UHCIQH *qh;
    uhci_u32 qhPhysical;
    unsigned address, maxPacket, configured, linked, waiting, control, input;
    unsigned toggle, halted, count, scan, actual, total, statusTD, statusPhase;
    unsigned retiring, retireFrame, retireAdvances, resumeStatus, retireResult;
    unsigned successful, startToggle, reserved;
    uhci_u64 deadline, retireDeadline;
    struct UHCIEndpoint *next;
    int result;
} UHCIEndpoint;

typedef struct UHCIDevice {
    unsigned used, ready, generation, address, port, speed;
    unsigned parent, parentPort, hub, hubPorts;
    unsigned hubCharacteristics;
    unsigned powerMA, selfPowerCapable, selfPowered, externalPorts, nonRemovable;
    unsigned portPowerMA[UHCI_MAX_HUB_PORTS + 1];
    unsigned configurationValue, storage;
    unsigned hubChanges, disconnecting, blockedPorts, controlBusy;
    USBMassStorageInterface storageInterface;
    USBCoreDevice usb;
    UHCIEndpoint endpoints[UHCI_ENDPOINTS];
} UHCIDevice;

typedef struct UHCIStorageBinding {
    uhci_u32 generation, removals;
    unsigned slot, lastPort;
} UHCIStorageBinding;

typedef struct UHCIEnumerationFailure {
    const char *stage;
    unsigned rootPort, hubPort, speed, vendor, product, protocol, packet, interval;
} UHCIEnumerationFailure;

typedef struct UHCIControllerState {
    unsigned ioBase, ioBytes, maxPorts, vendor;
    UHCIEndpoint *controlHead, *bulkHead;
    unsigned running, registersValid, dmaArmed, haltConfirmed, fatal;
    unsigned portChanges, disconnected, initialized, enumerating, blockedPorts;
    unsigned rootPortStatus[2], rootPortsScanned;
    unsigned scheduleBusy, rescan;
    unsigned addresses[4], nextGeneration;
    unsigned transferEvents, polls;
    unsigned scheduleChanges, timeouts, errors;
    const char *faultReason;
    unsigned faultDetail, lastCommand, lastStatus;
    unsigned waitOffset, waitMask, waitExpected, waitObserved;
    UHCIDMA asyncHead, frameList;
    UHCIDevice devices[UHCI_MAX_DEVICES];
    UHCIStorageBinding storage[USB_STORAGE_TARGETS];
    UHCIEnumerationFailure enumerationFailure;
    void *owner;
} UHCIControllerState;

/* Native glue serializes every port-I/O/PCI operation with IRQ containment.
 * Core entry points require the controller-state lock. Sleep releases that
 * lock in the native glue so completion service can progress. */
uhci_u32 UHCIPlatformRead(UHCIControllerState *, uhci_u32 offset);
int UHCIPlatformWrite(UHCIControllerState *, uhci_u32 offset, uhci_u32 value);
int UHCIPlatformPCIRead(UHCIControllerState *, unsigned, uhci_u32 *);
int UHCIPlatformPCIWrite(UHCIControllerState *, unsigned, uhci_u32);
void *UHCIPlatformAllocate(uhci_u32);
void UHCIPlatformFree(void *, uhci_u32);
uhci_u32 UHCIPlatformPhysical(void *);
uhci_u64 UHCIPlatformMilliseconds(void);
void UHCIPlatformPause(UHCIControllerState *, unsigned milliseconds);
/* Bounded hardware wait only: keeps the state lock, never the boundary lock. */
void UHCIPlatformDelay(UHCIControllerState *, unsigned microseconds);
void UHCIPlatformWake(void *);
void UHCIPlatformStorageWake(void *, unsigned);
void UHCIPlatformStorageAttached(void *);
#ifdef KERNEL
#import <driverkit/generalFuncs.h>
#define UHCIPlatformLog IOLog
#else
void UHCIPlatformLog(const char *, ...);
#endif

int UHCICoreInitialize(UHCIControllerState *);
int UHCICoreStart(UHCIControllerState *);
int UHCICoreQuiesce(UHCIControllerState *);
int UHCICoreReleaseDMA(UHCIControllerState *, int busMasterDisabled);
int UHCICoreService(UHCIControllerState *, unsigned causes);
int UHCICoreServicePorts(UHCIControllerState *);
int UHCICoreSupportsPCIClass(uhci_u32);
void UHCICoreFail(UHCIControllerState *, const char *, unsigned);
UHCIDevice *UHCICoreStorageDevice(UHCIControllerState *, unsigned, uhci_u32);
int UHCICoreStorageBulkStart(UHCIControllerState *, unsigned, uhci_u32,
                            uhci_u8, const void *, uhci_u32, uhci_u64);
int UHCICoreStorageControlStart(UHCIControllerState *, unsigned, uhci_u32,
                               const USBSetupPacket *, const void *, uhci_u64);
int UHCICoreStorageTransferResult(UHCIControllerState *, unsigned, uhci_u32,
                                  uhci_u8, void *, uhci_u32 *);
int UHCICoreStorageRecoverEndpoint(UHCIControllerState *, unsigned, uhci_u32,
                                   uhci_u8, uhci_u64);
int UHCICoreStorageRetireEndpoint(UHCIControllerState *, unsigned, uhci_u32,
                                  uhci_u8, uhci_u64);
void UHCICoreStorageOffline(UHCIControllerState *, unsigned, uhci_u32);
/* Shared schedule helpers are also exercised directly by host tests. */
int UHCICoreConfigureEndpoint(UHCIControllerState *, UHCIDevice *, unsigned,
                             unsigned address, unsigned packet, unsigned interval);
int UHCICoreSubmit(UHCIControllerState *, UHCIDevice *, UHCIEndpoint *,
                   const USBSetupPacket *, const void *, unsigned, uhci_u64);
int UHCICoreFinish(UHCIControllerState *, UHCIDevice *, UHCIEndpoint *);
int UHCICoreCancel(UHCIControllerState *, UHCIDevice *, UHCIEndpoint *);
void UHCICoreDisconnect(UHCIControllerState *, unsigned slot);

uhci_u32 UHCIPortWriteValue(uhci_u32 current, uhci_u32 set,
                           uhci_u32 clear, uhci_u32 changes);
#endif
