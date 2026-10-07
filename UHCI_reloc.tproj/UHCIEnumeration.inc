/* Included by UHCICore.c. All management runs on one sleepable worker. */
static int device_request(UHCIDevice *d, unsigned type, unsigned request,
                           unsigned value, unsigned index, void *data, unsigned size)
{
    USBSetupPacket s;
    uhci_u16 actual = 0;
    s.requestType = type; s.request = request; s.value = value;
    s.index = index; s.length = size;
    return USBCoreControlTransfer(&d->usb, &s, data, &actual) && actual == size;
}
static unsigned new_generation(UHCIControllerState *c)
{
    if (c->nextGeneration == 0xffffffffU) { UHCICoreFail(c, "generation exhausted", 0); return 0; }
    return ++c->nextGeneration;
}
static unsigned allocate_address(UHCIControllerState *c)
{
    unsigned a;
    for (a = 1; a < 128; a++) if (!(c->addresses[a / 32] & (1U << (a % 32)))) {
        c->addresses[a / 32] |= 1U << (a % 32); return a;
    }
    return 0;
}
static int child_slot(UHCIControllerState *c, unsigned parent, unsigned port)
{
    unsigned i;
    for (i = 0; i < UHCI_MAX_DEVICES; i++)
        if (c->devices[i].used && c->devices[i].parent == parent &&
            (parent ? c->devices[i].parentPort : c->devices[i].port) == port) return (int)i;
    return -1;
}

/* An addressed but unsupported/offline device still answers USB tokens.
 * Isolate its physical port before releasing the address. Failure escalates
 * to controller containment and retains all ownership/DMA for that device. */
static int root_isolated(UHCIControllerState *c, unsigned rootPort)
{
    uhci_u32 v;
    if (!rootPort || rootPort > c->maxPorts) return 0;
    v = read_op(c, UHCI_PORT(rootPort));
    return !c->fatal && (!(v & UHCI_PORT_CONNECT) ||
                        !(v & (UHCI_PORT_ENABLE | UHCI_PORT_RESET)));
}

static int isolate_port(UHCIControllerState *c, unsigned rootPort,
                        unsigned parent, unsigned parentPort)
{
    UHCIDevice *hub;
    uhci_u32 v;
    uhci_u8 status[4];
    if (!parent) {
        if (!rootPort || rootPort > c->maxPorts) return 0;
        v = read_op(c, UHCI_PORT(rootPort));
        if (c->fatal) return 0;
        if (!(v & UHCI_PORT_CONNECT)) return 1;
        c->blockedPorts |= 1U << rootPort;
        return write_op(c, UHCI_PORT(rootPort),
                        UHCIPortWriteValue(v, 0, UHCI_PORT_ENABLE, 0)) &&
               wait_bits(c, UHCI_PORT(rootPort), UHCI_PORT_ENABLE | UHCI_PORT_RESET, 0, 100);
    }
    if (parent > UHCI_MAX_DEVICES || !parentPort ||
        parentPort > UHCI_MAX_HUB_PORTS) return 0;
    /* Child transfer errors can be serviced before the root unplug event.
     * A disabled/disconnected upstream port needs no request to the dead hub. */
    if (root_isolated(c, rootPort)) return 1;
    if (c->fatal) return 0;
    hub = &c->devices[parent - 1];
    if (!hub->used || !hub->ready || !hub->hub || hub->disconnecting ||
        !device_request(hub, 0xa3, 0, 0, parentPort, status, 4))
        return root_isolated(c, rootPort);
    if (!(USBCoreReadLE16(status) & 1)) return 1;
    hub->blockedPorts |= 1U << parentPort;
    if (device_request(hub, 0x23, 1, 1, parentPort, 0, 0) &&
        device_request(hub, 0xa3, 0, 0, parentPort, status, 4) &&
        !(USBCoreReadLE16(status) & 0x12)) return 1;
    /* Hub control sleeps release the lock; unplug may occur during them. */
    return root_isolated(c, rootPort);
}

static void disconnect_device(UHCIControllerState *c, unsigned slot, int isolated)
{
    UHCIDevice *d;
    unsigned i, generation;
    if (slot >= UHCI_MAX_DEVICES || !c->devices[slot].used) return;
    d = &c->devices[slot];
    d->disconnecting = 1; d->ready = 0;
    for (i = 0; i < USB_STORAGE_TARGETS; i++) if (c->storage[i].slot == slot + 1) {
        c->storage[i].slot = 0;
        c->storage[i].generation = new_generation(c);
        c->storage[i].removals++;
        UHCIPlatformStorageWake(c->owner, i);
    }
    if (!isolated && !isolate_port(c, d->port, d->parent, d->parentPort)) {
        UHCICoreFail(c, "disconnect isolation", slot); return;
    }
    /* Isolating an upstream hub also isolates all its children; its EP0
     * cannot be used after this point to disable downstream ports. */
    for (i = 0; i < UHCI_MAX_DEVICES; i++)
        if (c->devices[i].used && c->devices[i].parent == slot + 1)
            disconnect_device(c, i, 1);
    /* Invalidate clients before dropping the lock for frame retirement. */
    for (i=0;i<UHCI_ENDPOINTS;i++)
        if (d->endpoints[i].configured && !UHCICoreCancel(c,d,&d->endpoints[i])) return;
    /* All child QHs remain unlinked while other schedule users run. */
    generation = new_generation(c);
    c->addresses[d->address / 32] &= ~(1U << (d->address % 32));
    c->addresses[0] |= 1;
    for (i = 0; i < UHCI_ENDPOINTS; i++) endpoint_free(&d->endpoints[i]);
    bzero(d, sizeof(*d)); d->generation = generation;
    c->disconnected &= ~(1U << slot);
}

void UHCICoreDisconnect(UHCIControllerState *c, unsigned slot)
{
    disconnect_device(c, slot, 0);
}

/* USB 2.0 sections 7.2.1 and 11.13: descriptor capability alone does not
 * establish the current supply. Reserve one unit for every external port. */
static int hub_power(UHCIDevice *d, unsigned *selfPowered)
{
    uhci_u8 deviceStatus[2], hubStatus[4];
    if (!device_request(d, 0x80, 0, 0, 0, deviceStatus, 2) ||
        !device_request(d, 0xa0, 0, 0, 0, hubStatus, 4) ||
        (hubStatus[0] & 2)) return 0;
    *selfPowered = d->selfPowerCapable && (deviceStatus[0] & 1) && !(hubStatus[0] & 1);
    if (*selfPowered) return d->powerMA <= 100;
    return d->powerMA && d->externalPorts <= 4 &&
           d->powerMA + d->externalPorts * 100 <= 500 &&
           !(d->hubCharacteristics & 2);
}

static void hub_power_off(UHCIControllerState *c, UHCIDevice *d)
{
    unsigned p;
    uhci_u8 status[4];
    /* Disable the entire subtree on loss/uncertainty of its supply. A USB
     * port-disable alone would leave configured high-current loads powered. */
    c->disconnected |= 1U << (d - c->devices);
    for (p = 1; p <= d->hubPorts && !c->fatal; p++) {
        if (!device_request(d, 0x23, 1, 8, p, 0, 0) ||
            !device_request(d, 0xa3, 0, 0, p, status, 4) ||
            (USBCoreReadLE16(status) & 0x100)) {
            if (!root_isolated(c, d->port)) UHCICoreFail(c, "hub power isolation", p);
            return;
        }
        d->portPowerMA[p] = 0;
    }
}

static int configuration_power(UHCIControllerState *c, UHCIDevice *d)
{
    UHCIDevice *hub;
    unsigned selfPowered, i, total;
    if (d->powerMA > 500 || (!d->powerMA && !d->selfPowerCapable)) return 0;
    if (!d->parent) return 1;
    hub = &c->devices[d->parent - 1];
    if (!hub->ready || hub->disconnecting ||
        (c->disconnected & (1U << (d->parent - 1)))) return 0;
    if (!hub_power(hub, &selfPowered) || (hub->selfPowered && !selfPowered)) {
        hub_power_off(c, hub); return 0;
    }
    hub->selfPowered = selfPowered;
    if (!selfPowered && (hub->nonRemovable & (1U << d->parentPort))) {
        total = hub->powerMA + hub->externalPorts * 100 + d->powerMA;
        for (i = 1; i <= hub->hubPorts; i++)
            if (i != d->parentPort) total += hub->portPowerMA[i];
        if (total > 500) return 0;
        /* Reserve before SET_CONFIGURATION: a failed status stage can still
         * leave the function configured. PORT_ENABLE clear does not release
         * its supply; retain this charge independently of the device slot. */
        hub->portPowerMA[d->parentPort] = d->powerMA;
        return 1;
    }
    return d->powerMA <= (selfPowered ? 500U : 100U);
}

static int configure_hub(UHCIControllerState *c, UHCIDevice *d, USBEnumerationData *info)
{
    USBDescriptorIterator it;
    const uhci_u8 *desc;
    uhci_u8 len, type, hubdesc[16];
    unsigned p, delay, descriptorLength;
    int active=0, found=0, rc;
    if (d->parent || d->speed!=UHCI_SPEED_FULL) return 0;
    USBCoreDescriptorIteratorInitialize(&it,info->configurationDescriptor,info->configurationLength);
    while ((rc=USBCoreDescriptorNext(&it,&desc,&len,&type))>0) {
        if (type==USB_DESC_INTERFACE) {
            if (len<9) return 0;
            active=desc[3]==0 && desc[5]==9 && desc[6]==0 && desc[7]==0;
        } else if (active && type==USB_DESC_ENDPOINT) {
            if (len<7 || desc[3]!=3 || !(desc[2]&0x80) || !(desc[2]&15) ||
                (desc[2]&0x70) || !USBCoreReadLE16(desc+4) || USBCoreReadLE16(desc+4)>64 || !desc[6]) return 0;
            found++;
        }
    }
    if (rc<0 || found!=1 || !configuration_power(c,d) ||
        !USBCoreSetConfiguration(&d->usb,info->configurationValue)) return 0;
    bzero(hubdesc, sizeof(hubdesc));
    if (!device_request(d, 0xa0, USB_REQ_GET_DESCRIPTOR, 0x2900, 0, hubdesc, 7) ||
        hubdesc[0] < 7 || hubdesc[1] != 0x29 || !hubdesc[2] || hubdesc[2] > UHCI_MAX_HUB_PORTS)
        return 0;
    d->hub = 1; d->hubPorts = hubdesc[2];
    d->hubCharacteristics = USBCoreReadLE16(hubdesc + 3);
    descriptorLength = 7 + 2 * ((d->hubPorts + 8) / 8);
    if (hubdesc[0] < descriptorLength ||
        !device_request(d, 0xa0, USB_REQ_GET_DESCRIPTOR, 0x2900, 0, hubdesc, descriptorLength) ||
        hubdesc[1] != 0x29 || hubdesc[2] != d->hubPorts) return 0;
    for (p = 1; p <= d->hubPorts; p++) {
        if (hubdesc[7 + p / 8] & (1U << (p % 8))) d->nonRemovable |= 1U << p;
        else d->externalPorts++;
    }
    if (!hub_power(d, &d->selfPowered)) return 0;
    for (p = 1; p <= d->hubPorts; p++)
        if (!device_request(d, 0x23, 3, 8, p, 0, 0)) return 0;
    delay = (unsigned)hubdesc[5] * 2 + 20;
    UHCIPlatformPause(c, delay);
    d->hubChanges = (1U << (d->hubPorts + 1)) - 1;
    return 1;
}

static int configuration_rank(void *context, const USBEnumerationData *info)
{
    USBMassStorageInterface storage;
    (void)context;
    if (info->error) return -1;
    if (info->deviceDescriptor[4]==9) return 1;
    return USBMassStorageFindInterface(info->configurationDescriptor,info->configurationLength,&storage) ? 1 : 0;
}

/* Capture the first request before failure isolation frees its DMA. Register
 * reads use the usual containment boundary; never resume I/O after a fault.
 * A partial allocation has no TD to inspect. No transfer is retried here. */
static void initial_descriptor_diagnostics(UHCIControllerState *c, UHCIDevice *d,
    unsigned frameBefore, int ok, unsigned actual, const uhci_u8 *first)
{
    UHCIEndpoint *e=&d->endpoints[0];
    unsigned i, cmd=c->lastCommand, sts=c->lastStatus;
    unsigned intr=0xffffffffU, frame=0xffffffffU, port=0xffffffffU, base=0xffffffffU;
    uhci_u32 pci=0xffffffffU, legacy=0xffffffffU;
    if (!c->fatal) {
        cmd=read_op(c,UHCI_CMD); sts=read_op(c,UHCI_STS);
        intr=read_op(c,UHCI_INTR); frame=read_op(c,UHCI_FRNUM);
        port=read_op(c,UHCI_PORT(d->port)); base=read_op(c,UHCI_FLBASE);
        if (!c->fatal) {
            UHCIPlatformPCIRead(c,4,&pci); UHCIPlatformPCIRead(c,UHCI_LEGSUP,&legacy);
        }
    }
    UHCIPlatformLog("UHCI: I/O %x initial desc ok=%u got=%u result=%d scan=%u/%u fatal=%u\n",
        c->ioBase,ok,actual,e->result,e->scan,e->count,c->fatal);
    UHCIPlatformLog("UHCI: I/O %x cmd=%04x sts=%04x intr=%04x frame=%x->%x port=%04x\n",
        c->ioBase,cmd,sts,intr,frameBefore,frame,port);
    UHCIPlatformLog("UHCI: I/O %x pci=%08x legsup=%08x flbase=%08x want=%08x\n",
        c->ioBase,pci,legacy,base,c->frameList.physicalAddress);
    UHCIPlatformLog("UHCI: I/O %x qh=%08x link=%08x elem=%08x\n",
        c->ioBase,e->qhPhysical,e->qh->link,e->qh->element);
    for (i=0;i<e->count && i<3 && e->tdPages[0].allocation;i++) {
        UHCITD *td=td_at(e,i);
        UHCIPlatformLog("UHCI: I/O %x td%u=%08x link=%08x sts=%08x tok=%08x buf=%08x\n",
            c->ioBase,i,td_phys(e,i),td->link,td->status,td->token,td->buffer);
    }
    UHCIPlatformLog("UHCI: I/O %x desc bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
        c->ioBase,first[0],first[1],first[2],first[3],first[4],first[5],first[6],first[7]);
}

static int attach_device(UHCIControllerState *c, unsigned rootPort,
                         unsigned parent, unsigned parentPort, unsigned speed)
{
    UHCIDevice *d;
    USBEnumerationData *info = 0;
    uhci_u8 first[8];
    uhci_u16 actual;
    unsigned slot, address, target, firstFrame;
    int selection, firstOK;
    const char *stage = "endpoint zero";
    if (speed!=UHCI_SPEED_FULL) {
        if (!isolate_port(c,rootPort,parent,parentPort)) UHCICoreFail(c,"unsupported speed isolation",rootPort);
        return 0;
    }
    for (slot = 0; slot < UHCI_MAX_DEVICES && c->devices[slot].used; slot++) {}
    if (slot == UHCI_MAX_DEVICES || !(address = allocate_address(c))) {
        /* A reset port also needs isolation if no slot/address is available:
         * otherwise it still responds at address zero during the next attach. */
        if (!isolate_port(c, rootPort, parent, parentPort)) UHCICoreFail(c, "no address isolation", rootPort);
        return 0;
    }
    d = &c->devices[slot]; bzero(d, sizeof(*d));
    d->used = 1; d->generation = new_generation(c); d->port = rootPort;
    d->parent = parent; d->parentPort = parentPort; d->speed = speed;
    info = UHCIPlatformAllocate(sizeof(*info));
    if (!info) goto fail;
    bzero(info, sizeof(*info));
    USBCoreDeviceInitialize(&d->usb, &usb_ops, c, d);
    if (!UHCICoreConfigureEndpoint(c, d, 0, 0, 8, 0)) goto fail;
    stage = "initial descriptor";
    bzero(first,sizeof(first)); actual=0;
    firstFrame=read_op(c,UHCI_FRNUM);
    firstOK=USBCoreGetDescriptor(&d->usb, 1, 0, first, 8, &actual);
    if (!firstOK || actual != 8 || first[0] < 18 || first[1] != 1 ||
        !update_ep0(c, d, first[7])) {
        initial_descriptor_diagnostics(c,d,firstFrame,firstOK,actual,first);
        goto fail;
    }
    stage = "set address";
    if (!device_request(d, 0, 5, address, 0, 0, 0)) goto fail;
    d->address = address;
    UHCIPlatformPause(c, 2);
    stage = "device/configuration descriptors";
    if (!USBCoreReadDevice(&d->usb, info)) goto fail;
    UHCIPlatformLog("UHCI: address %u USB %04x:%04x bNumConfigurations %u\n",
        address, (info ? info->vendorID : 0), (info ? info->productID : 0), info->numberConfigurations);
    selection = USBCoreSelectConfiguration(&d->usb, info, configuration_rank, d);
    if (selection != 1) {
        stage = selection < 0 ? "incomplete/invalid configuration scan" : "no supported storage or hub configuration";
        goto fail;
    }
    UHCIPlatformLog("UHCI: address %u selected configuration index %u bConfigurationValue %u\n",
        address, info->configurationIndex, info->configurationValue);
    d->configurationValue = info->configurationValue;
    d->powerMA = (unsigned)info->configurationDescriptor[8] * 2;
    d->selfPowerCapable = (info->configurationDescriptor[7] & 0x40) != 0;
    if (info->deviceDescriptor[4] == 9) {
        stage = "hub configuration";
        if (!configure_hub(c, d, info)) goto fail;
    } else if (USBMassStorageFindInterface(info->configurationDescriptor, info->configurationLength, &d->storageInterface)) {
        stage = "storage configuration";
        for (target = 0; target < USB_STORAGE_TARGETS && c->storage[target].slot; target++) {}
        if (target == USB_STORAGE_TARGETS ||
            !configuration_power(c, d) ||
            !USBCoreSetConfiguration(&d->usb, d->storageInterface.configuration) ||
            !UHCICoreConfigureEndpoint(c, d, UHCI_EP_BULK_IN, d->storageInterface.bulkIn, d->storageInterface.inPacket, 0) ||
            !UHCICoreConfigureEndpoint(c, d, UHCI_EP_BULK_OUT, d->storageInterface.bulkOut, d->storageInterface.outPacket, 0))
            goto fail;
        d->storage = 1;
        c->storage[target].generation = new_generation(c);
        c->storage[target].slot = slot + 1; c->storage[target].lastPort = rootPort;
    } else { stage = "unsupported USB interface"; goto fail; }
    d->ready = 1;
    UHCIPlatformLog("UHCI: port %u address %u %s ready (%04x:%04x, speed %u)\n",
        rootPort, address, d->hub ? "hub" : "storage", (info ? info->vendorID : 0), (info ? info->productID : 0), speed);
    if (d->storage) UHCIPlatformStorageAttached(c->owner);
    UHCIPlatformFree(info, sizeof(*info));
    return 1;
fail:
    c->enumerationFailure.stage = stage;
    c->enumerationFailure.rootPort = rootPort;
    c->enumerationFailure.hubPort = parentPort;
    c->enumerationFailure.speed = speed;
    c->enumerationFailure.vendor = (info ? info->vendorID : 0);
    c->enumerationFailure.product = (info ? info->productID : 0);
    UHCIPlatformLog("UHCI: I/O %x port %u hub port %u address %u rejected at %s (%04x:%04x)\n",
        c->ioBase,rootPort,parentPort,address,stage,info ? info->vendorID : 0,info ? info->productID : 0);
    /* SET_ADDRESS may have succeeded even if its status stage failed.
     * Keep the reserved address until physical isolation is verified. */
    d->address = address;
    UHCICoreDisconnect(c, slot);
    if (info) UHCIPlatformFree(info, sizeof(*info));
    return 0;
}

static int hub_status(UHCIDevice *d, unsigned port, unsigned *status, unsigned *change)
{
    uhci_u8 b[4];
    if (!device_request(d, port ? 0xa3 : 0xa0, 0, 0, port, b, 4)) return 0;
    *status = USBCoreReadLE16(b); *change = USBCoreReadLE16(b + 2);
    return 1;
}
static int debounce_port(UHCIControllerState *c, UHCIDevice *hub,
                          unsigned port, unsigned *status)
{
    unsigned change;
    uhci_u64 end = UHCIPlatformMilliseconds() + 1500;
    /* Connection-change bits are sticky and only this management worker
     * acknowledges them. Any bounce restarts the full 100 ms interval. */
    do {
        UHCIPlatformPause(c, 100);
        if (hub) {
            if (!hub_status(hub, port, status, &change) ||
                !(*status & 1) || (*status & 8)) return 0;
            if (!(change & 1)) return 1;
            if (!device_request(hub, 0x23, 1, 16, port, 0, 0)) return 0;
        } else {
            *status = read_op(c, UHCI_PORT(port));
            if (c->fatal || !(*status & UHCI_PORT_CONNECT) ||
                (*status & UHCI_PORT_OC)) return 0;
            if (!(*status & UHCI_PORT_CSC)) return 1;
            if (!write_op(c, UHCI_PORT(port),
                UHCIPortWriteValue(*status, 0, 0, UHCI_PORT_CSC))) return 0;
        }
    } while (UHCIPlatformMilliseconds() < end && !c->fatal);
    return 0;
}
static void service_hub(UHCIControllerState *c, unsigned slot)
{
    UHCIDevice *d = &c->devices[slot];
    unsigned p, status, change, changes = (1U << (d->hubPorts+1))-1;
    d->hubChanges = 0;
    if (changes & 1) {
        unsigned selfPowered;
        if (!hub_status(d, 0, &status, &change) ||
            !hub_power(d, &selfPowered) || (d->selfPowered && !selfPowered)) {
            hub_power_off(c, d); return;
        }
        d->selfPowered = selfPowered;
        if (change & 1) device_request(d, 0x20, 1, 0, 0, 0, 0);
        if (change & 2) device_request(d, 0x20, 1, 1, 0, 0, 0);
        if (status & 2) { c->disconnected |= 1U << slot; return; }
    }
    for (p = 1; p <= d->hubPorts && !c->fatal &&
        !(c->disconnected & (1U << slot)); p++) if (changes & (1U << p)) {
        int child;
        unsigned bit;
        uhci_u64 end;
        if (!hub_status(d, p, &status, &change)) { c->disconnected |= 1U << slot; return; }
        if (!(status & 1)) d->portPowerMA[p] = 0;
        for (bit = 0; bit < 5; bit++) if (change & (1U << bit))
            if (!device_request(d, 0x23, 1, 16 + bit, p, 0, 0)) return;
        child = child_slot(c, slot + 1, p);
        if (child >= 0 && (!(status & 1) || !(status & 2) || (change & 1) || (status & 8))) {
            UHCICoreDisconnect(c, child); child = -1;
        }
        if ((change & 1) || !(status & 1)) d->blockedPorts &= ~(1U << p);
        if (c->fatal || !(status & 1) || (status & 8) || child >= 0 ||
            (d->blockedPorts & (1U << p))) continue;
        if (!debounce_port(c, d, p, &status)) goto reject_port;
        if (!device_request(d, 0x23, 3, 4, p, 0, 0)) goto reject_port;
        end = UHCIPlatformMilliseconds() + 500;
        do {
            UHCIPlatformPause(c, 10);
            if (!hub_status(d, p, &status, &change)) goto reject_port;
            if (!(status & 0x10)) break;
        } while (UHCIPlatformMilliseconds() < end);
        if ((status & 0x13) != 3) goto reject_port;
        if (!device_request(d, 0x23, 1, 20, p, 0, 0)) goto reject_port;
        /* A completed USB reset returns the function to default power. */
        d->portPowerMA[p] = 0;
        UHCIPlatformPause(c, 10);
        attach_device(c, d->port, slot + 1, p, (status & 0x400) ? UHCI_SPEED_HIGH :
            (status & 0x200) ? UHCI_SPEED_LOW : UHCI_SPEED_FULL);
        continue;
reject_port:
        /* A reset may have enabled an unaddressed device even if its control
         * status/acknowledgement failed. Do not expose a second address zero. */
        if (!isolate_port(c, d->port, slot + 1, p)) UHCICoreFail(c, "hub port isolation", p);
    }
}

int UHCICoreServicePorts(UHCIControllerState *c)
{
    unsigned p,i;
    if (!c->running || c->fatal || c->enumerating) return 0;
    c->enumerating=1;
    for (i=0;i<UHCI_MAX_DEVICES && !c->fatal;i++)
        if (c->disconnected&(1U<<i)) UHCICoreDisconnect(c,i);
    for (p=1;p<=c->maxPorts && !c->fatal;p++) {
        unsigned v=read_op(c,UHCI_PORT(p));
        int child=child_slot(c,0,p);
        const char *stage;
        if (c->fatal) break;
        if (!(c->rootPortsScanned&(1U<<p)) || (v&UHCI_PORT_CSC))
            UHCIPlatformLog("UHCI: I/O %x root port %u status=%04x connect=%u enable=%u low=%u overcurrent=%u suspend=%u\n",
                c->ioBase,p,c->rootPortStatus[p-1],!!(v&UHCI_PORT_CONNECT),
                !!(v&UHCI_PORT_ENABLE),!!(v&UHCI_PORT_LOW),!!(v&UHCI_PORT_OC),!!(v&UHCI_PORT_SUSPEND));
        c->rootPortsScanned|=1U<<p;
        if (!write_op(c,UHCI_PORT(p),UHCIPortWriteValue(v,0,0,v))) break;
        if (child>=0 && (!(v&UHCI_PORT_CONNECT) || (v&UHCI_PORT_CSC) ||
            !(v&UHCI_PORT_ENABLE) || (v&UHCI_PORT_OC))) {
            UHCICoreDisconnect(c,child); child=-1;
        }
        if ((v&UHCI_PORT_CSC) || !(v&UHCI_PORT_CONNECT)) c->blockedPorts&=~(1U<<p);
        if (c->fatal || !(v&UHCI_PORT_CONNECT) || child>=0 || (c->blockedPorts&(1U<<p))) continue;
        stage="overcurrent"; if (v&UHCI_PORT_OC) goto reject;
        stage="low-speed unsupported"; if (v&UHCI_PORT_LOW) goto reject;
        stage="debounce"; if (!debounce_port(c,0,p,&v)) goto reject;
        if (!write_op(c,UHCI_PORT(p),UHCIPortWriteValue(v,UHCI_PORT_RESET,
            UHCI_PORT_ENABLE|UHCI_PORT_SUSPEND|UHCI_PORT_RESUME,0))) break;
        UHCIPlatformPause(c,50);
        v=read_op(c,UHCI_PORT(p));
        if (!write_op(c,UHCI_PORT(p),UHCIPortWriteValue(v,0,UHCI_PORT_RESET,0)) ||
            !wait_bits(c,UHCI_PORT(p),UHCI_PORT_RESET,0,100)) {
            UHCICoreFail(c,"root reset",p); break;
        }
        /* Let the controller re-detect connection, then enable promptly so
         * SOFs reach the device during reset recovery. A millisecond sleep
         * here can leave the disabled bus idle long enough to suspend it. */
        UHCIPlatformDelay(c,10);
        v=read_op(c,UHCI_PORT(p));
        stage="disconnected after reset"; if (!(v&UHCI_PORT_CONNECT)) goto reject;
        stage="overcurrent after reset"; if (v&UHCI_PORT_OC) goto reject;
        stage="low-speed after reset"; if (v&UHCI_PORT_LOW) goto reject;
        stage="enable";
        /* Reset itself sets connection/enable changes. Acknowledge these
         * before the separate enable write, as in the UHCI hub sequence. */
        if (!write_op(c,UHCI_PORT(p),UHCIPortWriteValue(v,0,0,UHCI_PORT_CHANGES))) break;
        v=read_op(c,UHCI_PORT(p));
        if (!write_op(c,UHCI_PORT(p),UHCIPortWriteValue(v,UHCI_PORT_ENABLE,0,0)) ||
            !wait_bits(c,UHCI_PORT(p),UHCI_PORT_ENABLE,UHCI_PORT_ENABLE,100)) goto reject;
        UHCIPlatformPause(c,10);
        attach_device(c,p,0,0,UHCI_SPEED_FULL);
        continue;
reject:
        c->enumerationFailure.stage=stage;
        c->enumerationFailure.rootPort=p; c->enumerationFailure.hubPort=0;
        c->enumerationFailure.vendor=c->enumerationFailure.product=0;
        UHCIPlatformLog("UHCI: I/O %x root port %u rejected at %s status=%04x\n",
            c->ioBase,p,stage,c->rootPortStatus[p-1]);
        if (!c->fatal && !isolate_port(c,p,0,0)) UHCICoreFail(c,"root isolation",p);
    }
    for (i=0;i<UHCI_MAX_DEVICES && !c->fatal;i++)
        if (c->devices[i].ready && c->devices[i].hub) service_hub(c,i);
    c->enumerating=0;
    return !c->fatal;
}
