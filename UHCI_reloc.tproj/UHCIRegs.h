#ifndef UHCI_REGS_H
#define UHCI_REGS_H
#include "UHCITypes.h"
#define UHCI_CMD 0
#define UHCI_STS 2
#define UHCI_INTR 4
#define UHCI_FRNUM 6
#define UHCI_FLBASE 8
#define UHCI_SOF 12
#define UHCI_PORT(p) (0x10 + 2*((p)-1))
#define UHCI_CMD_RUN 1
#define UHCI_CMD_RESET 2
#define UHCI_CMD_CONFIG 0x40
#define UHCI_CMD_MAX64 0x80
#define UHCI_STS_INT 1
#define UHCI_STS_ERROR 2
#define UHCI_STS_RESUME 4
#define UHCI_STS_FATAL 0x18
#define UHCI_STS_HALTED 0x20
#define UHCI_STS_W1C 0x1f
#define UHCI_INTR_DEFAULT 0x0d
#define UHCI_PORT_CONNECT 1
#define UHCI_PORT_CSC 2
#define UHCI_PORT_ENABLE 4
#define UHCI_PORT_PEC 8
#define UHCI_PORT_RESUME 0x40
#define UHCI_PORT_LOW 0x100
#define UHCI_PORT_RESET 0x200
#define UHCI_PORT_OC 0x400
#define UHCI_PORT_OCC 0x800
#define UHCI_PORT_SUSPEND 0x1000
#define UHCI_PORT_CHANGES (UHCI_PORT_CSC|UHCI_PORT_PEC|UHCI_PORT_OCC)
#define UHCI_PORT_WRITABLE (UHCI_PORT_ENABLE|UHCI_PORT_RESET|UHCI_PORT_RESUME|UHCI_PORT_SUSPEND)
#define UHCI_PCI_IO 1
#define UHCI_PCI_MASTER 4
#define UHCI_PCI_INT_DISABLE 0x400
#define UHCI_LEGSUP 0xc0
#define UHCI_PIRQ 0x2000
#define UHCI_LEGACY_ENABLES 0x00bf
#define UHCI_LINK_END 1U
#define UHCI_LINK_QH 2U
#define UHCI_LINK_DEPTH 4U
#define UHCI_TD_ACTIVE (1U<<23)
#define UHCI_TD_STALL (1U<<22)
#define UHCI_TD_DBUF (1U<<21)
#define UHCI_TD_BABBLE (1U<<20)
#define UHCI_TD_NAK (1U<<19)
#define UHCI_TD_CRC (1U<<18)
#define UHCI_TD_BITSTUFF (1U<<17)
#define UHCI_TD_ERRORS (UHCI_TD_STALL|UHCI_TD_DBUF|UHCI_TD_BABBLE|UHCI_TD_CRC|UHCI_TD_BITSTUFF)
#define UHCI_TD_IOC (1U<<24)
#define UHCI_TD_RETRIES (3U<<27)
#define UHCI_TD_SPD (1U<<29)
#define UHCI_PID_SETUP 0x2d
#define UHCI_PID_IN 0x69
#define UHCI_PID_OUT 0xe1
#define UHCI_SPEED_FULL 0
#define UHCI_SPEED_LOW 1
#define UHCI_SPEED_HIGH 2
typedef struct UHCITD { volatile uhci_u32 link, status, token, buffer; } UHCITD;
typedef struct UHCIQH { volatile uhci_u32 link, element; uhci_u32 reserved[2]; } UHCIQH;
typedef char uhci_td_size[sizeof(UHCITD)==16 ? 1 : -1];
typedef char uhci_qh_size[sizeof(UHCIQH)==16 ? 1 : -1];
#endif
