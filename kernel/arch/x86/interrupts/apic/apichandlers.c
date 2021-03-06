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
 * MollenOS x86 Advanced Programmable Interrupt Controller Driver
 * - Interrupt Handlers specific for the APIC
 */

#include <system/utils.h>
#include <threading.h>
#include <interrupts.h>
#include <thread.h>
#include <acpi.h>
#include <apic.h>

/* Externs 
 * We need access to a few of the variables
 * in the other apic files */
__EXTERN volatile size_t GlbTimerTicks[64];
__EXTERN size_t GlbTimerQuantum;

/* Extern to our assembly function
 * it loads the new task context */
__EXTERN void enter_thread(Context_t *Regs);

/* ApicTimerHandler
 * The scheduler interrupt handler. The only functionality this handler has is
 * to switch threads. */
InterruptStatus_t
ApicTimerHandler(
    _In_ void*  Context)
{
    Context_t *Regs;
    size_t TimeSlice;
    int TaskPriority;
    UUId_t CurrCpu = CpuGetCurrentId();

    // Yield => start by sending eoi. It is never certain that we actually return
    // to this function due to how signals are working
    GlbTimerTicks[CurrCpu]++;
    ApicSendEoi(APIC_NO_GSI, INTERRUPT_LAPIC);
    Regs = _ThreadingSwitch((Context_t*)Context, 1, &TimeSlice, &TaskPriority);
    
    // If we are idle task - disable timer untill we get woken up
    if (!ThreadingIsCurrentTaskIdle(CurrCpu)) {
        ApicSetTaskPriority(61 - TaskPriority);
        ApicWriteLocal(APIC_INITIAL_COUNT, GlbTimerQuantum * TimeSlice);
    }
    else {
        ApicSetTaskPriority(0);
        ApicWriteLocal(APIC_INITIAL_COUNT, 0);
    }

    // Manually update interrupt status
    InterruptSetActiveStatus(0);
    
    // Enter new thread, no returning
    enter_thread(Regs);
    return InterruptHandled;
}

/* ApicErrorHandler
 * Handles any internally errors that the apic encounters. Most of these
 * don't have any resolution. */
InterruptStatus_t
ApicErrorHandler(
    _In_ void*  Context)
{
    _CRT_UNUSED(Context);
    return InterruptHandled;
}
