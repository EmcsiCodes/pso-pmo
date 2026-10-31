#include "HAL9000.h"
#include "thread_internal.h"
#include "mutex.h"

#define MUTEX_MAX_RECURSIVITY_DEPTH         MAX_BYTE

typedef struct _MUTEX_SYSTEM_DATA
{
    LOCK                  Lock;

    _Guarded_by_(Lock)
    LIST_ENTRY            MutexList;
} MUTEX_SYSTEM_DATA, *PMUTEX_SYSTEM_DATA;

static MUTEX_SYSTEM_DATA m_mutexData;

STATUS
MutexSystemPreinit(
    void
    )
{
    memzero(&m_mutexData, sizeof(MUTEX_SYSTEM_DATA));
    LockInit(&m_mutexData.Lock);
    LockSetName(&m_mutexData.Lock, "MutexList");
    InitializeListHead(&m_mutexData.MutexList);

    return STATUS_SUCCESS;
}

void
MutexSystemGetMutexList(
    OUT PLOCK*             ListLock,
    OUT PLIST_ENTRY*       ListHead
    )
{
    ASSERT(ListLock != NULL);
    ASSERT(ListHead != NULL);

    *ListLock = &m_mutexData.Lock;
    *ListHead = &m_mutexData.MutexList;
}

_No_competing_thread_
void
MutexInit(
    OUT         PMUTEX      Mutex,
    IN          BOOLEAN     Recursive
    )
{
    INTR_STATE oldState;

    ASSERT( NULL != Mutex );

    memzero(Mutex, sizeof(MUTEX));

    LockInit(&Mutex->MutexLock);
    LockSetName(&Mutex->MutexLock, "MutexLock");

    InitializeListHead(&Mutex->WaitingList);

    Mutex->MaxRecursivityDepth = Recursive ? MUTEX_MAX_RECURSIVITY_DEPTH : 1;

    LockAcquire(&m_mutexData.Lock, &oldState);
    InsertTailList(&m_mutexData.MutexList, &Mutex->AllList);
    LockRelease(&m_mutexData.Lock, oldState);
}

_No_competing_thread_
void
MutexSetName(
    INOUT       PMUTEX      Mutex,
    IN          char*       Name
    )
{
    INTR_STATE oldState;
    DWORD length;

    ASSERT(Mutex != NULL);
    ASSERT(Name != NULL);

    length = strlen_s(Name, 16);
    LockAcquire(&Mutex->MutexLock, &oldState);
    if (length > 15)
    {
        length = 15;
        Mutex->Name[15] = 0;
    }
    strncpy(Mutex->Name, Name, length);
    LockRelease(&Mutex->MutexLock, oldState);
}

ACQUIRES_EXCL_AND_REENTRANT_LOCK(*Mutex)
REQUIRES_NOT_HELD_LOCK(*Mutex)
void
MutexAcquire(
    INOUT       PMUTEX      Mutex
    )
{
    INTR_STATE dummyState;
    INTR_STATE oldState;
    PTHREAD pCurrentThread = GetCurrentThread();

    ASSERT( NULL != Mutex);
    ASSERT( NULL != pCurrentThread );

    if (pCurrentThread == Mutex->Holder)
    {
        ASSERT( Mutex->CurrentRecursivityDepth < Mutex->MaxRecursivityDepth );

        Mutex->CurrentRecursivityDepth++;
        return;
    }

    oldState = CpuIntrDisable();

    LockAcquire(&Mutex->MutexLock, &dummyState );
    if (NULL == Mutex->Holder)
    {
        Mutex->Holder = pCurrentThread;
        Mutex->CurrentRecursivityDepth = 1;
    }

    while (Mutex->Holder != pCurrentThread)
    {
        InsertOrderedList(&Mutex->WaitingList, &pCurrentThread->ReadyList, _ThreadPriorityCopmpareListElems, NULL);
        ThreadTakeBlockLock();
        LockRelease(&Mutex->MutexLock, dummyState);
        ThreadBlock();
        LockAcquire(&Mutex->MutexLock, &dummyState );
    }

    _Analysis_assume_lock_acquired_(*Mutex);

    LockRelease(&Mutex->MutexLock, dummyState);

    CpuIntrSetState(oldState);
}

RELEASES_EXCL_AND_REENTRANT_LOCK(*Mutex)
REQUIRES_EXCL_LOCK(*Mutex)
void
MutexRelease(
    INOUT       PMUTEX      Mutex
    )
{
    INTR_STATE oldState;
    PLIST_ENTRY pEntry;

    ASSERT(NULL != Mutex);
    ASSERT(GetCurrentThread() == Mutex->Holder);

    if (Mutex->CurrentRecursivityDepth > 1)
    {
        Mutex->CurrentRecursivityDepth--;
        return;
    }

    pEntry = NULL;

    LockAcquire(&Mutex->MutexLock, &oldState);

    pEntry = RemoveHeadList(&Mutex->WaitingList);
    if (pEntry != &Mutex->WaitingList)
    {
        PTHREAD pThread = CONTAINING_RECORD(pEntry, THREAD, ReadyList);

        // wakeup first thread
        Mutex->Holder = pThread;
        Mutex->CurrentRecursivityDepth = 1;
        ThreadUnblock(pThread);
    }
    else
    {
        Mutex->Holder = NULL;
    }

    _Analysis_assume_lock_released_(*Mutex);

    LockRelease(&Mutex->MutexLock, oldState);
}

_No_competing_thread_
void
MutexDestroy(
    INOUT       PMUTEX      Mutex
    )
{
    INTR_STATE oldState;

    ASSERT(Mutex != NULL);

    LockDestroy(&Mutex->MutexLock);

    LockAcquire(&m_mutexData.Lock, &oldState);
    RemoveEntryList(&Mutex->AllList);
    LockRelease(&m_mutexData.Lock, oldState);

    memzero(Mutex, sizeof(MUTEX));
}
