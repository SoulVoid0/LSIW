/* t_seh: structured exceptions without compiler __try support (GCC).
 *
 * Vectored handlers for RaiseException (continuable), ud2 and int3 (the
 * handler moves Rip past the instruction), integer division by zero (the
 * handler records the code and jumps out with __builtin_longjmp), and an
 * access violation handled by SetUnhandledExceptionFilter in a child run
 * of this same program ("t_seh.exe child-av"), whose exit code must be the
 * exception code. Also checks the unwind-table functions used by unwinders.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define __USE_MINGW_ANSI_STDIO 0
#include "lsiw_test.h"
#include <setjmp.h>

#define MY_CODE 0xE0001234u

static volatile LONG g_calls_first, g_calls_second;
static volatile ULONG_PTR g_info0;
static volatile DWORD g_last_code;
static volatile ULONG_PTR g_last_addr;
static void *g_jmp[5];
static volatile int g_mode;   /* 0 idle, 1 custom, 2 int3, 3 ud2, 4 div */

static LONG CALLBACK first_handler(EXCEPTION_POINTERS *ep)
{
    if (g_mode == 0) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_calls_first);
    return EXCEPTION_CONTINUE_SEARCH;           /* let the next handler decide */
    (void)ep;
}

static LONG CALLBACK main_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (g_mode == 0) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_calls_second);
    g_last_code = er->ExceptionCode;
    g_last_addr = (ULONG_PTR)er->ExceptionAddress;
    switch (g_mode) {
    case 1:
        if (er->ExceptionCode == MY_CODE && er->NumberParameters == 2) {
            g_info0 = er->ExceptionInformation[0];
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        break;
    case 2:
        if (er->ExceptionCode == EXCEPTION_BREAKPOINT) {
            ep->ContextRecord->Rip += 1;            /* Rip points at the int3 */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        break;
    case 3:
        if (er->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
            ep->ContextRecord->Rip += 2;            /* skip the ud2 */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        break;
    case 4:
        if (er->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
            g_mode = 0;
            __builtin_longjmp(g_jmp, 1);
        }
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static __attribute__((noinline)) int divide(volatile int a, volatile int b) { return a / b; }

static void test_vectored(void)
{
    PVOID h2 = AddVectoredExceptionHandler(0, main_handler);
    PVOID h1 = AddVectoredExceptionHandler(1, first_handler);   /* first in the list */
    CHECK(h1 && h2, "AddVectoredExceptionHandler");

    ULONG_PTR args[2] = { 0xabcdef, 2 };
    g_mode = 1;
    RaiseException(MY_CODE, 0, 2, args);
    g_mode = 0;
    CHECK(g_calls_second == 1 && g_info0 == 0xabcdef, "RaiseException continued by a vectored handler");
    CHECK(g_calls_first == 1, "handler added with First=1 is called first and can pass it on");
    CHECK(g_last_code == MY_CODE, "ExceptionCode of RaiseException");

    g_mode = 3;
    __asm__ volatile ("ud2");
    g_mode = 0;
    CHECKF(g_last_code == EXCEPTION_ILLEGAL_INSTRUCTION && g_calls_second == 2,
           "ud2 -> EXCEPTION_ILLEGAL_INSTRUCTION, continued", "code=%lx", (unsigned long)g_last_code);

    volatile int result = -1;
    if (__builtin_setjmp(g_jmp) == 0) {
        g_mode = 4;
        result = divide(10, 0);
        g_mode = 0;
        CHECK(0, "division by zero raised an exception");
    } else {
        CHECKF(g_last_code == EXCEPTION_INT_DIVIDE_BY_ZERO, "integer divide by zero -> EXCEPTION_INT_DIVIDE_BY_ZERO",
               "code=%lx", (unsigned long)g_last_code);
        CHECK(result == -1, "jumped out of the handler");
    }
    g_mode = 0;

    CHECK(RemoveVectoredExceptionHandler(h1) && RemoveVectoredExceptionHandler(h2), "RemoveVectoredExceptionHandler");
    /* After removal a second fault must still be delivered (signal state was restored). */
    PVOID h3 = AddVectoredExceptionHandler(1, main_handler);
    g_mode = 3;
    __asm__ volatile ("ud2");
    g_mode = 0;
    CHECK(g_calls_second == 4, "exceptions keep working after jumping out of a handler");
    RemoveVectoredExceptionHandler(h3);
}

/* Last, because an engine that does not turn SIGTRAP into an exception
 * kills the program here. */
static void test_breakpoint(void)
{
    PVOID h = AddVectoredExceptionHandler(1, main_handler);
    LONG before = g_calls_second;
    g_mode = 2;
    __asm__ volatile ("int3");
    g_mode = 0;
    CHECKF(g_last_code == EXCEPTION_BREAKPOINT && g_calls_second == before + 1,
           "int3 -> EXCEPTION_BREAKPOINT, continued", "code=%lx", (unsigned long)g_last_code);
    RemoveVectoredExceptionHandler(h);
}

/* The C runtime's longjmp unwinds through the SEH frames in between. */
static jmp_buf g_crt_jmp;
static volatile int g_depth;
static __attribute__((noinline)) void deep(int n)
{
    g_depth = n;
    if (n <= 0) longjmp(g_crt_jmp, 7);
    if (n < 100) deep(n - 1);
    __asm__ volatile ("");
}

static void test_unwind_api(void)
{
    volatile int r = setjmp(g_crt_jmp);
    if (r == 0) deep(5);
    CHECK(r == 7, "msvcrt longjmp across frames");

    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)(ULONG_PTR)&deep, &base, NULL);
    CHECK(rf != NULL && base == (DWORD64)(ULONG_PTR)GetModuleHandleA(NULL), "RtlLookupFunctionEntry finds .pdata");
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    CHECK(ctx.Rsp != 0 && ctx.Rip != 0, "RtlCaptureContext");
    PVOID frames[16];
    USHORT n = RtlCaptureStackBackTrace(0, 16, frames, NULL);
    CHECKF(n >= 2, "RtlCaptureStackBackTrace walks the stack", "n=%u", n);
}

/* ---- child: access violation reaching the unhandled-exception filter ---- */

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    printf("child: unhandled code=%08lx write=%lu addr=%llx\n", (unsigned long)er->ExceptionCode,
           (unsigned long)er->ExceptionInformation[0], (unsigned long long)er->ExceptionInformation[1]);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;           /* terminate with the exception code */
}

static int child_av(void)
{
    SetUnhandledExceptionFilter(unhandled_filter);
    printf("child: writing to NULL\n");
    fflush(stdout);
    *(volatile int *)0 = 1;
    printf("child: still alive (wrong)\n");
    return 3;
}

static void test_child_av(const char *self)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    CHECK(CreatePipe(&rd, &wr, &sa, 0), "CreatePipe");
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "\"%s\" child-av", self);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    CHECK(ok, "CreateProcessA child-av");
    if (!ok) { CloseHandle(rd); return; }
    char out[1024];
    DWORD total = 0, n;
    while (total < sizeof out - 1 && ReadFile(rd, out + total, sizeof out - 1 - total, &n, NULL) && n) total += n;
    out[total] = 0;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CHECKF(strstr(out, "child: unhandled code=c0000005 write=1 addr=0") != NULL,
           "access violation reaches SetUnhandledExceptionFilter with write flag and address", "output: %s", out);
    CHECK(strstr(out, "still alive") == NULL, "faulting code does not continue");
    CHECKF(code == 0xC0000005, "process exit code is the exception code", "code=%lx", (unsigned long)code);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child-av")) return child_av();
    t_init("t_seh");
    test_vectored();
    test_unwind_api();
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);
    test_child_av(self);
    test_breakpoint();
    return t_done();
}
