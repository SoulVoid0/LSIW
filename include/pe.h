/* Minimal PE/PE32+ structure definitions (x86-64 focus).
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef LSIW_PE_H
#define LSIW_PE_H
#include <stdint.h>

#define IMAGE_DOS_SIGNATURE     0x5A4D      /* "MZ" */
#define IMAGE_NT_SIGNATURE      0x00004550  /* "PE\0\0" */
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_MACHINE_I386  0x014c
#define IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x10b
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20b

#define IMAGE_DIRECTORY_ENTRY_EXPORT    0
#define IMAGE_DIRECTORY_ENTRY_IMPORT    1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE  2
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION 3
#define IMAGE_DIRECTORY_ENTRY_BASERELOC 5
#define IMAGE_DIRECTORY_ENTRY_TLS       9
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT 13
#define IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR 14
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16

#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_MEM_READ    0x40000000
#define IMAGE_SCN_MEM_WRITE   0x80000000

#define IMAGE_REL_BASED_ABSOLUTE 0
#define IMAGE_REL_BASED_DIR64    10
#define IMAGE_REL_BASED_HIGHLOW  3

#define IMAGE_ORDINAL_FLAG64 0x8000000000000000ULL
#define IMAGE_SUBSYSTEM_WINDOWS_GUI 2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI 3
#define IMAGE_FILE_DLL 0x2000

#pragma pack(push,1)
typedef struct {
    uint16_t e_magic; uint16_t e_cblp; uint16_t e_cp; uint16_t e_crlc;
    uint16_t e_cparhdr; uint16_t e_minalloc; uint16_t e_maxalloc; uint16_t e_ss;
    uint16_t e_sp; uint16_t e_csum; uint16_t e_ip; uint16_t e_cs;
    uint16_t e_lfarlc; uint16_t e_ovno; uint16_t e_res[4]; uint16_t e_oemid;
    uint16_t e_oeminfo; uint16_t e_res2[10]; int32_t e_lfanew;
} IMAGE_DOS_HEADER;

typedef struct {
    uint16_t Machine; uint16_t NumberOfSections; uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable; uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader; uint16_t Characteristics;
} IMAGE_FILE_HEADER;

typedef struct { uint32_t VirtualAddress; uint32_t Size; } IMAGE_DATA_DIRECTORY;

typedef struct {
    uint16_t Magic; uint8_t MajorLinkerVersion; uint8_t MinorLinkerVersion;
    uint32_t SizeOfCode; uint32_t SizeOfInitializedData; uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint; uint32_t BaseOfCode;
    uint64_t ImageBase; uint32_t SectionAlignment; uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion; uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion; uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion; uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue; uint32_t SizeOfImage; uint32_t SizeOfHeaders;
    uint32_t CheckSum; uint16_t Subsystem; uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve; uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve; uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags; uint32_t NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER64;

typedef struct {
    uint32_t Signature; IMAGE_FILE_HEADER FileHeader;
    IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64;

typedef struct {
    uint8_t  Name[8]; uint32_t VirtualSize; uint32_t VirtualAddress;
    uint32_t SizeOfRawData; uint32_t PointerToRawData;
    uint32_t PointerToRelocations; uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations; uint16_t NumberOfLinenumbers; uint32_t Characteristics;
} IMAGE_SECTION_HEADER;

typedef struct {
    uint32_t OriginalFirstThunk; uint32_t TimeDateStamp; uint32_t ForwarderChain;
    uint32_t Name; uint32_t FirstThunk;
} IMAGE_IMPORT_DESCRIPTOR;

typedef struct { uint16_t Hint; uint8_t Name[1]; } IMAGE_IMPORT_BY_NAME;

typedef struct {
    uint32_t Characteristics; uint32_t TimeDateStamp; uint16_t MajorVersion;
    uint16_t MinorVersion; uint32_t Name; uint32_t Base; uint32_t NumberOfFunctions;
    uint32_t NumberOfNames; uint32_t AddressOfFunctions; uint32_t AddressOfNames;
    uint32_t AddressOfNameOrdinals;
} IMAGE_EXPORT_DIRECTORY;

typedef struct { uint32_t VirtualAddress; uint32_t SizeOfBlock; } IMAGE_BASE_RELOCATION;
typedef struct {
    uint64_t StartAddressOfRawData; uint64_t EndAddressOfRawData;
    uint64_t AddressOfIndex; uint64_t AddressOfCallBacks;
    uint32_t SizeOfZeroFill; uint32_t Characteristics;
} IMAGE_TLS_DIRECTORY64;

typedef struct {
    uint32_t Attributes; uint32_t DllNameRVA; uint32_t ModuleHandleRVA;
    uint32_t ImportAddressTableRVA; uint32_t ImportNameTableRVA;
    uint32_t BoundImportAddressTableRVA; uint32_t UnloadInformationTableRVA;
    uint32_t TimeDateStamp;
} IMAGE_DELAYLOAD_DESCRIPTOR;

typedef struct {
    uint32_t Characteristics; uint32_t TimeDateStamp; uint16_t MajorVersion;
    uint16_t MinorVersion; uint16_t NumberOfNamedEntries; uint16_t NumberOfIdEntries;
} IMAGE_RESOURCE_DIRECTORY;

typedef struct { uint32_t Name; uint32_t OffsetToData; } IMAGE_RESOURCE_DIRECTORY_ENTRY;

typedef struct {
    uint32_t OffsetToData; uint32_t Size; uint32_t CodePage; uint32_t Reserved;
} IMAGE_RESOURCE_DATA_ENTRY;

typedef struct { uint32_t BeginAddress; uint32_t EndAddress; uint32_t UnwindData; } RUNTIME_FUNCTION;
#pragma pack(pop)

#endif /* LSIW_PE_H */
