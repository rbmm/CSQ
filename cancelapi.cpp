#include "stdafx.h"

_NT_BEGIN

#include "cancelapi.h"

#define _GET_CANCEL_CONTEXT(Irp) Irp->Tail.Overlay.DriverContext
#define GET_CANCEL_CONTEXT(Irp) reinterpret_cast<CSQ_IRP_CONTEXT_ALT*>(_GET_CANCEL_CONTEXT(Irp))

class CSQ_IRP_CONTEXT_ALT
{
	friend IO_CSQ_ALT;

	IO_CSQ_ALT* _Csq;
	ULONG_PTR _Context;
	CSQ_IRP_CONTEXT_ALT* _next = 0;
	LONG _dwRefCount = 3;

	void Release(PIRP Irp, _In_opt_ CCHAR PriorityBoost = IO_NO_INCREMENT)
	{
		if (!InterlockedDecrement(&_dwRefCount))
		{
			DbgPrint("IofCompleteRequest<%p>(%x)\n", Irp, Irp->IoStatus.Status);
			IofCompleteRequest(Irp, PriorityBoost);
		}
	}

	void* operator new(size_t, PIRP Irp)
	{
		return _GET_CANCEL_CONTEXT(Irp);
	}

	CSQ_IRP_CONTEXT_ALT(IO_CSQ_ALT * Csq, ULONG_PTR Context) : _Csq(Csq), _Context(Context)
	{
	}

	void OnCancelOutsideSpinLock(PIRP Irp);

	static void CALLBACK IoCancelRoutine(_In_ PDEVICE_OBJECT , _In_ PIRP Irp)
	{
		IoReleaseCancelSpinLock(Irp->CancelIrql);
		GET_CANCEL_CONTEXT(Irp)->OnCancelOutsideSpinLock(Irp);
	}
};

C_ASSERT(sizeof(CSQ_IRP_CONTEXT_ALT) <= RTL_FIELD_SIZE(IRP, Tail.Overlay.DriverContext));

void CSQ_IRP_CONTEXT_ALT::OnCancelOutsideSpinLock(PIRP Irp)
{
	DbgPrint("%s(%p)\n", __FUNCTION__, Irp);

	PLIST_ENTRY Entry = &Irp->Tail.Overlay.ListEntry;

	if (!IsListEmpty(Entry))
	{
		BOOLEAN bRemoved;

		KIRQL Irql = _Csq->CsqAcquireLock();

		if (bRemoved = !IsListEmpty(Entry))
		{
			RemoveEntryList(Entry);
			InitializeListHead(Entry);
		}

		_Csq->CsqReleaseLock(Irql);

		if (bRemoved) 
		{
			DbgPrint("IRP<%p> removed from list\n", Irp);
			Irp->IoStatus.Status = STATUS_CANCELLED;
			Irp->IoStatus.Information = 0;
			Release(Irp);
		}
	}

	Release(Irp);
}

ULONG_PTR IO_CSQ_ALT::IoCsqInsertIrp(_In_ PIRP Irp, _In_ PLIST_ENTRY IrpList, _In_ PVOID InsertContext)
{
	CSQ_IRP_CONTEXT_ALT* ctx = 0;

	KIRQL Irql = CsqAcquireLock();

	ULONG_PTR Context = CsqInsertIrp(Irp, InsertContext);

	if (Context)
	{
		ctx = new (Irp) CSQ_IRP_CONTEXT_ALT(this, Context);

		IoMarkIrpPending(Irp);
		InsertTailList(IrpList, &Irp->Tail.Overlay.ListEntry);

		DbgPrint("CsqInsertIrp(%p)\n", Irp);
	}

	CsqReleaseLock(Irql);

	if (!Context)
	{
		return 0;
	}

	IoSetCancelRoutine(Irp, CSQ_IRP_CONTEXT_ALT::IoCancelRoutine);

	if (Irp->Cancel)
	{
		// The IRP was canceled.  Check whether our cancel routine was called.
		if (IoSetCancelRoutine(Irp, 0))
		{
			// The cancel routine was NOT called.  
			// So call it direct youself.
			ctx->OnCancelOutsideSpinLock(Irp);
		}
		// The cancel routine WAS called at this point  
	}

	ctx->Release(Irp);

	return Context;
}

void IO_CSQ_ALT::CompleteAllPending(_In_ PLIST_ENTRY IrpList, _In_ NTSTATUS status, _In_opt_ ULONG_PTR Information)
{
	CSQ_IRP_CONTEXT_ALT* first = 0, *ctx;

	KIRQL Irql = CsqAcquireLock();

	PLIST_ENTRY Entry = IrpList->Flink;

	while (Entry != IrpList)
	{
		PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

		Entry = Entry->Flink;

		ctx = GET_CANCEL_CONTEXT(Irp);

		if (IoSetCancelRoutine(Irp, 0))
		{
			// cancel routine not called
			InterlockedDecrement(&ctx->_dwRefCount);
		}

		InitializeListHead(&Irp->Tail.Overlay.ListEntry);

		ctx->_next = first, first = ctx;
	}

	InitializeListHead(IrpList);

	CsqReleaseLock(Irql);

	while (ctx = first)
	{
		PIRP Irp = CONTAINING_RECORD(ctx, IRP, Tail.Overlay.DriverContext);

		first = ctx->_next;

		DbgPrint("IRP<%p> removed from list\n", Irp);

		Irp->IoStatus.Status = status;
		Irp->IoStatus.Information = Information;
		ctx->Release(Irp);
	}
}

PIRP IO_CSQ_ALT::IoCsqRemoveIrp(_In_ PLIST_ENTRY IrpList, _In_opt_ ULONG_PTR Context)
{
	KIRQL Irql = CsqAcquireLock();

	PLIST_ENTRY Entry = IrpList;

	while ((Entry = Entry->Flink) != IrpList)
	{
		PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

		CSQ_IRP_CONTEXT_ALT* ctx = GET_CANCEL_CONTEXT(Irp);

		if (!Context || ctx->_Context == Context)
		{
			if (!IoSetCancelRoutine(Irp, 0))
			{
				break;
			}

			// cancel routine not called
			InterlockedDecrement(&ctx->_dwRefCount);

			RemoveEntryList(Entry);
			InitializeListHead(Entry);
			CsqReleaseLock(Irql);

			return Irp;
		}
	}

	CsqReleaseLock(Irql);

	return 0;
}

void IO_CSQ_ALT::ReleaseRemovedIrp(_In_ PIRP Irp, _In_opt_ CCHAR PriorityBoost)
{
	GET_CANCEL_CONTEXT(Irp)->Release(Irp, PriorityBoost);
}

_NT_END