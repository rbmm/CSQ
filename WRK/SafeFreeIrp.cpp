#define IRP_QUEUE_TO_FILE   IRP_HOLD_DEVICE_QUEUE
#define IRP_REF_COUNTING    0x00008000
#define IRP_10000           0x00010000


#define AddRefIrp(Irp)  MiniAddrefRelease((void**)&Irp->Overlay.AsynchronousParameters.IssuingProcess, +1)
#define ReleaseIrp(Irp) MiniAddrefRelease((void**)&Irp->Overlay.AsynchronousParameters.IssuingProcess, -1)

#define SafeFreeIrp(Irp) if (!(Irp->Flags & IRP_REF_COUNTING) || !ReleaseIrp(Irp)) FreeIrp(Irp)

/*++

AddRefIrp called from:
  IopQueueIrpToFileObject 
  IopCheckListForCancelableIrp

ReleaseIrp from:
  IoRemoveIoCompletion 
  IopDropIrp
  IopCompleteRequest
  
  // SafeFreeIrp(Irp)
  if (!(Irp->Flags & IRP_REF_COUNTING) || !ReleaseIrp(Irp)) FreeIrp(Irp);
  
  from IopCancelIrpsInFileObjectList:
  
  // ASSERT(Irp->Flags & IRP_REF_COUNTING);
  if (!ReleaseIrp(Irp)) IoFreeIrp(Irp);

--*/

//////////////////////////////////////////////////////////////////////////
// up to +3 reference
//

/*++

Arguments:
  ppv: pointer to some object pointer. because pointer to object aligned to 8 bytes low 3 (0,1,2) bits always 0
  bits 1,2 is used for store reference count
  why bit 0 not used ?! reserved for mark Wow64 (?!)

  i: -1 (Release) or +1 (AddRef)

Return Value:
  reference count, in range( [0..+3] )

--*/

ULONG MiniAddrefRelease(void** ppvObject, LONG i)
{
  ULONG_PTR r;

  void* CurrentValue, *PrevValue = *ppvObject;

  do 
  {
    CurrentValue = PrevValue;

    r = (((ULONG_PTR)CurrentValue >> 1) & 3) + i;

    PrevValue = _InterlockedCompareExchangePointer(ppvObject, 
      (void*)(((ULONG_PTR)CurrentValue & ~(3 << 1)) | (r << 1)), PrevValue);

  } while (CurrentValue != PrevValue);

  return (ULONG)r;
}

BOOLEAN IopCheckListForCancelableIrp(
  _In_ PLIST_ENTRY ListHead, 
  _In_ PEPROCESS Process, 
  _In_opt_ PIO_STATUS_BLOCK IoRequestToCancel, 
  _In_opt_ PETHREAD Thread, 
  _In_opt_ PLIST_ENTRY Entry, 
  _Out_ PIRP* pIrp)
{
  BOOLEAN bNotNeedDelay = TRUE;
  *pIrp = 0;

  if (!Entry)
  {
    Entry = ListHead->Flink;
  }

  for (; Entry != ListHead; Entry = Entry->Flink)
  {
    PIRP Irp = CONTAINING_RECORD(Entry, IRP, ThreadListEntry);

    if (
      Process != (PEPROCESS)((ULONG_PTR)Irp->Overlay.AsynchronousParameters.IssuingProcess & ~6) ||
      (IoRequestToCancel && IoRequestToCancel != Irp->UserIosb) ||
      (Thread && Thread != Irp->Tail.Overlay.Thread)
      )
    {
      continue;
    }

    if ((Irp->CurrentLocation >= Irp->StackCount + 2) || Irp->Cancel)
    {
      if (!(Irp->Flags & IRP_10000))
      {
        bNotNeedDelay = FALSE;
      }

      continue;
    }

    AddRefIrp(Irp);
    *pIrp = Irp;
    return FALSE;
  }

  return bNotNeedDelay;
}

BOOL IopCancelIrpsInFileObjectList(
  _In_ PFILE_OBJECT FileObject, 
  _In_ PEPROCESS Process, 
  _In_opt_ PIO_STATUS_BLOCK IoRequestToCancel, 
  _In_opt_ PETHREAD Thread, 
  _In_ BOOLEAN bAll, 
  _In_ BOOLEAN bNeverMore)
{
  BOOL bSomeCanceled = FALSE;

  LARGE_INTEGER Interval = { (ULONG)-10000, -1 };

  PLIST_ENTRY IrpList = &FileObject->IrpList;

  PKSPIN_LOCK IrpListLock = &FileObject->IrpListLock;

  KIRQL Irql = KeAcquireSpinLockRaiseToDpc(IrpListLock);

  if (bNeverMore)
  {
    FileObject->Flags |= FO_QUEUE_IRP_TO_THREAD;
  }

  for (PLIST_ENTRY Entry = 0; !IsListEmpty(IrpList); )
  {
    PIRP Irp;
    
    BOOLEAN bNotNeedDelay = IopCheckListForCancelableIrp(IrpList, Process, IoRequestToCancel, Thread, Entry, &Irp);
    
    if (Irp)
    {
      if (!Irp->Cancel)
      {
        Irp->Cancel = TRUE;

        bSomeCanceled = TRUE;

        KeReleaseSpinLock(IrpListLock, Irql);

        IoCancelIrp(Irp);

        Irql = KeAcquireSpinLockRaiseToDpc(IrpListLock);
      }

      Entry = Irp->Flags & IRP_QUEUE_TO_FILE ? Irp->ThreadListEntry.Flink : 0;

      if (!ReleaseIrp(Irp))
      {
        IoFreeIrp(Irp);
      }

      if (Entry != IrpList)
      {
        continue;
      }

      if (!bAll)
      {
        break;
      }     
    }
    else
    {
      if (!bAll)
      {
        break;
      }

      if (!bNotNeedDelay)
      {
        KeReleaseSpinLock(IrpListLock, Irql);

        KeDelayExecutionThread(KernelMode, FALSE, &Interval);

        Irql = KeAcquireSpinLockRaiseToDpc(IrpListLock);
      }

      if (!Entry)
      {
        break;
      }
    }

    Entry = 0;
  }

  KeReleaseSpinLock(IrpListLock, Irql);

  return bSomeCanceled;
}

void IopDequeueIrpFromFileObject(_In_ PIRP Irp, _In_ PFILE_OBJECT FileObject)
{
  PKSPIN_LOCK IrpListLock = &FileObject->IrpListLock;

  KIRQL Irql = KeAcquireSpinLockRaiseToDpc(IrpListLock);

  RemoveEntryList(&Irp->ThreadListEntry);

  ObfDereferenceObjectWithTag(
    (PEPROCESS)(~6 & (ULONG_PTR)Irp->Overlay.AsynchronousParameters.IssuingProcess), 'pCoI');

  KeReleaseSpinLock(IrpListLock, Irql);
}

void IopQueueIrpToFileObject(_In_ PIRP Irp, _In_ PFILE_OBJECT FileObject)
{
  PKSPIN_LOCK IrpListLock = &FileObject->IrpListLock;

  KIRQL Irql = KeAcquireSpinLockRaiseToDpc(IrpListLock);

  if (!(FileObject->Flags & FO_QUEUE_IRP_TO_THREAD))
  {
    InsertHeadList(&FileObject->IrpList, &Irp->ThreadListEntry);

    PEPROCESS IssuingProcess = IoGetCurrentProcess();
    ObfReferenceObject(IssuingProcess);

    Irp->Flags |= IRP_QUEUE_TO_FILE;
    Irp->Overlay.AsynchronousParameters.IssuingProcess = IssuingProcess;
    AddRefIrp(Irp);
  }

  KeReleaseSpinLock(IrpListLock, Irql);
}

VOID
IopDropIrp(
    _In_ PIRP Irp,
    _In_ PFILE_OBJECT FileObject
    )
{
    PMDL mdl;
    PMDL nextMdl;

    //
    // Free the resources associated with the IRP.
    //

    if (Irp->Flags & IRP_DEALLOCATE_BUFFER) {
        ExFreePool( Irp->AssociatedIrp.SystemBuffer );
    }

    if (Irp->MdlAddress) {
        for (mdl = Irp->MdlAddress; mdl; mdl = nextMdl) {
            nextMdl = mdl->Next;
            IoFreeMdl( mdl );
        }
    }

    if (Irp->UserEvent &&
        FileObject &&
        !(Irp->Flags & IRP_SYNCHRONOUS_API)) {
        ObDereferenceObject( Irp->UserEvent );
    }

  //++ new code
  if (Irp->Flags & IRP_QUEUE_TO_FILE)
  {
    IopDequeueIrpFromFileObject(Irp, FileObject);
  }
  //-- new code

    if (FileObject && !(Irp->Flags & IRP_CREATE_OPERATION)) {
        ObDereferenceObjectEx( FileObject, 1 );
    }

  //++ new code
  // same as SafeFreeIrp(Irp);
  if (!(Irp->Flags & IRP_REF_COUNTING) || !ReleaseIrp(Irp))
  //-- new code
  {
    //
    // Finally, free the IRP itself.
    //

    IoFreeIrp( Irp );
  }
}
