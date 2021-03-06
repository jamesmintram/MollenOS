/* MollenOS
 *
 * Copyright 2011, Philip Meulengracht
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
 * MollenOS MCore - Server & Process Management
 * - The process/server manager is known as Phoenix
 */

#ifndef _MCORE_ASH_H_
#define _MCORE_ASH_H_

#include <os/osdefs.h>
#include <ds/blbitmap.h>
#include <ds/mstring.h>
#include <ds/collection.h>

#include <criticalsection.h>
#include <memorybuffer.h>
#include <memoryspace.h>
#include <process/pe.h>
#include <pipe.h>

#define ASH_STACK_INIT          0x1000
#define ASH_STACK_MAX           (4 << 20)

// Types of processes that can be created.
typedef enum _MCoreAshType {
    AshBase,
    AshServer,
    AshProcess
} MCoreAshType_t;

// File Mapping Support
// Provides file-mapping support for processes.
typedef struct _MCoreAshFileMapping {
    CollectionItem_t    Header;
    DmaBuffer_t         BufferObject;
    UUId_t              FileHandle;
    uint64_t            FileBlock;
    uint64_t            BlockOffset;
    size_t              Length;
    Flags_t             Flags;
} MCoreAshFileMapping_t;

/* The phoenix base structure, this contains
* basic information shared across all ashes
* Whether it's a process or a server */
typedef struct _MCoreAsh {
    UUId_t                  MainThread;
    UUId_t                  Id;
    UUId_t                  Parent;
    MCoreAshType_t          Type;
    atomic_int              References;

    // The name of the Ash, this is usually derived from the file that spawned it
    MString_t*              Name;
    MString_t*              Path;
    Collection_t*           Pipes;
    Collection_t*           FileMappings;

    // Memory management and information,
    // Ashes run in their own space, and have their own bitmap allocators
    SystemMemorySpace_t*    MemorySpace;
    BlockBitmap_t*          Heap;
    uintptr_t               SignalHandler;

    // Below is everything related to
    // the startup and the executable information
    // that the Ash has
    MCorePeFile_t*          Executable;
    uintptr_t               NextLoadingAddress;
    uint8_t*                FileBuffer;
    size_t                  FileBufferLength;
    int                     Code;
} MCoreAsh_t;

/* MCoreAshFileMappingEvent
 * Descripes a file mapping access event. */
typedef struct _MCoreAshFileMappingEvent {
    MCoreAsh_t*         Ash;
    uintptr_t           Address;
    OsStatus_t          Result;
} MCoreAshFileMappingEvent_t;

/* PhoenixInitializeAsh
 * This function loads the executable and
 * prepares the ash-environment, at this point
 * it won't be completely running yet, it needs
 * its own thread for that. Returns 0 on success */
KERNELAPI OsStatus_t KERNELABI
PhoenixInitializeAsh(
    _InOut_ MCoreAsh_t  *Ash, 
    _In_ MString_t      *Path);

/* PhoenixStartupAsh
 * This is a wrapper for starting up a base Ash
 * and uses <PhoenixInitializeAsh> to setup the env
 * and do validation before starting */
KERNELAPI UUId_t KERNELABI
PhoenixStartupAsh(
    _In_ MString_t *Path);

/* PhoenixStartupEntry
 * This is the standard ash-boot function
 * which simply sets up the ash and jumps to userland */
KERNELAPI void KERNELABI
PhoenixStartupEntry(
    _In_ void *BasePointer);

/* This is the finalizor function for starting
 * up a new base Ash, it finishes setting up the environment
 * and memory mappings, must be called on it's own thread */
KERNELAPI void PhoenixFinishAsh(MCoreAsh_t *Ash);

/* PhoenixOpenAshPipe
 * Creates a new communication pipe available for use. */
KERNELAPI OsStatus_t KERNELABI
PhoenixOpenAshPipe(
    _In_ MCoreAsh_t*    Ash, 
    _In_ int            Port, 
    _In_ int            Type);

/* PhoenixWaitAshPipe
 * Waits for a pipe to be opened on the given
 * ash instance. */
KERNELAPI OsStatus_t KERNELABI
PhoenixWaitAshPipe(
    _In_ MCoreAsh_t *Ash, 
    _In_ int         Port);

/* PhoenixCloseAshPipe
 * Closes the pipe for the given Ash, and cleansup
 * resources allocated by the pipe. This shutsdown
 * any communication on the port */
KERNELAPI OsStatus_t KERNELABI
PhoenixCloseAshPipe(
    _In_ MCoreAsh_t *Ash, 
    _In_ int         Port);

/* PhoenixGetAshPipe
 * Retrieves an existing pipe instance for the given ash
 * and port-id. If it doesn't exist, returns NULL. */
KERNELAPI SystemPipe_t* KERNELABI
PhoenixGetAshPipe(
    _In_ MCoreAsh_t     *Ash, 
    _In_ int             Port);

/* PhoenixCleanupAsh
 * Cleans up a given Ash, freeing all it's allocated resources
 * and unloads it's executables, memory space is not cleaned up
 * must be done by external thread */
KERNELAPI void KERNELABI
PhoenixCleanupAsh(
    _In_ MCoreAsh_t *Ash);

/* PhoenixTerminateAsh
 * This marks an ash for termination by taking it out
 * of rotation and adding it to the cleanup list */
KERNELAPI void KERNELABI
PhoenixTerminateAsh(
    _In_ MCoreAsh_t*    Ash,
    _In_ int            ExitCode,
    _In_ int            TerminateDetachedThreads,
    _In_ int            TerminateInstantly);

/* PhoenixFileMappingEvent
 * Signals a new file-mapping access event to the phoenix process system. */
KERNELAPI void KERNELABI
PhoenixFileMappingEvent(
    _In_ MCoreAshFileMappingEvent_t* Event);

/* PhoenixGetAsh
 * This function looks up a ash structure by the given id */
KERNELAPI MCoreAsh_t* KERNELABI
PhoenixGetAsh(
    _In_ UUId_t AshId);

/* PhoenixGetCurrentAsh
 * Retrives the current ash for the running thread */
KERNELAPI MCoreAsh_t* KERNELABI
PhoenixGetCurrentAsh(void);

/* PhoenixGetAshByName
 * This function looks up a ash structure by the given name */
KERNELAPI MCoreAsh_t* KERNELABI
PhoenixGetAshByName(
    _In_ const char *Name);

#endif //!_MCORE_ASH_H_
