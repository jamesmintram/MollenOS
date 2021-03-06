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
 * MollenOS X86 PS2 Controller (Controller) Driver
 * http://wiki.osdev.org/PS2
 */
//#define __TRACE

#include <os/utils.h>
#include <threads.h>
#include <string.h>
#include <stdlib.h>
#include "ps2.h"

/* PS2InterfaceTest
 * Performs an interface test on the given port*/
OsStatus_t
PS2InterfaceTest(
    _In_ int        Port)
{
    uint8_t Response = 0;

    // Send command based on port
    if (Port == 0) {
        PS2SendCommand(PS2_INTERFACETEST_PORT1);
    }
    else {
        PS2SendCommand(PS2_INTERFACETEST_PORT2);
    }
    Response = PS2ReadData(0);
    return (Response == PS2_INTERFACETEST_OK) ? OsSuccess : OsError;
}

/* PS2ResetPort
 * Resets the given port and tests for a reset-ok */
OsStatus_t
PS2ResetPort(
    _In_ int        Index)
{
    uint8_t Response = 0;

    // Select the correct port
    if (Index != 0) {
        PS2SendCommand(PS2_SELECT_PORT2);
    }
    
    // Perform the self-test
    PS2WriteData(PS2_RESET_PORT);
    Response = PS2ReadData(0);

    // Check the response byte
    // Two results are ok, AA and FA
    if (Response == PS2_SELFTEST
        || Response == PS2_ACK) {
        Response = PS2ReadData(1);
        Response = PS2ReadData(1);
        return OsSuccess;
    }
    else {
        return OsError;
    }
}

/* PS2IdentifyPort
 * Identifies the device currently connected to the
 * given port index, if fails it returns 0xFFFFFFFF */
DevInfo_t
PS2IdentifyPort(
    _In_ int        Index)
{
    uint8_t ResponseExtra   = 0;
    uint8_t Response        = 0;

    // Select correct port
    if (Index != 0) {
        PS2SendCommand(PS2_SELECT_PORT2);
    }

    // Disable scanning when identifying
    if (PS2WriteData(PS2_DISABLE_SCANNING) != OsSuccess) {
        return 0xFFFFFFFF;
    }

    Response = PS2ReadData(0);
    if (Response != PS2_ACK) {
        return 0xFFFFFFFF;
    }

    // Select correct port
    if (Index != 0) {
        PS2SendCommand(PS2_SELECT_PORT2);
    }

    // Send the identify command
    if (PS2WriteData(PS2_IDENTIFY_PORT) != OsSuccess) {
        return 0xFFFFFFFF;
    }

    Response = PS2ReadData(0);
    if (Response != PS2_ACK) {
        return 0xFFFFFFFF;
    }

    // Read response byte
GetResponse:
    Response = PS2ReadData(0);
    if (Response == PS2_ACK) {
        goto GetResponse;
    }

    // Read next response byte
    ResponseExtra = PS2ReadData(0);
    if (ResponseExtra == 0xFF) {
        ResponseExtra = 0;
    }
    return (Response << 8) | ResponseExtra;
}

/* PS2RegisterDevice
 * Shortcut function for registering a new device */
OsStatus_t
PS2RegisterDevice(
    _In_ PS2Port_t* Port) 
{
    // Static Variables
    MCoreDevice_t Device = { 0 };
    Device.Length = sizeof(MCoreDevice_t);

    // Initialize VID/DID to us
    Device.VendorId = 0xFFEF;
    Device.DeviceId = 0x0030;

    // Invalidate generics
    Device.Class    = 0xFF0F;
    Device.Subclass = 0xFF0F;

    // Initialize the irq structure
    Device.Interrupt.Pin            = INTERRUPT_NONE;
    Device.Interrupt.Vectors[0]     = INTERRUPT_NONE;
    Device.Interrupt.AcpiConform    = 0;

    // Select source from port index
    if (Port->Index == 0) {
        Device.Interrupt.Line = PS2_PORT1_IRQ;
    }
    else {
        Device.Interrupt.Line = PS2_PORT2_IRQ;
    }

    // Lastly just register the device under the controller (todo)
    Port->Contract.DeviceId = RegisterDevice(UUID_INVALID, &Device, 
        __DEVICEMANAGER_REGISTER_LOADDRIVER);    
    if (Port->Contract.DeviceId == UUID_INVALID) {
        ERROR("Failed to register new device");
        return OsError;
    }
    else {
        return OsSuccess;
    }
}

/* PS2PortWrite 
 * Writes the given data-byte to the ps2-port */
OsStatus_t
PS2PortWrite(
    _In_ PS2Port_t* Port,
    _In_ uint8_t    Value)
{
    // Select port 2 if neccessary
    if (Port->Index != 0) {
        PS2SendCommand(PS2_SELECT_PORT2);
    }
    return PS2WriteData(Value);
}

/* PS2PortWaitForState
 * Waits for the port to enter the given state. The function can return OsError
 * if the state is not reached in a seconds time. */
OsStatus_t
PS2PortWaitForState(
    _In_ PS2Command_t*      Command,
    _In_ PS2CommandState_t  State)
{
    volatile PS2CommandState_t *ActiveState;
    int Timeout = 1000;

    ActiveState = (volatile PS2CommandState_t*)&Command->State;
    while (*ActiveState != State && Timeout > 0) {
        thrd_sleepex(10);
        Timeout -= 10;
    }
    if (Timeout == 0) {
        WARNING("PS2-Command state timeout (%i != %i), command %u", 
            *ActiveState, State, Command->Command);
        return OsError;
    }
    return OsSuccess;
}

/* PS2PortQueueCommand 
 * Queues the given command up for the given port
 * if a response is needed for the previous commnad
 * Set command = PS2_RESPONSE_COMMAND and pointer to response buffer */
OsStatus_t
PS2PortQueueCommand(
    _In_ PS2Port_t* Port,
    _In_ uint8_t    Command,
    _In_ uint8_t*   Response)
{
    PS2Command_t *pCommand  = NULL;
    OsStatus_t Result       = OsSuccess;
    pCommand                = &Port->Commands[Port->QueueIndex % PS2_MAXCOMMANDS];

    // Find a free command spot for the queue
    if (PS2PortWaitForState(pCommand, PS2Free) != OsSuccess) {
        return OsError;
    }
    Port->QueueIndex++;

    // Initiate the packet data
    pCommand->State         = PS2InQueue;
    pCommand->RetryCount    = 0;
    pCommand->Command       = Command;
    pCommand->Response      = Response;

    // Is the queue already running?
    // Otherwise start it by sending the command
    if (Port->CurrentCommand == NULL) {
        Port->CurrentCommand    = pCommand;
        Result                  = PS2PortWrite(Port, Command);
    }
    return PS2PortWaitForState(pCommand, PS2Free);
}

/* PS2PortExecuteNextCommand
 * Finds and executes the next ready command, we do it in round-robin fashion so
 * that we execute commands in order they were queued up in. */
OsStatus_t
PS2PortExecuteNextCommand(
    _In_ PS2Port_t* Port)
{
    int NextExecutionIndex  = (Port->ExecutionIndex + 1) % PS2_MAXCOMMANDS;
    Port->CurrentCommand    = NULL;

    // Increase the index for execution
    if (Port->Commands[NextExecutionIndex].State == PS2InQueue) {
        Port->ExecutionIndex++;
        Port->CurrentCommand = &Port->Commands[NextExecutionIndex];
        PS2PortWrite(Port, Port->Commands[NextExecutionIndex].Command);
    }
    return OsSuccess;
}

/* PS2PortFinishCommand 
 * Finalizes the current command and executes the next command in queue (if any). */
OsStatus_t
PS2PortFinishCommand(
    _In_ PS2Port_t* Port,
    _In_ uint8_t    Result)
{
    // Is there any command active? Otherwise ignore everything here
    if (Port->CurrentCommand == NULL) {
        return OsError;
    }

    switch (Port->CurrentCommand->State) {
        case PS2InQueue: {
            if (Result == PS2_RESEND_COMMAND) {
                if (Port->CurrentCommand->RetryCount < PS2_MAX_RETRIES) {
                    Port->CurrentCommand->RetryCount++;
                    PS2PortWrite(Port, Port->CurrentCommand->Command);
                    return OsSuccess;
                }
                Port->CurrentCommand->Command   = PS2_FAILED_COMMAND;
                Port->CurrentCommand->State     = PS2Free;
                // Go to next command
            }
            else if (Result == PS2_ACK_COMMAND) {
                if (Port->CurrentCommand->Response != NULL) {
                    Port->CurrentCommand->State = PS2WaitingResponse;
                    return OsSuccess;
                }
                Port->CurrentCommand->State = PS2Free;
                // Go to next command
            }
            else {
                Port->CurrentCommand->Command   = PS2_FAILED_COMMAND;
                Port->CurrentCommand->State     = PS2Free;
                // Go to next command
            }
        } break;

        case PS2WaitingResponse: {
            *(Port->CurrentCommand->Response)   = Result;
            Port->CurrentCommand->State         = PS2Free;
        } break;

        // Reached on PS2Free, should not happen
        default: {

        } break;
    }
    return PS2PortExecuteNextCommand(Port);
}

/* PS2PortInitialize
 * Initializes the given port and tries to identify the device on the port */
OsStatus_t
PS2PortInitialize(
    _In_ PS2Port_t* Port)
{
    uint8_t Temp = 0;

    // Initialize some variables for the port
    Port->CurrentCommand    = NULL;
    Port->ExecutionIndex    = 0;
    Port->QueueIndex        = 0;

    // Start out by doing an interface
    // test on the given port
    if (PS2InterfaceTest(Port->Index) != OsSuccess) {
        ERROR("PS2-Port (%i): Failed interface test", Port->Index);
        return OsError;
    }

    // Select the correct port
    if (Port->Index == 0) {
        PS2SendCommand(PS2_ENABLE_PORT1);
    }
    else {
        PS2SendCommand(PS2_ENABLE_PORT2);
    }

    // Get controller configuration
    PS2SendCommand(PS2_GET_CONFIGURATION);
    Temp = PS2ReadData(0);

    // Check if the port is enabled
    // Otherwise return error
    if (Temp & (1 << (4 + Port->Index))) {
        return OsError; 
    }

    // Enable IRQ
    Temp |= (1 << Port->Index);

    // Write back the configuration
    PS2SendCommand(PS2_SET_CONFIGURATION);
    if (PS2WriteData(Temp) != OsSuccess) {
        ERROR("PS2-Port (%i): Failed to update configuration", Port->Index);
        return OsError;
    }

    // Reset the port
    if (PS2ResetPort(Port->Index) != OsSuccess) {
        ERROR("PS2-Port (%i): Failed port reset", Port->Index);
        return OsError;
    }

    // Identify type of device on port
    Port->Signature = PS2IdentifyPort(Port->Index);
    
    // If the signature is ok - device present
    if (Port->Signature != 0xFFFFFFFF) {
        Port->Connected = 1;
    }
    return PS2RegisterDevice(Port);
}
