/* Read-only removable disks and data CD-ROMs, shared with host tests. */
#include "USBStorageSCSI.h"
#include "UHCIMemory.h"

static uhci_u32 be32(const uhci_u8 *p)
{
    return ((uhci_u32)p[0] << 24) | ((uhci_u32)p[1] << 16) |
           ((uhci_u32)p[2] << 8) | p[3];
}
static void put32(uhci_u8 *p, uhci_u32 n)
{
    p[0] = (uhci_u8)(n >> 24); p[1] = (uhci_u8)(n >> 16);
    p[2] = (uhci_u8)(n >> 8); p[3] = (uhci_u8)n;
}
static USBStorageResult good(void)
{
    USBStorageResult r;
    bzero(&r, sizeof(r));
    return r;
}
static USBStorageResult check(USBStorageSCSIState *s, unsigned key,
                               unsigned asc, unsigned ascq)
{
    USBStorageResult r = good();
    r.status = USB_STORAGE_CHECK; r.senseValid = 1;
    r.sense[0] = 0x70; r.sense[2] = (uhci_u8)key; r.sense[7] = 10;
    r.sense[12] = (uhci_u8)asc; r.sense[13] = (uhci_u8)ascq;
    bcopy(r.sense, s->sense, sizeof(r.sense));
    return r;
}
static USBStorageResult reply(void *data, uhci_u32 limit, const void *bytes,
                               uhci_u32 length, uhci_u32 allocation)
{
    USBStorageResult r = good();
    if (length > allocation) length = allocation;
    if (length > limit) length = limit;
    if (length) bcopy(bytes, data, length);
    r.actual = length;
    return r;
}

unsigned USBStorageCDBLength(uhci_u8 opcode)
{
    switch (opcode >> 5) {
    case 0: return 6;
    case 1: case 2: return 10;
    case 4: return 16;
    case 5: return 12;
    default: return 0;
    }
}

void USBStorageSCSIObserve(USBStorageSCSIState *s, uhci_u32 generation,
                            uhci_u32 removals, int present)
{
    if (s->generation != generation) {
        s->identified = s->capacityValid = s->ejected = s->preventRemoval = 0;
        s->deviceType = 0;
        s->blockSize = s->lastBlock = 0;
        s->attention = present;
        if (s->removals != removals) s->removed = 1;
        bzero(s->sense, sizeof(s->sense));
    }
    s->generation = generation; s->removals = removals; s->present = present;
}

static USBStorageResult backend(USBStorageSCSIState *s, USBMassStorage *bot,
    const uhci_u8 *cdb, unsigned n, void *data, uhci_u32 length,
    int autoSense, uhci_u64 deadline)
{
    /* Cache backend sense even when the caller requests explicit sense. The
     * facade owns REQUEST SENSE, so a subsequent request must not lose the
     * device's error or race another CDB on this target. */
    USBStorageResult r = USBMassStorageCommand(bot, cdb, n, data, length,
                                               1, 1, deadline);
    (void)autoSense;
    if (r.senseValid) {
        bcopy(r.sense, s->sense, sizeof(s->sense));
        if ((r.sense[2] & 15) == 6 || (r.sense[2] & 15) == 2)
            s->capacityValid = 0;
    }
    return r;
}

static USBStorageResult identify(USBStorageSCSIState *s, USBMassStorage *bot,
                                 uhci_u64 deadline)
{
    uhci_u8 cdb[6], bytes[36], lun;
    USBStorageResult r = good();
    if (s->identified) return r;
    r.status = USBMassStorageGetMaxLUN(bot, &lun, deadline);
    if (r.status) return r;
    bzero(cdb, sizeof(cdb)); bzero(bytes, sizeof(bytes));
    cdb[0] = 0x12; cdb[4] = sizeof(bytes);
    r = backend(s, bot, cdb, sizeof(cdb), bytes, sizeof(bytes), 1, deadline);
    if (r.status) return r;
    /* Require a connected direct-access disk or CD/DVD device. Do not mask
     * away the peripheral qualifier: an absent LUN must remain absent. */
    if (r.actual < 5 || (bytes[0] != 0 && bytes[0] != 5))
        return check(s, 2, 0x30, 0); /* incompatible medium/device type */
    s->deviceType = bytes[0];
    s->identified = 1;
    return good();
}

static USBStorageResult capacity(USBStorageSCSIState *s, USBMassStorage *bot,
                                 uhci_u64 deadline)
{
    uhci_u8 cdb[10], bytes[8];
    USBStorageResult r;
    if (s->capacityValid) return good();
    bzero(cdb, sizeof(cdb)); bzero(bytes, sizeof(bytes)); cdb[0] = 0x25;
    r = backend(s, bot, cdb, sizeof(cdb), bytes, sizeof(bytes), 1, deadline);
    if (r.status) return r;
    if (r.actual != sizeof(bytes)) { r.status = USB_TRANSFER_ERROR; return r; }
    s->lastBlock = be32(bytes); s->blockSize = be32(bytes + 4);
    /* The stock disk driver adds one in a 32-bit capacity field. */
    if (s->lastBlock == 0xffffffffUL || s->blockSize < 512 ||
        s->blockSize > 4096 || (s->blockSize & (s->blockSize - 1)))
        return check(s, 2, 0x30, 0);
    s->capacityValid = 1;
    return good();
}

USBStorageResult USBStorageSCSIExecute(USBStorageSCSIState *s, USBMassStorage *bot,
    const uhci_u8 *cdb, unsigned n, void *data, uhci_u32 limit,
    int read, int autoSense, uhci_u64 deadline)
{
    USBStorageResult r;
    uhci_u8 bytes[64], command[16];
    uhci_u32 allocation, lba = 0, blocks = 0, length;
    unsigned op, header, page;
    if (!cdb || !n || n > 16 || n != USBStorageCDBLength(cdb[0]) ||
        limit > USB_STORAGE_MAX_REQUEST || (limit && !data)) {
        r = good(); r.status = USB_STORAGE_INVALID; return r;
    }
    op = cdb[0]; bzero(bytes, sizeof(bytes));
    /* Reject media-changing commands before readiness or any USB request. */
    switch (op) {
    case 0x04: case 0x07: case 0x0a: case 0x15: case 0x2a: case 0x2e:
    case 0x3b: case 0x41: case 0x42: case 0x55: case 0x89: case 0x8a:
    case 0x8e: case 0x93: case 0x9f: case 0xaa: case 0xae:
        return check(s, 7, 0x27, 0);
    case 0: case 3: case 8: case 0x12: case 0x1a: case 0x1b: case 0x1e:
    case 0x25: case 0x28: case 0x35: case 0x5a: case 0x88: case 0x91:
    case 0x9e: case 0xa8:
        break;
    default: return check(s, 5, 0x20, 0);
    }
    if (limit && !read) return check(s, 5, 0x24, 0);
    if (op == 0x12) {
        /* Empty USB slots are absent targets, not empty removable drives.
         * SCSIDisk skips selection failures before creating a disk object. */
        if (!s->present) { r = good(); r.status = USB_TRANSFER_DISCONNECTED; return r; }
        if (cdb[1] || cdb[2]) return check(s, 5, 0x24, 0);
        /* OPENSTEP uses the peripheral type to find the installation CD.
         * Identify even an empty optical drive; INQUIRY requires no media.
         * Preserve pending removal/unit attention for subsequent commands. */
        r = identify(s, bot, deadline);
        if (r.status) return r;
        bytes[0] = s->deviceType;
        bytes[1] = 0x80; bytes[2] = 2; bytes[3] = 2; bytes[4] = 31;
        bcopy("OPENSTEP", bytes + 8, 8);
        bcopy(s->deviceType == 5 ? "USB CD-ROM slot " : "USB storage slot",
              bytes + 16, 16);
        bcopy("0001", bytes + 32, 4);
        return reply(data, limit, bytes, 36, cdb[4]);
    }
    if (op == 3) {
        if (cdb[1]) return check(s, 5, 0x24, 0);
        if (!s->sense[0]) { s->sense[0] = 0x70; s->sense[7] = 10; }
        r = reply(data, limit, s->sense, sizeof(s->sense), cdb[4]);
        if (r.actual) bzero(s->sense, sizeof(s->sense));
        return r;
    }
    if (s->removed) { s->removed = 0; return check(s, 2, 0x3a, 0); }
    if (!s->present) return check(s, 2, 0x3a, 0);
    if (s->attention) { s->attention = 0; return check(s, 6, 0x28, 0); }
    if (op == 0x1b) {
        if (cdb[1] || cdb[2] || cdb[3] || (cdb[4] & ~3))
            return check(s, 5, 0x24, 0);
        if ((cdb[4] & 3) == 2) {
            if (s->preventRemoval) return check(s, 5, 0x53, 2);
            s->ejected = 1; s->capacityValid = 0;
        } else if (cdb[4] & 1) {
            if (s->ejected) s->attention = 1;
            s->ejected = 0;
        }
        return good();
    }
    if (op == 0x1e) {
        if (cdb[1] || cdb[2] || cdb[3] || (cdb[4] & ~1))
            return check(s, 5, 0x24, 0);
        s->preventRemoval = cdb[4] & 1; return good();
    }
    if (s->ejected) return check(s, 2, 0x3a, 0);
    r = identify(s, bot, deadline);
    if (r.status) return r;
    if (op == 0)
        return backend(s, bot, cdb, n, 0, 0, autoSense, deadline);
    if (op == 0x35 || op == 0x91) return good();
    r = capacity(s, bot, deadline);
    if (r.status) return r;
    if (op == 0x25) {
        if (cdb[1] || be32(cdb + 2) || cdb[8]) return check(s, 5, 0x24, 0);
        put32(bytes, s->lastBlock); put32(bytes + 4, s->blockSize);
        return reply(data, limit, bytes, 8, 8);
    }
    if (op == 0x9e) {
        if (cdb[1] != 0x10 || be32(cdb + 2) || be32(cdb + 6) || cdb[14])
            return check(s, 5, 0x24, 0);
        put32(bytes + 4, s->lastBlock); put32(bytes + 8, s->blockSize);
        return reply(data, limit, bytes, 32, be32(cdb + 10));
    }
    if (op == 0x1a || op == 0x5a) {
        if ((cdb[1] & ~8) || (cdb[2] & 0xc0) || cdb[3])
            return check(s, 5, 0x24, 0);
        page = cdb[2] & 63;
        if (page != 0 && page != 8 && page != 0x3f) return check(s, 5, 0x24, 0);
        header = op == 0x1a ? 4 : 8; length = header;
        bytes[op == 0x1a ? 2 : 3] = 0x80;
        if (!(cdb[1] & 8)) {
            bytes[op == 0x1a ? 3 : 7] = 8;
            put32(bytes + header + 4, s->blockSize); length += 8;
        }
        if (page == 8 || page == 0x3f) {
            bytes[length] = 8; bytes[length + 1] = 18;
            length += 20; /* no write cache */
        }
        if (op == 0x1a) { bytes[0] = (uhci_u8)(length - 1); allocation = cdb[4]; }
        else { bytes[1] = (uhci_u8)(length - 2); allocation = (cdb[7] << 8) | cdb[8]; }
        return reply(data, limit, bytes, length, allocation);
    }
    bzero(command, sizeof(command));
    bcopy(cdb, command, n);
    if (op == 8) {
        if (cdb[1] & 0xe0) return check(s, 5, 0x24, 0);
        lba = ((uhci_u32)(cdb[1] & 31) << 16) | (cdb[2] << 8) | cdb[3];
        blocks = cdb[4] ? cdb[4] : 256;
        bzero(command, sizeof(command)); command[0] = 0x28;
        put32(command + 2, lba); command[7] = (uhci_u8)(blocks >> 8);
        command[8] = (uhci_u8)blocks; command[9] = cdb[5]; n = 10;
    } else if (op == 0x28 || op == 0xa8) {
        if (cdb[1] & ~0x18) return check(s, 5, 0x24, 0);
        lba = be32(cdb + 2);
        blocks = op == 0x28 ? (uhci_u32)((cdb[7] << 8) | cdb[8]) : be32(cdb + 6);
    } else if (op == 0x88) {
        if (cdb[1] & ~0x18) return check(s, 5, 0x24, 0);
        if (be32(cdb + 2)) return check(s, 5, 0x21, 0);
        lba = be32(cdb + 6); blocks = be32(cdb + 10);
    }
    if ((op == 0xa8 || op == 0x88) && (cdb[op == 0xa8 ? 10 : 14] & ~31U))
        return check(s, 5, 0x24, 0);
    if (!blocks) return good();
    if (lba > s->lastBlock || blocks - 1 > s->lastBlock - lba)
        return check(s, 5, 0x21, 0);
    if (blocks > limit / s->blockSize) return check(s, 5, 0x24, 0);
    length = blocks * s->blockSize;
    if (op == 0xa8 || op == 0x88) {
        unsigned group = cdb[op == 0xa8 ? 10 : 14];
        /* Capacity is bounded to 32-bit LBAs and a request is at most 256
         * blocks. READ(10) can represent every accepted read, including on
         * older BOT devices which do not implement READ(12) or READ(16). */
        bzero(command, sizeof(command)); command[0] = 0x28;
        command[1] = cdb[1]; put32(command + 2, lba);
        command[6] = (uhci_u8)group;
        command[7] = (uhci_u8)(blocks >> 8); command[8] = (uhci_u8)blocks;
        command[9] = cdb[n - 1]; n = 10;
    }
    r = backend(s, bot, command, n, data, length, autoSense, deadline);
    if (!r.status && r.actual != length) r.status = USB_TRANSFER_ERROR;
    return r;
}
