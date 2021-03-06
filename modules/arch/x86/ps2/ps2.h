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

#ifndef _DRIVER_PS2_CONTROLLER_H_
#define _DRIVER_PS2_CONTROLLER_H_

#include <os/contracts/base.h>
#include <os/interrupt.h>
#include <os/osdefs.h>
#include <os/io.h>

/* Io-space for accessing the PS2
 * Spans over 2 bytes from 0x60 & 0x64 */
#define PS2_IO_DATA_BASE            0x60
#define PS2_IO_STATUS_BASE        	0x64
#define PS2_IO_LENGTH            	0x01

#define PS2_REGISTER_DATA          	0x00
#define PS2_REGISTER_STATUS        	0x00
#define PS2_REGISTER_COMMAND        0x00

/* Some standard definitons for the PS2 controller 
 * like port count etc */
#define PS2_MAXPORTS                2
#define PS2_MAXCOMMANDS           	4
#define PS2_MAX_RETRIES             3

/* Status definitons from reading the status
 * register in the PS2-Controller */
#define PS2_STATUS_OUTPUT_FULL    	0x1
#define PS2_STATUS_INPUT_FULL     	0x2

#define PS2_GET_CONFIGURATION       0x20
#define PS2_SET_CONFIGURATION       0x60
#define PS2_INTERFACETEST_PORT1     0xAB
#define PS2_INTERFACETEST_PORT2     0xA9
#define PS2_SELFTEST                0xAA
#define PS2_SELECT_PORT2            0xD4
#define PS2_ACK                     0xFA
#define PS2_RESET_PORT              0xFF
#define PS2_ENABLE_SCANNING         0xF4
#define PS2_DISABLE_SCANNING        0xF5
#define PS2_IDENTIFY_PORT           0xF2

#define PS2_SELFTEST_OK             0x55
#define PS2_INTERFACETEST_OK        0x00

#define PS2_DISABLE_PORT1           0xAD
#define PS2_ENABLE_PORT1            0xAE

#define PS2_DISABLE_PORT2           0xA7
#define PS2_ENABLE_PORT2            0xA8

/* Configuration definitions used by the above
 * commands to read/write the configuration of the PS 2 */
#define PS2_CONFIG_PORT1_IRQ        0x01
#define PS2_CONFIG_PORT2_IRQ        0x02
#define PS2_CONFIG_POST             0x04
#define PS2_CONFIG_PORT1_DISABLED   0x10
#define PS2_CONFIG_PORT2_DISABLED   0x20
#define PS2_CONFIG_TRANSLATION   	0x40        /* First PS/2 port translation (1 = enabled, 0 = disabled) */

/* The IRQ lines the PS2 Controller uses, it's 
 * an ISA line so it's fixed */
#define PS2_PORT1_IRQ           	0x01
#define PS2_PORT2_IRQ              	0x0C

/* Command stack definitions */
#define PS2_FAILED_COMMAND          0xFF
#define PS2_RESEND_COMMAND       	0xFE
#define PS2_ACK_COMMAND           	0xFA

typedef enum _PS2CommandState {
    PS2Free             = 0,
    PS2InQueue          = 1,
    PS2WaitingResponse  = 2
} PS2CommandState_t;

/* PS2Command
 * This represents a ps2-device command that can be queued and executed */
typedef struct _PS2Command {
    volatile PS2CommandState_t  State;
    int                  	    RetryCount;
    uint8_t                	    Command;
    uint8_t*                    Response;
} PS2Command_t;

/* PS2Port
 * contains information about port status and the current device */
typedef struct _PS2Port {
    int                   	Index;
    MContract_t            	Contract;
    MCoreInterrupt_t        Interrupt;
    
    PS2Command_t            Commands[PS2_MAXCOMMANDS];
    PS2Command_t*           CurrentCommand;
    int                     ExecutionIndex;
    int                     QueueIndex;

    int                		Connected;
    int                 	Enabled;
    DevInfo_t             	Signature;
} PS2Port_t;

/* PS2Controller
 * contains all driver information and chip current status information */
typedef struct _PS2Controller {
    MContract_t            	Controller;
    DeviceIoSpace_t       	DataSpace;
    DeviceIoSpace_t       	CommandSpace;
    PS2Port_t            	Ports[PS2_MAXPORTS];
} PS2Controller_t;

/* PS2PortInitialize
 * Initializes the given port and tries 
 * to identify the device on the port */
__EXTERN OsStatus_t PS2PortInitialize(PS2Port_t *Port);

/* PS2PortQueueCommand 
 * Queues the given command up for the given port
 * if a response is needed for the previous commnad
 * Set command = PS2_RESPONSE_COMMAND and pointer to response buffer */
__EXTERN OsStatus_t PS2PortQueueCommand(PS2Port_t *Port,
    uint8_t Command, uint8_t *Response);

/* PS2PortFinishCommand 
 * Finalizes the current command and executes
 * the next command in queue (if any). */
__EXTERN OsStatus_t PS2PortFinishCommand(PS2Port_t *Port, uint8_t Result);

/* PS2ReadData
 * Reads a byte from the PS2 controller data port */
__EXTERN uint8_t PS2ReadData(int Dummy);

/* PS2WriteData
 * Writes a data byte to the PS2 controller data port */
__EXTERN OsStatus_t PS2WriteData(uint8_t Value);

/* PS2SendCommand
 * Writes the given command to the ps2-controller */
__EXTERN void PS2SendCommand(uint8_t Command);

/* PS2MouseInitialize 
 * Initializes an instance of an ps2-mouse on
 * the given PS2-Controller port */
__EXTERN OsStatus_t PS2MouseInitialize(PS2Port_t *Port);

/* PS2MouseCleanup 
 * Cleans up the ps2-mouse instance on the
 * given PS2-Controller port */
__EXTERN OsStatus_t PS2MouseCleanup(PS2Port_t *Port);

/* PS2KeyboardInitialize 
 * Initializes an instance of an ps2-keyboard on
 * the given PS2-Controller port */
__EXTERN OsStatus_t PS2KeyboardInitialize(PS2Port_t *Port, int Translation);

/* PS2KeyboardCleanup 
 * Cleans up the ps2-keyboard instance on the
 * given PS2-Controller port */
__EXTERN OsStatus_t PS2KeyboardCleanup(PS2Port_t *Port);

#endif //!_DRIVER_PS2_CONTROLLER_H_
