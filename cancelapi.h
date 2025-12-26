#pragma once

class CSQ_IRP_CONTEXT_ALT;

class __declspec(novtable) IO_CSQ_ALT
{
	friend CSQ_IRP_CONTEXT_ALT;

	virtual KIRQL CsqAcquireLock() = 0;
	virtual void CsqReleaseLock(KIRQL Irql) = 0;
	virtual ULONG_PTR CsqInsertIrp(PIRP Irp, PVOID InsertContext) = 0;
	virtual BOOL CsqIsNeedRemove(PIRP Irp, _In_ PVOID Context) = 0;

public:

	ULONG_PTR IoCsqInsertIrp(_In_ PIRP Irp, _In_ PLIST_ENTRY IrpList, _In_ PVOID InsertContext, _In_ BOOL bRelease = TRUE);

	void CompleteAllPending(_In_ PLIST_ENTRY IrpList, _In_ NTSTATUS status, _In_opt_ ULONG_PTR Information = 0);

	PIRP IoCsqRemoveIrp(_In_ PLIST_ENTRY IrpList, _In_opt_ ULONG_PTR Context = 0);

	CSQ_IRP_CONTEXT_ALT* IoCsqRemoveIrps(_In_ PLIST_ENTRY IrpList, _In_ PVOID Context);

	static void ReleaseIrp(_In_ PIRP Irp, _In_opt_ CCHAR PriorityBoost = IO_NO_INCREMENT);
	static CSQ_IRP_CONTEXT_ALT* GetNextIrp(_In_ CSQ_IRP_CONTEXT_ALT* ctx, _Out_ PIRP* pIrp);
};

