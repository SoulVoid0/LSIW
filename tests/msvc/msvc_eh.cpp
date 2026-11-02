// msvc_eh: an MSVC-ABI C++ program built with clang-cl + lld-link and no
// Microsoft SDK or CRT, to exercise LSIW's own vcruntime140.dll:
// _CxxThrowException, __CxxFrameHandler3 (catch matching, destructors run
// while unwinding, rethrow, nested try blocks), __RTtypeid, __RTDynamicCast
// and __std_type_info_name.
//
// Everything a normal program gets from headers is declared here by hand:
// a minimal type_info with the vcruntime layout, operator new/delete over
// malloc/free, a tiny printf over WriteFile, and the entry point
// mainCRTStartup, which runs the static constructors itself (.CRT$XCA ..
// .CRT$XCZ) and calls ExitProcess. Import libraries come from the .def
// files next to this file (see tests/Makefile, target msvc).
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <stdarg.h>   // clang's own header, not the SDK's

typedef unsigned long DWORD;
typedef void *HANDLE;
typedef unsigned long long size_t_;

extern "C" {
__declspec(dllimport) void __stdcall ExitProcess(unsigned int);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) void *__cdecl malloc(size_t_);
__declspec(dllimport) void __cdecl free(void *);
}

// ---- the vcruntime type_info ----
struct __std_type_info_data {
    const char *_UndecoratedName;
    const char _DecoratedName[1];
};
struct __type_info_node;
extern "C" __declspec(dllimport) __type_info_node __type_info_root_node;
extern "C" __declspec(dllimport) const char *__cdecl __std_type_info_name(__std_type_info_data *, __type_info_node *);
extern "C" __declspec(dllimport) int __cdecl __std_type_info_compare(const __std_type_info_data *, const __std_type_info_data *);

class type_info {
public:
    virtual ~type_info();   // ??1type_info@@UEAA@XZ in vcruntime140.dll
    const char *name() const { return __std_type_info_name(&_Data, &__type_info_root_node); }
    const char *raw_name() const { return _Data._DecoratedName; }
    bool operator==(const type_info &o) const { return __std_type_info_compare(&_Data, &o._Data) == 0; }
    bool operator!=(const type_info &o) const { return !(*this == o); }
private:
    mutable __std_type_info_data _Data;
};
namespace std { using ::type_info; }

// Every RTTI type descriptor points at type_info's vftable by absolute
// address, so it cannot come through an import. The MSVC CRT links a static
// copy; here a local stand-in is enough because no type_info object is ever
// destroyed through it.
extern "C" const void *lsiw_type_info_vftable[2] = { nullptr, nullptr };
#pragma comment(linker, "/alternatename:??_7type_info@@6B@=lsiw_type_info_vftable")

// Referenced by the compiler whenever floating point is used.
extern "C" int _fltused = 0;

// ---- allocation ----
void *operator new(size_t_ n) { return malloc(n ? n : 1); }
void *operator new[](size_t_ n) { return malloc(n ? n : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t_) noexcept { free(p); }
void operator delete[](void *p, size_t_) noexcept { free(p); }

// ---- output ----
static void out(const char *s, int n = -1)
{
    if (n < 0) { n = 0; while (s[n]) n++; }
    DWORD w;
    WriteFile(GetStdHandle((DWORD)-11), s, (DWORD)n, &w, nullptr);
}

static void tprintf(const char *fmt, ...)
{
    char buf[512];
    int k = 0;
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p && k < 480; p++) {
        if (*p != '%') { buf[k++] = *p; continue; }
        p++;
        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && k < 480) buf[k++] = *s++;
        } else if (*p == 'd' || *p == 'x') {
            long long v = va_arg(ap, int);
            unsigned long long u;
            int base = *p == 'd' ? 10 : 16;
            if (base == 10 && v < 0) { buf[k++] = '-'; u = (unsigned long long)-v; }
            else u = (unsigned)v;
            char t[24];
            int m = 0;
            do { t[m++] = "0123456789abcdef"[u % base]; u /= base; } while (u);
            while (m) buf[k++] = t[--m];
        } else {
            buf[k++] = *p;
        }
    }
    va_end(ap);
    out(buf, k);
}

static int g_failures;
#define CHECK(cond, name)                                                       \
    do {                                                                        \
        if (cond) tprintf("ok - %s\n", name);                                   \
        else { g_failures++; tprintf("FAIL - %s: %s\n", name, #cond); }         \
    } while (0)

static bool streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// ---- static constructors (no CRT: walked by mainCRTStartup) ----
typedef void (__cdecl *PVFV)();
#pragma section(".CRT$XCA", read)
#pragma section(".CRT$XCZ", read)
__declspec(allocate(".CRT$XCA")) static PVFV xc_a = nullptr;
__declspec(allocate(".CRT$XCZ")) static PVFV xc_z = nullptr;

struct StaticInit {
    int value;
    StaticInit() : value(1234) {}
};
static StaticInit g_static;

// ---- exception types with destructors ----
static char g_log[128];
static int g_logn;
static void logc(char c) { if (g_logn < 126) { g_log[g_logn++] = c; g_log[g_logn] = 0; } }
static void logreset() { g_logn = 0; g_log[0] = 0; }

struct Guard {
    char id;
    explicit Guard(char c) : id(c) { logc('+'); logc(c); }
    ~Guard() { logc('-'); logc(id); }
};

struct Base {
    int code;
    explicit Base(int c) : code(c) {}
    Base(const Base &o) : code(o.code) {}
    virtual ~Base() {}
    virtual const char *what() const { return "base"; }
};
struct Derived : Base {
    explicit Derived(int c) : Base(c) {}
    const char *what() const override { return "derived"; }
};
struct Other { int x; };

static int g_live;   // live Counted objects: exception copies must be destroyed
struct Counted {
    int v;
    explicit Counted(int x) : v(x) { g_live++; }
    Counted(const Counted &o) : v(o.v) { g_live++; }
    ~Counted() { g_live--; }
};

__declspec(noinline) static void f5() { Guard g('5'); throw Derived(5); }
__declspec(noinline) static void f4() { Guard g('4'); f5(); }
__declspec(noinline) static void f3() { Guard g('3'); f4(); }
__declspec(noinline) static void f2() { Guard g('2'); f3(); }
__declspec(noinline) static void f1() { Guard g('1'); f2(); }

static void test_eh()
{
    int got = 0;
    try { throw 42; } catch (int v) { got = v; }
    CHECK(got == 42, "throw/catch int");

    const char *w = nullptr;
    try { throw Derived(7); } catch (const Base &b) { w = b.what(); got = b.code; }
    CHECK(w && streq(w, "derived") && got == 7, "catch by reference to base, virtual what()");

    bool all = false;
    try { throw Other{ 3 }; } catch (int) { } catch (...) { all = true; }
    CHECK(all, "catch(...) after a non-matching handler");

    logreset();
    try {
        Guard a('a');
        {
            Guard b('b');
            throw Counted(1);
        }
    } catch (const Counted &c) {
        logc(c.v == 1 ? '!' : '?');
    }
    CHECK(streq(g_log, "+a+b-b-a!"), "destructors run during unwinding, in order");
    CHECK(g_live == 0, "exception object destroyed after the catch");

    int stage = 0;
    try {
        try { throw 1; }
        catch (int) {
            stage = 1;
            try { throw 2.5; } catch (double d) { if (d == 2.5) stage = 2; }
        }
        stage = stage == 2 ? 3 : -1;
    } catch (...) { stage = -2; }
    CHECK(stage == 3, "nested try inside a catch block");

    got = 0;
    try {
        try { throw Derived(9); } catch (Base &) { throw; }
    } catch (Derived &d) { got = d.code; }
    CHECK(got == 9, "rethrow with throw; keeps the dynamic type");

    got = 0;
    try {
        try { throw 1; }
        catch (int) { throw Derived(11); }
    } catch (const Base &b) { got = b.code; }
    CHECK(got == 11, "exception thrown inside a catch block caught by an outer try");

    logreset();
    got = 0;
    try { f1(); } catch (const Base &b) { got = b.code; logc('!'); }
    CHECK(got == 5 && streq(g_log, "+1+2+3+4+5-5-4-3-2-1!"), "exception across 5 frames, destructor order");

    got = 0;
    try { throw Counted(3); } catch (Counted c) { got = c.v; }
    CHECK(got == 3 && g_live == 0, "catch by value copies and destroys");

    int n = 0;
    for (int i = 0; i < 200; i++) { try { throw i; } catch (int v) { n += v == i; } }
    CHECK(n == 200, "200 throws in a row");

    Derived *pd = nullptr;
    try { throw new Derived(13); } catch (Base *pb) { pd = static_cast<Derived *>(pb); }
    CHECK(pd && pd->code == 13, "throw a pointer, catch a base pointer");
    delete pd;
}

// ---- RTTI ----
struct Animal { virtual ~Animal() {} virtual int legs() const { return 0; } };
struct Dog : Animal { int legs() const override { return 4; } };
struct Puppy : Dog { };
struct Bird : Animal { int legs() const override { return 2; } };
struct Pet { virtual ~Pet() {} int tag = 77; };
struct Cat : Animal, Pet { int legs() const override { return 4; } };

static void test_rtti()
{
    Animal *a = new Puppy;
    Animal *b = new Bird;
    Animal *c = new Cat;
    CHECK(typeid(*a) == typeid(Puppy) && typeid(*a) != typeid(Dog), "typeid of the dynamic type");
    const char *nm = typeid(*a).name();
    CHECK(nm && streq(nm, "struct Puppy"), "type_info::name() via __std_type_info_name");
    CHECK(streq(typeid(*b).raw_name(), ".?AUBird@@"), "type_info raw (decorated) name");
    CHECK(streq(typeid(int).name(), "int"), "typeid(int).name()");
    CHECK(dynamic_cast<Dog *>(a) != nullptr, "dynamic_cast pointer to an intermediate base");
    CHECK(dynamic_cast<Puppy *>(a) == static_cast<Puppy *>(a), "dynamic_cast pointer to the exact type");
    CHECK(dynamic_cast<Dog *>(b) == nullptr, "failed dynamic_cast pointer is null");
    Pet *p = dynamic_cast<Pet *>(c);
    CHECK(p && p->tag == 77, "dynamic_cast across to a second base (cross cast)");
    CHECK(dynamic_cast<Animal *>(p) == c, "dynamic_cast back from the second base");
    void *whole = dynamic_cast<void *>(p);
    CHECK(whole == static_cast<void *>(static_cast<Cat *>(c)), "dynamic_cast<void *> finds the complete object");
    Dog &dr = dynamic_cast<Dog &>(*a);
    CHECK(dr.legs() == 4, "dynamic_cast reference succeeds");
    bool thrown = false;
    try { (void)dynamic_cast<Dog &>(*b); } catch (...) { thrown = true; }
    CHECK(thrown, "failed reference dynamic_cast throws (std::bad_cast)");
    bool bad_typeid = false;
    Animal *null_animal = nullptr;
    try { (void)typeid(*null_animal); } catch (...) { bad_typeid = true; }
    CHECK(bad_typeid, "typeid(*nullptr) throws (std::bad_typeid)");
    delete a;
    delete b;
    delete c;
}

static int cpp_main()
{
    out("# msvc_eh (clang-cl, MSVC C++ ABI)\n");
    CHECK(g_static.value == 1234, "static constructor ran");
    test_eh();
    test_rtti();
    if (g_failures) tprintf("RESULT: FAIL (%d failures)\n", g_failures);
    else out("RESULT: PASS\n");
    return g_failures ? 1 : 0;
}

extern "C" void mainCRTStartup()
{
    for (PVFV *f = &xc_a + 1; f < &xc_z; f++)
        if (*f) (*f)();
    ExitProcess((unsigned)cpp_main());
}
