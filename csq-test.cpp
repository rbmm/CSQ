#include "stdafx.h"

_NT_BEGIN

#include "cancelapi.h"

//////////////////////////////////////////////////////////////////////////
//
#pragma warning (disable : 4273)

PLIST_ENTRY GetThreadIrpListEntry()
{
	return &reinterpret_cast<_TEB*>(NtCurrentTeb())->TlsLinks;
}

struct IRP_S : public IRP, public IO_STACK_LOCATION 
{
	IRP_S(PFILE_OBJECT fo)
	{
		RtlZeroMemory(this, sizeof(*this));
		Tail.Overlay.CurrentStackLocation = this;
		FileObject = fo;
		IoStatus.Status = STATUS_PENDING;
		Type = IO_TYPE_IRP;
		Size = sizeof(*this);
		InsertTailList(GetThreadIrpListEntry(), &ThreadListEntry);
		DbgPrint("%p> ++IRP \"%wZ\"\r\n", this, &fo->FileName);
	}

	~IRP_S()
	{
		if (IO_TYPE_IRP != Type || sizeof(*this) != Size)
		{
			__debugbreak();
		}

		RemoveEntryList(&ThreadListEntry);

		DbgPrint("%p> --IRP [%x, %p] \"%wZ\"\r\n", 
			this, IoStatus.Status, IoStatus.Information,
			&Tail.Overlay.CurrentStackLocation->FileObject->FileName);
	}
};

VOID FASTCALL IofCompleteRequest( _In_ PIRP Irp, _In_ CCHAR /*PriorityBoost*/ )
{
	delete reinterpret_cast<IRP_S*>(Irp);
}

VOID IoReleaseCancelSpinLock( _In_ _IRQL_restores_ _IRQL_uses_cancel_ KIRQL Irql )
{
	DbgPrint("IoReleaseCancelSpinLock(%p)\r\n", Irql);
}

//////////////////////////////////////////////////////////////////////////
//

struct FOA 
{
	PFILE_OBJECT _FileObjects[4];
	ULONG _N;

	BOOL Insert(PFILE_OBJECT FileObject)
	{
		ULONG n = _N;
		if (n < _countof(_FileObjects))
		{
			PFILE_OBJECT* FileObjects = _FileObjects;
			if (n)
			{
				do 
				{
					if (FileObject == *FileObjects++)
					{
						return FALSE;
					}
				} while (--n);
			}

			*FileObjects = FileObject;
			_N++;

			return TRUE;
		}

		return FALSE;
	}
};

struct DEV_EXTENSION : IO_CSQ_ALT, LIST_ENTRY
{
	PVOID _FileObject;
	SRWLOCK _SRWLock = 0;

	DEV_EXTENSION(PVOID FileObject) : _FileObject(FileObject)
	{
		InitializeListHead(this);
	}

	virtual KIRQL CsqAcquireLock()
	{
		AcquireSRWLockExclusive(&_SRWLock);
		return 0;
	}

	virtual void CsqReleaseLock(KIRQL /*Irql*/)
	{
		ReleaseSRWLockExclusive(&_SRWLock);
	}

	virtual ULONG_PTR CsqInsertIrp(PIRP /*Irp*/, PVOID FileObject)
	{
		return _FileObject && (FileObject != _FileObject);
	}

	virtual BOOL CsqIsNeedRemove(PIRP Irp, _In_ PVOID Context)
	{
		return reinterpret_cast<FOA*>(Context)->Insert(IoGetCurrentIrpStackLocation(Irp)->FileObject);
	}

	void CompletePendingReads()
	{
		DbgPrint("[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\r\n");
		FOA f {};
		if (CSQ_IRP_CONTEXT_ALT* ctx = IoCsqRemoveIrps(this, &f))
		{
			do 
			{
				PIRP Irp;
				ctx = GetNextIrp(ctx, &Irp);

				PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;

				DbgPrint("RemoveIrp: %p %p\r\n", Irp, FileObject);

				Irp->IoStatus.Status = STATUS_SINGLE_STEP;
				Irp->IoStatus.Information = 0x12345678;
				IO_CSQ_ALT::ReleaseIrp(Irp, IO_NO_INCREMENT);

			} while (ctx);
		}
		DbgPrint("]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]\r\n");
	}

	void Insert(PFILE_OBJECT FileObject)
	{
		IoCsqInsertIrp(new IRP_S(FileObject), this, FileObject);
	}
};

void CancelIrp()
{
	PLIST_ENTRY ListHead = GetThreadIrpListEntry();

	if (!IsListEmpty(ListHead))
	{
		PIRP Irp = CONTAINING_RECORD(RemoveHeadList(ListHead), IRP, ThreadListEntry);

		InitializeListHead(&Irp->ThreadListEntry);

		if (IO_TYPE_IRP != Irp->Type)
		{
			__debugbreak();
		}

		Irp->Cancel = TRUE;

		if (Irp->CancelRoutine)
		{
			Irp->CancelRoutine(0, Irp);
		}
		else
		{
			__debugbreak();
		}
	}
}

void TestCsq()
{
	PLIST_ENTRY ThreadIrpList = GetThreadIrpListEntry();

	InitializeListHead(ThreadIrpList);

	DEV_EXTENSION ext(INVALID_HANDLE_VALUE);

	FILE_OBJECT fo[4], *pfo = fo;
	WCHAR buf[0x100], *psz = buf;
	ULONG n = _countof(fo), cch = _countof(buf);
	int len;
	do 
	{
		if (0 >= (len = swprintf_s(psz, cch, L"[ %x ]", n)))
		{
			__debugbreak();
			break;
		}
		RtlInitUnicodeString(&pfo++->FileName, psz);
	} while (psz += ++len, cch -= len, --n);

	ULONG seed = ~GetTickCount();

	n = 32;
	do 
	{
		ext.Insert(fo + (RtlRandomEx(&seed) & (_countof(fo) - 1)));
	} while (--n);

	CancelIrp();
	ext.CompletePendingReads();
	CancelIrp();
	ext.CompletePendingReads();
	CancelIrp();
	ext.CompletePendingReads();
	CancelIrp();
	ext.CompleteAllPending(&ext, STATUS_CANCELLED);

	if (!IsListEmpty(ThreadIrpList))
	{
		__debugbreak();
	}
}

_NT_END
