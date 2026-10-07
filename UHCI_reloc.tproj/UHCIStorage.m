#define MACH_USER_API 1
#import "UHCIController.h"
#import "UHCIVersion.h"
#import "USBStorageBuffer.h"
#import "USBStorageWait.h"
#import <mach/vm_param.h>
#import <driverkit/generalFuncs.h>
#import <driverkit/kernelDriver.h>
#import <kernserv/prototypes.h>
#import <string.h>
#import <stdio.h>
#import <objc/objc-runtime.h>

/* Native OPENSTEP scheduler exports; PCIMSI uses the same timed-wait API.
 * Scheduler calls run with ordinary interrupt state, never raw IF exclusion. */
extern int hz;
extern void thread_wakeup(int event);

static uhci_u64 storageNanoseconds(void)
{
    ns_time_t now;
    IOGetTimestamp(&now);
    return (uhci_u64)now;
}

typedef struct UHCIStorageWaitContext {
    NXLock *lock;
    UHCIControllerState *state;
    UHCIInterruptState *interrupt;
    unsigned target;
    uhci_u32 generation;
    uhci_u8 endpoint;
    void *buffer;
    uhci_u32 *actual;
    int event;
    uhci_u64 *servicedAt;
    uhci_u64 resumedAt;
    uhci_u64 copyNS;
} UHCIStorageWaitContext;

static void waitLock(void *context)
{ [((UHCIStorageWaitContext *)context)->lock lock]; }
static void waitUnlock(void *context)
{ [((UHCIStorageWaitContext *)context)->lock unlock]; }
static int waitResult(void *context)
{
    UHCIStorageWaitContext *w = (UHCIStorageWaitContext *)context;
    uhci_u64 before = 0;
    int result;
    if (w->interrupt->stopping) return USB_TRANSFER_DISCONNECTED;
    if (w->servicedAt) {
        before = storageNanoseconds();
        if (*w->servicedAt && !w->resumedAt) w->resumedAt = before;
    }
    result = UHCICoreStorageTransferResult(w->state, w->target, w->generation,
                                           w->endpoint, w->buffer, w->actual);
    if (w->servicedAt && result != USB_TRANSFER_PENDING)
        w->copyNS += storageNanoseconds() - before;
    return result;
}
static uhci_u64 waitMilliseconds(void *context)
{ (void)context; return UHCIPlatformMilliseconds(); }
static void waitPrepare(void *context, int ticks)
{
    UHCIStorageWaitContext *w = (UHCIStorageWaitContext *)context;
    assert_wait(w->event, FALSE);
    thread_set_timeout(ticks);
}
static void waitBlock(void *context)
{ (void)context; thread_block(); }
static const USBStorageWaitOps storageWaitOps = {
    waitLock, waitUnlock, waitResult, waitMilliseconds, waitPrepare, waitBlock
};

typedef struct UHCIStorageSession {
    UHCIController *controller;
    unsigned target;
    uhci_u32 generation;
} UHCIStorageSession;

static int storageControl(void *context, const USBSetupPacket *setup,
    void *buffer, uhci_u32 *actual, uhci_u64 deadline)
{
    UHCIStorageSession *s = (UHCIStorageSession *)context;
    return [s->controller storageTransfer:s->target generation:s->generation
        endpoint:0 setup:setup buffer:buffer length:setup->length actual:actual deadline:deadline];
}
static int storageBulk(void *context, uhci_u8 endpoint, void *buffer,
    uhci_u32 length, uhci_u32 *actual, uhci_u64 deadline)
{
    UHCIStorageSession *s = (UHCIStorageSession *)context;
    return [s->controller storageTransfer:s->target generation:s->generation
        endpoint:endpoint setup:0 buffer:buffer length:length actual:actual deadline:deadline];
}
static int storageClear(void *context, uhci_u8 endpoint, uhci_u64 deadline)
{
    UHCIStorageSession *s = (UHCIStorageSession *)context;
    return [s->controller storageClearHalt:s->target generation:s->generation
        endpoint:endpoint deadline:deadline];
}
static uhci_u64 storageClock(void *context)
{
    (void)context;
    return UHCIPlatformMilliseconds();
}
static const USBStorageTransport storageTransport = {
    storageControl, storageBulk, storageClear, storageClock
};

/* These callbacks operate on already-wired SCSIDisk/generic-client buffers. */
static int clientPhysical(void *context, uhci_u32 address, uhci_u32 *physical)
{
    return IOPhysicalFromVirtual((vm_task_t)context, address,
                                  (unsigned *)physical) == IO_R_SUCCESS;
}
static int clientMap(void *context, uhci_u32 physical, uhci_u32 length, void **mapped)
{
    (void)context;
    return IOMapPhysicalIntoIOTask(physical, length,
                                  (vm_address_t *)mapped) == IO_R_SUCCESS;
}
static void clientUnmap(void *context, void *mapped, uhci_u32 length)
{
    (void)context;
    IOUnmapPhysicalFromIOTask((vm_address_t)mapped, length);
}
static const USBStorageBufferOps clientBufferOps = { clientPhysical, clientMap, clientUnmap };

static sc_status_t scsiStatus(USBStorageResult *result, unsigned char *status,
                              esense_reply_t *sense, int autoSense)
{
    *status = 0;
    bzero(sense, sizeof(*sense));
    switch (result->status) {
    case USB_TRANSFER_OK: return SR_IOST_GOOD;
    case USB_STORAGE_CHECK:
        *status = 2;
        if (autoSense && result->senseValid) {
            unsigned n = sizeof(*sense);
            if (n > sizeof(result->sense)) n = sizeof(result->sense);
            bcopy(result->sense, sense, n);
            return SR_IOST_CHKSV;
        }
        return SR_IOST_CHKSNV;
    case USB_TRANSFER_TIMEOUT: return SR_IOST_IOTO;
    case USB_TRANSFER_DISCONNECTED: return SR_IOST_SELTO;
    case USB_STORAGE_INVALID: return SR_IOST_INVALID;
    default: return SR_IOST_INT;
    }
}

static void storageProbeThread(void *context)
{
    [(UHCIController *)context runStorageProbeLoop];
    IOExitThread();
}

@implementation UHCIController (Storage)

- (BOOL)startStorageProbeWorker
{
    _storageDiskClass = objc_lookUpClass("SCSIDisk");
    if (!_storageDiskClass || ![_storageDiskClass respondsTo:@selector(probe:)]) {
        IOLog("UHCI: SCSIDisk probe is unavailable\n");
        return NO;
    }
    _storageProbeDescription = [[IODeviceDescription alloc] init];
    if (!_storageProbeDescription) return NO;
    [_storageProbeDescription setDirectDevice:self];
    /* Initial enumeration is complete; registerDevice will scan those targets.
     * Only subsequent attachments need the worker's additional scan. */
    [_eventLock lock];
    _storageProbePending = NO;
    _storageProbeRunning = YES;
    [_eventLock unlock];
    if (!IOForkThread(storageProbeThread, self)) {
        _storageProbeRunning = NO;
        IOLog("UHCI: cannot start storage probe worker\n");
        return NO;
    }
    return YES;
}

- (void)finishStorageRegistration
{
    [_eventLock lock];
    /* registerDevice synchronously runs the initial SCSIDisk probe. Only
     * then may the worker probe, including an attachment during that scan. */
    _storageRegistrationReady = YES;
    if (_storageProbePending) thread_wakeup((int)&_storageProbeEvent);
    [_eventLock unlock];
}

/* Called with the event lock held, or during single-threaded enumeration.
 * The callback never allocates or invokes the disk class on the I/O task. */
- (void)queueStorageProbe
{
    _storageProbePending = YES;
    if (_storageRegistrationReady)
        thread_wakeup((int)&_storageProbeEvent);
}

- (void)runStorageProbeLoop
{
    [_eventLock lock];
    while (!_interruptState.stopping) {
        if (!_storageRegistrationReady || !_storageProbePending) {
            assert_wait((int)&_storageProbeEvent, FALSE);
            [_eventLock unlock];
            thread_block();
            [_eventLock lock];
            continue;
        }
        _storageProbePending = NO;
        [_eventLock unlock];
        /* SCSIDisk reserves each target before probing and skips targets
         * already owned by a disk. It may issue synchronous USB I/O here. */
        [_storageDiskClass probe:_storageProbeDescription];
        [_eventLock lock];
        /* An attachment during the scan leaves pending set for another pass. */
    }
    _storageProbeRunning = NO;
    [_eventLock unlock];
}

- (void)waitForStorageProbeExit
{
    BOOL running;
    if (!_eventLock) return;
    do {
        [_eventLock lock];
        running = _storageProbeRunning;
        [_eventLock unlock];
        if (running) IOSleep(1);
    } while (running);
}

/* Called with _eventLock held. The event addresses live with the pinned
 * controller, and per-target serialization permits only one waiter each. */
- (void)wakeStorageTarget:(unsigned)target
{
    if (target < USB_STORAGE_TARGETS) {
        if (_storageTransferActive[target] && !_storageServicedAt[target])
            _storageServicedAt[target] = storageNanoseconds();
        thread_wakeup((int)&_storageWaitEvents[target]);
    }
}
- (void)wakeStorageWaiters
{
    unsigned target;
    thread_wakeup((int)&_storageProbeEvent);
    for (target = 0; target < USB_STORAGE_TARGETS; target++)
        [self wakeStorageTarget:target];
}

- (unsigned)maxTransfer { return 65536U; }
- (int)numberOfTargets { return USB_STORAGE_TARGETS; }
- (void)getDMAAlignment:(IODMAAlignment *)alignment
{
    alignment->readStart = alignment->writeStart = 1;
    alignment->readLength = alignment->writeLength = 1;
}

- (int)storageTransfer:(unsigned)target generation:(uhci_u32)generation
              endpoint:(uhci_u8)endpoint setup:(const USBSetupPacket *)setup
                buffer:(void *)buffer length:(uhci_u32)length
                actual:(uhci_u32 *)actual deadline:(uhci_u64)deadline
{
    int rc, recovery;
    UHCIStorageWaitContext wait;
    uhci_u64 start = 0, submitted = 0, finished, serviced;
    unsigned stage;
    BOOL profile;
    *actual = 0;
    if (target >= USB_STORAGE_TARGETS) return USB_TRANSFER_DISCONNECTED;
    if (UHCIPlatformMilliseconds() >= deadline) return USB_TRANSFER_TIMEOUT;
    [_eventLock lock];
    profile = _storageProfileActive[target];
    if (profile) {
        start = storageNanoseconds();
        _storageServicedAt[target] = 0;
        _storageTransferActive[target] = YES;
    }
    if (_interruptState.stopping || _state.fatal) rc = USB_TRANSFER_DISCONNECTED;
    else if (setup) rc = UHCICoreStorageControlStart(&_state, target, generation, setup, buffer, deadline);
    else rc = UHCICoreStorageBulkStart(&_state, target, generation, endpoint, buffer, length, deadline);
    if (profile) {
        submitted = storageNanoseconds();
        if (rc) _storageTransferActive[target] = NO;
    }
    [_eventLock unlock];
    if (rc) return rc;
    wait.lock = _eventLock; wait.state = &_state; wait.interrupt = &_interruptState;
    wait.target = target; wait.generation = generation; wait.endpoint = endpoint;
    wait.buffer = buffer; wait.actual = actual;
    wait.event = (int)&_storageWaitEvents[target];
    wait.servicedAt = profile ? &_storageServicedAt[target] : 0;
    wait.resumedAt = wait.copyNS = 0;
    /* Register while holding _eventLock, release it, then block. A completion
     * between unlock and block cancels the registered wait, so no wake is lost.
     * The reader never polls the hardware event ring. */
    rc = USBStorageWait(&wait, &storageWaitOps, deadline, (unsigned)hz);
    if (profile) {
        finished = storageNanoseconds();
        stage = setup ? UHCI_STAGE_CONTROL :
            (!(endpoint & 0x80) && length == 31 ? UHCI_STAGE_CBW :
             ((endpoint & 0x80) && length == 13 ? UHCI_STAGE_CSW : UHCI_STAGE_DATA));
        stage = UHCI_STAT_STAGE_BASE + stage * UHCI_STAGE_METRICS;
        [_eventLock lock];
        serviced = _storageServicedAt[target];
        _storageTransferActive[target] = NO;
        _storageMetrics[stage + UHCI_STAGE_COUNT]++;
        if (rc) _storageMetrics[stage + UHCI_STAGE_ERRORS]++;
        _storageMetrics[stage + UHCI_STAGE_PREPARE_NS] += submitted - start;
        if (serviced >= submitted && serviced)
            _storageMetrics[stage + UHCI_STAGE_SERVICE_NS] += serviced - submitted;
        if (wait.resumedAt >= serviced && serviced)
            _storageMetrics[stage + UHCI_STAGE_RESUME_NS] += wait.resumedAt - serviced;
        _storageMetrics[stage + UHCI_STAGE_COPY_NS] += wait.copyNS;
        _storageMetrics[stage + UHCI_STAGE_TOTAL_NS] += finished - start;
        [_eventLock unlock];
    }
    /* EP0 stalls are expected for GET MAX LUN. Reset the host endpoint without
     * marking the USB device for reprobe or clearing a non-existent EP0 halt. */
    if (rc == USB_TRANSFER_TIMEOUT || (!endpoint && rc == USB_TRANSFER_STALL)) {
        uhci_u64 recoveryDeadline = deadline;
        if (rc == USB_TRANSFER_TIMEOUT) recoveryDeadline = deadline + 5000;
        [_eventLock lock];
        recovery = UHCICoreStorageRecoverEndpoint(&_state, target, generation,
                                                    endpoint, recoveryDeadline);
        [_eventLock unlock];
        if (recovery && rc == USB_TRANSFER_STALL) rc = recovery;
    } else if (rc && rc != USB_TRANSFER_DISCONNECTED) {
        /* Retire failed split work before BOT reset/clear-halt can reuse a
         * shared TT buffer. Preserve the halt/toggle until USB recovery. */
        [_eventLock lock];
        recovery = UHCICoreStorageRetireEndpoint(&_state, target, generation,
                         endpoint, UHCIPlatformMilliseconds() + 5000);
        [_eventLock unlock];
        if (recovery) rc = recovery;
    }
    if (_state.fatal) [self containFatalController];
    return rc;
}

- (int)storageClearHalt:(unsigned)target generation:(uhci_u32)generation
              endpoint:(uhci_u8)endpoint deadline:(uhci_u64)deadline
{
    USBSetupPacket setup;
    uhci_u32 actual;
    int rc;
    setup.requestType = 2; setup.request = 1; /* CLEAR_FEATURE(ENDPOINT_HALT) */
    setup.value = 0; setup.index = endpoint; setup.length = 0;
    rc = [self storageTransfer:target generation:generation endpoint:0 setup:&setup
        buffer:0 length:0 actual:&actual deadline:deadline];
    if (!rc) {
        [_eventLock lock];
        rc = UHCICoreStorageRecoverEndpoint(&_state, target, generation, endpoint, deadline);
        [_eventLock unlock];
    }
    if (_state.fatal) [self containFatalController];
    return rc;
}

- (USBStorageResult)storageExecute:(unsigned)target cdb:(const uhci_u8 *)cdb
                           length:(unsigned)cdbLength buffer:(void *)buffer
                           client:(vm_task_t)client maximum:(unsigned)maximum
                             read:(int)read autoSense:(int)autoSense
                         deadline:(uhci_u64)deadline
{
    USBStorageResult result;
    UHCIStorageSession session;
    USBStorageBuffer pages;
    UHCIDevice *device;
    USBMassStorage *bot;
    UHCIStorageBinding binding;
    int present;
    BOOL profile;
    uhci_u64 started, locked, stamp, mapNS = 0, copyNS = 0, unmapNS = 0;
    bzero(&result, sizeof(result)); result.status = USB_STORAGE_INVALID;
    if (target >= USB_STORAGE_TARGETS || maximum > USB_STORAGE_MAX_REQUEST ||
        !_storageLocks[target]) return result;
    started = storageNanoseconds();
    [_storageLocks[target] lock];
    [_eventLock lock];
    locked = storageNanoseconds();
    _storageActiveRequests++;
    profile = _storageProfileEnabled && cdbLength &&
        (cdb[0] == 0x08 || cdb[0] == 0x28 || cdb[0] == 0xa8 || cdb[0] == 0x88);
    _storageProfileActive[target] = profile;
    binding = _state.storage[target];
    device = UHCICoreStorageDevice(&_state, target, binding.generation);
    present = device != 0 && !_interruptState.stopping;
    bot = &_storageBOT[target];
    if (_storageDisks[target].generation != binding.generation) bot->unusable = 0;
    if (device) bot->interface = device->storageInterface;
    USBStorageSCSIObserve(&_storageDisks[target], binding.generation, binding.removals, present);
    [_eventLock unlock];
    stamp = profile ? storageNanoseconds() : 0;
    if (!USBStorageBufferMap(&pages, &clientBufferOps, (void *)client,
            page_size, buffer, maximum, client == IOVmTaskSelf())) {
        if (profile) mapNS = storageNanoseconds() - stamp;
        goto out;
    }
    if (profile) mapNS = storageNanoseconds() - stamp;
    session.controller = self; session.target = target; session.generation = binding.generation;
    bot->transport = &storageTransport; bot->context = &session;
    result = USBStorageSCSIExecute(&_storageDisks[target], bot, cdb, cdbLength,
        _storageBuffers[target], maximum, read, autoSense, deadline);
    [_eventLock lock];
    /* Even a command that finished before unplug may not publish its data to
     * a caller after the attachment generation has changed. */
    if (_state.storage[target].generation != binding.generation ||
        (present && (!UHCICoreStorageDevice(&_state, target, binding.generation) ||
                     _interruptState.stopping))) {
        result.status = USB_TRANSFER_DISCONNECTED; result.actual = 0;
    }
    stamp = profile ? storageNanoseconds() : 0;
    if (!result.status && result.actual <= maximum)
        USBStorageBufferCopy(&pages, _storageBuffers[target], (unsigned)result.actual);
    else if (result.actual > maximum) { result.status = USB_TRANSFER_ERROR; result.actual = 0; }
    if (profile) copyNS = storageNanoseconds() - stamp;
    if (bot->unusable) UHCICoreStorageOffline(&_state, target, binding.generation);
    [_eventLock unlock];
    bot->context = 0;
    stamp = profile ? storageNanoseconds() : 0;
    USBStorageBufferUnmap(&pages);
    if (profile) unmapNS = storageNanoseconds() - stamp;
out:
    [_eventLock lock];
    if (profile) {
        _storageMetrics[UHCI_STAT_READS]++;
        if (result.status) _storageMetrics[UHCI_STAT_ERRORS]++;
        else _storageMetrics[UHCI_STAT_BYTES] += result.actual;
        _storageMetrics[UHCI_STAT_REQUEST_NS] += storageNanoseconds() - started;
        _storageMetrics[UHCI_STAT_LOCK_NS] += locked - started;
        _storageMetrics[UHCI_STAT_MAP_NS] += mapNS;
        _storageMetrics[UHCI_STAT_CLIENT_COPY_NS] += copyNS;
        _storageMetrics[UHCI_STAT_UNMAP_NS] += unmapNS;
    }
    _storageProfileActive[target] = NO;
    _storageActiveRequests--;
    [_eventLock unlock];
    [_storageLocks[target] unlock];
    if (_state.fatal) [self containFatalController];
    return result;
}

- (sc_status_t)executeRequest:(IOSCSIRequest *)request buffer:(void *)buffer client:(vm_task_t)client
{
    USBStorageResult result;
    ns_time_t start, end;
    unsigned n;
    if (!request) return SR_IOST_INVALID;
    IOGetTimestamp(&start);
    bzero(&result, sizeof(result)); result.status = USB_STORAGE_INVALID;
    n = request->cdbLength ? request->cdbLength : USBStorageCDBLength(((uhci_u8 *)&request->cdb)[0]);
    if (request->target >= USB_STORAGE_TARGETS || request->lun) result.status = USB_TRANSFER_DISCONNECTED;
    else if (n <= sizeof(request->cdb) && request->maxTransfer >= 0)
        result = [self storageExecute:request->target cdb:(uhci_u8 *)&request->cdb length:n
            buffer:buffer client:client maximum:request->maxTransfer read:request->read
            autoSense:!request->ignoreChkcond deadline:UHCIPlatformMilliseconds() +
                (uhci_u64)(request->timeoutLength > 0 ? request->timeoutLength : 5) * 1000];
    request->driverStatus = scsiStatus(&result, &request->scsiStatus, &request->senseData,
                                       !request->ignoreChkcond);
    request->bytesTransferred = result.status == USB_TRANSFER_OK ? (int)result.actual : 0;
    IOGetTimestamp(&end); request->totalTime = end - start; request->latentTime = 0;
    return request->driverStatus;
}

- (sc_status_t)executeSCSI3Request:(IOSCSI3Request *)request buffer:(void *)buffer client:(vm_task_t)client
{
    USBStorageResult result;
    ns_time_t start, end;
    unsigned n;
    if (!request) return SR_IOST_INVALID;
    IOGetTimestamp(&start);
    bzero(&result, sizeof(result)); result.status = USB_STORAGE_INVALID;
    n = request->cdbLength ? request->cdbLength : USBStorageCDBLength(((uhci_u8 *)&request->cdb)[0]);
    if (request->target >= USB_STORAGE_TARGETS || request->lun) result.status = USB_TRANSFER_DISCONNECTED;
    else if (n <= sizeof(request->cdb) && request->maxTransfer >= 0)
        result = [self storageExecute:(unsigned)request->target cdb:(uhci_u8 *)&request->cdb length:n
            buffer:buffer client:client maximum:request->maxTransfer read:request->read
            autoSense:1 deadline:UHCIPlatformMilliseconds() +
                (uhci_u64)(request->timeoutLength > 0 ? request->timeoutLength : 5) * 1000];
    request->driverStatus = scsiStatus(&result, &request->scsiStatus, &request->senseData, 1);
    request->bytesTransferred = result.status == USB_TRANSFER_OK ? (int)result.actual : 0;
    IOGetTimestamp(&end); request->totalTime = end - start; request->latentTime = 0;
    return request->driverStatus;
}

- (sc_status_t)resetSCSIBus
{
    unsigned target;
    int failed = 0;
    uhci_u64 deadline = UHCIPlatformMilliseconds() + 5000;
    [_eventLock lock];
    _storageActiveRequests++;
    [_eventLock unlock];
    for (target = 0; target < USB_STORAGE_TARGETS; target++) {
        UHCIStorageSession session;
        UHCIDevice *device;
        USBMassStorage *bot = &_storageBOT[target];
        [_storageLocks[target] lock];
        [_eventLock lock];
        session.controller = self; session.target = target;
        session.generation = _state.storage[target].generation;
        device = UHCICoreStorageDevice(&_state, target, session.generation);
        if (device) bot->interface = device->storageInterface;
        [_eventLock unlock];
        if (device) {
            bot->transport = &storageTransport; bot->context = &session;
            if (USBMassStorageReset(bot, deadline)) {
                failed = 1;
                [_eventLock lock];
                UHCICoreStorageOffline(&_state, target, session.generation);
                [_eventLock unlock];
            }
            bot->context = 0;
            _storageDisks[target].capacityValid = 0;
            _storageDisks[target].attention = 1;
        }
        [_storageLocks[target] unlock];
    }
    [_eventLock lock];
    _storageActiveRequests--;
    [_eventLock unlock];
    return failed ? SR_IOST_INT : SR_IOST_GOOD;
}

- (IOReturn)getCharValues:(unsigned char *)values forParameter:(IOParameterName)parameter
                   count:(unsigned *)count
{
    char report[768]; unsigned n, i, targets = 0;
    if (strcmp(parameter, "UHCIRuntimeState"))
        return [super getCharValues:values forParameter:parameter count:count];
    [_eventLock lock]; [_boundaryLock lock];
    for (i=0;i<USB_STORAGE_TARGETS;i++) if (_state.storage[i].slot) targets++;
    sprintf(report, "UHCI %s %s io=%x ports=%u targets=%u running=%u fatal=%u reason=%s detail=%x transfers=%u errors=%u timeouts=%u callbacks=%u foreign=%u irqDebt=%u dmaDebt=%u rearmDebt=%u port1=%04x port2=%04x blocked=%x lastReject=%s rejectPort=%u rejectHubPort=%u\n",
        UHCI_VERSION, _interruptState.mode==UHCI_MODE_INTX ? "INTx" : "Polling",
        _state.ioBase, _state.maxPorts, targets, _state.running, _state.fatal,
        _state.faultReason ? _state.faultReason : "none", _state.faultDetail,
        _state.transferEvents, _state.errors, _state.timeouts, _interruptState.callbacks,
        _interruptState.foreign, _interruptState.interruptDebt, _interruptState.dmaDebt, _interruptState.rearmDebt,
        _state.rootPortStatus[0], _state.rootPortStatus[1], _state.blockedPorts,
        _state.enumerationFailure.stage ? _state.enumerationFailure.stage : "none",
        _state.enumerationFailure.rootPort, _state.enumerationFailure.hubPort);
    [_boundaryLock unlock]; [_eventLock unlock];
    n=strlen(report)+1;
    if (*count<n) { *count=n; return IO_R_INVALID_ARG; }
    bcopy(report,values,n); *count=n; return IO_R_SUCCESS;
}
@end
