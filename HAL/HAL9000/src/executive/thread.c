#include "HAL9000.h"
#include "thread_internal.h"
#include "synch.h"
#include "cpumu.h"
#include "ex_event.h"
#include "core.h"
#include "vmm.h"
#include "exe_loader.h"
#include "process_internal.h"
#include "isr.h"
#include "gdtmu.h"
#include "pe_exports.h"
#include "cal_annotate.h"
#include "cal_atomic.h"
#include "cal_assembly.h"
#include "cal_seh.h"
#include "smp.h"

#define TID_INCREMENT               4

#define THREAD_TIME_SLICE           1

extern void MS_ABI ThreadStart();

typedef
void
(CDECL MS_ABI FUNC_ThreadSwitch)(
    OUT_PTR         PVOID*          OldStack,
    IN              PVOID           NewStack
    );

extern FUNC_ThreadSwitch            ThreadSwitch;

typedef struct _THREAD_SYSTEM_DATA
{
    LOCK                AllThreadsLock;

    // DO NOT CHANGE THE NAME OF THIS FIELD
    _Guarded_by_(AllThreadsLock)
    LIST_ENTRY          AllThreadsList;

    LOCK                ReadyThreadsLock;

    _Guarded_by_(ReadyThreadsLock)
    LIST_ENTRY          ReadyThreadsList;
} THREAD_SYSTEM_DATA, *PTHREAD_SYSTEM_DATA;

// DO NOT CHANGE THE NAME OF THIS VARIABLE
static THREAD_SYSTEM_DATA m_threadSystemData;

ALWAYS_INLINE
static
TID
_ThreadSystemGetNextTid(
    void
    )
{
    static volatile TID __currentTid = 0;

    return AtomicExchangeAdd64(&__currentTid, TID_INCREMENT);
}

static
STATUS
_ThreadInit(
    IN_Z        char*               Name,
    IN          THREAD_PRIORITY     Priority,
    OUT_PTR     PTHREAD*            Thread,
    IN          BOOLEAN             AllocateKernelStack
    );

static
STATUS
_ThreadSetupInitialState(
    IN      PTHREAD             Thread,
    IN      PVOID               StartFunction,
    IN      QWORD               FirstArgument,
    IN      QWORD               SecondArgument,
    IN      BOOLEAN             KernelStack
    );

static
STATUS
_ThreadSetupMainThreadUserStack(
    IN      PVOID               InitialStack,
    OUT     PVOID*              ResultingStack,
    IN      PPROCESS            Process
    );


REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
RELEASES_EXCL_AND_NON_REENTRANT_LOCK(m_threadSystemData.ReadyThreadsLock)
static
void
_ThreadSchedule(
    void
    );

REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
RELEASES_EXCL_AND_NON_REENTRANT_LOCK(m_threadSystemData.ReadyThreadsLock)
void
MS_ABI
ThreadCleanupPostSchedule(
    void
    );

REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
static
_Ret_notnull_
PTHREAD
_ThreadGetReadyThread(
    void
    );

static
void
_ThreadForcedExit(
    void
    );

static
void
_ThreadReference(
    INOUT   PTHREAD                 Thread
    );

static
void
_ThreadDereference(
    INOUT   PTHREAD                 Thread
    );

static FUNC_FreeFunction            _ThreadDestroy;

static
MS_ABI
void
_ThreadKernelFunction(
    IN      PFUNC_ThreadStart       Function,
    IN_OPT  PVOID                   Context
    );

static FUNC_ThreadStart     _IdleThread;

static
BOOLEAN
_ThreadShouldPreemptCurrent(
    void
    );

static FUNC_IpcProcessEvent     _ThreadPreemptCheckIpi;

void
_No_competing_thread_
ThreadSystemPreinit(
    void
    )
{
    memzero(&m_threadSystemData, sizeof(THREAD_SYSTEM_DATA));

    InitializeListHead(&m_threadSystemData.AllThreadsList);
    LockInit(&m_threadSystemData.AllThreadsLock);
    LockSetName(&m_threadSystemData.AllThreadsLock, "AllThreads");

    InitializeListHead(&m_threadSystemData.ReadyThreadsList);
    LockInit(&m_threadSystemData.ReadyThreadsLock);
    LockSetName(&m_threadSystemData.ReadyThreadsLock, "RdyThreads");
}

void
ThreadSystemGetThreadList(
    OUT PLOCK*              ListLock,
    OUT PLIST_ENTRY*        ListHead
    )
{
    ASSERT(ListLock != NULL);
    ASSERT(ListHead != NULL);

    *ListLock = &m_threadSystemData.AllThreadsLock;
    *ListHead = &m_threadSystemData.AllThreadsList;
}

STATUS
ThreadSystemInitMainForCurrentCPU(
    void
    )
{
    STATUS status;
    PPCPU pCpu;
    char mainThreadName[MAX_PATH];
    PTHREAD pThread;
    PPROCESS pProcess;

    LOG_FUNC_START;

    status = STATUS_SUCCESS;
    pCpu = GetCurrentPcpu();
    pThread = NULL;
    pProcess = ProcessRetrieveSystemProcess();

    ASSERT( NULL != pCpu );

    snprintf( mainThreadName, MAX_PATH, "%s-%02x", "main", pCpu->ApicId );

    status = _ThreadInit(mainThreadName, ThreadPriorityDefault, &pThread, FALSE);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_ThreadInit", status );
        return status;
    }
    LOGPL("_ThreadInit succeeded\n");

    pThread->InitialStackBase = pCpu->StackTop;
    pThread->StackSize = pCpu->StackSize;

    pThread->State = ThreadStateRunning;
    SetCurrentThread(pThread);

    // In case of the main thread of the BSP the process will be NULL so we need to handle that case
    // When the system process will be initialized it will insert into its thread list the current thread (which will
    // be the main thread of the BSP)
    if (pProcess != NULL)
    {
        ProcessInsertThreadInList(pProcess, pThread);
    }

    LOG_FUNC_END;

    return status;
}

STATUS
ThreadSystemInitIdleForCurrentCPU(
    void
    )
{
    EX_EVENT idleStarted;
    STATUS status;
    PPCPU pCpu;
    char idleThreadName[MAX_PATH];
    PTHREAD idleThread;

    ASSERT( INTR_OFF == CpuIntrGetState() );

    LOG_FUNC_START_THREAD;

    status = STATUS_SUCCESS;
    pCpu = GetCurrentPcpu();

    ASSERT(NULL != pCpu);

    status = ExEventInit(&idleStarted, ExEventTypeSynchronization, FALSE);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("EvtInitialize", status);
        return status;
    }
    LOGPL("EvtInitialize succeeded\n");
    ExEventSetName(&idleStarted, "IdleStarted");

    snprintf(idleThreadName, MAX_PATH, "%s-%02x", "idle", pCpu->ApicId);

    // create idle thread
    status = ThreadCreate(idleThreadName,
                          ThreadPriorityDefault,
                          _IdleThread,
                          &idleStarted,
                          &idleThread
                          );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("ThreadCreate", status);
        return status;
    }
    LOGPL("ThreadCreate for IDLE thread succeeded\n");

    ThreadCloseHandle(idleThread);
    idleThread = NULL;

    LOGPL("About to enable interrupts\n");

    // lets enable some interrupts :)
    CpuIntrEnable();

    LOGPL("Interrupts enabled :)\n");

    // wait for idle thread
    LOG_TRACE_THREAD("Waiting for idle thread signal\n");
    ExEventWaitForSignal(&idleStarted);
    LOG_TRACE_THREAD("Received idle thread signal\n");

    ExEventDestroy(&idleStarted);

    LOG_FUNC_END_THREAD;

    return status;
}

STATUS
ThreadCreate(
    IN_Z        char*               Name,
    IN          THREAD_PRIORITY     Priority,
    IN          PFUNC_ThreadStart   Function,
    IN_OPT      PVOID               Context,
    OUT_PTR     PTHREAD*            Thread
    )
{
    return ThreadCreateEx(Name,
                          Priority,
                          Function,
                          Context,
                          Thread,
                          ProcessRetrieveSystemProcess());
}

STATUS
ThreadCreateEx(
    IN_Z        char*               Name,
    IN          THREAD_PRIORITY     Priority,
    IN          PFUNC_ThreadStart   Function,
    IN_OPT      PVOID               Context,
    OUT_PTR     PTHREAD*            Thread,
    INOUT       struct _PROCESS*    Process
    )
{
    STATUS status;
    PTHREAD pThread;
    PPCPU pCpu;
    BOOLEAN bProcessIniialThread;
    PVOID pStartFunction;
    QWORD firstArg;
    QWORD secondArg;

    if (NULL == Name)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (NULL == Function)
    {
        return STATUS_INVALID_PARAMETER3;
    }

    if (NULL == Thread)
    {
        return STATUS_INVALID_PARAMETER5;
    }

    if (NULL == Process)
    {
        return STATUS_INVALID_PARAMETER6;
    }

    status = STATUS_SUCCESS;
    pThread = NULL;
    pCpu = GetCurrentPcpu();
    bProcessIniialThread = FALSE;
    pStartFunction = NULL;
    firstArg = 0;
    secondArg = 0;

    ASSERT(NULL != pCpu);

    status = _ThreadInit(Name, Priority, &pThread, TRUE);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_ThreadInit", status);
        return status;
    }

    ProcessInsertThreadInList(Process, pThread);

    // the reference must be done outside _ThreadInit
    _ThreadReference(pThread);

    PVOID entryAddress;
    ExecutableLoaderGetEntryPoint(Process->LoaderContext, &entryAddress);

    if (!Process->PagingData->Data.KernelSpace)
    {
        // Create user-mode stack
        pThread->UserStack = MmuAllocStack(STACK_DEFAULT_SIZE,
                                           TRUE,
                                           FALSE,
                                           Process);
        if (pThread->UserStack == NULL)
        {
            status = STATUS_MEMORY_CANNOT_BE_COMMITED;
            LOG_FUNC_ERROR_ALLOC("MmuAllocStack", STACK_DEFAULT_SIZE);
            return status;
        }

        bProcessIniialThread = (Function == entryAddress);

        // We are the first thread => we must pass the argc and argv parameters
        // and the whole command line which spawned the process
        if (bProcessIniialThread)
        {
            // It's one because we already incremented it when we called ProcessInsertThreadInList earlier
            ASSERT(Process->NumberOfThreads == 1);

            status = _ThreadSetupMainThreadUserStack(pThread->UserStack,
                                                     &pThread->UserStack,
                                                     Process);
            if (!SUCCEEDED(status))
            {
                LOG_FUNC_ERROR("_ThreadSetupUserStack", status);
                return status;
            }
        }
        else
        {
            pThread->UserStack = (PVOID) PtrDiff(pThread->UserStack, SHADOW_STACK_SIZE + sizeof(PVOID));
        }

        pStartFunction = (PVOID) (bProcessIniialThread ? entryAddress : (PVOID) Function);
        firstArg       = (QWORD) (bProcessIniialThread ? Process->NumberOfArguments : (QWORD) Context);
        secondArg      = (QWORD) (bProcessIniialThread ? PtrOffset(pThread->UserStack, SHADOW_STACK_SIZE + sizeof(PVOID)) : 0);
    }
    else
    {
        // Kernel mode

        // warning C4152: nonstandard extension, function/data pointer conversion in expression
MSVC_WARNING_SUPPRESS(4152)
        pStartFunction = _ThreadKernelFunction;

        firstArg =  (QWORD) Function;
        secondArg = (QWORD) Context;
    }

    status = _ThreadSetupInitialState(pThread,
                                      pStartFunction,
                                      firstArg,
                                      secondArg,
                                      Process->PagingData->Data.KernelSpace);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_ThreadSetupInitialState", status);
        return status;
    }

    if (NULL == pCpu->ThreadData.IdleThread)
    {
        pThread->State = ThreadStateReady;

        // this is the IDLE thread creation
        pCpu->ThreadData.IdleThread = pThread;
    }
    else
    {
        ThreadUnblock(pThread);
    }

    *Thread = pThread;

    ThreadYieldIfPreempted();

    return status;
}

void
ThreadTick(
    void
    )
{
    PPCPU pCpu = GetCurrentPcpu();
    PTHREAD pThread = GetCurrentThread();

    ASSERT( INTR_OFF == CpuIntrGetState());
    ASSERT( NULL != pCpu);

    LOG_TRACE_THREAD("Thread tick\n");
    if (pCpu->ThreadData.IdleThread == pThread)
    {
        pCpu->ThreadData.IdleTicks++;
    }
    else
    {
        pCpu->ThreadData.KernelTicks++;
    }
    pThread->TickCountCompleted++;

    if (++pCpu->ThreadData.RunningThreadTicks >= THREAD_TIME_SLICE)
    {
        LOG_TRACE_THREAD("Will yield on return\n");
        pCpu->ThreadData.YieldOnInterruptReturn = TRUE;
    }
}

void
ThreadYield(
    void
    )
{
    INTR_STATE dummyState;
    INTR_STATE oldState;
    PTHREAD pThread = GetCurrentThread();
    PPCPU pCpu;
    BOOLEAN bForcedYield;

    ASSERT( NULL != pThread);

    oldState = CpuIntrDisable();

    pCpu = GetCurrentPcpu();

    ASSERT( NULL != pCpu );

    bForcedYield = pCpu->ThreadData.YieldOnInterruptReturn;
    pCpu->ThreadData.YieldOnInterruptReturn = FALSE;

    if (THREAD_FLAG_FORCE_TERMINATE_PENDING == AtomicAnd32(&pThread->Flags, MAX_DWORD))
    {
        _ThreadForcedExit();
        NOT_REACHED;
    }

    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &dummyState);
    if (pThread != pCpu->ThreadData.IdleThread)
    {
        InsertOrderedList(&m_threadSystemData.ReadyThreadsList, &pThread->ReadyList, *_ThreadPriorityCopmpareListElems, NULL);
    }
    if (!bForcedYield)
    {
        pThread->TickCountEarly++;
    }
    pThread->State = ThreadStateReady;
    _ThreadSchedule();
    ASSERT( !LockIsOwner(&m_threadSystemData.ReadyThreadsLock));
    LOG_TRACE_THREAD("Returned from _ThreadSchedule\n");

    CpuIntrSetState(oldState);
}

void
ThreadBlock(
    void
    )
{
    INTR_STATE oldState;
    PTHREAD pCurrentThread;

    pCurrentThread = GetCurrentThread();

    ASSERT( INTR_OFF == CpuIntrGetState());
    ASSERT(LockIsOwner(&pCurrentThread->BlockLock));

    if (THREAD_FLAG_FORCE_TERMINATE_PENDING == AtomicAnd32(&pCurrentThread->Flags, MAX_DWORD))
    {
        _ThreadForcedExit();
        NOT_REACHED;
    }

    pCurrentThread->TickCountEarly++;
    pCurrentThread->State = ThreadStateBlocked;
    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &oldState);
    _ThreadSchedule();
    ASSERT( !LockIsOwner(&m_threadSystemData.ReadyThreadsLock));
}

void
ThreadUnblock(
    IN      PTHREAD              Thread
    )
{
    INTR_STATE oldState;
    INTR_STATE dummyState;

    ASSERT(NULL != Thread);

    LockAcquire(&Thread->BlockLock, &oldState);

    ASSERT(ThreadStateBlocked == Thread->State);

    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &dummyState);
    InsertOrderedList(&m_threadSystemData.ReadyThreadsList, &Thread->ReadyList, _ThreadPriorityCopmpareListElems, NULL);
    Thread->State = ThreadStateReady;
    LockRelease(&m_threadSystemData.ReadyThreadsLock, dummyState );
    LockRelease(&Thread->BlockLock, oldState);

    // Make the thread yield later if it has to
    if (_ThreadShouldPreemptCurrent()) {
        GetCurrentPcpu()->ThreadData.YieldOnInterruptReturn = TRUE;
    }

    // // Notify other CPUs to consider switching
    SmpSendGenericIpi(_ThreadPreemptCheckIpi, NULL, NULL, NULL, FALSE);
}

void
ThreadExit(
    IN      STATUS              ExitStatus
    )
{
    PTHREAD pThread;
    INTR_STATE oldState;

    LOG_FUNC_START_THREAD;

    pThread = GetCurrentThread();

    CpuIntrDisable();

    if (LockIsOwner(&pThread->BlockLock))
    {
        LockRelease(&pThread->BlockLock, INTR_OFF);
    }

    pThread->State = ThreadStateDying;
    pThread->ExitStatus = ExitStatus;
    ExEventSignal(&pThread->TerminationEvt);

    ProcessNotifyThreadTermination(pThread);

    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &oldState);
    _ThreadSchedule();
    NOT_REACHED;
}

BOOLEAN
ThreadYieldOnInterrupt(
    void
    )
{
    return GetCurrentPcpu()->ThreadData.YieldOnInterruptReturn;
}

void
ThreadTakeBlockLock(
    void
    )
{
    INTR_STATE dummyState;

    LockAcquire(&GetCurrentThread()->BlockLock, &dummyState);
}

void
ThreadWaitForTermination(
    IN      PTHREAD             Thread,
    OUT     STATUS*             ExitStatus
    )
{
    ASSERT( NULL != Thread );
    ASSERT( NULL != ExitStatus);

    ExEventWaitForSignal(&Thread->TerminationEvt);

    *ExitStatus = Thread->ExitStatus;
}

void
ThreadCloseHandle(
    INOUT   PTHREAD             Thread
    )
{
    ASSERT( NULL != Thread);

    _ThreadDereference(Thread);
}

void
ThreadTerminate(
    INOUT   PTHREAD             Thread
    )
{
    ASSERT( NULL != Thread );

    // it's not a problem if the thread already finished
    AtomicOr32(&Thread->Flags, THREAD_FLAG_FORCE_TERMINATE_PENDING );
}

const
char*
ThreadGetName(
    IN_OPT  PTHREAD             Thread
    )
{
    PTHREAD pThread = (NULL != Thread) ? Thread : GetCurrentThread();

    return (NULL != pThread) ? pThread->Name : "";
}

TID
ThreadGetId(
    IN_OPT  PTHREAD             Thread
    )
{
    PTHREAD pThread = (NULL != Thread) ? Thread : GetCurrentThread();

    return (NULL != pThread) ? pThread->Id : 0;
}

THREAD_PRIORITY
ThreadGetPriority(
    IN_OPT  PTHREAD             Thread
    )
{
    PTHREAD pThread = (NULL != Thread) ? Thread : GetCurrentThread();

    return (NULL != pThread) ? pThread->Priority : 0;
}

void
ThreadSetPriority(
    IN      THREAD_PRIORITY     NewPriority
    )
{
    ASSERT(ThreadPriorityLowest <= NewPriority && NewPriority <= ThreadPriorityMaximum);

    INTR_STATE donationState;
    
    MutexDonationLock(&donationState);    
    GetCurrentThread()->RealPriority = NewPriority;
    MutexRecomputeThreadPriority(GetCurrentThread());
    MutexDonationUnlock(donationState);
}

STATUS
ThreadExecuteForEachThreadEntry(
    IN      PFUNC_ListFunction  Function,
    IN_OPT  PVOID               Context
    )
{
    STATUS status;
    INTR_STATE oldState;

    if (NULL == Function)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    status = STATUS_SUCCESS;

    LockAcquire(&m_threadSystemData.AllThreadsLock, &oldState);
    status = ForEachElementExecute(&m_threadSystemData.AllThreadsList,
                                   Function,
                                   Context,
                                   FALSE
                                   );
    LockRelease(&m_threadSystemData.AllThreadsLock, oldState );

    return status;
}

void
SetCurrentThread(
    IN      PTHREAD     Thread
    )
{
    PPCPU pCpu;

    AsmWriteMsr(IA32_FS_BASE_MSR, (QWORD) Thread);

    pCpu = GetCurrentPcpu();
    ASSERT(pCpu != NULL);

    pCpu->ThreadData.CurrentThread = Thread->Self;
    if (NULL != Thread->Self)
    {
        pCpu->StackTop = Thread->InitialStackBase;
        pCpu->StackSize = Thread->StackSize;
        pCpu->Tss.Rsp[0] = (QWORD) Thread->InitialStackBase;
    }
}

static
STATUS
_ThreadInit(
    IN_Z        char*               Name,
    IN          THREAD_PRIORITY     Priority,
    OUT_PTR     PTHREAD*            Thread,
    IN          BOOLEAN             AllocateKernelStack
    )
{
    STATUS status;
    PTHREAD pThread;
    DWORD nameLen;
    PVOID pStack;
    INTR_STATE oldIntrState;

    LOG_FUNC_START;

    ASSERT(NULL != Name);
    ASSERT(NULL != Thread);
    ASSERT_INFO(ThreadPriorityLowest <= Priority && Priority <= ThreadPriorityMaximum,
                "Priority is 0x%x\n", Priority);

    status = STATUS_SUCCESS;
    pThread = NULL;
    nameLen = strlen(Name);
    pStack = NULL;

    __try
    {
        pThread = ExAllocatePoolWithTag(PoolAllocateZeroMemory, sizeof(THREAD), HEAP_THREAD_TAG, 0);
        if (NULL == pThread)
        {
            LOG_FUNC_ERROR_ALLOC("HeapAllocatePoolWithTag", sizeof(THREAD));
            status = STATUS_HEAP_INSUFFICIENT_RESOURCES;
            __leave;
        }

        RfcPreInit(&pThread->RefCnt);

        status = RfcInit(&pThread->RefCnt, _ThreadDestroy, NULL);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("RfcInit", status);
            __leave;
        }

        pThread->Self = pThread;

        status = ExEventInit(&pThread->TerminationEvt, ExEventTypeNotification, FALSE);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("ExEventInit", status);
            __leave;
        }
        ExEventSetName(&pThread->TerminationEvt, "ThrTermination");

        if (AllocateKernelStack)
        {
            pStack = MmuAllocStack(STACK_DEFAULT_SIZE, TRUE, FALSE, NULL);
            if (NULL == pStack)
            {
                LOG_FUNC_ERROR_ALLOC("MmuAllocStack", STACK_DEFAULT_SIZE);
                status = STATUS_MEMORY_CANNOT_BE_COMMITED;
                __leave;
            }
            pThread->Stack = pStack;
            pThread->InitialStackBase = pStack;
            pThread->StackSize = STACK_DEFAULT_SIZE;
        }

        pThread->Name = ExAllocatePoolWithTag(PoolAllocateZeroMemory, sizeof(char)*(nameLen + 1), HEAP_THREAD_TAG, 0);
        if (NULL == pThread->Name)
        {
            LOG_FUNC_ERROR_ALLOC("HeapAllocatePoolWithTag", sizeof(char)*(nameLen + 1));
            status = STATUS_HEAP_INSUFFICIENT_RESOURCES;
            __leave;
        }

        strcpy(pThread->Name, Name);

        pThread->Id = _ThreadSystemGetNextTid();
        pThread->State = ThreadStateBlocked;
        pThread->Priority = Priority;
        pThread->RealPriority = Priority;
        pThread->WaitingMutex = NULL;
        InitializeListHead(&pThread->AcquiredMutexList);

        LockInit(&pThread->BlockLock);
        LockSetName(&pThread->BlockLock, "ThrBlock");

        LockAcquire(&m_threadSystemData.AllThreadsLock, &oldIntrState);
        InsertTailList(&m_threadSystemData.AllThreadsList, &pThread->AllList);
        LockRelease(&m_threadSystemData.AllThreadsLock, oldIntrState);
    }
    __finally
    {
        if (!SUCCEEDED(status))
        {
            if (NULL != pThread)
            {
                _ThreadDereference(pThread);
                pThread = NULL;
            }
        }

        *Thread = pThread;

        LOG_FUNC_END;
    }

    return status;
}

//  STACK TOP
//  -----------------------------------------------------------------
//  |                                                               |
//  |       Shadow Space                                            |
//  |                                                               |
//  |                                                               |
//  -----------------------------------------------------------------
//  |     Dummy Function RA                                         |
//  ---------------------------------------------------------------------------------
//  |     SS     = DS64Supervisor        | DS64Usermode             |               |
//  -----------------------------------------------------------------               |
//  |     RSP    = &(Dummy Function RA)  | Thread->UserStack        |               |
//  -----------------------------------------------------------------               |
//  |     RFLAGS = RFLAGS_IF | RFLAGS_RESERVED                      |   Interrupt   |
//  -----------------------------------------------------------------     Stack     |
//  |     CS     = CS64Supervisor        | CS64Usermode             |               |
//  -----------------------------------------------------------------               |
//  |     RIP    = _ThreadKernelFunction | AddressOfEntryPoint      |               |
//  ---------------------------------------------------------------------------------
//  |     Thread Start Function                                     |
//  -----------------------------------------------------------------
//  |                                                               |
//  |       PROCESSOR_STATE                                         |
//  |                                                               |
//  |                                                               |
//  -----------------------------------------------------------------
//  STACK BASE <- RSP at ThreadSwitch
static
STATUS
_ThreadSetupInitialState(
    IN      PTHREAD             Thread,
    IN      PVOID               StartFunction,
    IN      QWORD               FirstArgument,
    IN      QWORD               SecondArgument,
    IN      BOOLEAN             KernelStack
    )
{
    STATUS status;
    PVOID* pStack;
    PCOMPLETE_PROCESSOR_STATE pState;
    PINTERRUPT_STACK pIst;

    ASSERT( NULL != Thread );
    ASSERT( NULL != StartFunction);

    status = STATUS_SUCCESS;

    pStack = (PVOID*) Thread->Stack;

    // The kernel function has to have a shadow space and a dummy RA
    pStack = pStack - ( 4 + 1 );

    pStack = (PVOID*) PtrDiff(pStack, sizeof(INTERRUPT_STACK));

    // setup pseudo-interrupt stack
    pIst = (PINTERRUPT_STACK) pStack;

    pIst->Rip = (QWORD) StartFunction;
    if (KernelStack)
    {
        pIst->CS = GdtMuGetCS64Supervisor();
        pIst->Rsp = (QWORD)(pIst + 1);
        pIst->SS = GdtMuGetDS64Supervisor();
    }
    else
    {
        ASSERT(Thread->UserStack != NULL);

        pIst->CS = GdtMuGetCS64Usermode() | RING_THREE_PL;
        pIst->Rsp = (QWORD) Thread->UserStack;
        pIst->SS = GdtMuGetDS64Usermode() | RING_THREE_PL;
    }

    pIst->RFLAGS = RFLAGS_INTERRUPT_FLAG_BIT | RFLAGS_RESERVED_BIT;

    pStack = pStack - 1;

    // warning C4054: 'type cast': from function pointer 'void (CDECL *)(const PFUNC_ThreadStart,const PVOID)' to data pointer 'PVOID'
MSVC_WARNING_SUPPRESS(4054)
    *pStack = (PVOID) ThreadStart;

    pStack = (PVOID*) PtrDiff(pStack, sizeof(COMPLETE_PROCESSOR_STATE));
    pState = (PCOMPLETE_PROCESSOR_STATE) pStack;

    memzero(pState, sizeof(COMPLETE_PROCESSOR_STATE));
    pState->RegisterArea.RegisterValues[RegisterRcx] = FirstArgument;
    pState->RegisterArea.RegisterValues[RegisterRdx] = SecondArgument;

    Thread->Stack = pStack;

    return STATUS_SUCCESS;
}


//  USER STACK TOP
//  -----------------------------------------------------------------
//  |                       Argument N-1                            |
//  -----------------------------------------------------------------
//  |                          ...                                  |
//  -----------------------------------------------------------------
//  |                       Argument 0                              |
//  -----------------------------------------------------------------
//  |                 argv[N-1] = &(Argument N-1)                   |
//  -----------------------------------------------------------------
//  |                          ...                                  |
//  -----------------------------------------------------------------
//  |                 argv[0] = &(Argument 0)                       |
//  -----------------------------------------------------------------
//  |                 Dummy 4th Arg = 0xDEADBEEF                    |
//  -----------------------------------------------------------------
//  |                 Dummy 3rd Arg = 0xDEADBEEF                    |
//  -----------------------------------------------------------------
//  |                 argv = &argv[0]                               |
//  -----------------------------------------------------------------
//  |                 argc = N (Process->NumberOfArguments)         |
//  -----------------------------------------------------------------
//  |                 Dummy RA = 0xDEADC0DE                         |
//  -----------------------------------------------------------------
//  USER STACK BASE
static
STATUS
_ThreadSetupMainThreadUserStack(
    IN      PVOID               InitialStack,
    OUT     PVOID*              ResultingStack,
    IN      PPROCESS            Process
    )
{
    ASSERT(InitialStack != NULL);
    ASSERT(ResultingStack != NULL);
    ASSERT(Process != NULL);

    *ResultingStack = (PVOID) PtrDiff(InitialStack, SHADOW_STACK_SIZE + sizeof(PVOID));

    return STATUS_SUCCESS;
}

REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
RELEASES_EXCL_AND_NON_REENTRANT_LOCK(m_threadSystemData.ReadyThreadsLock)
static
void
_ThreadSchedule(
    void
    )
{
    PTHREAD pCurrentThread;
    PTHREAD pNextThread;
    PCPU* pCpu;

    ASSERT(INTR_OFF == CpuIntrGetState());
    ASSERT(LockIsOwner(&m_threadSystemData.ReadyThreadsLock));

    pCurrentThread = GetCurrentThread();
    ASSERT( NULL != pCurrentThread );

    pCpu = GetCurrentPcpu();

    // save previous thread
    pCpu->ThreadData.PreviousThread = pCurrentThread;

    // get next thread
    pNextThread = _ThreadGetReadyThread();
    ASSERT( NULL != pNextThread );

    // if current differs from next
    // => schedule next
    if (pNextThread != pCurrentThread)
    {
        LOG_TRACE_THREAD("Before ThreadSwitch\n");
        LOG_TRACE_THREAD("Current thread: %s\n", pCurrentThread->Name);
        LOG_TRACE_THREAD("Next thread: %s\n", pNextThread->Name);

        if (pCurrentThread->Process != pNextThread->Process)
        {
            MmuChangeProcessSpace(pNextThread->Process);
        }

        // Before any thread is scheduled it executes this function, thus if we set the current
        // thread to be the next one it will be fine - there is no possibility of interrupts
        // appearing to cause inconsistencies
        pCurrentThread->UninterruptedTicks = 0;

        pNextThread->State = ThreadStateRunning;
        SetCurrentThread(pNextThread);
        ThreadSwitch( &pCurrentThread->Stack, pNextThread->Stack);

        ASSERT(INTR_OFF == CpuIntrGetState());
        ASSERT(LockIsOwner(&m_threadSystemData.ReadyThreadsLock));

        LOG_TRACE_THREAD("After ThreadSwitch\n");
        LOG_TRACE_THREAD("Current: %s\n", pCurrentThread->Name);

        // We cannot log the name of the 'next thread', i.e. the thread which formerly preempted
        // this one because a long time may have passed since then and it may have been destroyed

        // The previous thread may also have been destroyed after it was de-scheduled, we have
        // to be careful before logging its name
        if (pCpu->ThreadData.PreviousThread != NULL)
        {
            LOG_TRACE_THREAD("Prev thread: %s\n", pCpu->ThreadData.PreviousThread->Name);
        }
    }
    else
    {
        pCurrentThread->UninterruptedTicks++;
    }

    ThreadCleanupPostSchedule();
}

REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
RELEASES_EXCL_AND_NON_REENTRANT_LOCK(m_threadSystemData.ReadyThreadsLock)
void
MS_ABI
ThreadCleanupPostSchedule(
    void
    )
{
    PTHREAD prevThread;

    ASSERT(INTR_OFF == CpuIntrGetState());

    GetCurrentPcpu()->ThreadData.RunningThreadTicks = 0;
    prevThread = GetCurrentPcpu()->ThreadData.PreviousThread;

    LockRelease(&m_threadSystemData.ReadyThreadsLock, INTR_OFF);

    if (NULL != prevThread)
    {
        if (LockIsOwner(&prevThread->BlockLock))
        {
            // Unfortunately, we cannot use the inverse condition because it is not always
            // true, i.e. if the previous thread is the idle thread it's not 100% sure that
            // it was previously holding the block hold, it may have been preempted before
            // acquiring it.
            ASSERT(prevThread->State == ThreadStateBlocked
                   || prevThread == GetCurrentPcpu()->ThreadData.IdleThread);

            LOG_TRACE_THREAD("Will release block lock for thread [%s]\n", prevThread->Name);

            _Analysis_assume_lock_held_(prevThread->BlockLock);
            LockRelease(&prevThread->BlockLock, INTR_OFF);
        }
        else if (prevThread->State == ThreadStateDying)
        {
            LOG_TRACE_THREAD("Will dereference thread: [%s]\n", prevThread->Name);

            // dereference thread
            _ThreadDereference(prevThread);
            GetCurrentPcpu()->ThreadData.PreviousThread = NULL;
        }
    }
}

static
STATUS
(CDECL MS_ABI _IdleThread)(
    IN_OPT      PVOID       Context
    )
{
    PEX_EVENT pEvent;

    LOG_FUNC_START_THREAD;

    ASSERT( NULL != Context);

    pEvent = (PEX_EVENT) Context;
    ExEventSignal(pEvent);

    // warning C4127: conditional expression is constant
MSVC_WARNING_SUPPRESS(4127)
    while (TRUE)
    {
        CpuIntrDisable();
        ThreadTakeBlockLock();
        ThreadBlock();

        __sti_and_hlt();
    }

    NOT_REACHED;
}

REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
static
_Ret_notnull_
PTHREAD
_ThreadGetReadyThread(
    void
    )
{
    PTHREAD pNextThread;
    PLIST_ENTRY pEntry;
    BOOLEAN bIdleScheduled;

    ASSERT( INTR_OFF == CpuIntrGetState());
    ASSERT( LockIsOwner(&m_threadSystemData.ReadyThreadsLock));

    pNextThread = NULL;

    pEntry = RemoveHeadList(&m_threadSystemData.ReadyThreadsList);
    if (pEntry == &m_threadSystemData.ReadyThreadsList)
    {
        pNextThread = GetCurrentPcpu()->ThreadData.IdleThread;
        bIdleScheduled = TRUE;
    }
    else
    {
        pNextThread = CONTAINING_RECORD( pEntry, THREAD, ReadyList );

        ASSERT( pNextThread->State == ThreadStateReady );
        bIdleScheduled = FALSE;
    }

    // maybe we shouldn't update idle time each time a thread is scheduled
    // maybe it is enough only every x times
    // or maybe we can update time only on RTC updates
    CoreUpdateIdleTime(bIdleScheduled);

    return pNextThread;
}

static
void
_ThreadForcedExit(
    void
    )
{
    PTHREAD pCurrentThread = GetCurrentThread();

    AtomicOr32( &pCurrentThread->Flags, THREAD_FLAG_FORCE_TERMINATED );

    ThreadExit(STATUS_JOB_INTERRUPTED);
    NOT_REACHED;
}

static
void
_ThreadReference(
    INOUT   PTHREAD                 Thread
    )
{
    ASSERT( NULL != Thread );

    RfcReference(&Thread->RefCnt);
}

static
void
_ThreadDereference(
    INOUT   PTHREAD                 Thread
    )
{
    ASSERT( NULL != Thread );

    RfcDereference(&Thread->RefCnt);
}

static
void
_ThreadDestroy(
    IN      PVOID                   Object,
    IN_OPT  PVOID                   Context
    )
{
    INTR_STATE oldState;
    PTHREAD pThread = (PTHREAD) CONTAINING_RECORD(Object, THREAD, RefCnt);

    ASSERT(NULL != pThread);
    ASSERT(NULL == Context);

    LockAcquire(&m_threadSystemData.AllThreadsLock, &oldState);
    RemoveEntryList(&pThread->AllList);
    LockRelease(&m_threadSystemData.AllThreadsLock, oldState);

    // This must be done before removing the thread from the process list, else
    // this may be the last thread and the process VAS will be freed by the time
    // ProcessRemoveThreadFromList - this function also dereferences the process
    if (NULL != pThread->UserStack)
    {
        // Free UM stack
        MmuFreeStack(pThread->UserStack, pThread->Process);
        pThread->UserStack = NULL;
    }

    ProcessRemoveThreadFromList(pThread);

    ExEventDestroy(&pThread->TerminationEvt);
    LockDestroy(&pThread->BlockLock);

    if (NULL != pThread->Name)
    {
        ExFreePoolWithTag(pThread->Name, HEAP_THREAD_TAG);
        pThread->Name = NULL;
    }

    if (NULL != pThread->Stack)
    {
        // This is the kernel mode stack
        // It does not 'belong' to any process => pass NULL
        MmuFreeStack(pThread->Stack, NULL);
        pThread->Stack = NULL;
    }

    ExFreePoolWithTag(pThread, HEAP_THREAD_TAG);
}

static
MS_ABI
void
_ThreadKernelFunction(
    IN      PFUNC_ThreadStart       Function,
    IN_OPT  PVOID                   Context
    )
{
    STATUS exitStatus;

    ASSERT(NULL != Function);

    CHECK_STACK_ALIGNMENT;

    ASSERT(CpuIntrGetState() == INTR_ON);
    exitStatus = Function(Context);

    ThreadExit(exitStatus);
    NOT_REACHED;
}

static
INT64
CompareThreadPriority(PTHREAD FirstThread, PTHREAD SecondThread)
{
    // int64 cast mert ugy tunik a type nem signed es akkor atwrapel minuszba
    return (INT64)SecondThread->Priority - (INT64)FirstThread->Priority;
}

INT64 _ThreadPriorityCopmpareListElems(
    IN PLIST_ENTRY FirstElem,
    IN PLIST_ENTRY SecondElem,
    IN_OPT PVOID Context)
{
    PTHREAD pFirstThread;
    PTHREAD pSecondThread;

    // need this line otherwise compile error
    UNREFERENCED_PARAMETER(Context);

    pFirstThread = CONTAINING_RECORD(FirstElem, THREAD, ReadyList);
    pSecondThread = CONTAINING_RECORD(SecondElem, THREAD, ReadyList);

    return CompareThreadPriority(pFirstThread, pSecondThread);
}

static
BOOLEAN
_ThreadShouldPreemptCurrent(
    void
    )
{
    INTR_STATE oldState;
    BOOLEAN restult;

    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &oldState);

    PPCPU pCpu;
    PTHREAD pCurrentThread;
    PTHREAD pBestThread;

    pCpu = GetCurrentPcpu();
    pCurrentThread = GetCurrentThread();

    if (NULL == pCpu || NULL == pCurrentThread || NULL == pCpu->ThreadData.IdleThread){
        LockRelease(&m_threadSystemData.ReadyThreadsLock, oldState);
        return FALSE;
    }

    // List is empty
    if (m_threadSystemData.ReadyThreadsList.Flink == &m_threadSystemData.ReadyThreadsList) {
        LockRelease(&m_threadSystemData.ReadyThreadsLock, oldState);
        return FALSE;
    }

    // Check if in idle thread currently
    if (pCurrentThread == pCpu->ThreadData.IdleThread){
        LockRelease(&m_threadSystemData.ReadyThreadsLock, oldState);
        return TRUE;
    }

    pBestThread = CONTAINING_RECORD(m_threadSystemData.ReadyThreadsList.Flink, THREAD, ReadyList);

    restult = pBestThread->Priority > pCurrentThread->Priority;

    LockRelease(&m_threadSystemData.ReadyThreadsLock, oldState);

    return restult;
}

static
STATUS
_ThreadPreemptCheckIpi(
    IN_OPT PVOID Context
    )
{
    PPCPU pCpu;

    UNREFERENCED_PARAMETER(Context);

    pCpu = GetCurrentPcpu();

    if (NULL != pCpu && _ThreadShouldPreemptCurrent()) {
        pCpu->ThreadData.YieldOnInterruptReturn = TRUE;
    }

    return STATUS_SUCCESS;
}

void
ThreadYieldIfPreempted(
    void
    )
{
    if(!_ThreadShouldPreemptCurrent()) {
        return;
    }

    if (INTR_ON == CpuIntrGetState()) {
        // Can switch now
        ThreadYield();
    }
    else {
        // If interrupts not ON, the function is called from an ISR
        // => not safe to yield here, just set the flag.
        GetCurrentPcpu()->ThreadData.YieldOnInterruptReturn = TRUE;
    }
}

BOOLEAN
ThreadRepositionAfterPriorityChange(
    INOUT PTHREAD Thread)
{
    INTR_STATE oldState;
    BOOLEAN result = FALSE;

    ASSERT(NULL != Thread);

    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &oldState);
    if(Thread->State == ThreadStateReady) {
        RemoveEntryList(&Thread->ReadyList);
        InsertOrderedList(&m_threadSystemData.ReadyThreadsList, &Thread->ReadyList, _ThreadPriorityCopmpareListElems, NULL);
        result = TRUE;
    }
    LockRelease(&m_threadSystemData.ReadyThreadsLock, oldState);
    return result;
}

void
ThreadPreemptLowerPriorityCpus(
    IN THREAD_PRIORITY NewPriority)
{
    PLIST_ENTRY pCpuListHead = NULL;
    PPCPU pSelf = GetCurrentPcpu();

    SmpGetCpuList(&pCpuListHead);

    for (PLIST_ENTRY pEntry = pCpuListHead->Flink;
         pEntry != pCpuListHead;
         pEntry = pEntry->Flink)
    {
        PPCPU pCpu = CONTAINING_RECORD(pEntry, PCPU, ListEntry);
        PTHREAD pRunning = pCpu->ThreadData.CurrentThread;
        SMP_DESTINATION dest = {0};

        if (pCpu == pSelf)
        {
            continue;
        }

        dest.Cpu.ApicId = pCpu->ApicId;
        SmpSendGenericIpiEx(_ThreadPreemptCheckIpi, NULL, NULL, NULL, FALSE,
                            SmpIpiSendToCpu, dest);
    }
}