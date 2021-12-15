#pragma once

void CancelIo(PFILE_OBJECT FileObject);
PIRP AllocateIrp( _In_ CCHAR StackSize );
void FreeIrp(PIRP Irp);
void CompleteRequest(PIRP Irp);
NTSTATUS FASTCALL CallDriver( _In_ PDEVICE_OBJECT DeviceObject, _Inout_ __drv_aliasesMem PIRP Irp );

PIRP BuildDeviceIoControlRequest(
								 _In_ PFILE_OBJECT FileObject,
								 _In_opt_ PIO_APC_ROUTINE ApcRoutine,
								 _In_opt_ PVOID ApcContext,
								 _In_ ULONG IoControlCode,
								 _In_opt_  PVOID InputBuffer,
								 _In_  ULONG InputBufferLength,
								 _Out_opt_ PVOID OutputBuffer,
								 _In_ ULONG OutputBufferLength,
								 _Out_ PIO_STATUS_BLOCK IoStatusBlock);