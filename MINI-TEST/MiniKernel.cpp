#include "stdafx.h"

_NT_BEGIN

#define DbgPrint /##/

#include "MiniKernel.h"

LONG g_SpinCount, g_CancelCount, g_CancelRoutineCount, g_IrpAlloc, g_IrpFree;

#define IRP_QUEUE_TO_FILE IRP_HOLD_DEVICE_QUEUE
#define IRP_REF_COUNTING 0x00008000

#define AddRefIrp(Irp)  InterlockedIncrement16((SHORT*)&Irp->Size)
#define ReleaseIrp(Irp) InterlockedDecrement16((SHORT*)&Irp->Size)

#define SafeFreeIrp(Irp)	if (!(Irp->Flags & IRP_REF_COUNTING) || !ReleaseIrp(Irp)) FreeIrp(Irp)

void LockIrpList(PFILE_OBJECT FileObject)
{
	ULONG SpinCount = 0;

	while (_interlockedbittestandset((PLONG)&FileObject->IrpListLock, 0))
	{
		SpinCount++;
	}

	if (SpinCount)
	{
		InterlockedExchangeAddNoFence(&g_SpinCount, SpinCount);
	}
}

void UnlockIrpList(PFILE_OBJECT FileObject)
{
	_interlockedbittestandreset((PLONG)&FileObject->IrpListLock, 0);
}

void QueueIrp(PIRP Irp, PFILE_OBJECT FileObject)
{
	LockIrpList(FileObject);

	InsertTailList(&FileObject->IrpList, &Irp->ThreadListEntry);

	Irp->Flags |= IRP_QUEUE_TO_FILE | IRP_REF_COUNTING;

	AddRefIrp(Irp);

	UnlockIrpList(FileObject);
}

void DequeueIrp(PIRP Irp, PFILE_OBJECT FileObject)
{
	LockIrpList(FileObject);

	if (Irp->Flags & IRP_QUEUE_TO_FILE)
	{
		PLIST_ENTRY ListEntry = &Irp->ThreadListEntry;
		RemoveEntryList(ListEntry);
		InitializeListHead(ListEntry);
		Irp->Flags &= ~IRP_QUEUE_TO_FILE;
	}

	UnlockIrpList(FileObject);
}

void CancelIo(PFILE_OBJECT FileObject)
{
	IRP* Irp;

	LIST_ENTRY ListHead = { &ListHead, &ListHead };

	PLIST_ENTRY IrpList = &FileObject->IrpList;

	// moving cancelable Irp(s) from FileObject->IrpList to local list and reference it
	
	LockIrpList(FileObject);
	
	PLIST_ENTRY Entry = IrpList->Flink;

	while (Entry != IrpList)
	{
		Irp = CONTAINING_RECORD(Entry, IRP, ThreadListEntry);

		Entry = Entry->Flink;

		if (!Irp->Cancel)
		{
			Irp->Cancel = TRUE;

			AddRefIrp(Irp);

			if (!(Irp->Flags & IRP_QUEUE_TO_FILE)) __debugbreak();

			Irp->Flags &= ~IRP_QUEUE_TO_FILE;

			RemoveEntryList(&Irp->ThreadListEntry);

			InsertTailList(&ListHead, &Irp->ThreadListEntry);
		}
	}

	UnlockIrpList(FileObject);

	// cancel and release Irp(s) from local list

	Entry = ListHead.Flink;
	
	while (Entry != &ListHead)
	{
		Irp = CONTAINING_RECORD(Entry, IRP, ThreadListEntry);

		Entry = Entry->Flink;

		InitializeListHead(&Irp->ThreadListEntry);

		// Irp->Cancel = TRUE; already set !
		if (PDRIVER_CANCEL CancelRoutine = IoSetCancelRoutine(Irp, 0))
		{
			InterlockedIncrementNoFence(&g_CancelRoutineCount);
			CancelRoutine(FileObject->DeviceObject, Irp);
		}

		if (Irp->Type != IO_TYPE_IRP) __debugbreak();

		SafeFreeIrp(Irp);
	}
}

PIRP AllocateIrp( _In_ CCHAR StackSize )
{
	if (PIRP Irp = (PIRP)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(IRP) + sizeof(IO_STACK_LOCATION) * StackSize))
	{
		InterlockedIncrementNoFence(&g_IrpAlloc);

		Irp->Type = IO_TYPE_IRP;
		InitializeListHead(&Irp->ThreadListEntry);

		Irp->StackCount = StackSize;
		Irp->CurrentLocation = StackSize;// + 1 in real code;

		Irp->Tail.Overlay.CurrentStackLocation = (PIO_STACK_LOCATION)(Irp + 1) + StackSize;

		return Irp;
	}

	return 0;
}

void FreeIrp(PIRP Irp)
{
	if (Irp->Type != IO_TYPE_IRP ||
		Irp->StackCount != Irp->CurrentLocation ||
		!IsListEmpty(&Irp->ThreadListEntry))
	{
		__debugbreak();
	}

	Irp->Type = 0;

	if (HANDLE hThread = Irp->Tail.Overlay.Thread)
	{
		NtClose(hThread);
	}

	InterlockedIncrementNoFence(&g_IrpFree);
}

PIRP BuildDeviceIoControlRequest(
								 _In_ PFILE_OBJECT FileObject,
								 _In_opt_ PIO_APC_ROUTINE ApcRoutine,
								 _In_opt_ PVOID ApcContext,
								 _In_ ULONG IoControlCode,
								 _In_opt_  PVOID InputBuffer,
								 _In_  ULONG InputBufferLength,
								 _Out_opt_ PVOID OutputBuffer,
								 _In_ ULONG OutputBufferLength,
								 _Out_ PIO_STATUS_BLOCK IoStatusBlock)
{
	PDEVICE_OBJECT DeviceObject = FileObject->DeviceObject;

	if (PIRP Irp = AllocateIrp(DeviceObject->StackSize))
	{
		if (HANDLE hThread = OpenThread(THREAD_SET_CONTEXT, FALSE, GetCurrentThreadId()))
		{
			Irp->UserIosb = IoStatusBlock;

			Irp->Overlay.AsynchronousParameters.UserApcContext = ApcContext;
			Irp->Overlay.AsynchronousParameters.UserApcRoutine = ApcRoutine;
			Irp->Tail.Overlay.Thread = (PETHREAD)hThread;
			Irp->Tail.Overlay.OriginalFileObject = FileObject;

			PIO_STACK_LOCATION IrpSp = IoGetNextIrpStackLocation(Irp);

			IrpSp->MajorFunction = IRP_MJ_DEVICE_CONTROL;
			IrpSp->DeviceObject = DeviceObject;
			IrpSp->FileObject = FileObject;
			IrpSp->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
			IrpSp->Parameters.DeviceIoControl.InputBufferLength = InputBufferLength;
			IrpSp->Parameters.DeviceIoControl.OutputBufferLength = OutputBufferLength;
			IrpSp->Parameters.DeviceIoControl.Type3InputBuffer = InputBuffer;
			Irp->UserBuffer = OutputBuffer;

			QueueIrp(Irp, FileObject);

			return Irp;
		}

		FreeIrp(Irp);
	}

	return 0;
}

void CompleteRequest(PIRP Irp)
{
	CHAR StackCount = Irp->StackCount;

	if (Irp->CurrentLocation < StackCount)
	{
		StackCount -= Irp->CurrentLocation;

		PIO_STACK_LOCATION IrpSp = Irp->Tail.Overlay.CurrentStackLocation++;

		do 
		{
			Irp->Tail.Overlay.CurrentStackLocation++;
			Irp->CurrentLocation++;

			Irp->PendingReturned = IrpSp->Control & SL_PENDING_RETURNED;

			PIO_COMPLETION_ROUTINE CompletionRoutine = IrpSp->CompletionRoutine;
			PVOID Context = IrpSp->Context;

			PDEVICE_OBJECT DeviceObject = --StackCount ? (++IrpSp)->DeviceObject : 0;

			if (CompletionRoutine)
			{
				if (CompletionRoutine(DeviceObject, Irp, Context) == StopCompletion)
				{
					return ;
				}
			}
			else
			{
				IrpSp->Control |= SL_PENDING_RETURNED;
			}

		} while (StackCount);
	}

	Irp->UserIosb->Status = Irp->IoStatus.Status;
	Irp->UserIosb->Information = Irp->IoStatus.Information;

	if (HANDLE hThread = Irp->Tail.Overlay.Thread)
	{
		Irp->Tail.Overlay.Thread = 0;

		ZwQueueApcThread(hThread, 
			(PKNORMAL_ROUTINE)Irp->Overlay.AsynchronousParameters.UserApcRoutine, 
			Irp->Overlay.AsynchronousParameters.UserApcContext,
			Irp->UserIosb, 0);
	}

	DequeueIrp(Irp, Irp->Tail.Overlay.OriginalFileObject);

	SafeFreeIrp(Irp);
}

NTSTATUS FASTCALL CallDriver( _In_ PDEVICE_OBJECT DeviceObject, _Inout_ __drv_aliasesMem PIRP Irp )
{
	if (0 > --Irp->CurrentLocation)
	{
		__debugbreak();
	}

	PIO_STACK_LOCATION IrpSp = --Irp->Tail.Overlay.CurrentStackLocation;

	return DeviceObject->DriverObject->MajorFunction[IrpSp->MajorFunction](DeviceObject, Irp);
}

_NT_END