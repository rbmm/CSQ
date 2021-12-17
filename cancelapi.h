#pragma once

class CSQ_IRP_CONTEXT_ALT;

class __declspec(novtable) IO_CSQ_ALT
{
	friend CSQ_IRP_CONTEXT_ALT;

	virtual KIRQL CsqAcquireLock() = 0;
	virtual void CsqReleaseLock(KIRQL Irql) = 0;
	virtual ULONG_PTR CsqInsertIrp(PIRP Irp, PVOID InsertContext) = 0;

protected:

	ULONG_PTR IoCsqInsertIrp(_In_ PIRP Irp, _In_ PLIST_ENTRY IrpList, _In_ PVOID InsertContext);

	void CompleteAllPending(_In_ PLIST_ENTRY IrpList, _In_ NTSTATUS status, _In_opt_ ULONG_PTR Information = 0);

	PIRP IoCsqRemoveIrp(_In_ PLIST_ENTRY IrpList, _In_opt_ ULONG_PTR Context = 0);

	static void ReleaseRemovedIrp(_In_ PIRP Irp, _In_opt_ CCHAR PriorityBoost = IO_NO_INCREMENT);
};
