#include "HAL9000.h"
#include "thread_internal.h"
#include "mutex.h"
#include "thread.h"
#include "lock_common.h"

#define MUTEX_MAX_RECURSIVITY_DEPTH         MAX_BYTE

typedef struct _MUTEX_SYSTEM_DATA
{
    LOCK                  Lock;

    _Guarded_by_(Lock)
    LIST_ENTRY            MutexList;
} MUTEX_SYSTEM_DATA, *PMUTEX_SYSTEM_DATA;

static MUTEX_SYSTEM_DATA m_mutexData;

static LOCK m_donationLock;
static BOOLEAN _MutexDonatePriority(IN PTHREAD Donor, IN PMUTEX FirstMutex);
static PTHREAD _MutexRemoveBestWaiter(INOUT PMUTEX Mutex);

STATUS
MutexSystemPreinit(
    void
    )
{
    memzero(&m_mutexData, sizeof(MUTEX_SYSTEM_DATA));
    LockInit(&m_mutexData.Lock);
    LockSetName(&m_mutexData.Lock, "MutexList");
    InitializeListHead(&m_mutexData.MutexList);

    LockInit(&m_donationLock);
    LockSetName(&m_donationLock, "MutexDonation");

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
    InitializeListHead(&Mutex->AcquiredMutexList);

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
    INTR_STATE donationState;
    PTHREAD pCurrentThread = GetCurrentThread();
    BOOLEAN bPreemptOthers = FALSE;
    THREAD_PRIORITY donatedPriority = ThreadPriorityLowest;

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
        MutexDonationLock(&donationState);

        Mutex->Holder = pCurrentThread;
        Mutex->CurrentRecursivityDepth = 1;

        InsertTailList(&pCurrentThread->AcquiredMutexList, &Mutex->AcquiredMutexList);

        MutexDonationUnlock(donationState);
    }

    while (Mutex->Holder != pCurrentThread)
    {
        MutexDonationLock(&donationState);

        InsertOrderedList(&Mutex->WaitingList, &pCurrentThread->ReadyList, *_ThreadPriorityCopmpareListElems, NULL);
        pCurrentThread->WaitingMutex = Mutex;
        bPreemptOthers =  _MutexDonatePriority(pCurrentThread, Mutex);
        donatedPriority = pCurrentThread->Priority;

        MutexDonationUnlock(donationState);

        if (bPreemptOthers) {
            ThreadPreemptLowerPriorityCpus(donatedPriority);
        }

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
    INTR_STATE donationState;
    PTHREAD pCurrentThread = GetCurrentThread();
    PLIST_ENTRY pEntry;

    ASSERT(NULL != Mutex);
    ASSERT(pCurrentThread == Mutex->Holder);

    if (Mutex->CurrentRecursivityDepth > 1)
    {
        Mutex->CurrentRecursivityDepth--;
        return;
    }

    pEntry = NULL;

    LockAcquire(&Mutex->MutexLock, &oldState);
    MutexDonationLock(&donationState);

    RemoveEntryList(&Mutex->AcquiredMutexList);

    PTHREAD pNext = _MutexRemoveBestWaiter(Mutex);
    if(NULL != pNext)
    {
        Mutex->Holder = pNext;
        Mutex->CurrentRecursivityDepth = 1;

        pNext->WaitingMutex = NULL;
        InsertTailList(&pNext->AcquiredMutexList, &Mutex->AcquiredMutexList);

        MutexRecomputeThreadPriority(pNext);
    }
    else
    {
        Mutex->Holder = NULL;
    }

    MutexRecomputeThreadPriority(pCurrentThread);
    MutexDonationUnlock(donationState);

    if (pNext != NULL)
    {
        ThreadUnblock(pNext);
    }

    _Analysis_assume_lock_released_(*Mutex);

    LockRelease(&Mutex->MutexLock, oldState);

    ThreadYieldIfPreempted();
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

static
BOOLEAN
_MutexDonatePriority(
    IN PTHREAD Donor,
    IN PMUTEX FirstMutex)
{
    ASSERT(Donor != NULL);
    ASSERT(FirstMutex != NULL);

   THREAD_PRIORITY donorPriority = Donor->Priority;
   PMUTEX pMutex = FirstMutex;
   BOOLEAN bReadyChanged = FALSE;

   for(DWORD depth = 0; depth < 8 && pMutex != NULL; ++depth)
   {
        PTHREAD pHolder = pMutex->Holder;

        if (pHolder == NULL || pHolder->Priority >= donorPriority)
        {
            break;
        }

        pHolder->Priority = donorPriority;
        if(ThreadRepositionAfterPriorityChange(pHolder))
        {
            bReadyChanged = TRUE;
        }

        pMutex = pHolder->WaitingMutex;
   }

    return bReadyChanged;
}

void
MutexRecomputeThreadPriority(
    INOUT PTHREAD Thread
    )
{
    THREAD_PRIORITY best = Thread->RealPriority;

    for(PLIST_ENTRY pM = Thread->AcquiredMutexList.Flink; pM != &Thread->AcquiredMutexList; pM = pM->Flink)
    {
        PMUTEX pMutex = CONTAINING_RECORD(pM, MUTEX, AcquiredMutexList);
        
        for(PLIST_ENTRY pW = pMutex->WaitingList.Flink; pW != &pMutex->WaitingList; pW = pW->Flink)
        {
            PTHREAD pWaiter = CONTAINING_RECORD(pW, THREAD, ReadyList);
            if(pWaiter->Priority > best)
            {
                best = pWaiter->Priority;
            }
        }
    }

    Thread->Priority = best;
}

static
PTHREAD
_MutexRemoveBestWaiter(
    INOUT PMUTEX Mutex
)
{
    PLIST_ENTRY pBestEntry = NULL;

    for(PLIST_ENTRY pE = Mutex->WaitingList.Flink; pE != &Mutex->WaitingList; pE = pE->Flink)
    {
        if(pBestEntry == NULL)
        {
            pBestEntry = pE;
        }
        else
        {
            PTHREAD pCurrent = CONTAINING_RECORD(pE, THREAD, ReadyList);
            PTHREAD pBest = CONTAINING_RECORD(pBestEntry, THREAD, ReadyList);

            if(pCurrent->Priority > pBest->Priority)
            {
                pBestEntry = pE;
            }
        }
    }

    if(pBestEntry == NULL)
    {
        return NULL;
    }
    
    RemoveEntryList(pBestEntry);
    return CONTAINING_RECORD(pBestEntry, THREAD, ReadyList);
}

void
MutexDonationLock(
    OUT INTR_STATE* State
    )
{
    LockAcquire(&m_donationLock, State);
}

void
MutexDonationUnlock(
    IN INTR_STATE State
    )
{
    LockRelease(&m_donationLock, State);
}