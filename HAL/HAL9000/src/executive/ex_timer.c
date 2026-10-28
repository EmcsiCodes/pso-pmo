#include "HAL9000.h"
#include "ex_timer.h"
#include "iomu.h"
#include "thread_internal.h"

typedef struct _GLOBAL_TIMER_LIST
{
    /*
    TimerListHead is the beginning of our linked list of timers.
    TimerListLock protects that list from concurrent modifications.
    */
    LOCK TimerListLock;
    LIST_ENTRY TimerListHead;

} GLOBAL_TIMER_LIST;

//creating the global timer list for the operating system
//This list will be used to keep track of all the timers that have been created and are currently active
static GLOBAL_TIMER_LIST m_globalTimerList;


/*
    A comparison function for two timers based on their trigger times. It is used to sort timers in the global timer list.
    The function takes two list entries as input, which are expected to be part of EX_TIMER
    structures. It retrieves the EX_TIMER structures from the list entries and compares their TriggerTimeUs fields.
    The function returns:
    - A negative value if the first timer's trigger time is less than the second timer's
    - A positive value if the first timer's trigger time is greater than the second timer's
    - Zero if both timers have the same trigger time
*/
static
INT64
(CDECL _ExTimerCompareListElems)(
    IN      PLIST_ENTRY FirstElem,
    IN      PLIST_ENTRY SecondElem,
    IN_OPT  PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Context);

    PEX_TIMER firstTimer = CONTAINING_RECORD(
        FirstElem, EX_TIMER, TimerListElem);

    PEX_TIMER secondTimer = CONTAINING_RECORD(
        SecondElem, EX_TIMER, TimerListElem);

    return ExTimerCompareTimers(firstTimer, secondTimer);
}

/*
Initializes the global timer list and its lock. This function should be called during the system initialization phase before
any timers are created or used.
It sets the memory of the global timer list to zero, initializes the lock for the timer list,
sets a name for the lock for debugging purposes, and initializes the list head for the timer list
*/
void
ExTimerSystemPreinit(void)
{
    memzero(&m_globalTimerList, sizeof(GLOBAL_TIMER_LIST));

    LockInit(&m_globalTimerList.TimerListLock);

    LockSetName(&m_globalTimerList.TimerListLock, "TimerList");

    InitializeListHead(&m_globalTimerList.TimerListHead);
}

STATUS
ExTimerInit(
    OUT     PEX_TIMER       Timer,
    IN      EX_TIMER_TYPE   Type,
    IN      QWORD           Time
    )
{
    STATUS status;

    if (NULL == Timer)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (Type > ExTimerTypeMax)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    status = STATUS_SUCCESS;

    memzero(Timer, sizeof(EX_TIMER));

    //Initialize the event that will be used to notify waiting threads for this timer
    status = ExEventInit(&Timer->TimerEvent,
                        ExEventTypeNotification,
                        FALSE);

    if (!SUCCEEDED(status))
    {
        return status;
    }

    //Initialize the timer's linked-list entry.
    InitializeListHead(&Timer->TimerListElem);

    Timer->Type = Type;
    if (Timer->Type != ExTimerTypeAbsolute)
    {
        // relative time

        // if the time trigger time has already passed the timer will
        // be signaled after the first scheduler tick
        Timer->TriggerTimeUs = IomuGetSystemTimeUs() + Time;
        Timer->ReloadTimeUs = Time;
    }
    else
    {
        // absolute
        Timer->TriggerTimeUs = Time;
    }

    return status;
}

void
ExTimerStart(
    IN PEX_TIMER Timer
    )
{
    INTR_STATE oldState;

    ASSERT(Timer != NULL);

    LockAcquire(&m_globalTimerList.TimerListLock, &oldState);

    if (!Timer->TimerUninited && !Timer->TimerStarted)
    {
        Timer->TimerStarted = TRUE;

        // Zero-period timers need special handling.
        if (!(Timer->Type == ExTimerTypeRelativePeriodic &&
              Timer->ReloadTimeUs == 0))
        {
            InsertOrderedList(
                &m_globalTimerList.TimerListHead,
                &Timer->TimerListElem,
                _ExTimerCompareListElems,
                NULL
            );
        }
    }

    LockRelease(&m_globalTimerList.TimerListLock, oldState);
}

void
ExTimerStop(
    IN PEX_TIMER Timer
    )
{
    INTR_STATE oldState;

    ASSERT(Timer != NULL);

    //Protect the global timer list and timer state
    LockAcquire(&m_globalTimerList.TimerListLock, &oldState);

    if (!Timer->TimerUninited)
    {
        //Mark the timer as stopped
        Timer->TimerStarted = FALSE;

        //Remove the timer if it is in the global list
        if (!IsListEmpty(&Timer->TimerListElem))
        {
            RemoveEntryList(&Timer->TimerListElem);
            InitializeListHead(&Timer->TimerListElem);
        }

        //Wake all threads waiting for this timer
        ExEventSignal(&Timer->TimerEvent);
    }

    LockRelease(&m_globalTimerList.TimerListLock, oldState);
}

void
ExTimerWait(
    INOUT PEX_TIMER Timer
    )
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited || !Timer->TimerStarted)
    {
        return;
    }

    if (Timer->Type == ExTimerTypeRelativePeriodic)
    {
        if (Timer->ReloadTimeUs == 0)
        {
            return;
        }
    }
    else
    {
        if (IomuGetSystemTimeUs() >= Timer->TriggerTimeUs)
        {
            return;
        }
    }

    //Block until the timer event is signaled
    ExEventWaitForSignal(&Timer->TimerEvent);
}

void
ExTimerUninit(
    INOUT   PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    ExTimerStop(Timer);

    Timer->TimerUninited = TRUE;

    ExEventDestroy(&Timer->TimerEvent);
}

INT64
ExTimerCompareTimers(
    IN PEX_TIMER FirstElem,
    IN PEX_TIMER SecondElem
    )
{
    if (FirstElem->TriggerTimeUs < SecondElem->TriggerTimeUs)
        return -1;

    if (FirstElem->TriggerTimeUs > SecondElem->TriggerTimeUs)
        return 1;

    return 0;
}


void
ExTimerCheckAll(
    void
    )
{
    INTR_STATE oldState;
    QWORD currentTime;
    PEX_TIMER timer;

    LockAcquire(&m_globalTimerList.TimerListLock, &oldState);

    currentTime = IomuGetSystemTimeUs();

    while (!IsListEmpty(&m_globalTimerList.TimerListHead))
    {
        //Get the timer with the earliest expiration time
        timer = CONTAINING_RECORD(
            m_globalTimerList.TimerListHead.Flink,
            EX_TIMER,
            TimerListElem
        );

        if (timer->TriggerTimeUs > currentTime)
        {
            break;
        }

        //The timer has expired. Remove it from the list
        RemoveEntryList(&timer->TimerListElem);
        InitializeListHead(&timer->TimerListElem);

        //Notify all threads waiting on this timer
        ExEventSignal(&timer->TimerEvent);

        if (timer->Type == ExTimerTypeRelativePeriodic &&
            timer->ReloadTimeUs > 0)
        {
            ExEventClearSignal(&timer->TimerEvent);

            timer->TriggerTimeUs += timer->ReloadTimeUs;

            if (timer->TriggerTimeUs <= currentTime)
            {
                timer->TriggerTimeUs =
                    currentTime + timer->ReloadTimeUs;
            }

            InsertOrderedList(
                &m_globalTimerList.TimerListHead,
                &timer->TimerListElem,
                _ExTimerCompareListElems,
                NULL
            );
        }
    }

    LockRelease(&m_globalTimerList.TimerListLock, oldState);
}