#ifndef USB_MASS_STORAGE_H
#define USB_MASS_STORAGE_H

#include "USBCore.h"

#define USB_STORAGE_TARGETS 4
#define USB_STORAGE_MAX_TRANSFER 16384UL
/* OPENSTEP's raw disk path can issue 128 KiB even when maxTransfer was 64 KiB.
 * A BOT command may span several bounded host-controller bulk transfers. */
#define USB_STORAGE_MAX_REQUEST 131072UL
#define USB_STORAGE_SENSE_BYTES 18

/* Separate from the original boolean EP0 interface used by HID. */
#define USB_TRANSFER_OK            0
#define USB_TRANSFER_STALL         1
#define USB_TRANSFER_TIMEOUT       2
#define USB_TRANSFER_DISCONNECTED  3
#define USB_TRANSFER_ERROR         4
#define USB_TRANSFER_PENDING       5
#define USB_STORAGE_CHECK          6
#define USB_STORAGE_INVALID        7

typedef struct USBMassStorageInterface {
    uhci_u8 number;
    uhci_u8 configuration;
    uhci_u8 bulkIn;
    uhci_u8 bulkOut;
    uhci_u16 inPacket;
    uhci_u16 outPacket;
} USBMassStorageInterface;

/* Deadlines are absolute monotonic milliseconds, including all BOT phases. */
typedef struct USBStorageTransport {
    int (*control)(void *, const USBSetupPacket *, void *, uhci_u32 *, uhci_u64);
    int (*bulk)(void *, uhci_u8, void *, uhci_u32, uhci_u32 *, uhci_u64);
    int (*clearHalt)(void *, uhci_u8, uhci_u64);
    uhci_u64 (*milliseconds)(void *);
} USBStorageTransport;

typedef struct USBMassStorage {
    const USBStorageTransport *transport;
    void *context;
    USBMassStorageInterface interface;
    uhci_u32 tag;
    int unusable;
} USBMassStorage;

typedef struct USBStorageResult {
    int status;
    uhci_u32 actual;
    uhci_u8 sense[USB_STORAGE_SENSE_BYTES];
    int senseValid;
} USBStorageResult;

int USBMassStorageFindInterface(const uhci_u8 *, uhci_u16,
                                 USBMassStorageInterface *);
int USBMassStorageGetMaxLUN(USBMassStorage *, uhci_u8 *, uhci_u64);
int USBMassStorageReset(USBMassStorage *, uhci_u64);
USBStorageResult USBMassStorageCommand(USBMassStorage *, const uhci_u8 *,
    unsigned, void *, uhci_u32, int, int, uhci_u64);

#endif
