#pragma once

#include "list.h"
#include "synch.h"
#include "thread.h"

typedef struct _MUTEX
{
    // DO NOT CHANGE THE NAMES OF THESE FIELDS
    LOCK                MutexLock;

    _Guarded_by_(MutexLock)
    char                Name[16];

    BYTE                CurrentRecursivityDepth;
    BYTE                MaxRecursivityDepth;

    _Guarded_by_(MutexLock)
    LIST_ENTRY          WaitingList;
    struct _THREAD*     Holder;

    LIST_ENTRY          AcquiredMutexList;

    LIST_ENTRY          AllList;
} MUTEX, *PMUTEX;

//******************************************************************************
// Function:     MutexSystemPreinit
// Description:  Initializes the mutex system.
// Returns:      STATUS
// Parameter:    void
//******************************************************************************
STATUS
MutexSystemPreinit(
    void
    );

//******************************************************************************
// Function:     MutexSystemGetMutexList
// Description:  Retrieves the global mutex list and its lock.
// Returns:      void
// Parameter:    OUT PLOCK* ListLock
// Parameter:    OUT PLIST_ENTRY* ListHead
//******************************************************************************
void
MutexSystemGetMutexList(
    OUT PLOCK*             ListLock,
    OUT PLIST_ENTRY*       ListHead
    );

//******************************************************************************
// Function:     MutexInit
// Description:  Initializes a mutex.
// Returns:      void
// Parameter:    OUT PMUTEX Mutex
// Parameter:    IN BOOLEAN Recursive - if TRUE the mutex may be acquired
//               several times by the same thread, else only once.
// NOTE:         A recursive mutex must be released as many times as it has been
//               acquired. It notifies the debugger.
//******************************************************************************
_No_competing_thread_
void
MutexInit(
    OUT         PMUTEX      Mutex,
    IN          BOOLEAN     Recursive
    );

//******************************************************************************
// Function:     MutexSetName
// Description:  Sets the name of a mutex.
// Returns:      void
// Parameter:    OUT PMUTEX Mutex
// Parameter:    IN char* Name - name of the mutex, maximum length 15 chars,
//               excluding the null terminator.
// NOTE:         It is not mandatory to call this function. The name helps
//               helps to identify it in the debugger.
//******************************************************************************
_No_competing_thread_
void
MutexSetName(
    INOUT       PMUTEX      Mutex,
    IN          char*       Name
    );

//******************************************************************************
// Function:     MutexAcquire
// Description:  Acquires a mutex. If the mutex is currently held the thread
//               is placed in a waiting list and its execution is blocked.
// Returns:      void
// Parameter:    INOUT PMUTEX Mutex
//******************************************************************************
ACQUIRES_EXCL_AND_REENTRANT_LOCK(*Mutex)
REQUIRES_NOT_HELD_LOCK(*Mutex)
void
MutexAcquire(
    INOUT       PMUTEX      Mutex
    );

//******************************************************************************
// Function:     MutexRelease
// Description:  Releases a mutex. If there is a thread on the waiting list it
//               will be unblocked and placed as the lock's holder - this will
//               ensure fairness.
// Returns:      void
// Parameter:    INOUT PMUTEX Mutex
//******************************************************************************
RELEASES_EXCL_AND_REENTRANT_LOCK(*Mutex)
REQUIRES_EXCL_LOCK(*Mutex)
void
MutexRelease(
    INOUT       PMUTEX      Mutex
    );

//******************************************************************************
// Function:     MutexDestroy
// Description:  Destroys a mutex.
// Returns:      void
// Parameter:    INOUT PMUTEX Mutex
// NOTE:         It should be called to notify the debugger.
//******************************************************************************
_No_competing_thread_
void
MutexDestroy(
    INOUT       PMUTEX      Mutex
    );


static
BOOLEAN
_MutexDonatePriority(
    IN PTHREAD Donor,
    IN PMUTEX FirstMutex
    );

void MutexDonationLock(
    OUT INTR_STATE* State
    );
void MutexDonationUnlock(
    IN INTR_STATE State
    );
void MutexRecomputeThreadPriority(
    INOUT PTHREAD Thread
    );