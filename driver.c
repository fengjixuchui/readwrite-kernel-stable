#include "ntos.h"

// type definitions
typedef unsigned long long QWORD;
typedef unsigned short WORD;
typedef unsigned long DWORD, * PDWORD, * LPDWORD;

// credits to paracord, wjcsharp, & cheat-engine's repositories, without them I wouldn't of been able to figure out the puzzel of reading and writing with kernel mode buffers & user mode memory

/*

paracord - https://github.com/paracorded/read_write
wjcsharp - https://github.com/wjcsharp/wintools/blob/79b3883aacb5833d747d5bedce843086c327dff3/examples/random/ReadMemoryKernel.c
cheat-engine - https://github.com/cheat-engine/cheat-engine/blob/master/DBKKernel/memscan.c

*/

typedef struct _LDR_DATA_TABLE_ENTRY
{
	LIST_ENTRY64	InLoadOrderLinks;
	LIST_ENTRY64	InMemoryOrderLinks;
	LIST_ENTRY64	InInitializationOrderLinks;
	UINT64			DllBase;
	UINT64			EntryPoint;
	ULONG			SizeOfImage;
	UNICODE_STRING	FullDllName;
	UNICODE_STRING 	BaseDllName;
	ULONG			Flags;
	USHORT			LoadCount;
	USHORT			TlsIndex;
	PVOID			SectionPointer;
	ULONG			CheckSum;
	PVOID			LoadedImports;
	PVOID			EntryPointActivationContext;
	PVOID			PatchInformation;
	LIST_ENTRY64	ForwarderLinks;
	LIST_ENTRY64	ServiceTagLinks;
	LIST_ENTRY64	StaticLinks;
	PVOID			ContextInformation;
	ULONG64			OriginalBase;
	LARGE_INTEGER	LoadTime;
} LDR_DATA_TABLE_ENTRY, * PLDR_DATA_TABLE_ENTRY;

// structure definitions
typedef struct _MODULE_INFO
{
	UINT64 Base;
	ULONG Size;
	WCHAR Name[1024];
} MODULE_INFO, *PMODULE_INFO;

typedef struct _KERNEL_READ_REQUEST
{
	UINT64 Address; // Source
	PVOID Response; // Target
	SIZE_T Size;
} KERNEL_READ_REQUEST, * PKERNEL_READ_REQUEST;

typedef struct _KERNEL_WRITE_REQUEST
{
	UINT64 Address; // Target
	PVOID Value; // Source
	SIZE_T Size;
	BOOLEAN BytePatching;
} KERNEL_WRITE_REQUEST, *PKERNEL_WRITE_REQUEST;

typedef struct _KERNEL_MODULE_REQUEST
{
	MODULE_INFO* buffer;
	WCHAR moduleName[1024];
} KERNEL_MODULE_REQUEST, * PKERNEL_MODULE_REQUEST;

typedef struct _MEMORY_REQUEST
{
	ULONG ProcessId;
	KERNEL_READ_REQUEST read;
	KERNEL_WRITE_REQUEST write;
	KERNEL_MODULE_REQUEST module;
} MEMORY_REQUEST;

// method definitions
DWORD PEBLDR_OFFSET = 0x18; // peb.ldr
DWORD PEBLDR_MEMORYLOADED_OFFSET = 0x10; // peb.ldr.InMemoryOrderModuleList
NTSTATUS SPM(ULONG PID, MEMORY_REQUEST* sent) {
	PEPROCESS Process;
	KAPC_STATE APC;
	NTSTATUS Status = STATUS_FAIL_CHECK;

	if (!NT_SUCCESS(PsLookupProcessByProcessId((PVOID)PID, &Process)))
		return STATUS_INVALID_PARAMETER_1;

	PMODULE_INFO ModuleList = ExAllocatePool(PagedPool, sizeof(MODULE_INFO) * 512);
	if (ModuleList == NULL)
		return STATUS_MEMORY_NOT_ALLOCATED;

	RtlZeroMemory(ModuleList, sizeof(MODULE_INFO) * 512);

	PPEB Peb = PsGetProcessPeb(Process);
	if (!Peb)
		return STATUS_INVALID_PARAMETER_1;

	__try {
		KeStackAttachProcess(Process, &APC);

		UINT64 Ldr = (UINT64)Peb + PEBLDR_OFFSET;
		ProbeForRead((CONST PVOID)Ldr, 8, 8);

		PLIST_ENTRY ModListHead = (PLIST_ENTRY)(*(PULONG64)Ldr + PEBLDR_MEMORYLOADED_OFFSET);
		ProbeForRead((CONST PVOID)ModListHead, 8, 8);

		PLIST_ENTRY Module = ModListHead->Flink;
		
		DWORD index = 0;
		while (ModListHead != Module) {
			LDR_DATA_TABLE_ENTRY* Module_Ldr = (LDR_DATA_TABLE_ENTRY*)(Module);

			ModuleList[index].Base = Module_Ldr->DllBase;
			ModuleList[index].Size = Module_Ldr->SizeOfImage;
			RtlCopyMemory(ModuleList[index].Name, Module_Ldr->BaseDllName.Buffer, Module_Ldr->BaseDllName.Length);

			Module = Module->Flink;
			index++;
		}

		KeUnstackDetachProcess(&APC);

		Status = STATUS_SUCCESS;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		KeUnstackDetachProcess(&APC);
	}

	ModuleList[0].Base += (UINT64)PsGetProcessSectionBaseAddress(Process);

	WCHAR ModuleName[1024];

	RtlZeroMemory(ModuleName, 1024);
	wcsncpy(ModuleName, sent->module.moduleName, 1024);

	MODULE_INFO SelectedModule;
	for (DWORD i = 0; i < 512; i++) {
		MODULE_INFO CurrentModule = ModuleList[i];

		if (_wcsicmp(CurrentModule.Name, ModuleName) == 0) {
			SelectedModule = CurrentModule;
			break;
		}
	}

	if (SelectedModule.Base != NULL && SelectedModule.Size != NULL) {
		*sent->module.buffer = SelectedModule;
	}

	ExFreePool(ModuleList);
	ObfDereferenceObject(Process);

	return Status;
}

NTSTATUS RVM(ULONG PID, MEMORY_REQUEST* sent) {
	PEPROCESS Process;

	// lookup eprocess for use in attaching
	if (!NT_SUCCESS(PsLookupProcessByProcessId((PVOID)PID, &Process)))
		return STATUS_INVALID_PARAMETER_1;

	// create our own variables for usermode buffer, for some reason it will crash if we dont use these variables
	PVOID Address = (PVOID)sent->read.Address;
	SIZE_T Size = sent->read.Size;

	// alocate memory for our driverbuffer, will be used to read memory from the process
	PVOID* Buffer = (PVOID*)ExAllocatePool(NonPagedPool, Size); // Pointer to Allocated Memory
	if (Buffer == NULL) {
		ObfDereferenceObject(Process);

		return STATUS_MEMORY_NOT_ALLOCATED;
	}

	RtlSecureZeroMemory(Buffer, Size);

	KAPC_STATE APC;
	NTSTATUS Status = STATUS_FAIL_CHECK;

	__try {
		// attach
		KeStackAttachProcess(Process, &APC);

		// query information on memory to verify it meets our requirements
		MEMORY_BASIC_INFORMATION info;
		if (!NT_SUCCESS(ZwQueryVirtualMemory(ZwCurrentProcess(), Address, MemoryBasicInformation, &info, sizeof(MEMORY_BASIC_INFORMATION), NULL))) {
			KeUnstackDetachProcess(&APC);

			ExFreePool(Buffer);
			ObfDereferenceObject(Process);

			Status = STATUS_INVALID_ADDRESS_COMPONENT;

			return Status;
		}

		ULONG flags = PAGE_EXECUTE_READWRITE | PAGE_READWRITE | PAGE_EXECUTE_READ;
		ULONG page = PAGE_GUARD | PAGE_NOACCESS;

		// confirm memory block meets our requirements
		if (!(info.State & MEM_COMMIT) || !(info.Protect & flags) || (info.Protect & page)) {
			KeUnstackDetachProcess(&APC);

			ExFreePool(Buffer);
			ObfDereferenceObject(Process);

			Status = STATUS_ACCESS_DENIED;

			return Status;
		}

		// secure memory so it doesnt change between the beginning of the request & the end, practically the same as doing ZwProtectVirtualMemory
		HANDLE Secure = MmSecureVirtualMemory(Address, Size, PAGE_READWRITE);
		if (Secure == NULL) {
			KeUnstackDetachProcess(&APC);

			ExFreePool(Buffer);
			ObfDereferenceObject(Process);

			Status = STATUS_ACCESS_VIOLATION;

			return Status;
		}

		if (MmIsAddressValid(Address) == FALSE) {
			KeUnstackDetachProcess(&APC);

			ExFreePool(Buffer);
			ObfDereferenceObject(Process);

			Status = STATUS_ACCESS_VIOLATION;

			return Status;
		}

		// read memory to our driver's buffer
		memcpy(Buffer, Address, Size);

		// cleanup, unsecure memory & detach from process
		MmUnsecureVirtualMemory(Secure);
		KeUnstackDetachProcess(&APC);

		// read memory from our driver's buffer over to our usermode buffer
		memcpy(sent->read.Response, Buffer, Size);

		Status = STATUS_SUCCESS;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		// detach if anything goes wrong
		KeUnstackDetachProcess(&APC);
	}

	// cleanup for us, deallocate buffer memory & deref eprocess as we added a ref
	ExFreePool(Buffer);
	ObfDereferenceObject(Process);

	return Status;
}

NTSTATUS WVM(ULONG PID, MEMORY_REQUEST* sent) {
	PEPROCESS Process;

	if (!NT_SUCCESS(PsLookupProcessByProcessId((PVOID)PID, &Process)))
		return STATUS_INVALID_PARAMETER_1;

	PVOID Address = (PVOID)sent->write.Address;
	SIZE_T Size = sent->write.Size;

	// allocate memory for our driver buffer
	PVOID* Buffer = (PVOID*)ExAllocatePool(NonPagedPool, Size); // Pointer to Allocated Memory
	if (Buffer == NULL) {
		ObfDereferenceObject(Process);

		return STATUS_MEMORY_NOT_ALLOCATED;
	}

	RtlSecureZeroMemory(Buffer, Size);

	KAPC_STATE APC;
	NTSTATUS Status = STATUS_FAIL_CHECK;

	__try {
		// copy our usermode buffer's value over to our driver's buffer
		memcpy(Buffer, sent->write.Value, Size);

		KeStackAttachProcess(Process, &APC);

		MEMORY_BASIC_INFORMATION info;
		if (!NT_SUCCESS(ZwQueryVirtualMemory(ZwCurrentProcess(), Address, MemoryBasicInformation, &info, sizeof(MEMORY_BASIC_INFORMATION), NULL))) {
			KeUnstackDetachProcess(&APC);

			ExFreePool(Buffer);
			ObfDereferenceObject(Process);

			Status = STATUS_INVALID_PARAMETER_2;

			return Status;
		}

		ULONG flags = PAGE_EXECUTE_READWRITE | PAGE_READWRITE;
		ULONG page = PAGE_GUARD | PAGE_NOACCESS;

		if (!(info.State & MEM_COMMIT) || !(info.Protect & flags) || (info.Protect & page)) {
			KeUnstackDetachProcess(&APC);

			ExFreePool(Buffer);
			ObfDereferenceObject(Process);

			Status = STATUS_ACCESS_DENIED;

			return Status;
		}

		HANDLE Secure = MmSecureVirtualMemory(Address, Size, PAGE_READWRITE);
		if (Secure == NULL) {
			KeUnstackDetachProcess(&APC);

			ExFreePool(Buffer);
			ObfDereferenceObject(Process);

			Status = STATUS_ACCESS_VIOLATION;

			return Status;
		}

		if (MmIsAddressValid(Address) == FALSE) {
			KeUnstackDetachProcess(&APC);

			ExFreePool(Buffer);
			ObfDereferenceObject(Process);

			Status = STATUS_ACCESS_VIOLATION;

			return Status;
		}

		// send our driver's buffer to our applications memory address
		memcpy(Address, Buffer, Size);

		MmUnsecureVirtualMemory(Secure);
		KeUnstackDetachProcess(&APC);

		Status = STATUS_SUCCESS;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		KeUnstackDetachProcess(&APC);
	}

	ExFreePool(Buffer);
	ObfDereferenceObject(Process);

	return Status;
}

NTSTATUS WVMP(ULONG PID, MEMORY_REQUEST* sent) { // write virtual memory, with less restrictions, should only be used for byte patching in protected memory regions
	PEPROCESS Process;
	KAPC_STATE APC;
	NTSTATUS Status;

	if (!NT_SUCCESS(PsLookupProcessByProcessId((PVOID)PID, &Process)))
		return STATUS_INVALID_PARAMETER_1;

	PVOID Address = (PVOID)sent->write.Address;
	PVOID ProtectedAddress = (PVOID)sent->write.Address;
	SIZE_T Size = sent->write.Size;
	SIZE_T ProtectedSize = sent->write.Size;

	PVOID* Buffer = (PVOID*)ExAllocatePool(NonPagedPool, Size); // Pointer to Allocated Memory
	if (Buffer == NULL) {
		ObfDereferenceObject(Process);

		return STATUS_MEMORY_NOT_ALLOCATED;
	}

	RtlSecureZeroMemory(Buffer, Size);

	__try {
		memcpy(Buffer, sent->write.Value, Size);

		KeStackAttachProcess(Process, &APC);

		ULONG OldProtection;
		Status = ZwProtectVirtualMemory(ZwCurrentProcess(), &ProtectedAddress, &ProtectedSize, PAGE_EXECUTE_READWRITE, &OldProtection);
		if (!NT_SUCCESS(Status)) {
			KeUnstackDetachProcess(&APC);

			ExFreePool(Buffer);
			ObfDereferenceObject(Process);

			return Status;
		}

		ProtectedAddress = Address;
		ProtectedSize = Size;

		MEMORY_BASIC_INFORMATION info;
		Status = ZwQueryVirtualMemory(ZwCurrentProcess(), Address, MemoryBasicInformation, &info, sizeof(MEMORY_BASIC_INFORMATION), NULL);
		if (!NT_SUCCESS(Status)) {
			KeUnstackDetachProcess(&APC);

			ExFreePool(Buffer);
			ObfDereferenceObject(Process);

			return Status;
		}

		if (!(info.State & MEM_COMMIT)) {
			ZwProtectVirtualMemory(ZwCurrentProcess(), &ProtectedAddress, &ProtectedSize, OldProtection, &OldProtection);
			KeUnstackDetachProcess(&APC);

			ExFreePool(Buffer);
			ObfDereferenceObject(Process);

			Status = STATUS_ACCESS_DENIED;

			return Status;
		}

		memcpy(Address, Buffer, Size);

		ZwProtectVirtualMemory(ZwCurrentProcess(), &ProtectedAddress, &ProtectedSize, OldProtection, &OldProtection);
		KeUnstackDetachProcess(&APC);

		Status = STATUS_SUCCESS;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		KeUnstackDetachProcess(&APC);
	}

	ExFreePool(Buffer);
	ObfDereferenceObject(Process);

	return Status;
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
	UNREFERENCED_PARAMETER(pDriverObject);
	UNREFERENCED_PARAMETER(pRegistryPath);

	return STATUS_SUCCESS;
}
