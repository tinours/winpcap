/*
 * Copyright (c) 1999, 2000
 *	Politecnico di Torino.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that: (1) source code distributions
 * retain the above copyright notice and this paragraph in its entirety, (2)
 * distributions including binary code include the above copyright notice and
 * this paragraph in its entirety in the documentation or other materials
 * provided with the distribution, and (3) all advertising materials mentioning
 * features or use of this software display the following acknowledgement:
 * ``This product includes software developed by the Politecnico
 * di Torino, and its contributors.'' Neither the name of
 * the University nor the names of its contributors may be used to endorse
 * or promote products derived from this software without specific prior
 * written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "ntddk.h"
#include "ntiologc.h"
#include "ndis.h"

#include "debug.h"
#include "packet.h"

static NDIS_MEDIUM MediumArray[] = {
	NdisMedium802_3,
	NdisMediumWan,
	NdisMediumFddi,
	NdisMediumArcnet878_2,
	NdisMediumAtm,
	NdisMedium802_5
};

#define NUM_NDIS_MEDIA  (sizeof MediumArray / sizeof MediumArray[0])


ULONG NamedEventsCounter=0;

//Itoa. Replaces the buggy RtlIntegerToUnicodeString
void PacketItoa(UINT n,PUCHAR buf){
int i;

	for(i=0;i<20;i+=2){
		buf[18-i]=(n%10)+48;
		buf[19-i]=0;
		n/=10;
	}

}

//-------------------------------------------------------------------

NTSTATUS PacketOpen(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{

    PDEVICE_EXTENSION DeviceExtension;

    POPEN_INSTANCE    Open;

    PIO_STACK_LOCATION  IrpSp;

    NDIS_STATUS     Status;
    NDIS_STATUS     ErrorStatus;
    UINT            i;
	PUCHAR			tpointer;
    PLIST_ENTRY     PacketListEntry;
	LARGE_INTEGER	TimeFreq;
	LARGE_INTEGER	SystemTime;
	LARGE_INTEGER	PTime;
	PCHAR			EvName;


    IF_LOUD(DbgPrint("NPF: OpenAdapter\n");)

    DeviceExtension = DeviceObject->DeviceExtension;


    IrpSp = IoGetCurrentIrpStackLocation(Irp);


    //
    //  allocate some memory for the open structure
    //
    Open=ExAllocatePool(NonPagedPool,sizeof(OPEN_INSTANCE));


    if (Open==NULL) {
        //
        // no memory
        //
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(
        Open,
        sizeof(OPEN_INSTANCE)
        );


	EvName=ExAllocatePool(NonPagedPool, sizeof(L"\\BaseNamedObjects\\NPF0000000000"));

    if (EvName==NULL) {
        //
        // no memory
        //
        ExFreePool(Open);
	    Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    //  Save or open here
    //
    IrpSp->FileObject->FsContext=Open;
	
    Open->DeviceExtension=DeviceExtension;
	
	
    //
    //  Save the Irp here for the completeion routine to retrieve
    //
    Open->OpenCloseIrp=Irp;
	
    //
    //  Allocate a packet pool for our xmit and receive packets
    //
    NdisAllocatePacketPool(
        &Status,
        &Open->PacketPool,
        TRANSMIT_PACKETS,
        sizeof(PACKET_RESERVED));
	
	
    if (Status != NDIS_STATUS_SUCCESS) {
		
        IF_LOUD(DbgPrint("NPF: Failed to allocate packet pool\n");)
			
		ExFreePool(Open);
		ExFreePool(EvName);
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }


	RtlCopyBytes(EvName,L"\\BaseNamedObjects\\NPF0000000000",sizeof(L"\\BaseNamedObjects\\NPF0000000000"));

	//Create the string containing the name of the read event
	RtlInitUnicodeString(&Open->ReadEventName,(PCWSTR) EvName);

	PacketItoa(NamedEventsCounter,(PUCHAR)(Open->ReadEventName.Buffer+21));

	InterlockedIncrement(&NamedEventsCounter);
	
	IF_LOUD(DbgPrint("\nCreated the named event for the read; name=%ws, counter=%d\n", Open->ReadEventName.Buffer,NamedEventsCounter-1);)

	//allocate the event objects
	Open->ReadEvent=IoCreateNotificationEvent(&Open->ReadEventName,&Open->ReadEventHandle);
	if(Open->ReadEvent==NULL){
		ExFreePool(Open);
		ExFreePool(EvName);
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
	}
	
	KeInitializeEvent(Open->ReadEvent, NotificationEvent, FALSE);
	KeClearEvent(Open->ReadEvent);
	NdisInitializeEvent(&Open->WriteEvent);
	NdisInitializeEvent(&Open->IOEvent);
 	NdisInitializeEvent(&Open->DumpEvent);

    //  list to hold irp's want to reset the adapter
    InitializeListHead(&Open->ResetIrpList);
	
	
    //  Initialize the request list
    KeInitializeSpinLock(&Open->RequestSpinLock);
    InitializeListHead(&Open->RequestList);
	
	
	// get the absolute value of the system boot time.   
	PTime=KeQueryPerformanceCounter(&TimeFreq);
	KeQuerySystemTime(&SystemTime);
	Open->StartTime.QuadPart=(((SystemTime.QuadPart)%10000000)*TimeFreq.QuadPart)/10000000;
	SystemTime.QuadPart=SystemTime.QuadPart/10000000-11644473600;
	Open->StartTime.QuadPart+=(SystemTime.QuadPart)*TimeFreq.QuadPart-PTime.QuadPart;
	
	//initalize the open instance
	Open->BufSize=0;
	Open->Buffer=NULL;
	Open->Bhead=0;
	Open->Btail=0;
	Open->BLastByte=0;
	Open->Dropped=0;		//reset the dropped packets counter
	Open->Received=0;		//reset the received packets counter
	Open->bpfprogram=NULL;	//reset the filter
	Open->mode=MODE_CAPT;
	Open->Nbytes.QuadPart=0;
	Open->Npackets.QuadPart=0;
	Open->Nwrites=1;
	Open->Multiple_Write_Counter=0;
	Open->MinToCopy=0;
	Open->TimeOut.QuadPart=(LONGLONG)1;
	Open->Bound=TRUE;
	Open->DumpFileName.Buffer=NULL;
	Open->DumpFileHandle=NULL;

	//allocate the spinlock for the statistic counters
    NdisAllocateSpinLock(&Open->CountersLock);

	//allocate the spinlock for the buffer pointers
    NdisAllocateSpinLock(&Open->BufLock);
	
    IoMarkIrpPending(Irp);
	
	
    //
    //  Try to open the MAC
    //
    IF_LOUD(DbgPrint("NPF: Openinig the device %ws, BindingContext=%d\n",DeviceExtension->AdapterName.Buffer, Open);)

	NdisOpenAdapter(
        &Status,
        &ErrorStatus,
        &Open->AdapterHandle,
        &Open->Medium,
        MediumArray,
        NUM_NDIS_MEDIA,
        DeviceExtension->NdisProtocolHandle,
        Open,
        &DeviceExtension->AdapterName,
        0,
        NULL);

    IF_LOUD(DbgPrint("NPF: Opened the device, Status=%x\n",Status);)

	if (Status != NDIS_STATUS_PENDING)
    {
		PacketOpenAdapterComplete(Open,Status,NDIS_STATUS_SUCCESS);
    }
	
    return(STATUS_PENDING);
}

//-------------------------------------------------------------------

VOID PacketOpenAdapterComplete(
	IN NDIS_HANDLE  ProtocolBindingContext,
    IN NDIS_STATUS  Status,
    IN NDIS_STATUS  OpenErrorStatus)
{

    PIRP              Irp;
    POPEN_INSTANCE    Open;

    IF_LOUD(DbgPrint("NPF: OpenAdapterComplete\n");)

    Open= (POPEN_INSTANCE)ProtocolBindingContext;

    //
    //  get the open irp
    //
    Irp=Open->OpenCloseIrp;

    if (Status != NDIS_STATUS_SUCCESS) {

        IF_LOUD(DbgPrint("NPF: OpenAdapterComplete-FAILURE\n");)

        NdisFreePacketPool(Open->PacketPool);

		ExFreePool(Open->ReadEventName.Buffer);

        ExFreePool(Open);
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return;

}

//-------------------------------------------------------------------

NTSTATUS
PacketClose(IN PDEVICE_OBJECT DeviceObject,IN PIRP Irp)
{

    POPEN_INSTANCE    Open;
    NDIS_STATUS     Status;
    PIO_STACK_LOCATION  IrpSp;


    IF_LOUD(DbgPrint("NPF: CloseAdapter\n");)

	IrpSp = IoGetCurrentIrpStackLocation(Irp);

    Open=IrpSp->FileObject->FsContext;

 	// Reset the buffer size. This tells the dump thread to stop.
 	Open->BufSize=0;

	if( Open->Bound == FALSE){

		NdisWaitEvent(&Open->IOEvent,10000);

		// Free the bpf program
		if(Open->bpfprogram!=NULL)ExFreePool(Open->bpfprogram);
		
		// Free the JIT binary
		BPF_Destroy_JIT_Filter(Open->Filter);

		// Free the buffer
		Open->BufSize=0;
		if(Open->Buffer!=NULL)ExFreePool(Open->Buffer);

		// Free the string with the name of the dump file
		if(Open->DumpFileName.Buffer!=NULL)
			ExFreePool(Open->DumpFileName.Buffer);
		
		NdisFreePacketPool(Open->PacketPool);
				
		ExFreePool(Open->ReadEventName.Buffer);
		ExFreePool(Open);

		Irp->IoStatus.Information = 0;
		Irp->IoStatus.Status = STATUS_SUCCESS;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		
		return(STATUS_SUCCESS);
	}

 	// Unfreeze the consumer
 	if(Open->mode & MODE_DUMP)
 		NdisSetEvent(&Open->DumpEvent);
 	else
 		KeSetEvent(Open->ReadEvent,0,FALSE);

    // Save the IRP
    Open->OpenCloseIrp=Irp;

    IoMarkIrpPending(Irp);
 
	// If this instance is in dump mode, complete the dump and close the file
 	if((Open->mode & MODE_DUMP) && Open->DumpFileHandle != NULL)
 		PacketCloseDumpFile(Open);
 	
	// Destroy the read Event
	ZwClose(Open->ReadEventHandle);

	// Close the adapter
	NdisCloseAdapter(
		&Status,
		Open->AdapterHandle
		);

	if (Status != NDIS_STATUS_PENDING) {
		
		PacketCloseAdapterComplete(
			Open,
			Status
			);
		return STATUS_SUCCESS;
		
	}
	
	return(STATUS_PENDING);
}

//-------------------------------------------------------------------

VOID
PacketCloseAdapterComplete(IN NDIS_HANDLE  ProtocolBindingContext,IN NDIS_STATUS  Status)
{
    POPEN_INSTANCE    Open;
    PIRP              Irp;

    IF_LOUD(DbgPrint("NPF: CloseAdapterComplete\n");)

    Open= (POPEN_INSTANCE)ProtocolBindingContext;

	// free the allocated structures only if the instance is still bound to the adapter
	if(Open->Bound == TRUE){
		
		//free the bpf program
		if(Open->bpfprogram!=NULL)ExFreePool(Open->bpfprogram);
		
		//free the buffer
		Open->BufSize=0;
		if(Open->Buffer!=NULL)ExFreePool(Open->Buffer);
		
		NdisFreePacketPool(Open->PacketPool);
		
		Irp=Open->OpenCloseIrp;
		
		// Free the string with the name of the dump file
		if(Open->DumpFileName.Buffer!=NULL)
			ExFreePool(Open->DumpFileName.Buffer);

		ExFreePool(Open->ReadEventName.Buffer);
		ExFreePool(Open);
		
		// Complete the request only if the instance is still bound to the adapter
		Irp->IoStatus.Status = STATUS_SUCCESS;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
	}
	else
		NdisSetEvent(&Open->IOEvent);


	return;

}

//-------------------------------------------------------------------

VOID
PacketBindAdapter(
    OUT PNDIS_STATUS            Status,
    IN  NDIS_HANDLE             BindContext,
    IN  PNDIS_STRING            DeviceName,
    IN  PVOID                   SystemSpecific1,
    IN  PVOID                   SystemSpecific2
    )
{
	IF_LOUD(DbgPrint("NPF: PacketBindAdapter\n");)
}

//-------------------------------------------------------------------

VOID
PacketUnbindAdapter(
    OUT PNDIS_STATUS        Status,
    IN  NDIS_HANDLE         ProtocolBindingContext,
    IN  NDIS_HANDLE         UnbindContext
    )
{
    POPEN_INSTANCE   Open =(POPEN_INSTANCE)ProtocolBindingContext;
	NDIS_STATUS		 lStatus;

	IF_LOUD(DbgPrint("NPF: PacketUNBindAdapter\n");)

	// Reset the buffer size. This tells the dump thread to stop.
 	Open->BufSize=0;

	NdisResetEvent(&Open->IOEvent);

	// This open instance is no more bound to the adapter, set Bound to False
    InterlockedExchange( (PLONG) &Open->Bound, FALSE );

	// Awake a possible pending read on this instance
 	if(Open->mode & MODE_DUMP)
 		NdisSetEvent(&Open->DumpEvent);
 	else
 		KeSetEvent(Open->ReadEvent,0,FALSE);

	// If this instance is in dump mode, complete the dump and close the file
 	if((Open->mode & MODE_DUMP) && Open->DumpFileHandle != NULL)
 		PacketCloseDumpFile(Open);

	// Destroy the read Event
	ZwClose(Open->ReadEventHandle);

    //  close the adapter
    NdisCloseAdapter(
        &lStatus,
        Open->AdapterHandle
	    );

    if (lStatus != NDIS_STATUS_PENDING) {

        PacketCloseAdapterComplete(
            Open,
            lStatus
            );

		*Status = NDIS_STATUS_SUCCESS;
        return;

    }

	*Status = NDIS_STATUS_SUCCESS;
    return;
}

//-------------------------------------------------------------------

VOID
PacketResetComplete(IN NDIS_HANDLE  ProtocolBindingContext,IN NDIS_STATUS  Status)

{
    POPEN_INSTANCE      Open;
    PIRP                Irp;

    PLIST_ENTRY         ResetListEntry;

    IF_LOUD(DbgPrint("NPF: PacketResetComplte\n");)

    Open= (POPEN_INSTANCE)ProtocolBindingContext;


    //
    //  remove the reset IRP from the list
    //
    ResetListEntry=ExInterlockedRemoveHeadList(
                       &Open->ResetIrpList,
                       &Open->RequestSpinLock
                       );

#if DBG
    if (ResetListEntry == NULL) {
        DbgBreakPoint();
        return;
    }
#endif

    Irp=CONTAINING_RECORD(ResetListEntry,IRP,Tail.Overlay.ListEntry);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    IF_LOUD(DbgPrint("NPF: PacketResetComplte exit\n");)

    return;

}
