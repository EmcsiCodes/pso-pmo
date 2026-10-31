#include "HAL9000.h"
#include "ex_event.h"
#include "thread_internal.h"
#include "cal_atomic.h"

#include "cpumu.h"

typedef struct EX_EVENT_SYSTEM_DATA
{
    LOCK            Lock;
    LIST_ENTRY      EventList;
} EX_EVENT_SYSTEM_DATA, *PEX_EVENT_SYSTEM_DATA;

static EX_EVENT_SYSTEM_DATA m_exEventData;

STATUS
ExEventSystemPreinit(
    void
    )
{
    memzero(&m_exEventData, sizeof(EX_EVENT_SYSTEM_DATA));
    LockInit(&m_exEventData.Lock);
    LockSetName(&m_exEventData.Lock, "EventList");
    InitializeListHead(&m_exEventData.EventList);

    return STATUS_SUCCESS;
}

void
ExEventSystemGetEventList(
    OUT PLOCK*             ListLock,
    OUT PLIST_ENTRY*       ListHead
    )
{
    ASSERT(ListLock != NULL);
    ASSERT(ListHead != NULL);

    *ListLock = &m_exEventData.Lock;
    *ListHead = &m_exEventData.EventList;
}

STATUS
ExEventInit(
    OUT     EX_EVENT*     Event,
    IN      EX_EVT_TYPE   EventType,
    IN      BOOLEAN       Signaled
    )
{
    INTR_STATE oldState;

    if (NULL == Event)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (EventType >= ExEventTypeReserved)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    LockInit(&Event->EventLock);
    LockSetName(&Event->EventLock, "EventLock");
    InitializeListHead(&Event->WaitingList);

    Event->EventType = EventType;
    AtomicExchange8(&Event->Signaled, Signaled );

    LockAcquire(&m_exEventData.Lock, &oldState);
    InsertTailList(&m_exEventData.EventList, &Event->AllList);
    LockRelease(&m_exEventData.Lock, oldState);

    return STATUS_SUCCESS;
}

void
ExEventSetName(
    INOUT       PEX_EVENT   Event,
    IN          char*       Name
    )
{
    INTR_STATE oldState;
    DWORD length;

    ASSERT(Event != NULL);
    ASSERT(Name != NULL);

    length = strlen_s(Name, 16);
    LockAcquire(&Event->EventLock, &oldState);
    if (length > 15)
    {
        length = 15;
        Event->Name[15] = 0;
    }
    strncpy(Event->Name, Name, length);
    LockRelease(&Event->EventLock, oldState);
}

void
ExEventSignal(
    INOUT   EX_EVENT*      Event
    )
{
    INTR_STATE oldState;
    PLIST_ENTRY pEntry;

    ASSERT(NULL != Event);

    pEntry = NULL;

    LockAcquire(&Event->EventLock, &oldState);
    AtomicExchange8(&Event->Signaled, TRUE);

    for(pEntry = RemoveHeadList(&Event->WaitingList);
        pEntry != &Event->WaitingList;
        pEntry = RemoveHeadList(&Event->WaitingList)
            )
    {
        PTHREAD pThreadToSignal = CONTAINING_RECORD(pEntry, THREAD, ReadyList);
        ThreadUnblock(pThreadToSignal);

        if (ExEventTypeSynchronization == Event->EventType)
        {
            // sorry, we only wake one thread
            // we must not clear the signal here, because the first thread which will
            // wake up will claar it :)
            break;
        }
    }

    LockRelease(&Event->EventLock, oldState);
}

void
ExEventClearSignal(
    INOUT   EX_EVENT*      Event
    )
{
    ASSERT( NULL != Event );

    AtomicExchange8(&Event->Signaled, FALSE);
}

void
ExEventWaitForSignal(
    INOUT   EX_EVENT*      Event
    )
{
    PTHREAD pCurrentThread;
    INTR_STATE dummyState;
    INTR_STATE oldState;
    BYTE newState;

    ASSERT(NULL != Event);

    pCurrentThread = GetCurrentThread(); //gets current thread pointer that is running on the current CPU

    ASSERT( NULL != pCurrentThread);

    newState = ExEventTypeNotification == Event->EventType;

    oldState = CpuIntrDisable();
    while (TRUE != AtomicCompareExchange8(&Event->Signaled, newState, TRUE))
    {
        LockAcquire(&Event->EventLock, &dummyState);
        InsertOrderedList(&Event->WaitingList, &pCurrentThread->ReadyList, _ThreadPriorityCopmpareListElems, NULL);
        ThreadTakeBlockLock();
        LockRelease(&Event->EventLock, dummyState);
        ThreadBlock();

        // if we are waiting for a notification type event => all threads
        // must be woken up => we have no reason to check the state of the
        // event again
        if (ExEventTypeNotification == Event->EventType)
        {
            break;
        }
    }

    CpuIntrSetState(oldState);
}

void
ExEventDestroy(
    INOUT     EX_EVENT*     Event
    )
{
    INTR_STATE oldState;

    ASSERT(Event != NULL);

    LockAcquire(&m_exEventData.Lock, &oldState);
    RemoveEntryList(&Event->AllList);
    LockRelease(&m_exEventData.Lock, oldState);

    LockDestroy(&Event->EventLock);
    memzero(Event, sizeof(EX_EVENT));
}