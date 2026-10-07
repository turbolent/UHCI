#ifndef USB_STORAGE_SCSI_H
#define USB_STORAGE_SCSI_H
#include "USBMassStorage.h"

typedef struct USBStorageSCSIState {
    uhci_u32 generation;
    uhci_u32 removals;
    uhci_u32 blockSize;
    uhci_u32 lastBlock;
    int present;
    int removed;
    int attention;
    int identified;
    uhci_u8 deviceType;
    int capacityValid;
    int ejected;
    int preventRemoval;
    uhci_u8 sense[USB_STORAGE_SENSE_BYTES];
} USBStorageSCSIState;

void USBStorageSCSIObserve(USBStorageSCSIState *, uhci_u32, uhci_u32, int);
USBStorageResult USBStorageSCSIExecute(USBStorageSCSIState *, USBMassStorage *,
    const uhci_u8 *, unsigned, void *, uhci_u32, int, int, uhci_u64);
unsigned USBStorageCDBLength(uhci_u8);
#endif
