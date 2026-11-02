/* t_seh_try: compiler-level SEH (__try / __except / __finally), built with
 * clang --target=x86_64-w64-mingw32 (GCC does not support __try).
 *
 * clang emits .pdata/.xdata unwind tables with __C_specific_handler as the
 * language handler, so this exercises LSIW's exception dispatch
 * (RtlDispatchException), __C_specific_handler and RtlUnwindEx: hardware
 * faults caught by __except, filters returning EXCEPTION_CONTINUE_SEARCH to
 * an outer handler, GetExceptionCode() / GetExceptionInformation() in the
 * filter, and __finally blocks run during unwinding and on normal exit.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define __USE_MINGW_ANSI_STDIO 0
#include "lsiw_test.h"

static char g_trace[64];
static void trace(const char *s) { strcat(g_trace, s); }

static int filter_record(DWORD code, EXCEPTION_POINTERS *ep, DWORD *out, ULONG_PTR *info0)
{
    *out = code;
    *info0 = ep->ExceptionRecord->NumberParameters ? ep->ExceptionRecord->ExceptionInformation[0] : 99;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void test_av(void)
{
    volatile int caught = 0;
    DWORD code = 0;
    ULONG_PTR info0 = 0;
    __try {
        *(volatile int *)0 = 1;
        caught = -1;
    } __except (filter_record(GetExceptionCode(), GetExceptionInformation(), &code, &info0)) {
        caught = 1;
    }
    CHECK(caught == 1, "__try/__except catches a write to NULL");
    CHECKF(code == EXCEPTION_ACCESS_VIOLATION && info0 == 1, "GetExceptionCode/Information in the filter",
           "code=%lx info0=%llu", (unsigned long)code, (unsigned long long)info0);

    caught = 0;
    __try {
        volatile int z = 0;
        caught = 10 / z;
    } __except (GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO ? EXCEPTION_EXECUTE_HANDLER
                                                                   : EXCEPTION_CONTINUE_SEARCH) {
        caught = 2;
    }
    CHECK(caught == 2, "__except catches integer division by zero");
}

static int inner_filter(DWORD code)
{
    trace("f");
    return code == 0xE0000001 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static __attribute__((noinline)) void raise_level(int depth)
{
    __try {
        if (depth == 0) RaiseException(0xE0000002, 0, 0, NULL);
        else raise_level(depth - 1);
    } __finally {
        trace(AbnormalTermination() ? "u" : "n");
    }
}

static void test_nested(void)
{
    g_trace[0] = 0;
    volatile int where = 0;
    __try {
        __try {
            raise_level(2);
        } __except (inner_filter(GetExceptionCode())) {
            where = 1;
        }
    } __except (GetExceptionCode() == 0xE0000002 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        trace("o");
        where = 2;
    }
    CHECK(where == 2, "inner filter returning EXCEPTION_CONTINUE_SEARCH reaches the outer handler");
    /* Filters run first (search phase), then the __finally blocks during the
     * unwind (innermost first), then the handler. */
    CHECKF(strcmp(g_trace, "fuuuo") == 0, "filters, then __finally on unwind, then handler",
           "trace=%s", g_trace);

    g_trace[0] = 0;
    __try {
        trace("b");
    } __finally {
        trace(AbnormalTermination() ? "U" : "N");
    }
    CHECK(strcmp(g_trace, "bN") == 0, "__finally on normal exit");

    g_trace[0] = 0;
    for (int i = 0; i < 3; i++) {
        __try {
            if (i == 1) __leave;
            trace("x");
        } __finally {
            trace("F");
        }
    }
    CHECKF(strcmp(g_trace, "xFFxF") == 0, "__leave runs __finally", "trace=%s", g_trace);
}

static void test_continue(void)
{
    volatile int resumed = 0;
    __try {
        RaiseException(0xE0000003, 0, 0, NULL);   /* continuable */
        resumed = 1;
    } __except (EXCEPTION_CONTINUE_EXECUTION) {
        resumed = -1;
    }
    CHECK(resumed == 1, "filter returning EXCEPTION_CONTINUE_EXECUTION resumes after RaiseException");

}

static void test_repeat(void)
{
    int n = 0;
    for (int i = 0; i < 100; i++) {
        __try {
            *(volatile int *)(ULONG_PTR)(16 + i) = i;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            n++;
        }
    }
    CHECKF(n == 100, "100 access violations in a row are all caught", "n=%d", n);
}

int main(void)
{
    t_init("t_seh_try");
    test_av();
    test_nested();
    test_continue();
    test_repeat();
    return t_done();
}
