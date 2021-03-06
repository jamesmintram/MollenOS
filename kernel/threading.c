/* MollenOS
 *
 * Copyright 2011 - 2017, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * MollenOS Threading Interface
 * - Common routines that are platform independant to provide
 *   a flexible and generic threading platfrom
 */
#define __MODULE "MTIF"
//#define __TRACE

/* Includes 
 * - System */
#include <component/cpu.h>
#include <system/interrupts.h>
#include <system/thread.h>
#include <system/utils.h>

#include <garbagecollector.h>
#include <process/phoenix.h>
#include <memoryspace.h>
#include <interrupts.h>
#include <threading.h>
#include <scheduler.h>
#include <debug.h>
#include <heap.h>

/* Includes
 * - Library */
#include <ds/collection.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

/* Prototypes
 * The function handler for cleanup */
OsStatus_t ThreadingReap(void *Context);

/* Globals, we need a few variables to keep track of running threads, idle threads
 * and a thread resources lock */
static Collection_t Threads         = COLLECTION_INIT(KeyInteger);
static UUId_t GlbThreadGcId         = UUID_INVALID;
static _Atomic(UUId_t) GlbThreadId  = ATOMIC_VAR_INIT(1);

/* ThreadingInitialize
 * Initializes static data and allocates resources. */
OsStatus_t
ThreadingInitialize(void)
{
    GlbThreadGcId           = GcRegister(ThreadingReap);
    return OsSuccess;
}

/* ThreadingEnable
 * Enables the threading system for the given cpu calling the function. */
OsStatus_t
ThreadingEnable(void)
{
    // Variables
    MCoreThread_t *Thread = &GetCurrentProcessorCore()->IdleThread;
    DataKey_t Key;

    // Initialize the idle thread instance
    memset(Thread, 0, sizeof(MCoreThread_t));
    
    // Initialize members
    Thread->Id          = atomic_fetch_add(&GlbThreadId, 1);
    Thread->Name        = strdup("idle");
    Thread->ParentId    = UUID_INVALID;
    Thread->AshId       = UUID_INVALID;
    Thread->Flags       = THREADING_KERNELMODE | THREADING_IDLE | THREADING_CPUBOUND;
    SchedulerThreadInitialize(Thread, Thread->Flags);
    Thread->Pipe        = CreateSystemPipe(0, 6); // 64 entries, 4kb
    Thread->SignalQueue = CollectionCreate(KeyInteger);
    Key.Value           = (int)Thread->Id;
    COLLECTION_NODE_INIT(&Thread->CollectionHeader, Key);

    // Initialize arch-dependant members
    Thread->MemorySpace = GetCurrentSystemMemorySpace();
    if (ThreadingRegister(Thread) != OsSuccess) {
        ERROR("Failed to register thread with system. Threading is not enabled.");
        CpuHalt();
    }

    // Update active thread to new idle
    GetCurrentProcessorCore()->CurrentThread = Thread;
    return CollectionAppend(&Threads, &Thread->CollectionHeader);
}

/* ThreadingEntryPoint
 * Initializes and handles finish of the thread
 * all threads should use this entry point. No Return */
void
ThreadingEntryPoint(void)
{
    // Variables
    MCoreThread_t *Thread     = NULL;
    UUId_t Cpu                 = 0;

    // Debug
    TRACE("ThreadingEntryPoint()");

    // Initiate values
    Cpu         = CpuGetCurrentId();
    Thread      = ThreadingGetCurrentThread(Cpu);
    if (THREADING_RUNMODE(Thread->Flags) == THREADING_KERNELMODE || (Thread->Flags & THREADING_SWITCHMODE)) {
        Thread->Function(Thread->Arguments);
        ThreadingExitThread(0);
    }
    else {
        Thread->Contexts[THREADING_CONTEXT_LEVEL1] = ContextCreate(Thread->Flags, 
            THREADING_CONTEXT_LEVEL1, (uintptr_t)Thread->Function, 0, (uintptr_t)Thread->Arguments, 0);
        Thread->Contexts[THREADING_CONTEXT_SIGNAL1] = ContextCreate(Thread->Flags, 
            THREADING_CONTEXT_SIGNAL1, 0, 0, 0, 0);
        Thread->Flags |= THREADING_TRANSITION_USERMODE;
        ThreadingYield();
    }
    for (;;);
}

/* ThreadingCreateThread
 * Creates a new thread with the given paramaters and it is immediately
 * queued up for execution. */
UUId_t
ThreadingCreateThread(
    _In_ const char*    Name,
    _In_ ThreadEntry_t  Function,
    _In_ void*          Arguments,
    _In_ Flags_t        Flags)
{
    // Variables
    MCoreThread_t *Thread   = NULL;
    MCoreThread_t *Parent   = NULL;
    UUId_t Cpu              = 0;
    char NameBuffer[16];
    DataKey_t Key;
    
    // Debug
    TRACE("ThreadingCreateThread(%s, 0x%x)", Name, Flags);

    // Initialize variables
    Key.Value   = (int)atomic_fetch_add(&GlbThreadId, 1);
    Cpu         = CpuGetCurrentId();
    Parent      = ThreadingGetCurrentThread(Cpu);

    // Allocate a new thread instance and 
    // zero out the allocated instance
    Thread      = (MCoreThread_t*)kmalloc(sizeof(MCoreThread_t));
    memset(Thread, 0, sizeof(MCoreThread_t));

    // Sanitize name, if NULL generate a new
    // thread name of format 'Thread X'
    if (Name == NULL) {
        memset(&NameBuffer[0], 0, sizeof(NameBuffer));
        sprintf(&NameBuffer[0], "Thread %i", Key.Value);
        Thread->Name = strdup(&NameBuffer[0]);
    }
    else {
        Thread->Name = strdup(Name);
    }

    // Initialize some basic thread information 
    // The only flags we want to copy for now are
    // the running-mode
    Thread->Id          = (UUId_t)Key.Value;
    Thread->ParentId    = Parent->Id;
    Thread->AshId       = Parent->AshId;
    Thread->Function    = Function;
    Thread->Arguments   = Arguments;
    Thread->Flags       = Flags;
    COLLECTION_NODE_INIT(&Thread->CollectionHeader, Key);

    // Setup initial scheduler information
    SchedulerThreadInitialize(Thread, Flags);

    // Create communication members
    Thread->Pipe        = CreateSystemPipe(0, 6); // 64 entries, 4kb
    Thread->SignalQueue = CollectionCreate(KeyInteger);
    Thread->ActiveSignal.Signal = -1;

    // Flag-Special-Case
    // If it's NOT a kernel thread
    // we specify transition-mode
    if (THREADING_RUNMODE(Flags) != THREADING_KERNELMODE && !(Flags & THREADING_INHERIT)) {
        Thread->Flags |= THREADING_SWITCHMODE;
    }

    // Flag-Special-Case
    // Determine the address space we want
    // to initialize for this thread
    if (THREADING_RUNMODE(Flags) == THREADING_KERNELMODE) {
        Thread->MemorySpace = CreateSystemMemorySpace(MEMORY_SPACE_INHERIT);
    }
    else {
        Flags_t ASFlags = 0;

        if (THREADING_RUNMODE(Flags) == THREADING_DRIVERMODE) {
            ASFlags |= MEMORY_SPACE_SERVICE;
        }
        else {
            ASFlags |= MEMORY_SPACE_APPLICATION;
        }
        if (Flags & THREADING_INHERIT) {
            ASFlags |= MEMORY_SPACE_INHERIT;
        }
        Thread->MemorySpace = CreateSystemMemorySpace(ASFlags);
    }

    // Create context's neccessary
    Thread->Contexts[THREADING_CONTEXT_LEVEL0] = 
        ContextCreate(Thread->Flags, THREADING_CONTEXT_LEVEL0, (uintptr_t)&ThreadingEntryPoint, 0, 0, 0);
    Thread->Contexts[THREADING_CONTEXT_SIGNAL0] = 
        ContextCreate(Thread->Flags, THREADING_CONTEXT_SIGNAL0, 0, 0, 0, 0);
    if (ThreadingRegister(Thread) != OsSuccess) {
        ERROR("Failed to register a new thread with system.");
        // @todo
    }

    // Append it to list & scheduler
    Key.Value = (int)Thread->Id;
    CollectionAppend(&Threads, &Thread->CollectionHeader);
    SchedulerThreadQueue(Thread, 0);
    return Thread->Id;
}

/* ThreadingDetachThread
 * Detaches a running thread by marking it without parent, this will make
 * sure it runs untill it kills itself. */
OsStatus_t
ThreadingDetachThread(
    _In_  UUId_t        ThreadId)
{
    MCoreThread_t *Thread   = ThreadingGetCurrentThread(CpuGetCurrentId());
    MCoreThread_t *Target   = ThreadingGetThread(ThreadId);
    UUId_t PId              = Thread->AshId;

    // Perform security checks
    if (Target == NULL || Target->AshId != PId) {
        return OsError;
    }
    
    // Perform the detach
    Target->ParentId = UUID_INVALID;
    return OsSuccess;
}

/* ThreadingCleanupThread
 * Cleans up a thread and all it's resources, the
 * address space is not cleaned up untill all threads
 * in the given space has been shut down. Must be
 * called from a seperate thread */
void
ThreadingCleanupThread(
    _In_ MCoreThread_t* Thread)
{
    // Variables
    CollectionItem_t *fNode;
    int i;

    // Make sure we are completely removed as reference
    // from the entire system. We also signal all waiters for this
    // thread again before continueing just in case
    SchedulerHandleSignalAll((uintptr_t*)Thread);
    SchedulerThreadDequeue(Thread);
    ThreadingUnregister(Thread);

    _foreach(fNode, Thread->SignalQueue) {
        kfree(fNode->Data);
    }
    CollectionDestroy(Thread->SignalQueue);
    
    for (i = 0; i < THREADING_NUMCONTEXTS; i++) {
        if (Thread->Contexts[i] != NULL) {
            kfree(Thread->Contexts[i]);
        }
    }

    ReleaseSystemMemorySpace(Thread->MemorySpace);
    DestroySystemPipe(Thread->Pipe);

    // Remove a reference to the process
    if (Thread->AshId != UUID_INVALID) {
        // @todo
    }

    // Free resources allocated
    kfree((void*)Thread->Name);
    kfree(Thread);
}

/* ThreadingExitThread
 * Exits the current thread by marking it finished
 * and yielding control to scheduler */
void
ThreadingExitThread(
    _In_ int            ExitCode)
{
    CollectionItem_t *Node;
    MCoreThread_t *Thread;
    UUId_t Cpu;

    // Instantiate some values
    Cpu     = CpuGetCurrentId();
    Thread  = ThreadingGetCurrentThread(Cpu);
    assert(Thread != NULL);

    // Kill all children we have running around
    _foreach(Node, &Threads) {
        MCoreThread_t *Child = (MCoreThread_t*)Node;
        if (Child->ParentId == Thread->Id) {
            ThreadingKillThread(Thread->Id, ExitCode, 0);
        }
    }

    // Update thread state
    Thread->RetCode  = ExitCode;
    Thread->Flags   |= THREADING_FINISHED;

    // Wake people waiting for us
    SchedulerHandleSignalAll((uintptr_t*)Thread);
    ThreadingYield();
}

/* ThreadingKillThread
 * Marks the thread with the given id for finished, and it will be cleaned up
 * on next switch unless specified. The given exitcode will be stored. */
OsStatus_t
ThreadingKillThread(
    _In_ UUId_t         ThreadId,
    _In_ int            ExitCode,
    _In_ int            TerminateInstantly)
{
    // Variables
    MCoreThread_t *Target = ThreadingGetThread(ThreadId);

    // Security check
    if (Target == NULL || (Target->Flags & THREADING_IDLE)) {
        return OsError;
    }

    // Update thread state
    Target->Flags   |= THREADING_FINISHED;
    Target->RetCode  = ExitCode;

    // Wake-up threads waiting with ThreadJoin
    SchedulerHandleSignalAll((uintptr_t*)Target);

    // Should we instagib?
    if (TerminateInstantly) {
        if (ThreadId == ThreadingGetCurrentThreadId()) {
            ThreadingYield();
        }
        else {
            // Wake-up the thread if it's sleeping, this will cause a 
            // switch if nothing is running and terminate it
            if (SchedulerThreadSignal(Target) != OsSuccess) {
                // More drastic measures are neccessary here, this means someone else
                // has it in queue or it's currently running on another core
                ThreadingWakeCpu(Target->CoreId);
            }
        }
    }
    return OsSuccess;
}

/* ThreadingJoinThread
 * Can be used to wait for a thread the return 
 * value of this function is the ret-code of the thread */
int
ThreadingJoinThread(
    _In_ UUId_t         ThreadId)
{
    MCoreThread_t *Target = ThreadingGetThread(ThreadId);
    if (Target == NULL) {
        return -1;
    }
    SchedulerThreadSleep((uintptr_t*)Target, 0);
    return Target->RetCode;
}

/* ThreadingSwitchLevel
 * Initializes non-kernel mode and marks the thread
 * for transitioning, there is no return from this function */
void
ThreadingSwitchLevel(
    _In_ MCoreAsh_t*    Ash)
{
    // Variables
    MCoreThread_t *Thread   = ThreadingGetCurrentThread(CpuGetCurrentId());

    // Bind thread to process
    Thread->AshId       = Ash->Id;
    Thread->Function    = (ThreadEntry_t)Ash->Executable->EntryAddress;
    Thread->Arguments   = NULL;

    // Argument when calling a new process is just NULL
    Thread->Contexts[THREADING_CONTEXT_LEVEL1]  = ContextCreate(Thread->Flags,
        THREADING_CONTEXT_LEVEL1, (uintptr_t)Thread->Function, 0, 0, 0);
    Thread->Contexts[THREADING_CONTEXT_SIGNAL1] = ContextCreate(Thread->Flags,
        THREADING_CONTEXT_SIGNAL1, 0, 0, 0, 0);
    Thread->Flags |= THREADING_TRANSITION_USERMODE;

    // Safety-catch
    ThreadingYield();
    for (;;);
}

/* ThreadingTerminateAshThreads
 * Marks all running threads that are not detached unless specified
 * for complete and to terminate on next switch, unless specified. 
 * Returns the number of threads not killed (0 if we terminate detached). */
int
ThreadingTerminateAshThreads(
    _In_ UUId_t         AshId,
    _In_ int            TerminateDetached,
    _In_ int            TerminateInstantly)
{
    int ThreadsNotKilled = 0;
    foreach(tNode, &Threads) {
        MCoreThread_t *Thread = (MCoreThread_t*)tNode;
        if (Thread->AshId == AshId) {
            if ((Thread->ParentId == UUID_INVALID) && !TerminateDetached) {
                // If it's a detached thread calling this method we are killing ourselves
                // and shouldn't increase
                if (Thread->Id != ThreadingGetCurrentThreadId()) {
                    ThreadsNotKilled++;
                }
            }
            else {
                ThreadingKillThread(Thread->Id, 0, TerminateInstantly);
            }
        }
    }
    return ThreadsNotKilled;
}

/* ThreadingGetCurrentThread
 * Retrieves the current thread on the given cpu
 * if there is any issues it returns NULL */
MCoreThread_t*
ThreadingGetCurrentThread(
    _In_ UUId_t         CoreId)
{
    return GetProcessorCore(CoreId)->CurrentThread;
}

/* ThreadingGetCurrentThreadId
 * Retrives the current thread id on the current cpu
 * from the callers perspective */
UUId_t
ThreadingGetCurrentThreadId(void)
{
    if (GetCurrentProcessorCore()->CurrentThread == NULL) {
        return 0;
    }
    return GetCurrentProcessorCore()->CurrentThread->Id;
}

/* ThreadingGetThread
 * Lookup thread by the given thread-id, 
 * returns NULL if invalid */
MCoreThread_t*
ThreadingGetThread(
    _In_ UUId_t         ThreadId)
{
    // Iterate thread nodes and find the correct
    foreach(tNode, &Threads) {
        MCoreThread_t *Thread = (MCoreThread_t*)tNode;
        if (Thread->Id == ThreadId) {
            return Thread;
        }
    }
    return NULL;
}

/* ThreadingIsCurrentTaskIdle
 * Is the given cpu running it's idle task? */
int
ThreadingIsCurrentTaskIdle(
    _In_ UUId_t         CoreId)
{
    SystemCpuCore_t *Core = GetProcessorCore(CoreId);
    return (Core->CurrentThread == &Core->IdleThread) ? 1 : 0;
}

/* ThreadingGetCurrentMode
 * Returns the current run-mode for the current
 * thread on the current cpu */
Flags_t
ThreadingGetCurrentMode(void)
{
    if (ThreadingGetCurrentThread(CpuGetCurrentId()) == NULL) {
        return THREADING_KERNELMODE;
    }
    return ThreadingGetCurrentThread(CpuGetCurrentId())->Flags & THREADING_MODEMASK;
}

/* ThreadingReapZombies
 * Garbage-Collector function, it reaps and cleans up all threads */
OsStatus_t
ThreadingReap(
    _In_ void*          Context)
{
    MCoreThread_t *Thread = (MCoreThread_t*)Context;
    if (Thread == NULL) {
        return OsError;
    }
    CollectionRemoveByNode(&Threads, &Thread->CollectionHeader);

    // Cleanup the thread
    ThreadingCleanupThread(Thread);
    return OsSuccess;
}

/* ThreadingDebugPrint
 * Prints out debugging information about each thread
 * in the system, only active threads */
void
ThreadingDebugPrint(void)
{
    foreach(i, &Threads) {
        MCoreThread_t *Thread = (MCoreThread_t*)i;
        if (THREADING_STATE(Thread->Flags) == THREADING_ACTIVE) {
            WRITELINE("Thread %u (%s) - Flags 0x%x, Queue %i, Timeslice %u, Cpu: %u",
                Thread->Id, Thread->Name, Thread->Flags, Thread->Queue, 
                Thread->TimeSlice, Thread->CoreId);
        }
    }
}

/* ThreadingSwitch
 * This is the thread-switch function and must be 
 * be called from the below architecture to get the
 * next thread to run */
MCoreThread_t*
ThreadingSwitch(
    _In_ MCoreThread_t* Current, 
    _In_ int            PreEmptive,
    _InOut_ Context_t** Context)
{
    SystemCpuCore_t *Core;
    MCoreThread_t *NextThread;

    // Sanitize current thread
    assert(Current != NULL);

    Core                    = GetCurrentProcessorCore();
    Current->ContextActive  = *Context;
GetNextThread:
    if (Current->Flags & (THREADING_FINISHED | THREADING_IDLE)) {
        // If the thread is finished then add it to garbagecollector
        if (Current->Flags & THREADING_FINISHED) {
            THREADING_CLEARSTATE(Current->Flags);
            GcSignal(GlbThreadGcId, Current);
        }
        
        // Don't schedule the current
        NextThread = SchedulerThreadSchedule(NULL, PreEmptive);
    }
    else {
        NextThread = SchedulerThreadSchedule(Current, PreEmptive);
    }

    // Sanitize if we need to active our idle thread, otherwise
    // do a final check that we haven't just gotten ahold of a thread
    // marked for finish
    if (NextThread == NULL) {
        NextThread = &Core->IdleThread;
    }
    else if (NextThread->Flags & THREADING_FINISHED) {
        Current = NextThread;
        goto GetNextThread;
    }

    // Handle level switch // thread startup
    if (NextThread->Flags & THREADING_TRANSITION_USERMODE) {
        NextThread->Flags           &= ~(THREADING_SWITCHMODE | THREADING_TRANSITION_USERMODE);
        NextThread->ContextActive   = NextThread->Contexts[THREADING_CONTEXT_LEVEL1];
    }
    if (NextThread->ContextActive == NULL) {
        NextThread->ContextActive   = NextThread->Contexts[THREADING_CONTEXT_LEVEL0];
    }

    Core->CurrentThread = NextThread;
    *Context            = NextThread->ContextActive;
    return NextThread;
}
