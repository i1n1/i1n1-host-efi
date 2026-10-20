/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal UEFI 2.x definitions for i1n1.efi (own declarations, no EDK2
 * dependency). Only the services the loader uses are typed; every other
 * table slot is an opaque pointer that keeps the layout correct.
 *
 * Calling convention: the x86_64-unknown-windows target is Microsoft ABI,
 * which is exactly UEFI's, so no attribute is needed.
 */
#ifndef I1N1_EFI_UEFI_H
#define I1N1_EFI_UEFI_H

#include <stddef.h>
#include <stdint.h>

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint64_t UINTN;
typedef uint16_t CHAR16;
typedef uint64_t EFI_PHYSICAL_ADDRESS;

#define EFI_SUCCESS 0ull
#define EFI_ERROR_BIT (1ull << 63)
#define EFI_ERR(n) (EFI_ERROR_BIT | (n))
#define EFI_LOAD_ERROR EFI_ERR(1)
#define EFI_INVALID_PARAMETER EFI_ERR(2)
#define EFI_UNSUPPORTED EFI_ERR(3)
#define EFI_BUFFER_TOO_SMALL EFI_ERR(5)
#define EFI_OUT_OF_RESOURCES EFI_ERR(9)
#define EFI_NOT_FOUND EFI_ERR(14)
#define EFI_ABORTED EFI_ERR(21)

typedef struct {
  uint32_t d1;
  uint16_t d2;
  uint16_t d3;
  uint8_t d4[8];
} EFI_GUID;

typedef struct {
  uint64_t Signature;
  uint32_t Revision;
  uint32_t HeaderSize;
  uint32_t CRC32;
  uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef enum { AllocateAnyPages = 0, AllocateMaxAddress, AllocateAddress } EFI_ALLOCATE_TYPE;
#define EfiLoaderCode 1u
#define EfiLoaderData 2u
#define EfiResetShutdown 2u

typedef struct {
  uint32_t Type;
  uint32_t Pad;
  uint64_t PhysicalStart;
  uint64_t VirtualStart;
  uint64_t NumberOfPages;
  uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

struct EFI_SYSTEM_TABLE;

typedef struct {
  EFI_TABLE_HEADER Hdr;
  void *RaiseTPL;
  void *RestoreTPL;
  EFI_STATUS (*AllocatePages)(EFI_ALLOCATE_TYPE, uint32_t, UINTN, EFI_PHYSICAL_ADDRESS *);
  EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
  EFI_STATUS (*GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, uint32_t *);
  void *AllocatePool;
  void *FreePool;
  void *CreateEvent;
  void *SetTimer;
  void *WaitForEvent;
  void *SignalEvent;
  void *CloseEvent;
  void *CheckEvent;
  void *InstallProtocolInterface;
  void *ReinstallProtocolInterface;
  void *UninstallProtocolInterface;
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **);
  void *Reserved;
  void *RegisterProtocolNotify;
  void *LocateHandle;
  void *LocateDevicePath;
  void *InstallConfigurationTable;
  void *LoadImage;
  void *StartImage;
  void *Exit;
  void *UnloadImage;
  EFI_STATUS (*ExitBootServices)(EFI_HANDLE, UINTN);
  void *GetNextMonotonicCount;
  EFI_STATUS (*Stall)(UINTN);
  void *SetWatchdogTimer;
  void *ConnectController;
  void *DisconnectController;
  void *OpenProtocol;
  void *CloseProtocol;
  void *OpenProtocolInformation;
  void *ProtocolsPerHandle;
  void *LocateHandleBuffer;
  void *LocateProtocol;
  void *InstallMultipleProtocolInterfaces;
  void *UninstallMultipleProtocolInterfaces;
  void *CalculateCrc32;
  void *CopyMem;
  void *SetMem;
  void *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
  EFI_TABLE_HEADER Hdr;
  void *GetTime;
  void *SetTime;
  void *GetWakeupTime;
  void *SetWakeupTime;
  void *SetVirtualAddressMap;
  void *ConvertPointer;
  void *GetVariable;
  void *GetNextVariableName;
  void *SetVariable;
  void *GetNextHighMonotonicCount;
  void (*ResetSystem)(uint32_t, EFI_STATUS, UINTN, void *);
} EFI_RUNTIME_SERVICES;

typedef struct EFI_SYSTEM_TABLE {
  EFI_TABLE_HEADER Hdr;
  CHAR16 *FirmwareVendor;
  uint32_t FirmwareRevision;
  uint32_t Pad;
  EFI_HANDLE ConsoleInHandle;
  void *ConIn;
  EFI_HANDLE ConsoleOutHandle;
  void *ConOut;
  EFI_HANDLE StandardErrorHandle;
  void *StdErr;
  EFI_RUNTIME_SERVICES *RuntimeServices;
  EFI_BOOT_SERVICES *BootServices;
  UINTN NumberOfTableEntries;
  void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* EFI_LOADED_IMAGE_PROTOCOL */
typedef struct {
  uint32_t Revision;
  uint32_t Pad;
  EFI_HANDLE ParentHandle;
  EFI_SYSTEM_TABLE *SystemTable;
  EFI_HANDLE DeviceHandle;
  void *FilePath;
  void *Reserved;
  uint32_t LoadOptionsSize;
  uint32_t Pad2;
  void *LoadOptions;
  void *ImageBase;
  uint64_t ImageSize;
  uint32_t ImageCodeType;
  uint32_t ImageDataType;
  void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

struct EFI_FILE_PROTOCOL;
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
  uint64_t Revision;
  EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **, const CHAR16 *, uint64_t, uint64_t);
  EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *);
  void *Delete;
  EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *, UINTN *, void *);
  void *Write;
  void *GetPosition;
  void *SetPosition;
  EFI_STATUS (*GetInfo)(EFI_FILE_PROTOCOL *, const EFI_GUID *, UINTN *, void *);
  void *SetInfo;
  void *Flush;
};

typedef struct {
  uint64_t Revision;
  EFI_STATUS (*OpenVolume)(void *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
  uint64_t Size;
  uint64_t FileSize;
  uint64_t PhysicalSize;
  uint8_t CreateTime[16];
  uint8_t LastAccessTime[16];
  uint8_t ModificationTime[16];
  uint64_t Attribute;
  CHAR16 FileName[1];
} EFI_FILE_INFO;

#define EFI_FILE_MODE_READ 1ull

#endif
