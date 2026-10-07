/* UHCI 1.1 controller engine. BSD-2-Clause.
 * State lock required. PlatformPause releases it; no hardware-owned object
 * is edited or reclaimed until its queue has passed the retirement barrier. */
#include "UHCICore.h"
#include "UHCIMemory.h"

static void barrier(void) { __asm__ volatile("" : : : "memory"); }
void UHCICoreFail(UHCIControllerState *c, const char *why, unsigned detail)
{
    if (!c->faultReason) {
        c->faultReason=why; c->faultDetail=detail;
        UHCIPlatformLog("UHCI: offline: %s (%x) cmd=%x sts=%x\n",why,detail,c->lastCommand,c->lastStatus);
    }
    c->fatal=1;
}
static uhci_u32 read_op(UHCIControllerState *c, unsigned r)
{
    uhci_u32 v;
    if (c->fatal) return 0xffffffffU;
    v=UHCIPlatformRead(c,r);
    if (r==UHCI_PORT(1) || r==UHCI_PORT(2))
        c->rootPortStatus[(r-UHCI_PORT(1))/2]=v;
    if (v!=0xffffffffU && c->vendor==0x1106 && r>=UHCI_PORT(1) && r<=UHCI_PORT(2))
        v^=UHCI_PORT_OC;
    if (r==UHCI_CMD) c->lastCommand=v;
    if (r==UHCI_STS) c->lastStatus=v;
    if (v==0xffffffffU) UHCICoreFail(c,"I/O read",r);
    return v;
}
static int write_op(UHCIControllerState *c, unsigned r, uhci_u32 v)
{
    if (c->fatal) return 0;
    if (!UHCIPlatformWrite(c,r,v)) { UHCICoreFail(c,"I/O write",r); return 0; }
    if (r==UHCI_CMD) c->lastCommand=v;
    return 1;
}
static int wait_bits(UHCIControllerState *c, unsigned r, unsigned mask, unsigned value, unsigned timeout)
{
    uhci_u64 end=UHCIPlatformMilliseconds()+timeout;
    for (;;) {
        unsigned v=read_op(c,r);
        if (c->fatal) return 0;
        if ((v&mask)==value) return 1;
        if (UHCIPlatformMilliseconds()>=end) {
            c->waitOffset=r; c->waitMask=mask; c->waitExpected=value; c->waitObserved=v;
            c->timeouts++; return 0;
        }
        UHCIPlatformPause(c,1);
    }
}
static int dma_alloc(UHCIDMA *d, unsigned bytes)
{
    unsigned long aligned;
    if (!bytes || bytes>UHCI_PAGE_SIZE) return 0;
    bzero(d,sizeof(*d)); d->allocationBytes=bytes+UHCI_PAGE_SIZE;
    d->allocation=UHCIPlatformAllocate(d->allocationBytes);
    if (!d->allocation) return 0;
    aligned=((unsigned long)d->allocation+UHCI_PAGE_SIZE-1)&~(unsigned long)(UHCI_PAGE_SIZE-1);
    d->virtualAddress=(void *)aligned; d->bytes=bytes;
    d->physicalAddress=UHCIPlatformPhysical(d->virtualAddress);
    if (!d->physicalAddress || (d->physicalAddress&(UHCI_PAGE_SIZE-1)) ||
        d->physicalAddress>0xffffffffU-(bytes-1) ||
        UHCIPlatformPhysical((uhci_u8 *)d->virtualAddress+bytes-1)!=d->physicalAddress+bytes-1) {
        UHCIPlatformFree(d->allocation,d->allocationBytes); bzero(d,sizeof(*d)); return 0;
    }
    bzero(d->virtualAddress,bytes); return 1;
}
static void dma_free(UHCIDMA *d)
{
    if (d->allocation) UHCIPlatformFree(d->allocation,d->allocationBytes);
    bzero(d,sizeof(*d));
}
static void endpoint_free(UHCIEndpoint *e)
{
    unsigned i;
    for (i=0;i<UHCI_DATA_PAGES;i++) dma_free(&e->data[i]);
    for (i=0;i<UHCI_DESC_PAGES;i++) dma_free(&e->tdPages[i]);
    dma_free(&e->descriptors); bzero(e,sizeof(*e));
}
int UHCICoreSupportsPCIClass(uhci_u32 code) { return code==0x0c0300; }
uhci_u32 UHCIPortWriteValue(uhci_u32 v, uhci_u32 set, uhci_u32 clear, uhci_u32 changes)
{ return (((v&UHCI_PORT_WRITABLE)|set)&~clear)|(changes&UHCI_PORT_CHANGES); }

int UHCICoreInitialize(UHCIControllerState *c)
{
    unsigned i;
    UHCIQH *heads;
    c->registersValid=1; c->maxPorts=2; c->addresses[0]=1;
    if (!write_op(c,UHCI_INTR,0) || !write_op(c,UHCI_CMD,0) ||
        !wait_bits(c,UHCI_STS,UHCI_STS_HALTED,UHCI_STS_HALTED,100)) return 0;
    c->haltConfirmed=1;
    if (!write_op(c,UHCI_CMD,UHCI_CMD_RESET) || !wait_bits(c,UHCI_CMD,UHCI_CMD_RESET,0,100)) return 0;
    /* HCRESET leaves the root-port Suspend, Reset and Resume Detect bits
     * intact. Firmware may leave any of them set, even on a boot device.
     * Disable both ports and clear that state before scanning either one;
     * writing zero preserves sticky connection changes for the first scan. */
    for (i=1;i<=c->maxPorts;i++) {
        unsigned v=read_op(c,UHCI_PORT(i));
        if (c->fatal) return 0;
        if (v&UHCI_PORT_WRITABLE)
            UHCIPlatformLog("UHCI: I/O %x root port %u clearing firmware state %04x\n",
                c->ioBase,i,c->rootPortStatus[i-1]);
        if (!write_op(c,UHCI_PORT(i),0)) return 0;
    }
    if (!dma_alloc(&c->frameList,UHCI_PAGE_SIZE) || !dma_alloc(&c->asyncHead,UHCI_PAGE_SIZE)) return 0;
    heads=c->asyncHead.virtualAddress;
    heads[0].link=(c->asyncHead.physicalAddress+16)|UHCI_LINK_QH;
    heads[0].element=UHCI_LINK_END; heads[1].link=heads[1].element=UHCI_LINK_END;
    for (i=0;i<UHCI_FRAME_COUNT;i++) ((uhci_u32 *)c->frameList.virtualAddress)[i]=c->asyncHead.physicalAddress|UHCI_LINK_QH;
    barrier();
    if (!write_op(c,UHCI_FRNUM,0) || !write_op(c,UHCI_FLBASE,c->frameList.physicalAddress) ||
        read_op(c,UHCI_FLBASE)!=c->frameList.physicalAddress || !write_op(c,UHCI_SOF,64) ||
        !write_op(c,UHCI_STS,UHCI_STS_W1C)) return 0;
    c->initialized=1; return 1;
}
int UHCICoreStart(UHCIControllerState *c)
{
    if (!c->initialized || c->fatal) return 0;
    c->dmaArmed=1; c->haltConfirmed=0;
    if (!write_op(c,UHCI_CMD,UHCI_CMD_RUN|UHCI_CMD_CONFIG|UHCI_CMD_MAX64) ||
        !wait_bits(c,UHCI_STS,UHCI_STS_HALTED,0,100)) return 0;
    c->running=1; return 1;
}
static UHCITD *td_at(UHCIEndpoint *e, unsigned n)
{ return &((UHCITD *)e->tdPages[n/256].virtualAddress)[n%256]; }
static uhci_u32 td_phys(UHCIEndpoint *e, unsigned n)
{ return e->tdPages[n/256].physicalAddress+(n%256)*sizeof(UHCITD); }

static void link_endpoint(UHCIControllerState *c, UHCIEndpoint *e)
{
    UHCIEndpoint **head=e->control ? &c->controlHead : &c->bulkHead, *tail=*head;
    UHCIQH *anchor=&((UHCIQH *)c->asyncHead.virtualAddress)[e->control ? 0 : 1];
    e->next=0;
    e->qh->link=e->control ? (c->asyncHead.physicalAddress+16)|UHCI_LINK_QH : UHCI_LINK_END;
    barrier();
    if (tail) {
        while (tail->next) tail=tail->next;
        tail->next=e; tail->qh->link=e->qhPhysical|UHCI_LINK_QH;
    } else { *head=e; anchor->link=e->qhPhysical|UHCI_LINK_QH; }
    e->linked=1; c->scheduleChanges++;
}
static void unlink_endpoint(UHCIControllerState *c, UHCIEndpoint *e)
{
    UHCIEndpoint **head=e->control ? &c->controlHead : &c->bulkHead, *prev=0, *p=*head;
    UHCIQH *anchor=&((UHCIQH *)c->asyncHead.virtualAddress)[e->control ? 0 : 1];
    if (!e->linked) return;
    while (p && p!=e) { prev=p; p=p->next; }
    if (!p) { UHCICoreFail(c,"queue membership",e->qhPhysical); return; }
    if (prev) { prev->next=e->next; prev->qh->link=e->qh->link; }
    else { *head=e->next; anchor->link=e->qh->link; }
    barrier(); e->linked=0; e->next=0; c->scheduleChanges++;
}
static void retire(UHCIControllerState *c, UHCIEndpoint *e, int result, int status)
{
    if (e->retiring) { if (!status) e->resumeStatus=0; e->retireResult=result; return; }
    unlink_endpoint(c,e);
    e->retiring=1; e->retireResult=result; e->resumeStatus=status;
    e->retireFrame=read_op(c,UHCI_FRNUM)&2047; e->retireAdvances=0;
    e->retireDeadline=UHCIPlatformMilliseconds()+100;
}
static int retirement(UHCIControllerState *c, UHCIEndpoint *e)
{
    unsigned frame=read_op(c,UHCI_FRNUM)&2047, delta=(frame-e->retireFrame)&2047;
    if (c->fatal) return USB_TRANSFER_ERROR;
    /* Only observed controller frame advancement is evidence; wall time alone
     * never proves that a fetched queue pointer has been abandoned. */
    e->retireAdvances+=delta; e->retireFrame=frame;
    if (e->retireAdvances<2) {
        if (UHCIPlatformMilliseconds()>=e->retireDeadline) UHCICoreFail(c,"retirement stalled",frame);
        return USB_TRANSFER_PENDING;
    }
    e->retiring=0;
    if (e->resumeStatus) {
        e->resumeStatus=0; e->statusPhase=1; e->scan=e->statusTD;
        e->qh->element=td_phys(e,e->statusTD); barrier(); link_endpoint(c,e);
        return USB_TRANSFER_PENDING;
    }
    e->waiting=0; e->result=e->retireResult;
    if (!e->control) e->toggle=e->startToggle^(e->successful&1);
    if (e->result) { e->halted=1; c->errors++; }
    c->transferEvents++;
    return e->result;
}
int UHCICoreConfigureEndpoint(UHCIControllerState *c, UHCIDevice *d, unsigned index,
                             unsigned address, unsigned packet, unsigned interval)
{
    UHCIEndpoint *e;
    (void)interval;
    if (c->fatal || index>=UHCI_ENDPOINTS || d->speed!=UHCI_SPEED_FULL ||
        (packet!=8 && packet!=16 && packet!=32 && packet!=64)) return 0;
    e=&d->endpoints[index];
    if (e->configured || !dma_alloc(&e->descriptors,UHCI_PAGE_SIZE)) return 0;
    e->qh=e->descriptors.virtualAddress; e->qhPhysical=e->descriptors.physicalAddress;
    e->address=address; e->maxPacket=packet; e->configured=1; e->control=index==0;
    e->qh->link=e->qh->element=UHCI_LINK_END; return 1;
}
static void fill_td(UHCIEndpoint *e, unsigned n, unsigned device, unsigned pid,
                    unsigned toggle, unsigned length, unsigned physical)
{
    UHCITD *td=td_at(e,n);
    td->link=n+1<e->count ? td_phys(e,n+1)|UHCI_LINK_DEPTH : UHCI_LINK_END;
    td->status=UHCI_TD_ACTIVE|UHCI_TD_RETRIES|0x7ffU;
    if (pid==UHCI_PID_IN) td->status|=UHCI_TD_SPD;
    if (n+1==e->count) td->status|=UHCI_TD_IOC;
    td->token=pid|(device<<8)|((e->address&15)<<15)|(toggle<<19)|(((length-1)&2047)<<21);
    td->buffer=physical;
}
static int submit_transfer(UHCIControllerState *c, UHCIDevice *d, UHCIEndpoint *e,
    const USBSetupPacket *setup, const void *buffer, unsigned length, uhci_u64 deadline, int owned)
{
    unsigned i, packets, n=0, pos=0, toggle;
    (void)owned;
    if (c->fatal || !c->running || !d->used || d->disconnecting) return USB_TRANSFER_DISCONNECTED;
    if (!e->configured || e->waiting || e->retiring || e->linked || length>USB_STORAGE_MAX_TRANSFER ||
        (!!setup)!=e->control || (setup && setup->length!=length)) return USB_STORAGE_INVALID;
    if (e->halted && !setup) return USB_TRANSFER_STALL;
    if (UHCIPlatformMilliseconds()>=deadline) return USB_TRANSFER_TIMEOUT;
    packets=(length+e->maxPacket-1)/e->maxPacket;
    if (!setup && !packets) packets=1;
    e->count=packets+(setup ? 2 : 0);
    if (e->count>UHCI_TD_COUNT) return USB_STORAGE_INVALID;
    for (i=0;i<(e->count+255)/256;i++)
        if (!e->tdPages[i].allocation && !dma_alloc(&e->tdPages[i],UHCI_PAGE_SIZE)) return USB_TRANSFER_ERROR;
    for (i=0;i<(length+4095)/4096;i++)
        if (!e->data[i].allocation && !dma_alloc(&e->data[i],UHCI_PAGE_SIZE)) return USB_TRANSFER_ERROR;
    e->input=setup ? (setup->requestType&0x80)!=0 : (e->address&0x80)!=0;
    e->total=length; e->actual=e->scan=e->successful=e->statusPhase=0;
    e->startToggle=e->toggle; e->deadline=deadline; e->halted=0;
    if (!e->input && length && !buffer) return USB_STORAGE_INVALID;
    if (!e->input) for (i=0;i<length;) {
        unsigned bytes=length-i; if (bytes>4096) bytes=4096;
        bcopy((const uhci_u8 *)buffer+i,e->data[i/4096].virtualAddress,bytes); i+=bytes;
    }
    if (setup) {
        bcopy(setup,(uhci_u8 *)e->descriptors.virtualAddress+16,8);
        fill_td(e,n++,d->address,UHCI_PID_SETUP,0,8,e->qhPhysical+16);
    }
    toggle=setup ? 1 : e->toggle;
    for (i=0;i<packets;i++) {
        unsigned bytes=length-pos, physical=0;
        if (bytes>e->maxPacket) bytes=e->maxPacket;
        if (bytes) physical=e->data[pos/4096].physicalAddress+(pos%4096);
        fill_td(e,n++,d->address,e->input ? UHCI_PID_IN : UHCI_PID_OUT,toggle,bytes,physical);
        toggle^=1; pos+=bytes;
    }
    if (setup) {
        e->statusTD=n;
        fill_td(e,n,d->address,(!length || !e->input) ? UHCI_PID_IN : UHCI_PID_OUT,1,0,0);
        /* SPD only on data IN; zero-byte status is not short. */
        td_at(e,n)->status&=~UHCI_TD_SPD;
    }
    e->qh->element=td_phys(e,0); e->waiting=1; e->result=USB_TRANSFER_PENDING;
    barrier(); link_endpoint(c,e); UHCIPlatformWake(c->owner);
    return USB_TRANSFER_OK;
}
int UHCICoreSubmit(UHCIControllerState *c, UHCIDevice *d, UHCIEndpoint *e,
    const USBSetupPacket *s, const void *b, unsigned n, uhci_u64 end)
{ return submit_transfer(c,d,e,s,b,n,end,0); }
int UHCICoreFinish(UHCIControllerState *c, UHCIDevice *d, UHCIEndpoint *e)
{
    (void)d;
    if (c->fatal) return USB_TRANSFER_ERROR;
    if (!e->waiting) return e->result;
    if (e->retiring) return retirement(c,e);
    while (e->scan<e->count) {
        UHCITD *td=td_at(e,e->scan);
        unsigned status=td->status, actual=(status+1)&2047;
        unsigned wanted=((td->token>>21)+1)&2047;
        int data=!e->control || (e->scan>0 && e->scan<e->statusTD);
        barrier();
        if (status&UHCI_TD_ACTIVE) break;
        if (status&UHCI_TD_ERRORS) {
            /* UHCI sets Stalled for terminal bus/DMA errors too. Only an
             * otherwise clean STALL denotes the device's STALL handshake;
             * BOT must reset-recover from CRC/babble/buffer failures. */
            retire(c,e,(status&(UHCI_TD_ERRORS&~UHCI_TD_STALL)) ?
                USB_TRANSFER_ERROR : USB_TRANSFER_STALL,0);
            return USB_TRANSFER_PENDING;
        }
        if (actual>wanted || (( !data || !e->input) && actual!=wanted)) {
            retire(c,e,USB_TRANSFER_ERROR,0); return USB_TRANSFER_PENDING;
        }
        if (data) { e->actual+=actual; e->successful++; }
        e->scan++;
        if (data && e->input && actual<wanted) {
            retire(c,e,USB_TRANSFER_OK,e->control); return USB_TRANSFER_PENDING;
        }
    }
    if (e->scan==e->count) retire(c,e,USB_TRANSFER_OK,0);
    else if (UHCIPlatformMilliseconds()>=e->deadline) {
        c->timeouts++; retire(c,e,USB_TRANSFER_TIMEOUT,0);
    }
    return USB_TRANSFER_PENDING;
}
int UHCICoreCancel(UHCIControllerState *c, UHCIDevice *d, UHCIEndpoint *e)
{
    unsigned generation=d->generation;
    if (c->fatal) return 0;
    if (e->linked || e->retiring) {
        retire(c,e,USB_TRANSFER_TIMEOUT,0);
        while (e->retiring && !c->fatal) {
            retirement(c,e);
            if (e->retiring) UHCIPlatformPause(c,1);
            if (d->generation!=generation) return 0;
        }
    }
    e->waiting=0; return !c->fatal;
}
static void copy_input(UHCIEndpoint *e, void *buffer)
{
    unsigned pos=0;
    if (!buffer) return;
    while (pos<e->actual) {
        unsigned n=e->actual-pos; if (n>4096) n=4096;
        bcopy(e->data[pos/4096].virtualAddress,(uhci_u8 *)buffer+pos,n); pos+=n;
    }
}
static int control_transfer(void *host, void *device, const USBSetupPacket *setup, void *buffer, uhci_u16 *actual)
{
    UHCIControllerState *c=host; UHCIDevice *d=device; UHCIEndpoint *e=&d->endpoints[0];
    unsigned generation=d->generation;
    uhci_u64 end=UHCIPlatformMilliseconds()+UHCI_TIMEOUT_MS;
    int ok=0, submitted;
    if (actual) *actual=0;
    while (d->controlBusy && !c->fatal && d->used && !d->disconnecting && d->generation==generation) {
        if (UHCIPlatformMilliseconds()>=end) return 0;
        UHCIPlatformPause(c,1);
    }
    if (c->fatal || !d->used || d->disconnecting || d->generation!=generation) return 0;
    d->controlBusy=1;
    if (e->waiting && !UHCICoreCancel(c,d,e)) goto done;
    submitted=submit_transfer(c,d,e,setup,buffer,setup->length,end,1);
    if (submitted) { e->result=submitted; goto done; }
    while (e->waiting && !c->fatal && d->used && !d->disconnecting && d->generation==generation) {
        UHCICoreFinish(c,d,e);
        if (e->waiting) UHCIPlatformPause(c,1);
    }
    if (!c->fatal && d->used && !d->disconnecting && d->generation==generation && !e->result) {
        if (e->input) copy_input(e,buffer);
        if (actual) *actual=e->actual;
        ok=1;
    }
done:
    if (d->generation==generation) d->controlBusy=0;
    return ok;
}
static int update_ep0(void *host, void *device, uhci_u8 packet)
{
    UHCIDevice *d=device; (void)host;
    if (packet!=8 && packet!=16 && packet!=32 && packet!=64) return 0;
    d->endpoints[0].maxPacket=packet; return 1;
}
static const USBTransportOperations usb_ops={control_transfer,update_ep0};
int UHCICoreService(UHCIControllerState *c, unsigned causes)
{
    unsigned i,j;
    if (c->fatal || !c->running) return 0;
    if (causes&UHCI_STS_FATAL) { UHCICoreFail(c,"host error",causes); return 0; }
    c->polls++;
    for (i=0;i<UHCI_MAX_DEVICES;i++) {
        UHCIDevice *d=&c->devices[i];
        if (!d->used || d->disconnecting) continue;
        for (j=0;j<UHCI_ENDPOINTS;j++) if (d->endpoints[j].waiting) {
            int rc=UHCICoreFinish(c,d,&d->endpoints[j]);
            if (rc!=USB_TRANSFER_PENDING) {
                unsigned target;
                for (target=0;target<USB_STORAGE_TARGETS;target++)
                    if (c->storage[target].slot==i+1) UHCIPlatformStorageWake(c->owner,target);
            }
        }
    }
    return !c->fatal;
}
UHCIDevice *UHCICoreStorageDevice(UHCIControllerState *c, unsigned target, uhci_u32 generation)
{
    unsigned slot;
    if (target>=USB_STORAGE_TARGETS || c->fatal || !c->running || c->storage[target].generation!=generation) return 0;
    slot=c->storage[target].slot;
    if (!slot || slot>UHCI_MAX_DEVICES || !c->devices[slot-1].ready || c->devices[slot-1].disconnecting) return 0;
    return &c->devices[slot-1];
}
static UHCIEndpoint *storage_endpoint(UHCIDevice *d, unsigned address)
{
    unsigned i;
    for (i=0;i<UHCI_ENDPOINTS;i++) if (d->endpoints[i].configured && d->endpoints[i].address==address) return &d->endpoints[i];
    return 0;
}
int UHCICoreStorageBulkStart(UHCIControllerState *c, unsigned target, uhci_u32 gen,
    uhci_u8 address, const void *buffer, uhci_u32 length, uhci_u64 deadline)
{
    UHCIDevice *d=UHCICoreStorageDevice(c,target,gen);
    UHCIEndpoint *e=d ? storage_endpoint(d,address) : 0;
    if (!e || !address) return USB_TRANSFER_DISCONNECTED;
    return UHCICoreSubmit(c,d,e,0,buffer,length,deadline);
}
int UHCICoreStorageControlStart(UHCIControllerState *c, unsigned target, uhci_u32 gen,
    const USBSetupPacket *s, const void *buffer, uhci_u64 deadline)
{
    UHCIDevice *d=UHCICoreStorageDevice(c,target,gen);
    if (!d || d->controlBusy) return USB_TRANSFER_DISCONNECTED;
    d->controlBusy=1;
    {
        int rc=UHCICoreSubmit(c,d,&d->endpoints[0],s,buffer,s->length,deadline);
        if (rc) d->controlBusy=0;
        return rc;
    }
}
int UHCICoreStorageTransferResult(UHCIControllerState *c, unsigned target, uhci_u32 gen,
    uhci_u8 address, void *buffer, uhci_u32 *actual)
{
    UHCIDevice *d=UHCICoreStorageDevice(c,target,gen);
    UHCIEndpoint *e=d ? storage_endpoint(d,address) : 0;
    if (!e) return USB_TRANSFER_DISCONNECTED;
    if (e->waiting) return USB_TRANSFER_PENDING;
    *actual=0;
    if (!e->result || e->result==USB_TRANSFER_STALL) {
        *actual=e->actual; if (e->input) copy_input(e,buffer);
    }
    if (!address) d->controlBusy=0;
    return e->result;
}
int UHCICoreStorageRetireEndpoint(UHCIControllerState *c, unsigned target, uhci_u32 gen,
    uhci_u8 address, uhci_u64 end)
{
    UHCIDevice *d=UHCICoreStorageDevice(c,target,gen);
    UHCIEndpoint *e=d ? storage_endpoint(d,address) : 0;
    (void)end;
    if (!e) return USB_TRANSFER_DISCONNECTED;
    if (!UHCICoreCancel(c,d,e)) return USB_TRANSFER_ERROR;
    if (UHCICoreStorageDevice(c,target,gen)!=d) return USB_TRANSFER_DISCONNECTED;
    e->halted=1; if (!address) d->controlBusy=0; return USB_TRANSFER_OK;
}
int UHCICoreStorageRecoverEndpoint(UHCIControllerState *c, unsigned t, uhci_u32 g, uhci_u8 a, uhci_u64 end)
{
    UHCIDevice *d; UHCIEndpoint *e;
    int rc=UHCICoreStorageRetireEndpoint(c,t,g,a,end);
    if (rc) return rc;
    d=UHCICoreStorageDevice(c,t,g); e=d ? storage_endpoint(d,a) : 0;
    if (!e) return USB_TRANSFER_DISCONNECTED;
    e->halted=0; e->toggle=0; e->result=0; return 0;
}
void UHCICoreStorageOffline(UHCIControllerState *c, unsigned t, uhci_u32 g)
{
    if (t<USB_STORAGE_TARGETS && c->storage[t].generation==g && c->storage[t].slot)
        c->disconnected|=1U<<(c->storage[t].slot-1);
}
int UHCICoreQuiesce(UHCIControllerState *c)
{
    if (!c->registersValid) return !c->dmaArmed;
    if (!write_op(c,UHCI_INTR,0) || !write_op(c,UHCI_CMD,0) ||
        !wait_bits(c,UHCI_STS,UHCI_STS_HALTED,UHCI_STS_HALTED,100)) return 0;
    c->running=0; c->haltConfirmed=1; return 1;
}
int UHCICoreReleaseDMA(UHCIControllerState *c, int fenced)
{
    unsigned i,j;
    if (c->dmaArmed && !c->haltConfirmed && !fenced) return 0;
    for (i=0;i<UHCI_MAX_DEVICES;i++) for (j=0;j<UHCI_ENDPOINTS;j++) endpoint_free(&c->devices[i].endpoints[j]);
    dma_free(&c->frameList); dma_free(&c->asyncHead); c->dmaArmed=0; return 1;
}
#include "UHCIEnumeration.inc"
