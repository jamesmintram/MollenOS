/* MollenOS Ohci Module
*
* Copyright 2011 - 2014, Philip Meulengracht
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
* MollenOS X86-32 USB OHCI Controller Driver
* Todo:
* Linux has a periodic timer event that checks if all finished td's has generated a interrupt to make sure
* Stability (Only tested on emulators and one real hardware pc).
*/

/* Includes */
#include <Module.h>
#include "Ohci.h"

/* Additional Includes */
#include <UsbCore.h>
#include <Scheduler.h>
#include <Heap.h>
#include <Timers.h>
#include <DeviceManager.h>

/* Better abstraction? Woah */
#include <x86\Pci.h>
#include <x86\Memory.h>

/* CLib */
#include <assert.h>
#include <string.h>

/* Globals */
uint32_t GlbOhciControllerId = 0;

/* Prototypes */
int OhciInterruptHandler(void *data);
void OhciReset(OhciController_t *Controller);
void OhciSetup(OhciController_t *Controller);

Addr_t OhciAllocateTd(OhciEndpoint_t *Ep, UsbTransferType_t Type);

/* Endpoint Prototypes */
void OhciEndpointSetup(void *Controller, UsbHcEndpoint_t *Endpoint);
void OhciEndpointDestroy(void *Controller, UsbHcEndpoint_t *Endpoint);

/* Transaction Prototypes */
void OhciTransactionInit(void *Controller, UsbHcRequest_t *Request);
UsbHcTransaction_t *OhciTransactionSetup(void *Controller, UsbHcRequest_t *Request);
UsbHcTransaction_t *OhciTransactionIn(void *Controller, UsbHcRequest_t *Request);
UsbHcTransaction_t *OhciTransactionOut(void *Controller, UsbHcRequest_t *Request);
void OhciTransactionSend(void *Controller, UsbHcRequest_t *Request);
void OhciTransactionDestroy(void *Controller, UsbHcRequest_t *Request);

/* Error Codes */
static const int _Balance[] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
const char *OhciErrorMessages[] =
{
	"No Error",
	"CRC Error",
	"Bit Stuffing Violation",
	"Data Toggle Mismatch",
	"Stall PID recieved",
	"Device Not Responding",
	"PID Check Failure",
	"Unexpected PID",
	"Data Overrun",
	"Data Underrun",
	"Reserved",
	"Reserved",
	"Buffer Overrun",
	"Buffer Underrun",
	"Not Accessed",
	"Not Accessed"
};

/* Entry point of a module */
MODULES_API void ModuleInit(void *Data)
{
	uint16_t PciCommand;
	OhciController_t *Controller = NULL;
	PciDevice_t *Device = (PciDevice_t*)Data;

	/* Allocate Resources for this Controller */
	Controller = (OhciController_t*)kmalloc(sizeof(OhciController_t));
	Controller->PciDevice = Device;
	Controller->Id = GlbOhciControllerId;

	/* Enable memory and Bus mastering and clear interrupt disable */
	PciCommand = (uint16_t)PciDeviceRead(Device, 0x4, 2);
	PciDeviceWrite(Device, 0x4, (PciCommand & ~(0x400)) | 0x2 | 0x4, 2);

	/* Get location of Registers */
	Controller->ControlSpace = Device->Header->Bar0;

	/* Sanity */
	if (Controller->ControlSpace == 0 || (Controller->ControlSpace & 0x1))
	{
		/* Yea, give me my hat back */
		kfree(Controller);
		return;
	}

	/* Now we initialise */
	GlbOhciControllerId++;

	/* Memory map needed space */
	Controller->Registers = (volatile OhciRegisters_t*)MmVirtualMapSysMemory(Controller->ControlSpace, 1);
	Controller->HccaSpace = (uint32_t)MmPhysicalAllocateBlockDma();
	Controller->HCCA = (volatile OhciHCCA_t*)Controller->HccaSpace;
	SpinlockReset(&Controller->Lock);

	/* Memset HCCA Space */
	memset((void*)Controller->HCCA, 0, 0x1000);

	/* Install IRQ Handler */
	InterruptInstallPci(Device, OhciInterruptHandler, Controller);

	/* Debug */
#ifdef _OHCI_DIAGNOSTICS_
	printf("OHCI - Id %u, bar0: 0x%x (0x%x), dma: 0x%x\n",
		Controller->Id, Controller->ControlSpace,
		(Addr_t)Controller->Registers, Controller->HccaSpace);
#endif

	/* Reset Controller */
	OhciSetup(Controller);
}

/* Helpers */
void OhciSetMode(OhciController_t *Controller, uint32_t Mode)
{
	/* First we clear the current Operation Mode */
	uint32_t Val = Controller->Registers->HcControl;
	Val = (Val & ~X86_OHCI_CTRL_USB_SUSPEND);
	Val |= Mode;
	Controller->Registers->HcControl = Val;
}

Addr_t OhciAlign(Addr_t Address, Addr_t AlignmentBitMask, Addr_t Alignment)
{
	Addr_t AlignedAddr = Address;

	if (AlignedAddr & AlignmentBitMask)
	{
		AlignedAddr &= ~AlignmentBitMask;
		AlignedAddr += Alignment;
	}

	return AlignedAddr;
}

/* Stop/Start */
void OhciStop(OhciController_t *Controller)
{
	uint32_t Temp;

	/* Disable BULK and CONTROL queues */
	Temp = Controller->Registers->HcControl;
	Temp = (Temp & ~0x00000030);
	Controller->Registers->HcControl = Temp;

	/* Tell Command Status we dont have the list filled */
	Temp = Controller->Registers->HcCommandStatus;
	Temp = (Temp & ~0x00000006);
	Controller->Registers->HcCommandStatus = Temp;
}

/* This resets a Port, this is only ever
* called from an interrupt and thus we can't use StallMs :/ */
void OhciPortReset(OhciController_t *Controller, uint32_t Port)
{
	/* Set reset */
	Controller->Registers->HcRhPortStatus[Port] = (X86_OHCI_PORT_RESET);

	/* Wait with timeout */
	WaitForCondition((Controller->Registers->HcRhPortStatus[Port] & X86_OHCI_PORT_RESET) == 0, 200, 10, "USB_OHCI: Failed to reset device on port %u\n", Port);

	/* Set Enable */
	if (Controller->PowerMode == X86_OHCI_POWER_PORT_CONTROLLED)
		Controller->Registers->HcRhPortStatus[Port] = X86_OHCI_PORT_ENABLED | X86_OHCI_PORT_POWER_ENABLE;
	else
		Controller->Registers->HcRhPortStatus[Port] = X86_OHCI_PORT_ENABLED;

	/* Stall */
	StallMs(50);
}

/* Callbacks */
void OhciPortStatus(void *ControllerData, UsbHcPort_t *Port)
{
	OhciController_t *Controller = (OhciController_t*)ControllerData;
	uint32_t Status;

	/* Wait for power to stabilize */
	StallMs(Controller->PowerOnDelayMs);

	/* Reset Port */
	OhciPortReset(Controller, Port->Id);

	/* Update information in Port */
	Status = Controller->Registers->HcRhPortStatus[Port->Id];

	/* Is it connected? */
	if (Status & X86_OHCI_PORT_CONNECTED)
		Port->Connected = 1;
	else
		Port->Connected = 0;

	/* Is it enabled? */
	if (Status & X86_OHCI_PORT_ENABLED)
		Port->Enabled = 1;
	else
		Port->Enabled = 0;

	/* Is it full-speed? */
	if (Status & X86_OHCI_PORT_LOW_SPEED)
		Port->FullSpeed = 0;
	else
		Port->FullSpeed = 1;

	/* Clear Connect Event */
	if (Controller->Registers->HcRhPortStatus[Port->Id] & X86_OHCI_PORT_CONNECT_EVENT)
		Controller->Registers->HcRhPortStatus[Port->Id] = X86_OHCI_PORT_CONNECT_EVENT;

	/* If Enable Event bit is set, clear it */
	if (Controller->Registers->HcRhPortStatus[Port->Id] & X86_OHCI_PORT_ENABLE_EVENT)
		Controller->Registers->HcRhPortStatus[Port->Id] = X86_OHCI_PORT_ENABLE_EVENT;

	/* If Suspend Event is set, clear it */
	if (Controller->Registers->HcRhPortStatus[Port->Id] & X86_OHCI_PORT_SUSPEND_EVENT)
		Controller->Registers->HcRhPortStatus[Port->Id] = X86_OHCI_PORT_SUSPEND_EVENT;

	/* If Over Current Event is set, clear it */
	if (Controller->Registers->HcRhPortStatus[Port->Id] & X86_OHCI_PORT_OVR_CURRENT_EVENT)
		Controller->Registers->HcRhPortStatus[Port->Id] = X86_OHCI_PORT_OVR_CURRENT_EVENT;

	/* If reset bit is set, clear it */
	if (Controller->Registers->HcRhPortStatus[Port->Id] & X86_OHCI_PORT_RESET_EVENT)
		Controller->Registers->HcRhPortStatus[Port->Id] = X86_OHCI_PORT_RESET_EVENT;
}

/* Port Functions */
void OhciPortCheck(OhciController_t *Controller, uint32_t Port)
{
	UsbHc_t *HcCtrl;

	/* Was it connect event and not disconnect ? */
	if (Controller->Registers->HcRhPortStatus[Port] & X86_OHCI_PORT_CONNECT_EVENT)
	{
		/* Reset on Attach */
		OhciPortReset(Controller, Port);

		if (!(Controller->Registers->HcRhPortStatus[Port] & X86_OHCI_PORT_CONNECTED))
		{
			/* Nah, disconnect event */
			/* Get HCD data */
			HcCtrl = UsbGetHcd(Controller->HcdId);

			/* Sanity */
			if (HcCtrl == NULL)
				return;

			/* Disconnect */
			UsbEventCreate(HcCtrl, (int)Port, HcdDisconnectedEvent);
		}

		/* If Device is enabled, and powered, set it up */
		if ((Controller->Registers->HcRhPortStatus[Port] & X86_OHCI_PORT_ENABLED)
			&& (Controller->Registers->HcRhPortStatus[Port] & X86_OHCI_PORT_POWER_ENABLE))
		{
			/* Get HCD data */
			HcCtrl = UsbGetHcd(Controller->HcdId);

			/* Sanity */
			if (HcCtrl == NULL)
			{
				LogDebug("OHCI", "Controller %u is zombie and is trying to register Ports!!", Controller->Id);
				return;
			}

			/* Register Device */
			UsbEventCreate(HcCtrl, (int)Port, HcdConnectedEvent);
		}
	}

	/* Clear Connect Event */
	if (Controller->Registers->HcRhPortStatus[Port] & X86_OHCI_PORT_CONNECT_EVENT)
		Controller->Registers->HcRhPortStatus[Port] = X86_OHCI_PORT_CONNECT_EVENT;

	/* If Enable Event bit is set, clear it */
	if (Controller->Registers->HcRhPortStatus[Port] & X86_OHCI_PORT_ENABLE_EVENT)
		Controller->Registers->HcRhPortStatus[Port] = X86_OHCI_PORT_ENABLE_EVENT;

	/* If Suspend Event is set, clear it */
	if (Controller->Registers->HcRhPortStatus[Port] & X86_OHCI_PORT_SUSPEND_EVENT)
		Controller->Registers->HcRhPortStatus[Port] = X86_OHCI_PORT_SUSPEND_EVENT;

	/* If Over Current Event is set, clear it */
	if (Controller->Registers->HcRhPortStatus[Port] & X86_OHCI_PORT_OVR_CURRENT_EVENT)
		Controller->Registers->HcRhPortStatus[Port] = X86_OHCI_PORT_OVR_CURRENT_EVENT;

	/* If reset bit is set, clear it */
	if (Controller->Registers->HcRhPortStatus[Port] & X86_OHCI_PORT_RESET_EVENT)
		Controller->Registers->HcRhPortStatus[Port] = X86_OHCI_PORT_RESET_EVENT;
}

void OhciPortsCheck(void *CtrlData)
{
	OhciController_t *Controller = (OhciController_t*)CtrlData;
	uint32_t i;

	/* Go through Ports */
	for (i = 0; i < Controller->Ports; i++)
	{
		/* Check �f Port has connected */
		OhciPortCheck(Controller, i);
	}
}

/* Initializes Controller Queues */
void OhciInitQueues(OhciController_t *Controller)
{
	/* Vars */
	Addr_t EdLevel;
	int i;

	/* Create the NULL Td */
	Controller->NullTd = (OhciGTransferDescriptor_t*)OhciAlign(((Addr_t)kmalloc(sizeof(OhciGTransferDescriptor_t) + X86_OHCI_STRUCT_ALIGN)), X86_OHCI_STRUCT_ALIGN_BITS, X86_OHCI_STRUCT_ALIGN);
	Controller->NullTd->BufferEnd = 0;
	Controller->NullTd->Cbp = 0;
	Controller->NullTd->NextTD = 0x0;
	Controller->NullTd->Flags = 0;

	/* Initialise ED Pool */
	for (i = 0; i < X86_OHCI_POOL_NUM_ED; i++)
	{
		/* Allocate */
		Addr_t aSpace = (Addr_t)kmalloc(sizeof(OhciEndpointDescriptor_t) + X86_OHCI_STRUCT_ALIGN);
		Controller->EDPool[i] = (OhciEndpointDescriptor_t*)OhciAlign(aSpace, X86_OHCI_STRUCT_ALIGN_BITS, X86_OHCI_STRUCT_ALIGN);

		/* Zero it out */
		memset((void*)Controller->EDPool[i], 0, sizeof(OhciEndpointDescriptor_t));

		/* Link to previous */
		Controller->EDPool[i]->NextED = 0;

		/* Set to skip and valid null Td */
		Controller->EDPool[i]->HcdFlags = 0;
		Controller->EDPool[i]->Flags = X86_OHCI_EP_SKIP;
		Controller->EDPool[i]->HeadPtr =
			(Controller->EDPool[i]->TailPtr = MmVirtualGetMapping(NULL, (Addr_t)Controller->NullTd)) | 0x1;
	}

	/* Setup Interrupt Table
	* We simply use the DMA
	* allocation */
	Controller->IntrTable = (OhciIntrTable_t*)(Controller->HccaSpace + 512);

	/* Setup first level */
	EdLevel = Controller->HccaSpace + 512;
	EdLevel += 16 * sizeof(OhciEndpointDescriptor_t);
	for (i = 0; i < 16; i++)
	{
		Controller->IntrTable->Ms16[i].NextED = EdLevel + ((i / 2) * sizeof(OhciEndpointDescriptor_t));
		Controller->IntrTable->Ms16[i].NextEDVirtual = EdLevel + ((i / 2) * sizeof(OhciEndpointDescriptor_t));
		Controller->IntrTable->Ms16[i].Flags = X86_OHCI_EP_SKIP;
	}

	/* Second level (8 ms) */
	EdLevel += 8 * sizeof(OhciEndpointDescriptor_t);
	for (i = 0; i < 8; i++)
	{
		Controller->IntrTable->Ms8[i].NextED = EdLevel + ((i / 2) * sizeof(OhciEndpointDescriptor_t));
		Controller->IntrTable->Ms8[i].NextEDVirtual = EdLevel + ((i / 2) * sizeof(OhciEndpointDescriptor_t));
		Controller->IntrTable->Ms8[i].Flags = X86_OHCI_EP_SKIP;
	}

	/* Third level (4 ms) */
	EdLevel += 4 * sizeof(OhciEndpointDescriptor_t);
	for (i = 0; i < 4; i++)
	{
		Controller->IntrTable->Ms4[i].NextED = EdLevel + ((i / 2) * sizeof(OhciEndpointDescriptor_t));
		Controller->IntrTable->Ms4[i].NextEDVirtual = EdLevel + ((i / 2) * sizeof(OhciEndpointDescriptor_t));
		Controller->IntrTable->Ms4[i].Flags = X86_OHCI_EP_SKIP;
	}

	/* Fourth level (2 ms) */
	EdLevel += 2 * sizeof(OhciEndpointDescriptor_t);
	for (i = 0; i < 2; i++)
	{
		Controller->IntrTable->Ms2[i].NextED = EdLevel + sizeof(OhciEndpointDescriptor_t);
		Controller->IntrTable->Ms2[i].NextEDVirtual = EdLevel + sizeof(OhciEndpointDescriptor_t);
		Controller->IntrTable->Ms2[i].Flags = X86_OHCI_EP_SKIP;
	}

	/* Last level (1 ms) */
	Controller->IntrTable->Ms1[0].NextED = 0;
	Controller->IntrTable->Ms1[0].NextEDVirtual = 0;
	Controller->IntrTable->Ms1[0].Flags = X86_OHCI_EP_SKIP;

	/* Setup HCCA */
	for (i = 0; i < 32; i++)
	{
		/* 0 -> 0     16 -> 0
		* 1 -> 8     17 -> 8
		* 2 -> 4     18 -> 4
		* 3 -> 12    19 -> 12
		* 4 -> 2     20 -> 2
		* 5 -> 10    21 -> 10
		* 6 -> 6     22 -> 6
		* 7 -> 14    23 -> 14
		* 8 -> 1     24 -> 1
		* 9 -> 9     25 -> 9
		* 10 -> 5    26 -> 5
		* 11 -> 13   27 -> 13
		* 12 -> 3    28 -> 3
		* 13 -> 11   29 -> 11
		* 14 -> 7    30 -> 7
		* 15 -> 15   31 -> 15 */
		Controller->ED32[i] = (OhciEndpointDescriptor_t*)
			((Controller->HccaSpace + 512) + (_Balance[i & 0xF] * sizeof(OhciEndpointDescriptor_t)));
		Controller->HCCA->InterruptTable[i] =
			((Controller->HccaSpace + 512) + (_Balance[i & 0xF] * sizeof(OhciEndpointDescriptor_t)));

		/* This gives us the tree
		* This means our 16 first ED's in the IntrTable are the buttom of the tree
		*   0          1         2         3        4         5         6        7        8        9        10        11        12        13        15
		*  / \        / \       / \       / \      / \       / \       / \      / \      / \      / \       / \       / \       / \       / \       / \
		* 0  16      8  24     4  20     12 28    2  18     10 26     6  22    14 30    1  17    9  25     5  21     13 29     3  19     7  23     15 31
		*/

	}

	/* Load Balancing */
	Controller->I32 = 0;
	Controller->I16 = 0;
	Controller->I8 = 0;
	Controller->I4 = 0;
	Controller->I2 = 0;

	/* Allocate a Transaction list */
	Controller->TransactionsWaitingBulk = 0;
	Controller->TransactionsWaitingControl = 0;
	Controller->TransactionQueueBulk = 0;
	Controller->TransactionQueueControl = 0;
	Controller->TransactionList = list_create(LIST_SAFE);
}

/* Take control of OHCI controller */
int OhciTakeControl(OhciController_t *Controller)
{
	/* Hold stuff */
	uint32_t Temp, i;

	/* Is SMM the bitch? */
	if (Controller->Registers->HcControl & X86_OHCI_CTRL_INT_ROUTING)
	{
		/* Ok, SMM has control, now give me my hat back */
		Temp = Controller->Registers->HcCommandStatus;
		Temp |= X86_OHCI_CMD_OWNERSHIP;
		Controller->Registers->HcCommandStatus = Temp;

		/* Wait for InterruptRouting to clear */
		WaitForConditionWithFault(i, (Controller->Registers->HcControl & X86_OHCI_CTRL_INT_ROUTING) == 0, 250, 10);

		if (i != 0)
		{
			/* Did not work, reset bit, try that */
			Controller->Registers->HcControl &= ~X86_OHCI_CTRL_INT_ROUTING;
			WaitForConditionWithFault(i, (Controller->Registers->HcControl & X86_OHCI_CTRL_INT_ROUTING) == 0, 250, 10);

			if (i != 0)
			{
				LogDebug("OHCI", "USB_OHCI: failed to clear routing bit");
				LogDebug("OHCI", "USB_OHCI: SMM Won't give us the Controller, we're backing down >(");
				
				MmPhysicalFreeBlock(Controller->HccaSpace);
				kfree(Controller);
				return -1;
			}
		}
	}
	/* Is BIOS the bitch?? */
	else if (Controller->Registers->HcControl & X86_OHCI_CTRL_FSTATE_BITS)
	{
		if ((Controller->Registers->HcControl & X86_OHCI_CTRL_FSTATE_BITS) != X86_OHCI_CTRL_USB_WORKING)
		{
			/* Resume Usb Operations */
			OhciSetMode(Controller, X86_OHCI_CTRL_USB_WORKING);

			/* Wait 10 ms */
			StallMs(10);
		}
	}
	else
	{
		/* Cold Boot */

		/* Wait 10 ms */
		StallMs(10);
	}

	return 0;
}

/* Reset Controller */
void OhciReset(OhciController_t *Controller)
{
	uint32_t TempValue, Temp, FmInt;
	int i;

	/* Step 4. Verify HcFmInterval and save it  */
	FmInt = Controller->Registers->HcFmInterval;

	/* Sanity */
	if (X86_OHCI_GETFSMP(FmInt) == 0)
	{
		/* What the fuck OHCI */
		FmInt |= X86_OHCI_FSMP(FmInt) << 16;
	}

	if ((FmInt & X86_OHCI_FI_MASK) == 0)
	{
		/* Really, BIOS?! */
		FmInt |= X86_OHCI_FI;
	}

	/* We should check here if HcControl has RemoteWakeup Connected and then set device to remote wake capable */

	/* Disable All Interrupts */
	Controller->Registers->HcInterruptDisable = (uint32_t)X86_OHCI_INTR_MASTER_INTR;

	/* Perform a reset of HcCtrl Controller */
	OhciSetMode(Controller, X86_OHCI_CTRL_USB_SUSPEND);
	StallMs(10);

	/* Set bit 0 to Request reboot */
	Temp = Controller->Registers->HcCommandStatus;
	Temp |= X86_OHCI_CMD_RESETCTRL;
	Controller->Registers->HcCommandStatus = Temp;

	/* Wait for reboot (takes maximum of 10 ms) */
	WaitForConditionWithFault(i, (Controller->Registers->HcCommandStatus & X86_OHCI_CMD_RESETCTRL) == 0, 50, 1);

	/* Sanity */
	if (i != 0)
	{
		LogDebug("OHCI", "controller %u failed to reboot", Controller->HcdId);
		LogDebug("OHCI", "Reset Timeout :(");
		return;
	}

	/* Restore FmInt */
	Controller->Registers->HcFmInterval = FmInt;

	/* Toggle FIT */
	Controller->Registers->HcFmInterval ^= 0x80000000;

	/**************************************/
	/* We now have 2 ms to complete setup
	* and put it in Operational Mode */
	/**************************************/

	/* Set HcHCCA to phys Address of HCCA */
	Controller->Registers->HcHCCA = Controller->HccaSpace;

	/* Initial values for frame */
	Controller->HCCA->CurrentFrame = 0;
	Controller->HCCA->HeadDone = 0;

	/* Setup initial ED points */
	Controller->Registers->HcControlHeadED =
		Controller->Registers->HcControlCurrentED = 0;
	Controller->Registers->HcBulkHeadED =
		Controller->Registers->HcBulkCurrentED = 0;

	/* Set HcEnableInterrupt to all except SOF and OC */
	Controller->Registers->HcInterruptDisable = (X86_OHCI_INTR_SOF | X86_OHCI_INTR_ROOT_HUB_EVENT | X86_OHCI_INTR_OWNERSHIP_EVENT);
	Controller->Registers->HcInterruptStatus = ~(uint32_t)0;
	Controller->Registers->HcInterruptEnable = (X86_OHCI_INTR_SCHEDULING_OVRRN | X86_OHCI_INTR_HEAD_DONE |
		X86_OHCI_INTR_RESUME_DETECT | X86_OHCI_INTR_FATAL_ERROR | X86_OHCI_INTR_FRAME_OVERFLOW | X86_OHCI_INTR_MASTER_INTR);

	/* Set HcPeriodicStart to a value that is 90% of FrameInterval in HcFmInterval */
	TempValue = (FmInt & X86_OHCI_FI_MASK);
	Controller->Registers->HcPeriodicStart = (TempValue / 10) * 9;

	/* Clear Lists, Mode, Ratio and IR */
	Temp = (Temp & ~(0x0000003C | X86_OHCI_CTRL_USB_SUSPEND | 0x3 | 0x100));

	/* Set Ratio (4:1) and Mode (Operational) */
	Temp |= (0x3 | X86_OHCI_CTRL_USB_WORKING);
	Controller->Registers->HcControl = Temp | X86_OHCI_CTRL_ALL_LISTS | X86_OHCI_CTRL_REMOTE_WAKE;
}

/* Resets the controllre to a working state from initial */
void OhciSetup(OhciController_t *Controller)
{
	UsbHc_t *HcCtrl;
	uint32_t TempValue = 0;
	int i;

	/* Step 1. Verify the Revision */
	TempValue = (Controller->Registers->HcRevision & 0xFF);
	if (TempValue != X86_OHCI_REVISION
		&& TempValue != 0x11)
	{
		LogDebug("OHCI", "Revision is wrong (0x%x), exiting :(", TempValue);
		MmPhysicalFreeBlock(Controller->HccaSpace);
		kfree(Controller);
		return;
	}

	/* Step 2. Init Virtual Queues */
	OhciInitQueues(Controller);

	/* Step 3. Gain control of Controller */
	if (OhciTakeControl(Controller) == -1)
		return;

	/* Step 4. Reset Controller */
	OhciReset(Controller);

	/* Controller is now running! */
#ifdef _OHCI_DIAGNOSTICS_
	DebugPrint("OHCI: Controller %u Started, Control 0x%x, Ints 0x%x, FmInterval 0x%x\n",
		Controller->Id, Controller->Registers->HcControl, Controller->Registers->HcInterruptEnable, Controller->Registers->HcFmInterval);
#endif

	/* Check Power Mode */
	if (Controller->Registers->HcRhDescriptorA & (1 << 9))
	{
		Controller->PowerMode = X86_OHCI_POWER_ALWAYS_ON;
		Controller->Registers->HcRhStatus = X86_OHCI_STATUS_POWER_ON;
		Controller->Registers->HcRhDescriptorB = 0;
	}
	else
	{
		/* Ports are power-switched
		* Check Mode */
		if (Controller->Registers->HcRhDescriptorA & (1 << 8))
		{
			/* This is favorable Mode
			* (If this is supported we set power-mask so that all Ports control their own power) */
			Controller->PowerMode = X86_OHCI_POWER_PORT_CONTROLLED;
			Controller->Registers->HcRhDescriptorB = 0xFFFF0000;
		}
		else
		{
			/* Global Power Switch */
			Controller->Registers->HcRhDescriptorB = 0;
			Controller->Registers->HcRhStatus = X86_OHCI_STATUS_POWER_ON;
			Controller->PowerMode = X86_OHCI_POWER_PORT_GLOBAL;
		}
	}

	/* Get Port count from (DescriptorA & 0x7F) */
	Controller->Ports = Controller->Registers->HcRhDescriptorA & 0x7F;

	/* Sanity */
	if (Controller->Ports > 15)
		Controller->Ports = 15;

	/* Set RhA */
	Controller->Registers->HcRhDescriptorA &= ~(0x00000000 | X86_OHCI_DESCA_DEVICE_TYPE);

	/* Get Power On Delay
	* PowerOnToPowerGoodTime (24 - 31)
	* This byte specifies the duration HCD has to wait before
	* accessing a powered-on Port of the Root Hub.
	* It is implementation-specific.  The unit of time is 2 ms.
	* The duration is calculated as POTPGT * 2 ms.
	*/
	TempValue = Controller->Registers->HcRhDescriptorA;
	TempValue >>= 24;
	TempValue &= 0x000000FF;
	TempValue *= 2;

	/* Give it atleast 100 ms :p */
	if (TempValue < 100)
		TempValue = 100;

	Controller->PowerOnDelayMs = TempValue;

#ifdef _OHCI_DIAGNOSTICS_
	DebugPrint("OHCI: Ports %u (power Mode %u, power delay %u)\n",
		Controller->Ports, Controller->PowerMode, TempValue);
#endif

	/* Setup HCD */
	HcCtrl = UsbInitController((void*)Controller, OhciController, Controller->Ports);

	/* Port Functions */
	HcCtrl->PortSetup = OhciPortStatus;

	/* Callback Functions */
	HcCtrl->RootHubCheck = OhciPortsCheck;
	HcCtrl->Reset = OhciReset;

	/* Endpoint Functions */
	HcCtrl->EndpointSetup = OhciEndpointSetup;
	HcCtrl->EndpointDestroy = OhciEndpointDestroy;

	/* Transaction Functions */
	HcCtrl->TransactionInit = OhciTransactionInit;
	HcCtrl->TransactionSetup = OhciTransactionSetup;
	HcCtrl->TransactionIn = OhciTransactionIn;
	HcCtrl->TransactionOut = OhciTransactionOut;
	HcCtrl->TransactionSend = OhciTransactionSend;
	HcCtrl->TransactionDestroy = OhciTransactionDestroy;

	Controller->HcdId = UsbRegisterController(HcCtrl);

	/* Setup Ports */
	for (i = 0; i < (int)Controller->Ports; i++)
	{
		int p = i;

		/* Make sure power is on */
		if (!(Controller->Registers->HcRhPortStatus[i] & X86_OHCI_PORT_POWER_ENABLE))
		{
			/* Powerup! */
			Controller->Registers->HcRhPortStatus[i] = X86_OHCI_PORT_POWER_ENABLE;
		}

		/* Check if Port is connected */
		if (Controller->Registers->HcRhPortStatus[i] & X86_OHCI_PORT_CONNECTED)
			UsbEventCreate(UsbGetHcd(Controller->HcdId), p, HcdConnectedEvent);
	}

	/* Now we can enable hub events (and clear interrupts) */
	Controller->Registers->HcInterruptStatus &= ~(uint32_t)0;
	Controller->Registers->HcInterruptEnable = X86_OHCI_INTR_ROOT_HUB_EVENT;
}

/* Try to visualize the IntrTable */
void OhciVisualizeQueue(OhciController_t *Controller)
{
	/* For each of the 32 base's */
	int i;

	for (i = 0; i < 32; i++)
	{
		/* Get a base */
		OhciEndpointDescriptor_t *EpPtr = Controller->ED32[i];

		/* Keep going */
		while (EpPtr)
		{
			/* Print */
			LogRaw("0x%x -> ", (EpPtr->Flags & X86_OHCI_EP_SKIP));

			/* Get next */
			EpPtr = (OhciEndpointDescriptor_t*)EpPtr->NextEDVirtual;
		}

		/* Newline */
		LogRaw("\n");
	}
}

/* ED Functions */
Addr_t OhciAllocateEp(OhciController_t *Controller, UsbTransferType_t Type)
{
	Addr_t cIndex = 0;
	int i;

	/* Pick a QH */
	SpinlockAcquire(&Controller->Lock);

	/* Grap it, locked operation */
	if (Type == ControlTransfer
		|| Type == BulkTransfer)
	{
		/* Grap Index */
		for (i = 0; i < X86_OHCI_POOL_NUM_ED; i++)
		{
			/* Sanity */
			if (Controller->EDPool[i]->HcdFlags & X86_OHCI_ED_ALLOCATED)
				continue;

			/* Yay!! */
			Controller->EDPool[i]->HcdFlags |= X86_OHCI_ED_ALLOCATED;
			cIndex = (Addr_t)i;
			break;
		}

		/* Sanity */
		if (i == 50)
			kernel_panic("USB_OHCI::WTF RAN OUT OF EDS\n");
	}
	else if (Type == InterruptTransfer
		|| Type == IsochronousTransfer)
	{
		/* Allocate */
		Addr_t aSpace = (Addr_t)kmalloc(sizeof(OhciEndpointDescriptor_t) + X86_OHCI_STRUCT_ALIGN);
		cIndex = OhciAlign(aSpace, X86_OHCI_STRUCT_ALIGN_BITS, X86_OHCI_STRUCT_ALIGN);

		/* Zero it out */
		memset((void*)cIndex, 0, sizeof(OhciEndpointDescriptor_t));
	}

	/* Release Lock */
	SpinlockRelease(&Controller->Lock);

	return cIndex;
}

void OhciEpInit(OhciEndpointDescriptor_t *Ed, UsbHcTransaction_t *FirstTd, UsbTransferType_t Type,
	uint32_t Address, uint32_t Endpoint, uint32_t PacketSize, uint32_t LowSpeed)
{
	/* Set Head & Tail Td */
	if ((Addr_t)FirstTd->TransferDescriptor == X86_OHCI_TRANSFER_END_OF_LIST)
	{
		Ed->HeadPtr = X86_OHCI_TRANSFER_END_OF_LIST;
		Ed->TailPtr = 0;
	}
	else
	{
		Addr_t FirstTdAddr = (Addr_t)FirstTd->TransferDescriptor;
		Addr_t LastTd = 0;

		/* Get tail */
		UsbHcTransaction_t *FirstLink = FirstTd;
		while (FirstLink->Link)
			FirstLink = FirstLink->Link;
		LastTd = (Addr_t)FirstLink->TransferDescriptor;

		Ed->TailPtr = MmVirtualGetMapping(NULL, (Addr_t)LastTd);
		Ed->HeadPtr = MmVirtualGetMapping(NULL, (Addr_t)FirstTdAddr) | 0x1;
	}

	/* Shared flags */
	Ed->Flags = X86_OHCI_EP_SKIP;
	Ed->Flags |= (Address & X86_OHCI_EP_ADDR_BITS);
	Ed->Flags |= X86_OHCI_EP_EP_NUM((Endpoint & X86_OHCI_EP_EP_NUM_BITS));
	Ed->Flags |= X86_OHCI_EP_PID_TD; /* Get PID from Td */
	Ed->Flags |= X86_OHCI_EP_LOWSPEED(LowSpeed); /* Problem!! */
	Ed->Flags |= X86_OHCI_EP_PACKET_SIZE((PacketSize & X86_OHCI_EP_PACKET_BITS));
	Ed->Flags |= X86_OHCI_EP_TYPE(((uint32_t)Type & 0xF));

	/* Add for IsoChro */
	if (Type == IsochronousTransfer)
		Ed->Flags |= X86_OHCI_EP_ISOCHRONOUS;
}

/* Td Functions */
Addr_t OhciAllocateTd(OhciEndpoint_t *Ep, UsbTransferType_t Type)
{
	size_t i;
	Addr_t cIndex = 0xFFFF;
	OhciGTransferDescriptor_t *Td;

	/* Pick a QH */
	SpinlockAcquire(&Ep->Lock);

	/* Sanity */
	if (Type == ControlTransfer
		|| Type == BulkTransfer)
	{
		/* Grap it, locked operation */
		for (i = 0; i < Ep->TdsAllocated; i++)
		{
			/* Sanity */
			if (Ep->TDPool[i]->Flags & X86_OHCI_TD_ALLOCATED)
				continue;

			/* Yay!! */
			Ep->TDPool[i]->Flags |= X86_OHCI_TD_ALLOCATED;
			cIndex = (Addr_t)i;
			break;
		}

		/* Sanity */
		if (i == Ep->TdsAllocated)
			kernel_panic("USB_OHCI::WTF ran out of TD's!!!!\n");
	}
	else if (Type == InterruptTransfer)
	{
		/* Allocate a new */
		Td = (OhciGTransferDescriptor_t*)OhciAlign(((Addr_t)kmalloc(sizeof(OhciGTransferDescriptor_t) + X86_OHCI_STRUCT_ALIGN)), X86_OHCI_STRUCT_ALIGN_BITS, X86_OHCI_STRUCT_ALIGN);

		/* Null it */
		memset((void*)Td, 0, sizeof(OhciGTransferDescriptor_t));

		/* Set as index */
		cIndex = (Addr_t)Td;
	}
	else
	{
		/* Allocate iDescriptor */
		OhciITransferDescriptor_t *iTd = (OhciITransferDescriptor_t*)OhciAlign(((Addr_t)kmalloc(sizeof(OhciITransferDescriptor_t) + X86_OHCI_STRUCT_ALIGN)), X86_OHCI_STRUCT_ALIGN_BITS, X86_OHCI_STRUCT_ALIGN);

		/* Null it */
		memset((void*)iTd, 0, sizeof(OhciITransferDescriptor_t));

		/* Set as index */
		cIndex = (Addr_t)iTd;
	}

	/* Release Lock */
	SpinlockRelease(&Ep->Lock);

	return cIndex;
}

OhciGTransferDescriptor_t *OhciTdSetup(OhciEndpoint_t *Ep, UsbTransferType_t Type,
	Addr_t NextTD, uint32_t Toggle, uint8_t RequestDirection,
	uint8_t RequestType, uint8_t RequestValueLo, uint8_t RequestValueHi, uint16_t RequestIndex,
	uint16_t RequestLength, void **TDBuffer)
{
	UsbPacket_t *Packet;
	OhciGTransferDescriptor_t *Td;
	Addr_t TdPhys;
	void *Buffer;
	Addr_t TDIndex;

	/* Allocate a Td */
	TDIndex = OhciAllocateTd(Ep, Type);

	/* Grab a Td and a Buffer */
	Td = Ep->TDPool[TDIndex];
	Buffer = Ep->TDPoolBuffers[TDIndex];
	TdPhys = Ep->TDPoolPhysical[TDIndex];

	/* EOL ? */
	if (NextTD == X86_OHCI_TRANSFER_END_OF_LIST)
		Td->NextTD = 0;
	else	/* Get physical Address of NextTD and set NextTD to that */
		Td->NextTD = MmVirtualGetMapping(NULL, (VirtAddr_t)NextTD);

	/* Setup the Td for a SETUP Td */
	Td->Flags = X86_OHCI_TD_ALLOCATED;
	Td->Flags |= X86_OHCI_TRANSFER_BUF_ROUNDING;
	Td->Flags |= X86_OHCI_TRANSFER_BUF_PID_SETUP;
	Td->Flags |= X86_OHCI_TRANSFER_BUF_NO_INTERRUPT;
	Td->Flags |= (Toggle << 24);
	Td->Flags |= X86_OHCI_TRANSFER_BUF_TD_TOGGLE;
	Td->Flags |= X86_OHCI_TRANSFER_BUF_NOCC;

	/* Setup the SETUP Request */
	*TDBuffer = Buffer;
	Packet = (UsbPacket_t*)Buffer;
	Packet->Direction = RequestDirection;
	Packet->Type = RequestType;
	Packet->ValueLo = RequestValueLo;
	Packet->ValueHi = RequestValueHi;
	Packet->Index = RequestIndex;
	Packet->Length = RequestLength;

	/* Set Td Buffer */
	Td->Cbp = MmVirtualGetMapping(NULL, (VirtAddr_t)Buffer);
	Td->BufferEnd = Td->Cbp + sizeof(UsbPacket_t) - 1;

	return Td;
}

OhciGTransferDescriptor_t *OhciTdIo(OhciEndpoint_t *OhciEp, UsbTransferType_t Type,
	Addr_t NextTD, UsbHcEndpoint_t *Endpoint, uint32_t pid,
	uint32_t Length, void **TDBuffer)
{
	OhciGTransferDescriptor_t *Td = NULL;
	OhciITransferDescriptor_t *iTd = NULL;
	void *Buffer;
	Addr_t TDIndex;

	/* Allocate a Td */
	TDIndex = OhciAllocateTd(OhciEp, Type);

	/* Sanity */
	if (Type == ControlTransfer || Type == BulkTransfer)
	{
		/* Grab a Td and a Buffer */
		Td = OhciEp->TDPool[TDIndex];
		Buffer = OhciEp->TDPoolBuffers[TDIndex];
	}
	else if (Type == InterruptTransfer)
	{
		/* Cast */
		Td = (OhciGTransferDescriptor_t*)TDIndex;

		/* Allocate a buffer */
		Buffer = (void*)kmalloc_a(0x1000);
	}
	else
	{
		/* Cast */
		iTd = (OhciITransferDescriptor_t*)TDIndex;

		/* Allocate a buffer */
		Buffer = (void*)kmalloc_a(Length);

		/* Calculate frame count */
		uint32_t BufItr = 0;
		uint32_t FrameItr = 0;
		uint32_t Crossed = 0;
		uint32_t FrameCount = Length / Endpoint->MaxPacketSize;
		if (Length % Endpoint->MaxPacketSize)
			FrameCount++;

		/* IF framecount is > 8, nono */
		if (FrameCount > 8)
			FrameCount = 8;

		/* Setup */
		iTd->Flags = 0;
		iTd->Flags |= X86_OHCI_TRANSFER_BUF_FRAMECOUNT(FrameCount - 1);
		iTd->Flags |= X86_OHCI_TRANSFER_BUF_NOCC;

		/* Buffer */
		iTd->Bp0 = MmVirtualGetMapping(NULL, (VirtAddr_t)Buffer);
		iTd->BufferEnd = iTd->Bp0 + Length - 1;

		/* Setup offsets */
		while (FrameCount)
		{
			/* Set offset 0 */
			iTd->Offsets[FrameItr] = (BufItr & 0xFFF);
			iTd->Offsets[FrameItr] = ((Crossed & 0x1) << 12);

			/* Increase buffer */
			BufItr += Endpoint->MaxPacketSize;

			/* Sanity */
			if (BufItr >= 0x1000)
			{
				/* Reduce, set crossed */
				BufItr -= 0x1000;
				Crossed = 1;
			}

			/* Set iterators */
			FrameItr++;
			FrameCount--;
		}

		/* EOL ? */
		if (NextTD == X86_OHCI_TRANSFER_END_OF_LIST)
			iTd->NextTD = X86_OHCI_TRANSFER_END_OF_LIST;
		else	/* Get physical Address of NextTD and set NextTD to that */
			iTd->NextTD = MmVirtualGetMapping(NULL, (VirtAddr_t)NextTD);

		/* Done */
		return (OhciGTransferDescriptor_t*)iTd;
	}

	/* EOL ? */
	if (NextTD == X86_OHCI_TRANSFER_END_OF_LIST)
		Td->NextTD = X86_OHCI_TRANSFER_END_OF_LIST;
	else	/* Get physical Address of NextTD and set NextTD to that */
		Td->NextTD = MmVirtualGetMapping(NULL, (VirtAddr_t)NextTD);

	/* Setup the Td for a IO Td */
	Td->Flags = X86_OHCI_TD_ALLOCATED;
	Td->Flags |= X86_OHCI_TRANSFER_BUF_ROUNDING;
	Td->Flags |= pid;
	Td->Flags |= X86_OHCI_TRANSFER_BUF_NO_INTERRUPT;
	Td->Flags |= X86_OHCI_TRANSFER_BUF_TD_TOGGLE;
	Td->Flags |= X86_OHCI_TRANSFER_BUF_NOCC;
	Td->Flags |= (Endpoint->Toggle << 24);

	*TDBuffer = Buffer;

	/* Bytes to transfer?? */
	if (Length > 0)
	{
		Td->Cbp = MmVirtualGetMapping(NULL, (VirtAddr_t)Buffer);
		Td->BufferEnd = Td->Cbp + Length - 1;
	}
	else
	{
		Td->Cbp = 0;
		Td->BufferEnd = 0;
	}

	return Td;
}

/* Endpoint Functions */
void OhciEndpointSetup(void *Controller, UsbHcEndpoint_t *Endpoint)
{
	/* Cast */
	OhciController_t *oCtrl = (OhciController_t*)Controller;
	Addr_t BufAddr = 0, BufAddrMax = 0;
	Addr_t Pool, PoolPhys;
	size_t i;

	/* Allocate a structure */
	OhciEndpoint_t *oEp = (OhciEndpoint_t*)kmalloc(sizeof(OhciEndpoint_t));

	/* Construct the lock */
	SpinlockReset(&oEp->Lock);

	/* Woah */
	_CRT_UNUSED(oCtrl);

	/* Now, we want to allocate some TD's 
	 * but it largely depends on what kind of endpoint this is */
	if (Endpoint->Type == X86_USB_EP_TYPE_CONTROL)
		oEp->TdsAllocated = OHCI_ENDPOINT_MIN_ALLOCATED;
	else if (Endpoint->Type == X86_USB_EP_TYPE_BULK)
	{
		/* Depends on the maximum transfer */
		oEp->TdsAllocated = DEVICEMANAGER_MAX_IO_SIZE / Endpoint->MaxPacketSize;
		
		/* Take in account control packets and other stuff */
		oEp->TdsAllocated += OHCI_ENDPOINT_MIN_ALLOCATED;
	}
	else
	{
		/* We handle interrupt & iso dynamically 
		 * we don't predetermine their sizes */
		oEp->TdsAllocated = 0;
		Endpoint->AttachedData = oEp;
		return;
	}

	/* Now, we do the actual allocation */
	oEp->TDPool = (OhciGTransferDescriptor_t**)kmalloc(sizeof(OhciGTransferDescriptor_t*) * oEp->TdsAllocated);
	oEp->TDPoolBuffers = (Addr_t**)kmalloc(sizeof(Addr_t*) * oEp->TdsAllocated);
	oEp->TDPoolPhysical = (Addr_t*)kmalloc(sizeof(Addr_t) * oEp->TdsAllocated);

	/* Allocate a TD block */
	Pool = (Addr_t)kmalloc((sizeof(OhciGTransferDescriptor_t) * oEp->TdsAllocated) + X86_OHCI_STRUCT_ALIGN);
	Pool = OhciAlign(Pool, X86_OHCI_STRUCT_ALIGN_BITS, X86_OHCI_STRUCT_ALIGN);
	PoolPhys = MmVirtualGetMapping(NULL, Pool);

	/* Allocate buffers */
	BufAddr = (Addr_t)kmalloc_a(PAGE_SIZE);
	BufAddrMax = BufAddr + PAGE_SIZE - 1;

	/* Memset it */
	memset((void*)Pool, 0, sizeof(OhciGTransferDescriptor_t) * oEp->TdsAllocated);

	/* Iterate it */
	for (i = 0; i < oEp->TdsAllocated; i++)
	{
		/* Set */
		oEp->TDPool[i] = (OhciGTransferDescriptor_t*)Pool;
		oEp->TDPoolPhysical[i] = PoolPhys;

		/* Allocate another page? */
		if (BufAddr > BufAddrMax)
		{
			BufAddr = (Addr_t)kmalloc_a(PAGE_SIZE);
			BufAddrMax = BufAddr + PAGE_SIZE - 1;
		}

		/* Setup Buffer */
		oEp->TDPoolBuffers[i] = (Addr_t*)BufAddr;
		oEp->TDPool[i]->Cbp = MmVirtualGetMapping(NULL, BufAddr);
		oEp->TDPool[i]->NextTD = 0x1;

		/* Increase */
		Pool += sizeof(OhciGTransferDescriptor_t);
		PoolPhys += sizeof(OhciGTransferDescriptor_t);
		BufAddr += Endpoint->MaxPacketSize;
	}

	/* Done! Save */
	Endpoint->AttachedData = oEp;
}

void OhciEndpointDestroy(void *Controller, UsbHcEndpoint_t *Endpoint)
{
	/* Cast */
	OhciController_t *oCtrl = (OhciController_t*)Controller;
	OhciEndpoint_t *oEp = (OhciEndpoint_t*)Endpoint->AttachedData;
	
	/* Sanity */
	if (oEp == NULL)
		return;
	
	OhciGTransferDescriptor_t *oTd = oEp->TDPool[0];
	size_t i;

	/* Woah */
	_CRT_UNUSED(oCtrl);

	/* Sanity */
	if (oEp->TdsAllocated != 0)
	{
		/* Let's free all those resources */
		for (i = 0; i < oEp->TdsAllocated; i++)
		{
			/* free buffer */
			kfree(oEp->TDPoolBuffers[i]);
		}

		/* Free blocks */
		kfree(oTd);
		kfree(oEp->TDPoolBuffers);
		kfree(oEp->TDPoolPhysical);
		kfree(oEp->TDPool);
	}

	/* Free the descriptor */
	kfree(oEp);
}

/* Transaction Functions */

/* This one prepaires an ED */
void OhciTransactionInit(void *Controller, UsbHcRequest_t *Request)
{
	/* Vars */
	OhciController_t *Ctrl = (OhciController_t*)Controller;
	Addr_t Temp;

	/* Allocate an EP */
	Temp = OhciAllocateEp(Ctrl, Request->Type);

	/* We allocate new ep descriptors for Iso & Int */
	if (Request->Type == ControlTransfer
		|| Request->Type == BulkTransfer)
	{
		Ctrl->EDPool[Temp]->Flags |= X86_OHCI_EP_SKIP;
		Request->Data = Ctrl->EDPool[Temp];
	}
	else
		Request->Data = (void*)Temp;

	/* Set as not Completed for start */
	Request->Status = TransferNotProcessed;
}

/* This one prepaires an setup Td */
UsbHcTransaction_t *OhciTransactionSetup(void *Controller, UsbHcRequest_t *Request)
{
	OhciController_t *Ctrl = (OhciController_t*)Controller;
	UsbHcTransaction_t *Transaction;

	/* Unused */
	_CRT_UNUSED(Ctrl);

	/* Allocate Transaction */
	Transaction = (UsbHcTransaction_t*)kmalloc(sizeof(UsbHcTransaction_t));
	Transaction->IoBuffer = 0;
	Transaction->IoLength = 0;
	Transaction->Link = NULL;

	/* Create the Td */
	Transaction->TransferDescriptor = (void*)OhciTdSetup(Request->Endpoint->AttachedData, 
		Request->Type, X86_OHCI_TRANSFER_END_OF_LIST,
		Request->Endpoint->Toggle, Request->Packet.Direction, Request->Packet.Type,
		Request->Packet.ValueLo, Request->Packet.ValueHi, Request->Packet.Index,
		Request->Packet.Length, &Transaction->TransferBuffer);

	/* If previous Transaction */
	if (Request->Transactions != NULL)
	{
		OhciGTransferDescriptor_t *PrevTd;
		UsbHcTransaction_t *cTrans = Request->Transactions;

		while (cTrans->Link)
			cTrans = cTrans->Link;

		PrevTd = (OhciGTransferDescriptor_t*)cTrans->TransferDescriptor;
		PrevTd->NextTD = MmVirtualGetMapping(NULL, (VirtAddr_t)Transaction->TransferDescriptor);
	}

	return Transaction;
}

/* This one prepaires an in Td */
UsbHcTransaction_t *OhciTransactionIn(void *Controller, UsbHcRequest_t *Request)
{
	OhciController_t *Ctrl = (OhciController_t*)Controller;
	UsbHcTransaction_t *Transaction;

	/* Unused */
	_CRT_UNUSED(Ctrl);

	/* Allocate Transaction */
	Transaction = (UsbHcTransaction_t*)kmalloc(sizeof(UsbHcTransaction_t));
	Transaction->TransferDescriptorCopy = NULL;
	Transaction->IoBuffer = Request->IoBuffer;
	Transaction->IoLength = Request->IoLength;
	Transaction->Link = NULL;

	/* Setup Td */
	Transaction->TransferDescriptor = (void*)OhciTdIo(Request->Endpoint->AttachedData, 
		Request->Type, X86_OHCI_TRANSFER_END_OF_LIST, Request->Endpoint,
		X86_OHCI_TRANSFER_BUF_PID_IN, Request->IoLength,
		&Transaction->TransferBuffer);

	/* If previous Transaction */
	if (Request->Transactions != NULL)
	{
		OhciGTransferDescriptor_t *PrevTd;
		UsbHcTransaction_t *cTrans = Request->Transactions;

		while (cTrans->Link)
			cTrans = cTrans->Link;

		PrevTd = (OhciGTransferDescriptor_t*)cTrans->TransferDescriptor;
		PrevTd->NextTD = MmVirtualGetMapping(NULL, (VirtAddr_t)Transaction->TransferDescriptor);
	}

	/* We might need a copy */
	if (Request->Type == InterruptTransfer
		|| Request->Type == IsochronousTransfer)
	{
		/* Allocate TD */
		Transaction->TransferDescriptorCopy = 
			(void*)OhciAllocateTd(Request->Endpoint->AttachedData, Request->Type);
	}

	/* Done */
	return Transaction;
}

/* This one prepaires an out Td */
UsbHcTransaction_t *OhciTransactionOut(void *Controller, UsbHcRequest_t *Request)
{
	OhciController_t *Ctrl = (OhciController_t*)Controller;
	UsbHcTransaction_t *Transaction;

	/* Unused */
	_CRT_UNUSED(Ctrl);

	/* Allocate Transaction */
	Transaction = (UsbHcTransaction_t*)kmalloc(sizeof(UsbHcTransaction_t));
	Transaction->TransferDescriptorCopy = NULL;
	Transaction->IoBuffer = 0;
	Transaction->IoLength = 0;
	Transaction->Link = NULL;

	/* Setup Td */
	Transaction->TransferDescriptor = (void*)OhciTdIo(Request->Endpoint->AttachedData, 
		Request->Type, X86_OHCI_TRANSFER_END_OF_LIST, Request->Endpoint,
		X86_OHCI_TRANSFER_BUF_PID_OUT, Request->IoLength,
		&Transaction->TransferBuffer);

	/* Copy Data */
	if (Request->IoBuffer != NULL && Request->IoLength != 0)
		memcpy(Transaction->TransferBuffer, Request->IoBuffer, Request->IoLength);

	/* If previous Transaction */
	if (Request->Transactions != NULL)
	{
		OhciGTransferDescriptor_t *PrevTd;
		UsbHcTransaction_t *cTrans = Request->Transactions;

		while (cTrans->Link)
			cTrans = cTrans->Link;

		PrevTd = (OhciGTransferDescriptor_t*)cTrans->TransferDescriptor;
		PrevTd->NextTD = MmVirtualGetMapping(NULL, (VirtAddr_t)Transaction->TransferDescriptor);
	}

	/* We might need a copy */
	if (Request->Type == InterruptTransfer
		|| Request->Type == IsochronousTransfer)
	{
		/* Allocate TD */
		Transaction->TransferDescriptorCopy = 
			(void*)OhciAllocateTd(Request->Endpoint->AttachedData, Request->Type);
	}

	/* Done */
	return Transaction;
}

/* This one queues the Transaction up for processing */
void OhciTransactionSend(void *Controller, UsbHcRequest_t *Request)
{
	/* Wuhu */
	UsbHcTransaction_t *Transaction = Request->Transactions;
	OhciController_t *Ctrl = (OhciController_t*)Controller;
	UsbTransferStatus_t Completed;
	OhciEndpointDescriptor_t *Ep = NULL;
	OhciGTransferDescriptor_t *Td = NULL;
	uint32_t CondCode;
	Addr_t EdAddress;

	/* Cast */
	Ep = (OhciEndpointDescriptor_t*)Request->Data;

	/* Get physical */
	EdAddress = MmVirtualGetMapping(NULL, (VirtAddr_t)Ep);

	/* Set as not Completed for start */
	Request->Status = TransferNotProcessed;

	/* Add dummy Td to end
	 * But we have to keep the endpoint toggle */
	CondCode = Request->Endpoint->Toggle;
	UsbTransactionOut(UsbGetHcd(Ctrl->HcdId), Request,
		(Request->Type == ControlTransfer) ? (uint32_t)1 : (uint32_t)0, NULL, (uint32_t)0);
	Request->Endpoint->Toggle = CondCode;
	CondCode = 0;

	/* Iterate and set last to INT */
	Transaction = Request->Transactions;
	while (Transaction->Link
		&& Transaction->Link->Link)
	{
#ifdef _OHCI_DIAGNOSTICS_
		printf("Td (Addr 0x%x) Flags 0x%x, Cbp 0x%x, BufferEnd 0x%x, Next Td 0x%x\n", 
			MmVirtualGetMapping(NULL, (VirtAddr_t)Td), Td->Flags, Td->Cbp, Td->BufferEnd, Td->NextTD);
#endif
		/* Next */
		Transaction = Transaction->Link;
	}

	/* Retrieve Td */
#ifndef _OHCI_DIAGNOSTICS_
	Td = (OhciGTransferDescriptor_t*)Transaction->TransferDescriptor;
	Td->Flags &= ~(X86_OHCI_TRANSFER_BUF_NO_INTERRUPT);
#endif

	/* Initialize the allocated ED */
	OhciEpInit(Ep, Request->Transactions, Request->Type,
		Request->Device->Address, Request->Endpoint->Address,
		Request->Endpoint->MaxPacketSize, Request->LowSpeed);

	/* Now lets try the Transaction */
	SpinlockAcquire(&Ctrl->Lock);

	/* Set true */
	Completed = TransferFinished;

	/* Add this Transaction to list */
	list_append((list_t*)Ctrl->TransactionList, list_create_node(0, Request));

	/* Remove Skip */
	Ep->Flags &= ~(X86_OHCI_EP_SKIP);
	Ep->HeadPtr &= ~(0x1);

#ifdef _OHCI_DIAGNOSTICS_
	printf("Ed Address 0x%x, Flags 0x%x, Ed Tail 0x%x, Ed Head 0x%x, Next 0x%x\n", EdAddress, ((OhciEndpointDescriptor_t*)Request->Data)->Flags,
		((OhciEndpointDescriptor_t*)Request->Data)->TailPtr, ((OhciEndpointDescriptor_t*)Request->Data)->HeadPtr, ((OhciEndpointDescriptor_t*)Request->Data)->NextED);
#endif

	/* Add them to list */
	if (Request->Type == ControlTransfer)
	{
		if (Ctrl->TransactionsWaitingControl > 0)
		{
			/* Insert it */
			if (Ctrl->TransactionQueueControl == 0)
				Ctrl->TransactionQueueControl = (uint32_t)Request->Data;
			else
			{
				OhciEndpointDescriptor_t *Ed = (OhciEndpointDescriptor_t*)Ctrl->TransactionQueueControl;

				/* Find tail */
				while (Ed->NextED)
					Ed = (OhciEndpointDescriptor_t*)Ed->NextEDVirtual;

				/* Insert it */
				Ed->NextED = EdAddress;
				Ed->NextEDVirtual = (Addr_t)Request->Data;
			}

			/* Increase */
			Ctrl->TransactionsWaitingControl++;
		}
		else
		{
			/* Add it HcControl/BulkCurrentED */
			Ctrl->Registers->HcControlHeadED =
				Ctrl->Registers->HcControlCurrentED = EdAddress;

			/* Increase */
			Ctrl->TransactionsWaitingControl++;

			/* Set Lists Filled (Enable Them) */
			Ctrl->Registers->HcCommandStatus |= X86_OHCI_CMD_TDACTIVE_CTRL;
		}
	}
	else if (Request->Type == BulkTransfer)
	{
		if (Ctrl->TransactionsWaitingBulk > 0)
		{
			/* Insert it */
			if (Ctrl->TransactionQueueBulk == 0)
				Ctrl->TransactionQueueBulk = (Addr_t)Request->Data;
			else
			{
				OhciEndpointDescriptor_t *Ed = (OhciEndpointDescriptor_t*)Ctrl->TransactionQueueBulk;

				/* Find tail */
				while (Ed->NextED)
					Ed = (OhciEndpointDescriptor_t*)Ed->NextEDVirtual;

				/* Insert it */
				Ed->NextED = EdAddress;
				Ed->NextEDVirtual = (uint32_t)Request->Data;
			}

			/* Increase */
			Ctrl->TransactionsWaitingBulk++;
		}
		else
		{
			/* Add it HcControl/BulkCurrentED */
			Ctrl->Registers->HcBulkHeadED =
				Ctrl->Registers->HcBulkCurrentED = EdAddress;

			/* Increase */
			Ctrl->TransactionsWaitingBulk++;

			/* Set Lists Filled (Enable Them) */
			Ctrl->Registers->HcCommandStatus |= X86_OHCI_CMD_TDACTIVE_BULK;
		}
	}
	else
	{
		/* Update saved copies, now all is prepaired */
		Transaction = Request->Transactions;
		while (Transaction)
		{
			/* Do an exact copy */
			memcpy(Transaction->TransferDescriptorCopy, Transaction->TransferDescriptor,
				(Request->Type == InterruptTransfer) ? sizeof(OhciGTransferDescriptor_t) :
				sizeof(OhciITransferDescriptor_t));

			/* Next */
			Transaction = Transaction->Link;
		}

		/* DISABLE SCHEDULLER!!! */
		Ctrl->Registers->HcControl &= ~(X86_OCHI_CTRL_PERIODIC_LIST | X86_OHCI_CTRL_ISOCHRONOUS_LIST);

		/* Iso / Interrupt */
		OhciEndpointDescriptor_t *IEd;
		uint32_t Period = 32;

		/* Calculate period */
		for (; (Period >= Request->Endpoint->Interval) && (Period > 0);)
			Period >>= 1;

		if (Period == 1)
		{
			/* Isochronous List */
			IEd = &Ctrl->IntrTable->Ms1[0];
		}
		else if (Period == 2)
		{
			IEd = &Ctrl->IntrTable->Ms2[Ctrl->I2];

			/* Increase i2 */
			Ctrl->I2++;
			if (Ctrl->I2 == 2)
				Ctrl->I2 = 0;
		}
		else if (Period == 4)
		{
			IEd = &Ctrl->IntrTable->Ms4[Ctrl->I4];

			/* Increase i4 */
			Ctrl->I4++;
			if (Ctrl->I4 == 4)
				Ctrl->I4 = 0;
		}
		else if (Period == 8)
		{
			IEd = &Ctrl->IntrTable->Ms8[Ctrl->I8];

			/* Increase i8 */
			Ctrl->I8++;
			if (Ctrl->I8 == 8)
				Ctrl->I8 = 0;
		}
		else if (Period == 16)
		{
			IEd = &Ctrl->IntrTable->Ms16[Ctrl->I16];

			/* Increase i16 */
			Ctrl->I16++;
			if (Ctrl->I16 == 16)
				Ctrl->I16 = 0;
		}
		else
		{
			/* 32 */
			IEd = Ctrl->ED32[Ctrl->I32];

			/* Insert it */
			Ep->NextEDVirtual = (Addr_t)IEd;
			Ep->NextED = MmVirtualGetMapping(NULL, (VirtAddr_t)IEd);

			/* Make int-table point to this */
			Ctrl->HCCA->InterruptTable[Ctrl->I32] = EdAddress;
			Ctrl->ED32[Ctrl->I32] = Ep;

			/* Increase i32 */
			Ctrl->I32++;
			if (Ctrl->I32 == 32)
				Ctrl->I32 = 0;
		}

		/* Sanity */
		if (Period != 32)
		{
			/* Insert it */
			Ep->NextEDVirtual = IEd->NextEDVirtual;
			Ep->NextED = IEd->NextED;
			
			IEd->NextED = EdAddress;
			IEd->NextEDVirtual = (Addr_t)Ep;
		}

		/* ENABLE SCHEDULEER */
		Ctrl->Registers->HcControl |= X86_OCHI_CTRL_PERIODIC_LIST | X86_OHCI_CTRL_ISOCHRONOUS_LIST;

		/* Done */
		SpinlockRelease(&Ctrl->Lock);

		/* Skip this */
		return;
	}

	/* Wait for interrupt */
#ifdef _OHCI_DIAGNOSTICS_
	printf("Current Frame 0x%x (HCCA), HcFrameNumber 0x%x, HcFrameInterval 0x%x, HcFrameRemaining 0x%x\n", Ctrl->HCCA->CurrentFrame,
		Ctrl->Registers->HcFmNumber, Ctrl->Registers->HcFmInterval, Ctrl->Registers->HcFmRemaining);
	printf("Transaction sent.. waiting for reply..\n");
	StallMs(5000);
	printf("Current Frame 0x%x (HCCA), HcFrameNumber 0x%x, HcFrameInterval 0x%x, HcFrameRemaining 0x%x\n", Ctrl->HCCA->CurrentFrame,
		Ctrl->Registers->HcFmNumber, Ctrl->Registers->HcFmInterval, Ctrl->Registers->HcFmRemaining);
	printf("1. Current Control: 0x%x, Current Head: 0x%x\n",
		Ctrl->Registers->HcControlCurrentED, Ctrl->Registers->HcControlHeadED);
	StallMs(5000);
	printf("Current Frame 0x%x (HCCA), HcFrameNumber 0x%x, HcFrameInterval 0x%x, HcFrameRemaining 0x%x\n", Ctrl->HCCA->CurrentFrame,
		Ctrl->Registers->HcFmNumber, Ctrl->Registers->HcFmInterval, Ctrl->Registers->HcFmRemaining);
	printf("2. Current Control: 0x%x, Current Head: 0x%x\n",
		Ctrl->Registers->HcControlCurrentED, Ctrl->Registers->HcControlHeadED);
	printf("Current Control 0x%x, Current Cmd 0x%x\n",
		Ctrl->Registers->HcControl, Ctrl->Registers->HcCommandStatus);
#else
	/* Sleep Us */
	SchedulerSleepThread((Addr_t*)Request->Data);

	/* Release GlbDescriptor->Spinlock at latest possible */
	SpinlockRelease(&Ctrl->Lock);

	/* Yield us */
	_ThreadYield();
#endif


	/* Check Conditions (WithOUT dummy) */
#ifdef _OHCI_DIAGNOSTICS_
	printf("Ed Flags 0x%x, Ed Tail 0x%x, Ed Head 0x%x\n", ((OhciEndpointDescriptor_t*)Request->Data)->Flags,
		((OhciEndpointDescriptor_t*)Request->Data)->TailPtr, ((OhciEndpointDescriptor_t*)Request->Data)->HeadPtr);
#endif
	Transaction = Request->Transactions;
	while (Transaction->Link)
	{
		Td = (OhciGTransferDescriptor_t*)Transaction->TransferDescriptor;
		CondCode = (Td->Flags & 0xF0000000) >> 28;
#ifdef _OHCI_DIAGNOSTICS_
		printf("Td Flags 0x%x, Cbp 0x%x, BufferEnd 0x%x, Td Condition Code %u (%s)\n", Td->Flags, Td->Cbp, Td->BufferEnd, CondCode, OhciErrorMessages[CondCode]);
#endif

		if (CondCode == 0 && Completed == TransferFinished)
			Completed = TransferFinished;
		else
		{
			if (CondCode == 4)
				Completed = TransferStalled;
			else if (CondCode == 3)
				Completed = TransferInvalidToggles;
			else if (CondCode == 5)
				Completed = TransferNotResponding;
			else {
				LogDebug("OHCI", "Error: 0x%x (%s)", CondCode, OhciErrorMessages[CondCode]);
				Completed = TransferInvalidData;
			}
			break;
		}

		Transaction = Transaction->Link;
	}

#ifdef _OHCI_DIAGNOSTICS_
	printf("HcDoneHead: 0x%x\n", Ctrl->HCCA->HeadDone);
	printf("Current Frame 0x%x (HCCA), HcFrameNumber 0x%x, HcFrameInterval 0x%x, HcFrameRemaining 0x%x\n", Ctrl->HCCA->CurrentFrame,
		Ctrl->Registers->HcFmNumber, Ctrl->Registers->HcFmInterval, Ctrl->Registers->HcFmRemaining);
#endif

	/* Lets see... */
	if (Completed == TransferFinished)
	{
		/* Build Buffer */
		Transaction = Request->Transactions;

		while (Transaction->Link)
		{
			/* Copy Data? */
			if (Transaction->IoBuffer != NULL && Transaction->IoLength != 0)
			{
				//printf("Buffer Copy 0x%x, Length 0x%x\n", Transaction->IoBuffer, Transaction->IoLength);
				memcpy(Transaction->IoBuffer, Transaction->TransferBuffer, Transaction->IoLength);
			}

			/* Next Link */
			Transaction = Transaction->Link;
		}
	}

	/* Update Status */
	Request->Status = Completed;

#ifdef _OHCI_DIAGNOSTICS_
	for (;;);
#endif
}

/* This one cleans up */
void OhciTransactionDestroy(void *Controller, UsbHcRequest_t *Request)
{
	UsbHcTransaction_t *Transaction = Request->Transactions;
	OhciController_t *Ctrl = (OhciController_t*)Controller;

	/* Unallocate Ed */
	if (Request->Type == ControlTransfer
		|| Request->Type == BulkTransfer)
	{
		/* Iterate and reset */
		while (Transaction)
		{
			/* Cast */
			OhciGTransferDescriptor_t *Td =
				(OhciGTransferDescriptor_t*)Transaction->TransferDescriptor;

			/* Memset */
			memset((void*)Td, 0, sizeof(OhciGTransferDescriptor_t));

			/* Next */
			Transaction = Transaction->Link;
		}

		/* Reset the ED */
		memset(Request->Data, 0, sizeof(OhciEndpointDescriptor_t));
	}
	else
	{
		/* Iterate transactions and free buffers & td's */
		while (Transaction)
		{
			/* free buffer */
			kfree(Transaction->TransferBuffer);

			/* free both TD's */
			kfree((void*)Transaction->TransferDescriptor);
			kfree((void*)Transaction->TransferDescriptorCopy);

			/* Next */
			Transaction = Transaction->Link;
		}

		/* Unhook ED from the list it's in */
		SpinlockAcquire(&Ctrl->Lock);

		/* DISABLE SCHEDULLER!!! */
		Ctrl->Registers->HcControl &= ~(X86_OCHI_CTRL_PERIODIC_LIST | X86_OHCI_CTRL_ISOCHRONOUS_LIST);

		/* Iso / Interrupt */
		OhciEndpointDescriptor_t *Ed = (OhciEndpointDescriptor_t*)Request->Data;
		OhciEndpointDescriptor_t *IEd, *PrevIEd;
		uint32_t Period = 32;

		/* Calculate period */
		for (; (Period >= Request->Endpoint->Interval) && (Period > 0);)
			Period >>= 1;

		if (Period == 1)
			IEd = &Ctrl->IntrTable->Ms1[0];
		else if (Period == 2)
			IEd = &Ctrl->IntrTable->Ms2[0];
		else if (Period == 4)
			IEd = &Ctrl->IntrTable->Ms4[0];
		else if (Period == 8)
			IEd = &Ctrl->IntrTable->Ms8[0];
		else if (Period == 16)
			IEd = &Ctrl->IntrTable->Ms16[0];
		else
			IEd = Ctrl->ED32[0];

		/* Iterate */
		PrevIEd = NULL;
		while (IEd != Ed)
		{
			/* Sanity */
			if (IEd->NextED == 0
				|| IEd->NextED == 0x1)
			{
				IEd = NULL;
				break;
			}

			/* Save */
			PrevIEd = IEd;

			/* Go to next */
			IEd = (OhciEndpointDescriptor_t*)IEd->NextEDVirtual;
		}

		/* Sanity */
		if (IEd != NULL)
		{
			/* Either we are first, or we are not */
			if (PrevIEd == NULL
				&& Period == 32)
			{
				/* Only special case for 32 period */
				Ctrl->ED32[0] = (OhciEndpointDescriptor_t*)Ed->NextEDVirtual;
				Ctrl->HCCA->InterruptTable[0] = Ed->NextED;
			}
			else if (PrevIEd != NULL)
			{
				/* Make it skip over */
				PrevIEd->NextED = Ed->NextED;
				PrevIEd->NextEDVirtual = Ed->NextEDVirtual;
			}
		}

		/* ENABLE SCHEDULEER */
		Ctrl->Registers->HcControl |= X86_OCHI_CTRL_PERIODIC_LIST | X86_OHCI_CTRL_ISOCHRONOUS_LIST;

		/* Done */
		SpinlockRelease(&Ctrl->Lock);

		/* Free it */
		kfree(Request->Data);
	}
}

/* Reload Controller */
void OhciReloadControlBulk(OhciController_t *Controller, UsbTransferType_t TransferType)
{
	/* So now, before waking up a sleeper we see if Transactions are pending
	* if they are, we simply copy the queue over to the current */
	SpinlockAcquire(&Controller->Lock);

	/* Any Controls waiting? */
	if (TransferType == 0)
	{
		if (Controller->TransactionsWaitingControl > 0)
		{
			/* Get physical of Ed */
			Addr_t EdPhysical = MmVirtualGetMapping(NULL, Controller->TransactionQueueControl);

			/* Set it */
			Controller->Registers->HcControlHeadED =
				Controller->Registers->HcControlCurrentED = EdPhysical;

			/* Start queue */
			Controller->Registers->HcCommandStatus |= X86_OHCI_CMD_TDACTIVE_CTRL;
		}

		/* Reset control queue */
		Controller->TransactionQueueControl = 0;
		Controller->TransactionsWaitingControl = 0;
	}
	else if (TransferType == 1)
	{
		/* Bulk */
		if (Controller->TransactionsWaitingBulk > 0)
		{
			/* Get physical of Ed */
			Addr_t EdPhysical = MmVirtualGetMapping(NULL, Controller->TransactionQueueBulk);

			/* Add it to queue */
			Controller->Registers->HcBulkHeadED =
				Controller->Registers->HcBulkCurrentED = EdPhysical;

			/* Start queue */
			Controller->Registers->HcCommandStatus |= X86_OHCI_CMD_TDACTIVE_BULK;
		}

		/* Reset control queue */
		Controller->TransactionQueueBulk = 0;
		Controller->TransactionsWaitingBulk = 0;
	}

	/* Done */
	SpinlockRelease(&Controller->Lock);
}

/* Process Done Queue */
void OhciProcessDoneQueue(OhciController_t *Controller, Addr_t DoneHeadAddr)
{
	/* Get transaction list */
	list_t *Transactions = (list_t*)Controller->TransactionList;

	/* Get Ed with the same td address as DoneHeadAddr */
	foreach(Node, Transactions)
	{
		/* Cast UsbRequest */
		UsbHcRequest_t *HcRequest = (UsbHcRequest_t*)Node->data;

		/* Get Ed */
		OhciEndpointDescriptor_t *Ed = (OhciEndpointDescriptor_t*)HcRequest->Data;
		UsbTransferType_t TransferType = (UsbTransferType_t)((Ed->Flags >> 27) & 0xF);

		/* Get transaction list */
		UsbHcTransaction_t *tList = (UsbHcTransaction_t*)HcRequest->Transactions;

		/* Is it this? */
		while (tList)
		{
			/* Get Td */
			OhciGTransferDescriptor_t *Td =
				(OhciGTransferDescriptor_t*)tList->TransferDescriptor;

			/* Get physical of TD */
			Addr_t TdPhysical = MmVirtualGetMapping(NULL, (VirtAddr_t)Td);

			/* Is it this one? */
			if (DoneHeadAddr == TdPhysical)
			{
				/* Depending on type */
				if (TransferType == ControlTransfer
					|| TransferType == BulkTransfer)
				{
					/* Reload */
					OhciReloadControlBulk(Controller, TransferType);

					/* Wake a node */
					SchedulerWakeupOneThread((Addr_t*)Ed);

					/* Remove from list */
					list_remove_by_node(Transactions, Node);

					/* Cleanup node */
					kfree(Node);
				}
				else if (TransferType == InterruptTransfer
					|| TransferType == IsochronousTransfer)
				{
					/* Re-Iterate */
					UsbHcTransaction_t *lIterator = HcRequest->Transactions;

					/* Copy data if not dummy */
					while (lIterator)
					{
						/* Let's see */
						if (lIterator->IoLength != 0)
						{
							/* Copy Data from transfer buffer to IoBuffer */
							memcpy(lIterator->IoBuffer, lIterator->TransferBuffer, lIterator->IoLength);
						}

						/* Switch toggle */
						if (TransferType == InterruptTransfer)
						{
							OhciGTransferDescriptor_t *__Td =
								(OhciGTransferDescriptor_t*)lIterator->TransferDescriptorCopy;

							/* If set */
							if (__Td->Flags & (1 << 24))
								__Td->Flags &= ~(1 << 24);
							else
								__Td->Flags |= (1 << 24);
						}

						/* Restart Td */
						memcpy(lIterator->TransferDescriptor,
							lIterator->TransferDescriptorCopy, 
							TransferType == InterruptTransfer ? 
							sizeof(OhciGTransferDescriptor_t) : sizeof(OhciITransferDescriptor_t));

						/* Eh, next link */
						lIterator = lIterator->Link;
					}

					/* Callback */
					HcRequest->Callback->Callback(HcRequest->Callback->Args);

					/* Restart Ed */
					Ed->HeadPtr = 
						MmVirtualGetMapping(NULL, (VirtAddr_t)HcRequest->Transactions->TransferDescriptor);
				}

				/* Done */
				return;
			}

			/* Go to next */
			tList = tList->Link;
		}
	}
}

/* Interrupt Handler
* Make sure that this Controller actually made the interrupt
* as this interrupt will be shared with other OHCI's */
int OhciInterruptHandler(void *data)
{
	/* Vars */
	OhciController_t *Controller = (OhciController_t*)data;
	uint32_t IntrState = 0;

	/* Is this our interrupt ? */
	if (Controller->HCCA->HeadDone != 0)
	{
		/* Acknowledge */
		IntrState = X86_OHCI_INTR_HEAD_DONE;

		if (Controller->HCCA->HeadDone & 0x1)
		{
			/* Get rest of interrupts, since head_done has halted */
			IntrState |= (Controller->Registers->HcInterruptStatus & Controller->Registers->HcInterruptEnable);
		}
	}
	else
	{
		/* Was it this Controller that made the interrupt?
		* We only want the interrupts we have set as enabled */
		IntrState = (Controller->Registers->HcInterruptStatus & Controller->Registers->HcInterruptEnable);

		/* Sanity */
		if (IntrState == 0)
			return X86_IRQ_NOT_HANDLED;
	}

	/* Disable Interrupts */
	Controller->Registers->HcInterruptDisable = (uint32_t)X86_OHCI_INTR_MASTER_INTR;

	/* Fatal Error? */
	if (IntrState & X86_OHCI_INTR_FATAL_ERROR)
	{
		/* Port does not matter here */
		UsbEventCreate(UsbGetHcd(Controller->HcdId), 0, HcdFatalEvent);

		/* Done */
		return X86_IRQ_HANDLED;
	}

	/* Flag for end of frame Type interrupts */
	if (IntrState & (X86_OHCI_INTR_SCHEDULING_OVRRN | X86_OHCI_INTR_HEAD_DONE
		| X86_OHCI_INTR_SOF | X86_OHCI_INTR_FRAME_OVERFLOW))
		IntrState |= X86_OHCI_INTR_MASTER_INTR;

	/* Scheduling Overrun? */
	if (IntrState & X86_OHCI_INTR_SCHEDULING_OVRRN)
	{
		LogDebug("OHCI", "%u: Scheduling Overrun", Controller->Id);

		/* Acknowledge Interrupt */
		Controller->Registers->HcInterruptStatus = X86_OHCI_INTR_SCHEDULING_OVRRN;
		IntrState = IntrState & ~(X86_OHCI_INTR_SCHEDULING_OVRRN);
	}

	/* Resume Detection? */
	if (IntrState & X86_OHCI_INTR_RESUME_DETECT)
	{
		/* We must wait 20 ms before putting Controller to Operational */
		DelayMs(20);
		OhciSetMode(Controller, X86_OHCI_CTRL_USB_WORKING);

		/* Acknowledge Interrupt */
		Controller->Registers->HcInterruptStatus = X86_OHCI_INTR_RESUME_DETECT;
		IntrState = IntrState & ~(X86_OHCI_INTR_RESUME_DETECT);
	}

	/* Frame Overflow
	 * Happens when it rolls over from 0xFFFF to 0 */
	if (IntrState & X86_OHCI_INTR_FRAME_OVERFLOW)
	{
		/* Acknowledge Interrupt */
		Controller->Registers->HcInterruptStatus = X86_OHCI_INTR_FRAME_OVERFLOW;
		IntrState = IntrState & ~(X86_OHCI_INTR_FRAME_OVERFLOW);
	}

	/* Why yes, yes it was, wake up the Td handler thread
	* if it was head_done_writeback */
	if (IntrState & X86_OHCI_INTR_HEAD_DONE)
	{
		/* Wuhu, handle this! */
		uint32_t td_address = (Controller->HCCA->HeadDone & ~(0x00000001));

		/* Call our processor */
		OhciProcessDoneQueue(Controller, td_address);

		/* Acknowledge Interrupt */
		Controller->HCCA->HeadDone = 0;
		Controller->Registers->HcInterruptStatus = X86_OHCI_INTR_HEAD_DONE;
		IntrState = IntrState & ~(X86_OHCI_INTR_HEAD_DONE);
	}

	/* Root Hub Status Change
	* Do a Port Status check */
	if (IntrState & X86_OHCI_INTR_ROOT_HUB_EVENT)
	{
		/* Port does not matter here */
		UsbEventCreate(UsbGetHcd(Controller->HcdId), 0, HcdRootHubEvent);

		/* Acknowledge Interrupt */
		Controller->Registers->HcInterruptStatus = X86_OHCI_INTR_ROOT_HUB_EVENT;
		IntrState = IntrState & ~(X86_OHCI_INTR_ROOT_HUB_EVENT);
	}

	/* Start of Frame? */
	if (IntrState & X86_OHCI_INTR_SOF)
	{
		/* Acknowledge Interrupt */
		Controller->Registers->HcInterruptStatus = X86_OHCI_INTR_SOF;
		IntrState = IntrState & ~(X86_OHCI_INTR_SOF);
	}

	/* Mask out remaining interrupts, we dont use them */
	if (IntrState & ~(X86_OHCI_INTR_MASTER_INTR))
		Controller->Registers->HcInterruptDisable = IntrState;

	/* Enable Interrupts */
	Controller->Registers->HcInterruptEnable = (uint32_t)X86_OHCI_INTR_MASTER_INTR;

	return X86_IRQ_HANDLED;
}