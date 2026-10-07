#define MACH_USER_API 1
#import "UHCIController.h"
#import "UHCIVersion.h"
#import "UHCICompletionWait.h"
#import <driverkit/generalFuncs.h>
#import <driverkit/kernelDriver.h>
#import <driverkit/i386/kernelDriver.h>
#import <driverkit/i386/IOPCIDirectDevice.h>
#import <driverkit/i386/IOPCIDeviceDescription.h>
#import <driverkit/i386/PCI.h>
#import <driverkit/i386/ioPorts.h>
#import <kernserv/prototypes.h>
#import <mach/vm_param.h>
#import <string.h>
#import <stdio.h>

extern void thread_wakeup(int);
extern int hz;
static int pciRead(void *ctx, unsigned offset, uhci_u32 *v)
{
    unsigned long data;
    UHCIController *c = ctx;
    if ([IODirectDevice getPCIConfigData:&data atRegister:offset
        withDeviceDescription:c->_pciDescription] != IO_R_SUCCESS) return 0;
    *v = data; return 1;
}
static int pciWrite(void *ctx, unsigned offset, uhci_u32 v)
{
    UHCIController *c = ctx;
    return [IODirectDevice setPCIConfigData:v atRegister:offset
        withDeviceDescription:c->_pciDescription] == IO_R_SUCCESS;
}
static const UHCIPCIOps pciOps = {pciRead, pciWrite};
/* Raw functions may only be called inside _boundaryLock. They never acquire
 * the event lock or wait for hardware. Observation-time loss is latched by
 * the calling portable policy or PlatformRead before unlocking. */
static uhci_u32 rawRead(UHCIController *c, unsigned r)
{
    unsigned value;
    if (c->_interruptState.mmioLost || !c->_state.ioBase || r>=c->_state.ioBytes) return 0xffffffffU;
    if (r==UHCI_FLBASE) return inl(c->_state.ioBase+r);
    if (r==UHCI_SOF) return inb(c->_state.ioBase+r);
    if ((r&1) || r+2>c->_state.ioBytes) return 0xffffffffU;
    value=inw(c->_state.ioBase+r);
    return value==65535 ? 0xffffffffU : value;
}
static int rawWrite(UHCIController *c, unsigned r, uhci_u32 v)
{
    if (c->_interruptState.mmioLost || !c->_state.ioBase || r>=c->_state.ioBytes) return 0;
    if (r==UHCI_FLBASE) outl(c->_state.ioBase+r,v);
    else if (r==UHCI_SOF) outb(c->_state.ioBase+r,v);
    else {
        if ((r&1) || r+2>c->_state.ioBytes) return 0;
        outw(c->_state.ioBase+r,v);
    }
    return 1;
}
static uhci_u32 irqRead(void *ctx, unsigned r) { return rawRead(ctx,r); }
static int irqWrite(void *ctx, unsigned r, uhci_u32 v) { return rawWrite(ctx,r,v); }
static int irqGate(void *ctx, int closed)
{
    UHCIController *c=ctx;
    if (!c->_gateKind) {
        if (!closed || !UHCIPCISelectGate(&pciOps,c,c->_state.vendor,&c->_gateKind)) return 0;
    }
    return UHCIPCIGate(&pciOps,c,c->_gateKind,closed);
}
static int irqPCIRead(void *ctx, uhci_u32 *v) { return pciRead(ctx, 4, v); }
static int irqPCIWrite(void *ctx, uhci_u32 v) { return pciWrite(ctx, 4, v); }
static int irqRearm(void *ctx) { return [(UHCIController *)ctx enableAllInterrupts] == IO_R_SUCCESS; }
static void irqPublish(void *ctx)
{ thread_wakeup((int)&((UHCIController *)ctx)->_serviceEvent); }
static const UHCIInterruptOps interruptOps = {
    irqRead, irqWrite, irqPCIRead, irqPCIWrite, irqGate, irqRearm, irqPublish
};
static void completionLock(void *ctx)
{ UHCIController *c = ctx; [c->_eventLock lock]; [c->_boundaryLock lock]; }
static void completionUnlock(void *ctx)
{ UHCIController *c = ctx; [c->_boundaryLock unlock]; [c->_eventLock unlock]; }
static int completionReady(void *ctx)
{
    UHCIController *c = ctx;
    return UHCICompletionWorkReady(c->_startupReady, c->_interruptState.work,
        c->_interruptState.stopping, c->_state.fatal, c->_state.rescan,
        c->_state.scheduleBusy);
}
static void completionPrepare(void *ctx)
{
    UHCIController *c = ctx;
    assert_wait((int)&c->_serviceEvent, FALSE);
    thread_set_timeout(1);
}
static void completionBlock(void *ctx) { (void)ctx; thread_block(); }
static const UHCICompletionWaitOps completionWaitOps = {
    completionLock, completionUnlock, completionReady, completionPrepare, completionBlock
};
static void completionThread(void *ctx) { [(UHCIController *)ctx runCompletionLoop]; IOExitThread(); }
static void managementThread(void *ctx) { [(UHCIController *)ctx runManagementLoop]; IOExitThread(); }
static void retryThread(void *ctx) { [(UHCIController *)ctx runRetryLoop]; IOExitThread(); }
@implementation UHCIController
- (IOReturn)startIOThread
{
    /* IOSCSIController initializes resources before invoking this hook.
     * Delay IRQ publication until exclusive ownership and PCI fencing. */
    if (_deferIOThread) return IO_R_SUCCESS;
    return [super startIOThread];
}
+ (BOOL)probe:description
{
    UHCIController *c = [[self alloc] initFromDeviceDescription:description];
    if (!c) return NO;
    /* OPENSTEP frees the description, delegate and kernel device after a
     * rejected probe. An owned, pinned failure must retain that loader claim
     * so its containment worker and possible IRQ callbacks keep valid PCI
     * and interrupt providers. It publishes no healthy storage controller. */
    if (c->_initializationFailed) return YES;
    if (![c startStorageProbeWorker] || ![c registerDevice]) {
        [c failInitializationAt:__LINE__]; return YES;
    }
    [c finishStorageRegistration];
    return YES;
}
- initFromDeviceDescription:description
{
    IOPCIConfigSpace pci;
    IORange range;
    unsigned address, bytes, i, mode;
    uhci_u32 routing;
    const char *value;
    IOConfigTable *table = [description configTable];
    /* IOSCSIController free decrements its counter when unit == counter - 1.
     * With no controllers, ~0U matches -1 and corrupts that counter even
     * though this rejected probe never called superclass initialization. */
    [self setUnit:0x7fffffffU];
    _pciDescription = description;
    _state.owner = self;
    _interruptState.ops = &interruptOps; _interruptState.context = self;
    _boundaryLock = [[NXLock alloc] init]; _eventLock = [[NXLock alloc] init];
    if (!_boundaryLock || !_eventLock) return [self failInitializationAt:__LINE__];
    bzero(&pci, sizeof(pci));
    if ([IODirectDevice getPCIConfigSpace:&pci withDeviceDescription:description] != IO_R_SUCCESS ||
        !UHCICoreSupportsPCIClass(pci.ClassCode)) return [self failInitializationAt:__LINE__];
    _state.vendor=pci.VendorID;
    value = [table valueForStringKey:"Interrupt Mode"];
    mode = UHCIInterruptModeParse(value);
    if (value) [table freeString:value];
    value = [table valueForStringKey:"Polling Only"];
    if (value) { [table freeString:value]; mode = UHCI_MODE_INVALID; }
    if (!mode) { IOLog("UHCI: Interrupt Mode must be INTx or Polling\n"); return [self failInitializationAt:__LINE__]; }
    _interruptState.mode = mode;
    if (mode == UHCI_MODE_INTX) {
        value = [table valueForStringKey:"Share IRQ Levels"];
        i = value && (!strcmp(value, "Yes") || !strcmp(value, "YES"));
        if (value) [table freeString:value];
        if (!i || !pciRead(self, 0x3c, &routing) || !(routing & 0xff00) ||
            ((routing >> 8) & 255) > 4 || !(routing & 255) || (routing & 255) >= 16 || (routing & 255) == 2)
            return [self failInitializationAt:__LINE__];
        _irq = routing & 255;
        if ([description setInterruptList:&_irq num:1] != IO_R_SUCCESS) return [self failInitializationAt:__LINE__];
    } else if ([description setInterruptList:0 num:0] != IO_R_SUCCESS) return [self failInitializationAt:__LINE__];
    if (!UHCIPCIBar(&pciOps, self, &address, &bytes)) return [self failInitializationAt:__LINE__];
    range.start=address; range.size=bytes;
    if ([description setPortRangeList:&range num:1]!=IO_R_SUCCESS) return [self failInitializationAt:__LINE__];
    _state.ioBase=address; _state.ioBytes=bytes;
    /* Provision the failure/retry owner before superclass initialization,
     * whose failure path calls [self free]. Until resourcesOwned is set,
     * this worker cannot read or change PCI/controller state. */
    if (!IOForkThread(retryThread, self)) return [self failInitializationAt:__LINE__];
    _workersStarted = 1;
    _deferIOThread = YES;
    if (![super initFromDeviceDescription:description]) return [self failInitializationAt:__LINE__];
    /* DriverKit now owns the I/O range and IRQ reservation. A conflict has
     * already failed without any hardware write or IRQ-task publication. */
    [_boundaryLock lock];
    _pciOwned = _interruptState.resourcesOwned = 1;
    i = UHCIPCISelectGate(&pciOps,self,pci.VendorID,&_gateKind) &&
        UHCIPCIDisableMessages(&pciOps,self) &&
        UHCIPCICommand(&pciOps,self,UHCI_PCI_IO,UHCI_PCI_MASTER);
    if (i) _interruptState.fenced = _interruptState.dmaFenced = 1;
    [_boundaryLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    /* Even a partial real start is pinned. Early callbacks can return their
     * shared IRQ vote under the already-verified whole-function fence. */
    _interruptState.participant=mode==UHCI_MODE_INTX;
    _deferIOThread = NO; _scsiThreadStarted = YES;
    if ([super startIOThread] != IO_R_SUCCESS) return [self failInitializationAt:__LINE__];
    {
        char name[24]; id existing; unsigned unit = [self unit];
        /* A previously rejected controller can leave DriverKit's shared
         * counter negative. Recover the registered namespace from sc0. */
        if (unit & 0x80000000U) unit = 0;
        do { sprintf(name, "sc%u", unit++); }
        while (IOGetObjectForDeviceName(name, &existing) == IO_R_SUCCESS);
        [self setUnit:unit - 1]; [self setName:name];
    }
    if (mode == UHCI_MODE_INTX) {
        unsigned *list = [[self deviceDescription] interruptList];
        if ([[self deviceDescription] numInterrupts] != 1 || !list || list[0] != _irq ||
            [self interruptPort] == PORT_NULL) return [self failInitializationAt:__LINE__];
    } else if ([[self deviceDescription] numInterrupts]) return [self failInitializationAt:__LINE__];
    {
        unsigned char dev, func, bus;
        IOLog("UHCI %s: %s %04x:%04x I/O %x, 2 ports, %s%s\n", UHCI_VERSION,
            [self name], pci.VendorID, pci.DeviceID, address,
            mode == UHCI_MODE_INTX ? "INTx" : "Polling", mode == UHCI_MODE_INTX ? " shared IRQ" : " 5 ms");
        if ([description getPCIdevice:&dev function:&func bus:&bus] == IO_R_SUCCESS)
            IOLog("UHCI: %s PCI bus %u device %u function %u\n", [self name], bus, dev, func);
    }
    for (i = 0; i < USB_STORAGE_TARGETS; i++) {
        _storageLocks[i] = [[NXLock alloc] init];
        _storageBuffers[i] = IOMalloc(USB_STORAGE_MAX_REQUEST);
        if (!_storageLocks[i] || !_storageBuffers[i]) return [self failInitializationAt:__LINE__];
    }
    [_eventLock lock];
    i = UHCICoreInitialize(&_state);
    [_eventLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    if (!IOForkThread(completionThread, self)) return [self failInitializationAt:__LINE__];
    [_boundaryLock lock];
    i = UHCIPCICommand(&pciOps, self, UHCI_PCI_MASTER | UHCI_PCI_IO, 0);
    if (i) _interruptState.dmaFenced = 0;
    [_boundaryLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    [_eventLock lock]; i = UHCICoreStart(&_state); [_eventLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    [_boundaryLock lock];
    if (mode == UHCI_MODE_INTX) {
        _interruptState.participant = 1; _interruptState.rearmDebt = 1;
        /* Pending port status may intentionally defer opening to the worker. */
        UHCIInterruptRestore(&_interruptState);
    }
    _startupReady = 1;
    [_boundaryLock unlock];
    irqPublish(self);
    [_eventLock lock]; i = UHCICoreServicePorts(&_state); [_eventLock unlock];
    if (!i || _interruptState.stopping) return [self failInitializationAt:__LINE__];
    {
        unsigned targets = 0;
        [_eventLock lock];
        for (i = 0; i < USB_STORAGE_TARGETS; i++)
            if (_state.storage[i].slot) targets++;
        [_eventLock unlock];
        IOLog("UHCI: %s initial scan: %u storage targets\n",
            [self name], targets);
    }
    if (!IOForkThread(managementThread,self)) return [self failInitializationAt:__LINE__];
    return self;
}
- failInitialization
{ return [self failInitializationAt:0]; }
- failInitializationAt:(unsigned)line
{
    IOLog("UHCI: initialization failed at line %u; I/O %x/%u fatal %u lost %u\n",
        line,_state.ioBase,_state.ioBytes,_state.fatal,_interruptState.mmioLost);
    if (_pciOwned) {
        _initializationFailed = YES;
        [self free];
        IOLog("UHCI: failed controller claimed offline until reboot\n");
        return self;
    }
    [self free]; return nil;
}
- (void)interruptOccurred
{
    /* x86 locked admission precedes any blocking lock acquisition. Pinned
     * lifetime keeps both this token and queued callbacks valid until reboot. */
    __asm__ volatile("lock; incl (%0)" : : "r" (&_interruptState.admitted) : "memory", "cc");
    [_boundaryLock lock];
    UHCIInterruptCallback(&_interruptState);
    __asm__ volatile("lock; decl (%0)" : : "r" (&_interruptState.admitted) : "memory", "cc");
    [_boundaryLock unlock];
}
- (void)runRetryLoop
{
    for (;;) {
        [_boundaryLock lock];
        UHCIInterruptRetry(&_interruptState);
        /* Watchdog detects a stuck asserted source, never services transfers
         * or calls clients. INTx failure goes offline, never silently polls. */
        if (_startupReady)
            UHCIInterruptWatchdog(&_interruptState, UHCIPlatformMilliseconds());
        [_boundaryLock unlock];
        IOSleep(5);
    }
}
- (void)runCompletionLoop
{
    for (;;) {
        unsigned causes = 0, work = 0, fatal = 0;
        [_boundaryLock lock];
        if (_startupReady && !_interruptState.stopping) {
            if (_interruptState.mode == UHCI_MODE_POLLING) UHCIInterruptPoll(&_interruptState);
            work = _interruptState.work;
            if (work) causes = UHCIInterruptTakeWork(&_interruptState);
        }
        [_boundaryLock unlock];
        if (_startupReady) {
            [_eventLock lock];
            if (_interruptState.stopping) {
                UHCICoreFail(&_state, "interrupt stopped", _interruptState.mmioLost);
                [self wakeStorageWaiters];
            } else UHCICoreService(&_state, causes);
            fatal = _state.fatal;
            if (fatal) [self wakeStorageWaiters];
            [_eventLock unlock];
            if (fatal) { [self containFatalController]; return; }
            [_boundaryLock lock];
            if (!_interruptState.stopping && !_interruptState.active)
                UHCIInterruptRestore(&_interruptState);
            [_boundaryLock unlock];
        }
        /* Register under both publication locks before blocking. A wake
         * after unlock cancels the registered wait, including before block.
         * The timeout keeps startup/retry progress; idle INTx does not poll. */
        if (_interruptState.mode == UHCI_MODE_POLLING) IOSleep(5);
        else UHCICompletionWait(self, &completionWaitOps);
    }
}
- (void)runManagementLoop
{
    for (;;) {
        unsigned fatal, stopping;
        [_eventLock lock];
        if (!_interruptState.stopping) {
            UHCICoreServicePorts(&_state);
        }
        fatal = _state.fatal; stopping = _interruptState.stopping;
        [_eventLock unlock];
        if (fatal) [self containFatalController];
        if (fatal || stopping) return;
        IOSleep(100);
    }
}
- (void)containFatalController
{
    [_boundaryLock lock];
    if (!_interruptState.stopping) UHCIInterruptStop(&_interruptState);
    else UHCIInterruptRetry(&_interruptState);
    [_boundaryLock unlock];
    [_eventLock lock];
    UHCICoreFail(&_state, "controller containment", _interruptState.mmioLost);
    [self wakeStorageWaiters];
    if (_contained) { [_eventLock unlock]; return; }
    _contained = 1;
    [_eventLock unlock];
}
- free
{
    unsigned i;
    if (_boundaryLock && _pciOwned) {
        [_boundaryLock lock];
        UHCIInterruptStop(&_interruptState);
        [_boundaryLock unlock];
    }
    /* IOSCSIController, its I/O task, native providers and queued callbacks
     * have no usable final-drain API in this target. Never release their
     * object, BAR, locks or DMA after superclass publication. */
    if (_scsiThreadStarted || _workersStarted || _interruptState.interruptDebt || _interruptState.dmaDebt) {
        IOLog("UHCI: offline resources and retry worker retained until reboot\n");
        return self;
    }
    for (i = 0; i < USB_STORAGE_TARGETS; i++) {
        [_storageLocks[i] free];
        if (_storageBuffers[i]) IOFree(_storageBuffers[i], USB_STORAGE_MAX_REQUEST);
    }
    [_eventLock free]; [_boundaryLock free];
    return [super free];
}
@end

uhci_u32 UHCIPlatformRead(UHCIControllerState *s, uhci_u32 offset)
{
    UHCIController *c = s->owner; uhci_u32 v;
    [c->_boundaryLock lock];
    v = c->_interruptState.stopping ? 0xffffffffU : rawRead(c, offset);
    if (v == 0xffffffffU && !c->_interruptState.mmioLost && !c->_interruptState.stopping)
        UHCIInterruptLost(&c->_interruptState);
    [c->_boundaryLock unlock]; return v;
}
int UHCIPlatformWrite(UHCIControllerState *s, uhci_u32 offset, uhci_u32 v)
{
    UHCIController *c = s->owner; int ok;
    [c->_boundaryLock lock];
    ok = !c->_interruptState.stopping && rawWrite(c, offset, v);
    [c->_boundaryLock unlock]; return ok;
}
int UHCIPlatformPCIRead(UHCIControllerState *s, unsigned offset, uhci_u32 *v)
{ UHCIController *c = s->owner; int ok; [c->_boundaryLock lock]; ok = pciRead(c, offset, v); [c->_boundaryLock unlock]; return ok; }
int UHCIPlatformPCIWrite(UHCIControllerState *s, unsigned offset, uhci_u32 v)
{ UHCIController *c = s->owner; int ok; [c->_boundaryLock lock]; ok = pciWrite(c, offset, v); [c->_boundaryLock unlock]; return ok; }
void *UHCIPlatformAllocate(uhci_u32 bytes) { return IOMalloc(bytes); }
void UHCIPlatformFree(void *p, uhci_u32 bytes) { IOFree(p, bytes); }
uhci_u32 UHCIPlatformPhysical(void *p)
{
    vm_address_t address = 0;
    if (IOPhysicalFromVirtual(IOVmTaskSelf(), (vm_address_t)p, &address) != IO_R_SUCCESS) return 0;
    return address;
}
uhci_u64 UHCIPlatformMilliseconds(void)
{ ns_time_t now; IOGetTimestamp(&now); return (uhci_u64)now / 1000000ULL; }
void UHCIPlatformPause(UHCIControllerState *s, unsigned ms)
{ UHCIController *c = s->owner; [c->_eventLock unlock]; IOSleep(ms); [c->_eventLock lock]; }
void UHCIPlatformDelay(UHCIControllerState *s, unsigned us)
{ (void)s; IODelay(us); }
void UHCIPlatformWake(void *c) { irqPublish(c); }
void UHCIPlatformStorageWake(void *c, unsigned t) { [(UHCIController *)c wakeStorageTarget:t]; }
void UHCIPlatformStorageAttached(void *c) { [(UHCIController *)c queueStorageProbe]; }
