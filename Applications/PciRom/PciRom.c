/*
 * Copyright (C) 2017 Andrei Evgenievich Warkentin
 *
 * This program and the accompanying materials
 * are licensed and made available under the terms and conditions of the BSD License
 * which accompanies this distribution.  The full text of the license may be found at
 * http://opensource.org/licenses/bsd-license.php
 *
 * THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
 * WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
 */

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Pi/PiDxeCis.h>
#include <Library/UtilsLib.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <IndustryStandard/Pci.h>

static EFI_STATUS
Usage (
       IN CHAR16 *Name
       )
{
  Print(L"Usage: %s seg bus dev func\n", Name);
  return EFI_INVALID_PARAMETER;
}

static VOID
DumpImage (
           IN VOID *RomImage,
           IN VOID *RomHeader,
           IN UINTN Length,
           IN PCI_DATA_STRUCTURE *Pcir
           )
{
  CHAR16 *Type;
  EFI_PCI_EXPANSION_ROM_HEADER *EfiRomHeader;
  UINTN RomOffset = (UINTN) RomHeader - (UINTN) RomImage;

  if (Pcir->CodeType == PCI_CODE_TYPE_EFI_IMAGE) {
    Type = L"EFI";
  } else if (Pcir->CodeType == PCI_CODE_TYPE_PCAT_IMAGE) {
    Type = L"BIOS";
  } else {
    Type = L"Unknown";
  }

  Print(L"+0x%x: %s image (0x%x bytes)\n", RomOffset, Type, Length);
  if (Pcir->CodeType != PCI_CODE_TYPE_EFI_IMAGE) {
    return;
  }

  EfiRomHeader = (void *) RomHeader;
  Print(L"  Machine Type: 0x%x\n", EfiRomHeader->EfiMachineType);
  Print(L"  Subsystem:    0x%x\n", EfiRomHeader->EfiSubsystem);
}

static VOID
ListROMImages (
               IN VOID   *RomImage,
               IN UINT64 RomSize
               )
{
  PCI_EXPANSION_ROM_HEADER *RomHeader;
  PCI_DATA_STRUCTURE       *RomPcir;
  UINT8                    Indicator;

  Indicator = 0;
  RomHeader = RomImage;
  if (RomHeader == NULL) {
    return;
  }

  do {
    UINTN ImageLength;

    if (RomHeader->Signature != PCI_EXPANSION_ROM_HEADER_SIGNATURE) {
      RomHeader = (PCI_EXPANSION_ROM_HEADER *) ((UINT8 *) RomHeader + 512);
      continue;
    }

    //
    // The PCI Data Structure must be DWORD aligned. 
    //
    if (RomHeader->PcirOffset == 0 ||
        (RomHeader->PcirOffset & 3) != 0 ||
        (UINT8 *) RomHeader + RomHeader->PcirOffset + sizeof (PCI_DATA_STRUCTURE) > (UINT8 *) RomImage + RomSize) {
      break;
    }

    RomPcir = (PCI_DATA_STRUCTURE *) ((UINT8 *) RomHeader + RomHeader->PcirOffset);
    if (RomPcir->Signature != PCI_DATA_STRUCTURE_SIGNATURE) {
      break;
    }

    ImageLength = RomPcir->ImageLength;
    if (RomPcir->CodeType == PCI_CODE_TYPE_PCAT_IMAGE) {
      EFI_LEGACY_EXPANSION_ROM_HEADER *Legacy = (void *) RomHeader;
      //
      // Some Legacy Cards do not report the correct
      // ImageLength so used the maximum
      // of the legacy length and the PCIR Image Length
      //
      ImageLength = MAX(ImageLength, Legacy->Size512);
    }

    DumpImage (RomImage, RomHeader, ImageLength * 512, RomPcir);

    Indicator = RomPcir->Indicator;
    RomHeader = (PCI_EXPANSION_ROM_HEADER *)
      ((UINT8 *) RomHeader + ImageLength * 512);
  } while (((UINT8 *) RomHeader < (UINT8 *) RomImage + RomSize) &&
           ((Indicator & 0x80) == 0x00));
}

static EFI_STATUS
AnalyzeROM (
            IN EFI_PCI_IO_PROTOCOL *PciIo
            )
{
  if (PciIo->RomSize == 0) {
    Print(L"No ROM\n");
    return EFI_SUCCESS;
  }

  Print(L"ROM 0x%08x bytes\n", PciIo->RomSize);
  Print(L"--------------------\n");
  ListROMImages(PciIo->RomImage, PciIo->RomSize);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UefiMain (
          IN EFI_HANDLE ImageHandle,
          IN EFI_SYSTEM_TABLE *SystemTable
          )

{
  UINTN Argc;
  CHAR16 **Argv;
  UINTN PciIndex;
  UINTN PciCount = 0;
  EFI_HANDLE *PciHandles = NULL;
  GET_OPT_CONTEXT GetOptContext;
  UINTN WantSeg;
  UINTN WantBus;
  UINTN WantDev;
  UINTN WantFunc;
  EFI_STATUS Status;
  EFI_PCI_IO_PROTOCOL *PciIo;

  Status = GetShellArgcArgv(ImageHandle, &Argc, &Argv);
  if (Status != EFI_SUCCESS || Argc < 1) {
    Print(L"This program requires Microsoft Windows.\n"
          "Just kidding...only the UEFI Shell!\n");
    return EFI_ABORTED;
  }

  INIT_GET_OPT_CONTEXT(&GetOptContext);
  while ((Status = GetOpt(Argc, Argv, L"",
                          &GetOptContext)) == EFI_SUCCESS) {
    switch (GetOptContext.Opt) {
    default:
      Print(L"Unknown option '%c'\n", GetOptContext.Opt);
      return Usage(Argv[0]);
    }
  }

  if ((Argc - GetOptContext.OptIndex) < 4) {
    return Usage(Argv[0]);
  }

  WantSeg = StrHexToUintn(Argv[GetOptContext.OptIndex + 0]);
  WantBus = StrHexToUintn(Argv[GetOptContext.OptIndex + 1]);
  WantDev = StrHexToUintn(Argv[GetOptContext.OptIndex + 2]);
  WantFunc = StrHexToUintn(Argv[GetOptContext.OptIndex + 3]);

  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiPciIoProtocolGuid,
                                   NULL, &PciCount, &PciHandles);
  if (Status != EFI_SUCCESS) {
    Print(L"No PCI devices found\n");
    return EFI_SUCCESS;
  }

  for (PciIndex = 0; PciIndex < PciCount; PciIndex++) {
    UINTN Seg;
    UINTN Bus;
    UINTN Dev;
    UINTN Func;

    Status = gBS->HandleProtocol(PciHandles[PciIndex],
                                 &gEfiPciIoProtocolGuid,
                                 (VOID *) &PciIo);

    if (Status != EFI_SUCCESS) {
      continue;
    }

    Status = PciIo->GetLocation(PciIo, &Seg, &Bus, &Dev, &Func);
    if (Status != EFI_SUCCESS) {
      continue;
    }

    if (WantSeg != Seg ||
        WantBus != Bus ||
        WantDev != Dev ||
        WantFunc != Func) {
      continue;
    }

    break;
  }

  if (PciIndex == PciCount) {
    Print(L"SBDF 0x%02x%02x%02x%02x not found\n",
          WantSeg, WantBus, WantDev, WantFunc);
    Status = EFI_NOT_FOUND;
  } else {
    Status = AnalyzeROM(PciIo);
  }

  gBS->FreePool(PciHandles);
  return Status;
}
