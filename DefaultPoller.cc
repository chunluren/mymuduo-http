#include "Poller.h"
#include <stdlib.h>
#include "EPollPoller.h"

Poller* Poller::newDefaultPoller(EventLoop* loop)
{
    if (::getenv("MUDUO_USE_POLL"))
    {
        return nullptr;//new Poller(loop);
    }
    else
    {
        return new EPollPoller(loop);
    }
}