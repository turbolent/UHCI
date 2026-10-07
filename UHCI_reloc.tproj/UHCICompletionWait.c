#include "UHCICompletionWait.h"

int UHCICompletionWorkReady(unsigned started, unsigned work, unsigned stopping,
    unsigned fatal, unsigned rescan, unsigned scheduleBusy)
{
    return started && (work || stopping || fatal || (rescan && !scheduleBusy));
}

void UHCICompletionWait(void *context, const UHCICompletionWaitOps *ops)
{
    int wait;
    ops->lock(context);
    wait = !ops->ready(context);
    if (wait) ops->prepare(context);
    ops->unlock(context);
    if (wait) ops->block(context);
}
