/* Windows x64 exception-handling structures shared by seh.c (the
 * dispatcher and unwinder) and vcruntime.c (the C++ exception runtime).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef LSIW_SEH_H
#define LSIW_SEH_H

#include "native.h"
#include "pe.h"

typedef struct __attribute__((aligned(16))) {
    uint64_t P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;   /* 0x000 */
    uint32_t ContextFlags;                                     /* 0x030 */
    uint32_t MxCsr;                                            /* 0x034 */
    uint16_t SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;         /* 0x038 */
    uint32_t EFlags;                                           /* 0x044 */
    uint64_t Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;                     /* 0x048 */
    uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;           /* 0x078 */
    uint64_t R8, R9, R10, R11, R12, R13, R14, R15;             /* 0x0B8 */
    uint64_t Rip;                                              /* 0x0F8 */
    uint8_t  FltSave[512];      /* XMM_SAVE_AREA32; Xmm0 at 0x1A0 */
    uint64_t VectorRegister[52];                               /* 0x300 */
    uint64_t VectorControl;                                    /* 0x4A0 */
    uint64_t DebugControl, LastBranchToRip, LastBranchFromRip, LastExceptionToRip, LastExceptionFromRip;
} CONTEXT;                                                     /* 0x4D0 */

_Static_assert(sizeof(CONTEXT) == 0x4D0, "CONTEXT size");
_Static_assert(offsetof(CONTEXT, Rip) == 0xF8, "CONTEXT.Rip");

#define CONTEXT_FULL 0x10000B
#define XMM(c, n) ((c)->FltSave + 0xA0 + (n) * 16)

#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    void *ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD;

struct exc_pointers { EXCEPTION_RECORD *rec; CONTEXT *ctx; };

#define EXCEPTION_NONCONTINUABLE  0x01
#define EXCEPTION_UNWINDING       0x02
#define EXCEPTION_EXIT_UNWIND     0x04
#define EXCEPTION_NESTED_CALL     0x10
#define EXCEPTION_TARGET_UNWIND   0x20
#define EXCEPTION_COLLIDED_UNWIND 0x40

#define STATUS_ACCESS_VIOLATION         0xC0000005
#define STATUS_ILLEGAL_INSTRUCTION      0xC000001D
#define STATUS_NONCONTINUABLE_EXCEPTION 0xC0000025
#define STATUS_UNWIND                   0xC0000027
#define STATUS_INTEGER_DIVIDE_BY_ZERO   0xC0000094
#define STATUS_INTEGER_OVERFLOW         0xC0000095
#define STATUS_FLOAT_DIVIDE_BY_ZERO     0xC000008E
#define STATUS_FLOAT_INVALID_OPERATION  0xC0000090
#define STATUS_FLOAT_OVERFLOW           0xC0000091
#define STATUS_FLOAT_UNDERFLOW          0xC0000093
#define STATUS_FLOAT_INEXACT_RESULT     0xC000008F
#define STATUS_STACK_OVERFLOW           0xC00000FD
#define STATUS_DATATYPE_MISALIGNMENT    0x80000002
#define STATUS_UNWIND_CONSOLIDATE       0x80000029
#define STATUS_STACK_BUFFER_OVERRUN     0xC0000409

typedef enum { ExceptionContinueExecution, ExceptionContinueSearch,
               ExceptionNestedException, ExceptionCollidedUnwind } EXCEPTION_DISPOSITION;

typedef struct {
    ULONG_PTR ControlPc;
    ULONG_PTR ImageBase;
    RUNTIME_FUNCTION *FunctionEntry;
    ULONG_PTR EstablisherFrame;
    ULONG_PTR TargetIp;
    CONTEXT *ContextRecord;
    void *LanguageHandler;
    void *HandlerData;
    void *HistoryTable;
    DWORD ScopeIndex;
    DWORD Fill0;
} DISPATCHER_CONTEXT;

typedef EXCEPTION_DISPOSITION (WINAPI *exc_handler_fn)(EXCEPTION_RECORD *, void *, CONTEXT *, DISPATCHER_CONTEXT *);

#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH    0
#define EXCEPTION_EXECUTE_HANDLER    1

#define CXX_EXCEPTION_CODE 0xE06D7363u
#define STATUS_LONGJUMP    0x80000026u

/* Assembly entry points (seh.c). Each captures the Windows caller's
 * context before doing its work, so they must be reached from Windows
 * code (or jumped to with the Windows return address on top of the
 * stack, never called from LSIW C code that should appear in the walk). */
extern void WINAPI lsiw_RtlCaptureContext(CONTEXT *);
extern void WINAPI lsiw_RaiseException(DWORD, DWORD, DWORD, const ULONG_PTR *);
extern void WINAPI lsiw_RtlRaiseException(EXCEPTION_RECORD *);
extern void WINAPI lsiw_RtlUnwindEx(void *, void *, EXCEPTION_RECORD *, void *, CONTEXT *, void *);
extern void WINAPI lsiw_RtlUnwind(void *, void *, EXCEPTION_RECORD *, void *);
extern __attribute__((noreturn)) void lsiw_restore_context(CONTEXT *);

RUNTIME_FUNCTION *lsiw_lookup_function_entry(ULONG_PTR pc, ULONG_PTR *image_base);

/* A consolidation callback (a C++ catch block runner) may attach a hook to
 * the stretch of host stack it runs in. The hook is called when a stack
 * walk crosses that region: unwinding = 0 during a handler search (a new
 * exception propagating out of the catch block), 1 when an unwind passes
 * through it (the catch block is being abandoned). */
typedef void (*lsiw_region_hook)(void *arg, EXCEPTION_RECORD *rec, int unwinding);
void lsiw_seh_set_consolidate_hook(lsiw_region_hook fn, void *arg);

#endif
