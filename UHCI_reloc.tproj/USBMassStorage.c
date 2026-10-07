/* USB-IF Bulk-Only Transport 1.0. Transport and DMA are supplied by the caller. */
#include "USBMassStorage.h"
#include "UHCIMemory.h"

static uhci_u32 le32(const uhci_u8 *p)
{
    return p[0] | ((uhci_u32)p[1] << 8) | ((uhci_u32)p[2] << 16) |
           ((uhci_u32)p[3] << 24);
}

static void put32(uhci_u8 *p, uhci_u32 n)
{
    unsigned i;
    for (i = 0; i < 4; i++) { p[i] = (uhci_u8)n; n >>= 8; }
}

int USBMassStorageFindInterface(const uhci_u8 *bytes, uhci_u16 length,
                                USBMassStorageInterface *result)
{
    USBDescriptorIterator it;
    USBMassStorageInterface candidate, found;
    const uhci_u8 *d;
    uhci_u8 len, type;
    int active = 0, matches = 0, rc, endpoints = 0;
    if (!bytes || !result || length < 9 || bytes[0] < 9 ||
        bytes[1] != USB_DESC_CONFIG || !bytes[5] ||
        USBCoreReadLE16(bytes + 2) != length) return 0;
    bzero(&candidate, sizeof(candidate));
    bzero(&found, sizeof(found));
    USBCoreDescriptorIteratorInitialize(&it, bytes, length);
    while ((rc = USBCoreDescriptorNext(&it, &d, &len, &type)) > 0) {
        if (type == USB_DESC_INTERFACE) {
            if (active) {
                if (endpoints != 2 || !candidate.bulkIn || !candidate.bulkOut)
                    return 0;
                found = candidate;
                matches++;
            }
            if (len < 9) return 0;
            active = d[3] == 0 && d[5] == 8 && d[6] == 6 && d[7] == 0x50;
            bzero(&candidate, sizeof(candidate));
            endpoints = 0;
            if (active) {
                if (d[4] != 2) return 0;
                candidate.number = d[2];
                candidate.configuration = bytes[5];
            }
        } else if (type == USB_DESC_ENDPOINT && active) {
            uhci_u16 packet;
            if (len < 7 || d[3] != 2 || !(d[2] & 15) || (d[2] & 0x70))
                return 0;
            packet = USBCoreReadLE16(d + 4);
            if (packet != 8 && packet != 16 && packet != 32 && packet != 64) return 0;
            if (d[2] & USB_DIR_IN) {
                if (candidate.bulkIn) return 0;
                candidate.bulkIn = d[2]; candidate.inPacket = packet;
            } else {
                if (candidate.bulkOut) return 0;
                candidate.bulkOut = d[2]; candidate.outPacket = packet;
            }
            endpoints++;
        }
    }
    if (rc < 0) return 0;
    if (active) {
        if (endpoints != 2 || !candidate.bulkIn || !candidate.bulkOut) return 0;
        found = candidate;
        matches++;
    }
    if (matches != 1) return 0;
    *result = found;
    return 1;
}

int USBMassStorageGetMaxLUN(USBMassStorage *s, uhci_u8 *lun, uhci_u64 deadline)
{
    USBSetupPacket setup;
    uhci_u32 actual = 0;
    int rc;
    *lun = 0;
    setup.requestType = 0xa1; setup.request = 0xfe;
    setup.value = 0; setup.index = s->interface.number; setup.length = 1;
    rc = s->transport->control(s->context, &setup, lun, &actual, deadline);
    if (rc == USB_TRANSFER_STALL) { *lun = 0; return USB_TRANSFER_OK; }
    if (rc) return rc;
    return actual == 1 && *lun <= 15 ? USB_TRANSFER_OK : USB_TRANSFER_ERROR;
}

int USBMassStorageReset(USBMassStorage *s, uhci_u64 deadline)
{
    USBSetupPacket setup;
    uhci_u32 actual = 0;
    int rc;
    setup.requestType = 0x21; setup.request = 0xff;
    setup.value = 0; setup.index = s->interface.number; setup.length = 0;
    s->unusable = 1;
    rc = s->transport->control(s->context, &setup, 0, &actual, deadline);
    if (!rc) rc = s->transport->clearHalt(s->context, s->interface.bulkIn, deadline);
    if (!rc) rc = s->transport->clearHalt(s->context, s->interface.bulkOut, deadline);
    if (!rc) s->unusable = 0;
    return rc;
}

static USBStorageResult command(USBMassStorage *s, const uhci_u8 *cdb,
    unsigned cdbLength, void *data, uhci_u32 length, int read, uhci_u64 deadline)
{
    USBStorageResult result;
    uhci_u8 cbw[31], csw[13];
    uhci_u32 actual = 0, received = 0, residue;
    int rc;
    bzero(&result, sizeof(result));
    result.status = USB_STORAGE_INVALID;
    if (!cdb || !cdbLength || cdbLength > 16 ||
        length > USB_STORAGE_MAX_REQUEST || (length && !data)) return result;
    if (s->unusable) { result.status = USB_TRANSFER_ERROR; return result; }
    if (s->transport->milliseconds(s->context) >= deadline) {
        result.status = USB_TRANSFER_TIMEOUT; return result;
    }
    bzero(cbw, sizeof(cbw));
    put32(cbw, 0x43425355UL);
    if (++s->tag == 0) ++s->tag;
    put32(cbw + 4, s->tag); put32(cbw + 8, length);
    /* BOT 5.1 ignores direction when length is zero. Use OUT then so devices
     * such as VBox MSD skip the nonexistent data phase and return the CSW. */
    cbw[12] = length && read ? 0x80 : 0; cbw[14] = (uhci_u8)cdbLength;
    bcopy(cdb, cbw + 15, cdbLength);
    rc = s->transport->bulk(s->context, s->interface.bulkOut,
                             cbw, sizeof(cbw), &actual, deadline);
    if (rc || actual != sizeof(cbw)) goto transportError;
    while (received < length) {
        uhci_u8 endpoint = read ? s->interface.bulkIn : s->interface.bulkOut;
        uhci_u32 chunk = length - received;
        if (chunk > USB_STORAGE_MAX_TRANSFER) chunk = USB_STORAGE_MAX_TRANSFER;
        actual = 0;
        rc = s->transport->bulk(s->context, endpoint,
            (uhci_u8 *)data + received, chunk, &actual, deadline);
        if (actual > chunk) { rc = USB_TRANSFER_ERROR; goto transportError; }
        received += actual;
        if (rc == USB_TRANSFER_STALL) {
            rc = s->transport->clearHalt(s->context, endpoint, deadline);
            if (rc) goto transportError;
            break; /* A stall ends the data phase; next read is the CSW. */
        }
        if (rc) goto transportError;
        if (actual < chunk) break;
    }
    bzero(csw, sizeof(csw));
    rc = s->transport->bulk(s->context, s->interface.bulkIn,
                             csw, sizeof(csw), &actual, deadline);
    if (rc == USB_TRANSFER_STALL) {
        rc = s->transport->clearHalt(s->context, s->interface.bulkIn, deadline);
        if (!rc) rc = s->transport->bulk(s->context, s->interface.bulkIn,
                                          csw, sizeof(csw), &actual, deadline);
    }
    if (rc || actual != sizeof(csw) || le32(csw) != 0x53425355UL ||
        le32(csw + 4) != s->tag || csw[12] > 1) goto transportError;
    residue = le32(csw + 8);
    if (residue > length || length - residue > received) goto transportError;
    result.actual = length - residue;
    result.status = csw[12] ? USB_STORAGE_CHECK : USB_TRANSFER_OK;
    return result;
transportError:
    result.status = rc ? rc : USB_TRANSFER_ERROR;
    /* Never replay a CDB after ambiguous transport completion. Recover only
     * to establish a safe starting point for the caller's next command. */
    if (rc != USB_TRANSFER_DISCONNECTED) {
        uhci_u64 recovery = s->transport->milliseconds(s->context);
        if (recovery > deadline) recovery = deadline;
        (void)USBMassStorageReset(s, recovery + 5000);
    }
    return result;
}

USBStorageResult USBMassStorageCommand(USBMassStorage *s, const uhci_u8 *cdb,
    unsigned cdbLength, void *data, uhci_u32 length, int read, int autoSense,
    uhci_u64 deadline)
{
    USBStorageResult result, senseResult;
    uhci_u8 senseCDB[6];
    result = command(s, cdb, cdbLength, data, length, read, deadline);
    if (result.status != USB_STORAGE_CHECK || !autoSense || cdb[0] == 3)
        return result;
    bzero(senseCDB, sizeof(senseCDB));
    senseCDB[0] = 3; senseCDB[4] = sizeof(result.sense);
    senseResult = command(s, senseCDB, sizeof(senseCDB), result.sense,
                          sizeof(result.sense), 1, deadline);
    if (senseResult.status == USB_TRANSFER_OK && senseResult.actual >= 14 &&
        (result.sense[0] & 0x7e) == 0x70)
        result.senseValid = 1;
    else if (senseResult.status != USB_STORAGE_CHECK)
        result.status = senseResult.status ? senseResult.status : USB_TRANSFER_ERROR;
    return result;
}
