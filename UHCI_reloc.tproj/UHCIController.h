#ifndef UHCI_CONTROLLER_H
#define UHCI_CONTROLLER_H
#import <driverkit/IOSCSIController.h>
#import <machkit/NXLock.h>
#import "UHCICore.h"
#import "UHCIInterruptState.h"
#import "UHCIPCI.h"
#import "USBStorageSCSI.h"
#import "UHCIStorageStats.h"
@interface UHCIController : IOSCSIController
{
@public /* C platform bridge; all access follows the documented lock order. */
    UHCIControllerState _state;
    UHCIInterruptState _interruptState;
    id _pciDescription;
    NXLock *_eventLock, *_boundaryLock;
    unsigned _irq, _pciOwned, _contained, _gateKind;
    volatile unsigned _workersStarted, _startupReady;
    unsigned _serviceEvent;
    NXLock *_storageLocks[USB_STORAGE_TARGETS];
    int _storageWaitEvents[USB_STORAGE_TARGETS];
    unsigned _storageActiveRequests;
    BOOL _storageProfileEnabled;
    BOOL _storageProfileActive[USB_STORAGE_TARGETS];
    BOOL _storageTransferActive[USB_STORAGE_TARGETS];
    uhci_u64 _storageServicedAt[USB_STORAGE_TARGETS];
    uhci_u64 _storageMetrics[UHCI_STORAGE_METRICS];
    USBStorageSCSIState _storageDisks[USB_STORAGE_TARGETS];
    USBMassStorage _storageBOT[USB_STORAGE_TARGETS];
    void *_storageBuffers[USB_STORAGE_TARGETS];
    BOOL _scsiThreadStarted, _deferIOThread, _initializationFailed;
    id _storageDiskClass;
    id _storageProbeDescription;
    BOOL _storageRegistrationReady;
    BOOL _storageProbePending;
    BOOL _storageProbeRunning;
    int _storageProbeEvent;
}
+ (BOOL)probe:description;
- initFromDeviceDescription:description;
- (IOReturn)startIOThread;
- failInitialization;
- failInitializationAt:(unsigned)line;
- (void)interruptOccurred;
- (void)runCompletionLoop;
- (void)runManagementLoop;
- (void)runRetryLoop;
- (void)containFatalController;
@end

@interface UHCIController (Storage)
- (BOOL)startStorageProbeWorker;
- (void)finishStorageRegistration;
- (void)queueStorageProbe;
- (void)runStorageProbeLoop;
- (void)waitForStorageProbeExit;
- (void)wakeStorageTarget:(unsigned)target;
- (void)wakeStorageWaiters;
- (sc_status_t)executeRequest:(IOSCSIRequest *)request buffer:(void *)buffer
                       client:(vm_task_t)client;
- (sc_status_t)executeSCSI3Request:(IOSCSI3Request *)request buffer:(void *)buffer
                            client:(vm_task_t)client;
- (sc_status_t)resetSCSIBus;
- (unsigned)maxTransfer;
- (void)getDMAAlignment:(IODMAAlignment *)alignment;
- (int)numberOfTargets;
- (int)storageTransfer:(unsigned)target generation:(uhci_u32)generation
              endpoint:(uhci_u8)endpoint setup:(const USBSetupPacket *)setup
                buffer:(void *)buffer length:(uhci_u32)length
                actual:(uhci_u32 *)actual deadline:(uhci_u64)deadline;
- (int)storageClearHalt:(unsigned)target generation:(uhci_u32)generation
              endpoint:(uhci_u8)endpoint deadline:(uhci_u64)deadline;
- (USBStorageResult)storageExecute:(unsigned)target cdb:(const uhci_u8 *)cdb
                           length:(unsigned)cdbLength buffer:(void *)buffer
                           client:(vm_task_t)client maximum:(unsigned)maximum
                             read:(int)read autoSense:(int)autoSense
                         deadline:(uhci_u64)deadline;

@end

#endif
