#include "UHCIInterruptState.h"
#include <string.h>
unsigned UHCIInterruptModeParse(const char *s)
{
    if (!s || !strcmp(s, "INTx")) return UHCI_MODE_INTX;
    if (!strcmp(s, "Polling")) return UHCI_MODE_POLLING;
    return UHCI_MODE_INVALID;
}
void UHCIInterruptLost(UHCIInterruptState *s)
{
    if (!s->resourcesOwned) return;
    if (!s->mmioLost) s->generation++;
    s->mmioLost = 1; s->stopping = 1; s->active = 0;
    s->interruptDebt = s->dmaDebt = 1;
    s->fenced = s->dmaFenced = 0;
    s->work = 1; s->ops->publish(s->context);
}
static uhci_u32 rd(UHCIInterruptState *s, unsigned reg)
{
    uhci_u32 v;
    if (s->mmioLost) return 0xffffffffU;
    v = s->ops->read(s->context, reg);
    if (v == 0xffffffffU) UHCIInterruptLost(s);
    return v;
}
static int wr(UHCIInterruptState *s, unsigned reg, uhci_u32 v)
{
    if (s->mmioLost) return 0;
    if (!s->ops->write(s->context, reg, v)) { UHCIInterruptLost(s); return 0; }
    return 1;
}
static void acknowledge(UHCIInterruptState *s, unsigned causes)
{
    if (wr(s, UHCI_STS, causes) && rd(s, UHCI_STS) != 0xffffffffU) {
        /* A new pending bit may appear before the next watchdog sample.
         * Completing an acknowledgement is progress even if that sample
         * never observes the brief quiet/fenced interval between IRQs. */
        s->watchdogPending = 0;
    }
}
/* UHCI USBSTS and USBINTR use different bit positions. Fatal causes
 * assert regardless of USBINTR; IOC/SPD share one USBSTS bit. */
unsigned UHCIInterruptCauses(unsigned status, unsigned enabled)
{
    unsigned causes=status & UHCI_STS_FATAL;
    if (enabled & 12) causes|=status & UHCI_STS_INT;
    if (enabled & 1) causes|=status & UHCI_STS_ERROR;
    if (enabled & 2) causes|=status & UHCI_STS_RESUME;
    return causes;
}
int UHCIInterruptFence(UHCIInterruptState *s, int dma)
{
    uhci_u32 before,desired,after;
    if (!s->resourcesOwned) return 0;
    s->interruptDebt=1; s->fenced=0; s->active=0;
    if (dma) { s->dmaDebt=1; s->dmaFenced=0; }
    if (!s->ops->gate(s->context,1)) return 0;
    s->fenced=1; s->interruptDebt=0;
    if (!dma) return 1;
    if (!s->ops->pciRead(s->context,&before) || (before&65535)==65535) return 0;
    desired=(before&65535)&~UHCI_PCI_MASTER;
    if (!s->ops->pciWrite(s->context,desired) || !s->ops->pciRead(s->context,&after) ||
        (after&65535)!=desired) return 0;
    s->dmaDebt=0; s->dmaFenced=1; return 1;
}
static int rearm(UHCIInterruptState *s, int freshForeign)
{
    if (!s->participant || !s->rearmDebt) return 1;
    if ((!freshForeign || s->mmioLost || s->stopping) && !s->fenced) return 0;
    if (!s->ops->rearm(s->context)) { s->failures++; return 0; }
    s->rearmDebt = 0;
    return 1;
}
static void publish(UHCIInterruptState *s, unsigned cause)
{
    s->pending |= cause; s->work = 1;
    s->ops->publish(s->context);
}
static void examine(UHCIInterruptState *s)
{
    unsigned status, enabled, causes;
    if (s->stopping || s->mmioLost) {
        /* Resolve IRQ proof independently: a BM-clear failure must not hold
         * an otherwise contained shared interrupt vote forever. */
        UHCIInterruptFence(s, 0);
        rearm(s, 0);
        return;
    }
    if (s->fenced) { rearm(s, 0); return; }
    status = rd(s, UHCI_STS); enabled = rd(s, UHCI_INTR);
    if (s->mmioLost) { UHCIInterruptFence(s, 0); rearm(s, 0); return; }
    causes = UHCIInterruptCauses(status, enabled);
    if (!causes) { s->foreign++; rearm(s, 1); return; }
    s->owned++;
    /* Gate the entire function, including fatal causes, before rearm. */
    UHCIInterruptFence(s, 0);
    wr(s, UHCI_INTR, 0);
    acknowledge(s, causes);
    publish(s, causes);
    if (s->mmioLost && !s->fenced) UHCIInterruptFence(s, 0);
    rearm(s, 0);
}
void UHCIInterruptCallback(UHCIInterruptState *s)
{
    if (!s->participant) return;
    s->callbacks++; s->rearmDebt = 1;
    if (!s->resourcesOwned) return;
    examine(s);
}
void UHCIInterruptRetry(UHCIInterruptState *s)
{
    s->retries++;
    if (!s->resourcesOwned) return;
    if (s->interruptDebt) UHCIInterruptFence(s, 0);
    if (s->rearmDebt) examine(s);
    if (s->dmaDebt) {
        /* Keep the previously established INTx proof if the independent DMA
         * transition fails. Re-establish it from current PCI readback. */
        if (!UHCIInterruptFence(s, 1)) UHCIInterruptFence(s, 0);
    }
}
unsigned UHCIInterruptTakeWork(UHCIInterruptState *s)
{
    unsigned pending = s->pending;
    s->pending = 0; s->work = 0; s->services++;
    return pending;
}
void UHCIInterruptWatchdog(UHCIInterruptState *s, uhci_u64 now)
{
    unsigned status;
    if (!s->resourcesOwned || s->mode != UHCI_MODE_INTX || !s->active || s->stopping) {
        s->watchdogPending = 0; return;
    }
    status = rd(s, UHCI_STS);
    if (s->mmioLost) return;
    if (!(UHCIInterruptCauses(status, UHCI_INTR_DEFAULT))) { s->watchdogPending = 0; return; }
    if (!s->watchdogPending || now < s->watchdogSince) {
        s->watchdogSince = now; s->watchdogPending = 1;
    } else if (now - s->watchdogSince >= 100) UHCIInterruptStop(s);
}
int UHCIInterruptRestore(UHCIInterruptState *s)
{
    unsigned status, generation = s->generation;
    if (!s->resourcesOwned || s->mode != UHCI_MODE_INTX || !s->participant || s->stopping ||
        s->mmioLost || s->admitted || s->work || s->interruptDebt || s->dmaDebt) return 0;
    if (!s->fenced && !UHCIInterruptFence(s, 0)) return 0;
    if (!rearm(s, 0)) return 0;
    status = rd(s, UHCI_STS);
    if (s->mmioLost) return 0;
    if (UHCIInterruptCauses(status, UHCI_INTR_DEFAULT)) {
        acknowledge(s, UHCIInterruptCauses(status, UHCI_INTR_DEFAULT));
        publish(s, UHCIInterruptCauses(status, UHCI_INTR_DEFAULT)); return 0;
    }
    if (!wr(s, UHCI_INTR, UHCI_INTR_DEFAULT) || rd(s, UHCI_INTR) != UHCI_INTR_DEFAULT ||
        generation != s->generation || s->mmioLost) goto failed;
    if (!s->ops->gate(s->context,0) || generation!=s->generation) goto failed;
    s->active = 1; s->fenced = 0;
    return 1;
failed:
    s->stopping = 1; s->dmaDebt = 1;
    UHCIInterruptFence(s, 0); publish(s, 0);
    return 0;
}
void UHCIInterruptPoll(UHCIInterruptState *s)
{
    unsigned status;
    if (!s->resourcesOwned || s->mode != UHCI_MODE_POLLING || s->stopping || s->mmioLost) return;
    status = rd(s, UHCI_STS);
    if (s->mmioLost) return;
    if (status & UHCI_STS_W1C) {
        acknowledge(s, status & UHCI_STS_W1C);
    }
    publish(s, UHCIInterruptCauses(status, UHCI_INTR_DEFAULT));
}
void UHCIInterruptStop(UHCIInterruptState *s)
{
    s->stopping = 1; s->active = 0; s->generation++;
    if (!s->resourcesOwned) { publish(s, 0); return; }
    s->interruptDebt = s->dmaDebt = 1;
    UHCIInterruptFence(s, 0); rearm(s, 0);
    if (!UHCIInterruptFence(s, 1)) UHCIInterruptFence(s, 0);
    publish(s, 0);
}
