#ifndef UHCI_TYPES_H
#define UHCI_TYPES_H
#include "UHCIUSBNames.h"
typedef unsigned char uhci_u8;
typedef unsigned short uhci_u16;
typedef unsigned int uhci_u32;
typedef unsigned long long uhci_u64;
typedef signed char uhci_s8;
typedef char uhci_word_is_32_bits[sizeof(uhci_u32) == 4 ? 1 : -1];
#define UHCI_PAGE_SIZE 4096U
#define UHCI_MAX_DEVICES 16U
#define UHCI_MAX_PORTS 15U
#define UHCI_MAX_HUB_PORTS 15U
#define UHCI_TIMEOUT_MS 5000U
typedef struct UHCIDMA {
    void *allocation;
    void *virtualAddress;
    uhci_u32 physicalAddress;
    uhci_u32 allocationBytes;
    uhci_u32 bytes;
} UHCIDMA;
#endif
