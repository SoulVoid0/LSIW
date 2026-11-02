// t_cpp: C++ built with MinGW g++ (libstdc++, libgcc's SEH-based unwinder
// running on top of LSIW's RtlUnwindEx / RtlVirtualUnwind).
//
// Exceptions (throw/catch, base-class catch, nested, rethrow, exception_ptr,
// destructor order across frames), the standard containers and algorithms,
// iostreams, threads with mutex / condition_variable / atomic, chrono,
// RTTI, std::filesystem and virtual functions.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "lsiw_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

static std::string g_log;

struct Guard {
    char id;
    explicit Guard(char c) : id(c) { g_log += '+'; g_log += c; }
    ~Guard() { g_log += '-'; g_log += id; }
};

struct MyError : std::runtime_error {
    int code;
    MyError(const char *w, int c) : std::runtime_error(w), code(c) {}
};

[[gnu::noinline]] static void frame5() { Guard g('5'); throw MyError("deep", 5); }
[[gnu::noinline]] static void frame4() { Guard g('4'); frame5(); }
[[gnu::noinline]] static void frame3() { Guard g('3'); frame4(); }
[[gnu::noinline]] static void frame2() { Guard g('2'); frame3(); }
[[gnu::noinline]] static void frame1() { Guard g('1'); frame2(); }

static void test_exceptions()
{
    int got = 0;
    try { throw 42; } catch (int v) { got = v; }
    CHECK(got == 42, "throw/catch int");

    std::string what;
    try { throw std::runtime_error("boom"); } catch (const std::runtime_error &e) { what = e.what(); }
    CHECK(what == "boom", "std::runtime_error what()");

    what.clear();
    try { throw std::out_of_range("range"); } catch (const std::exception &e) { what = e.what(); }
    CHECK(what == "range", "catch by base class reference");

    int stage = 0;
    try {
        try { throw 1; }
        catch (int) {
            stage = 1;
            try { throw std::string("inner"); } catch (const std::string &s) { if (s == "inner") stage = 2; }
            throw 3.5;
        }
    } catch (double d) {
        if (stage == 2 && d == 3.5) stage = 3;
    }
    CHECK(stage == 3, "nested try in catch, new exception from a catch block");

    int rethrown = 0;
    try {
        try { throw MyError("re", 7); } catch (...) { throw; }
    } catch (const MyError &e) { rethrown = e.code; }
    CHECK(rethrown == 7, "rethrow with throw;");

    g_log.clear();
    int code = 0;
    try { frame1(); } catch (const MyError &e) { code = e.code; g_log += '!'; }
    CHECKF(code == 5 && g_log == "+1+2+3+4+5-5-4-3-2-1!", "exception across 5 frames, destructors in order",
           "log=%s", g_log.c_str());

    std::exception_ptr ep;
    try { throw std::logic_error("saved"); } catch (...) { ep = std::current_exception(); }
    what.clear();
    try { std::rethrow_exception(ep); } catch (const std::logic_error &e) { what = e.what(); }
    CHECK(what == "saved", "exception_ptr + rethrow_exception");

    bool any = false;
    try { throw Guard('x'); } catch (...) { any = true; }
    CHECK(any, "catch(...)");

    bool bad = false;
    try { std::vector<int> v(3); (void)v.at(10); } catch (const std::out_of_range &) { bad = true; }
    CHECK(bad, "vector::at throws out_of_range");

    bool alloc = false;
    try { auto p = std::make_unique<char[]>(size_t(1) << 62); (void)p; } catch (const std::bad_alloc &) { alloc = true; }
    CHECK(alloc, "huge new throws bad_alloc");

    // Many throws in a row (unwinder state must not leak).
    int n = 0;
    for (int i = 0; i < 1000; i++) { try { throw i; } catch (int v) { n += v == i; } }
    CHECK(n == 1000, "1000 throws");

    bool noexcept_ok = std::uncaught_exceptions() == 0;
    CHECK(noexcept_ok, "uncaught_exceptions() == 0 outside handlers");
}

static void test_containers()
{
    std::string s = "Hello";
    s += ", world";
    CHECK(s.size() == 12 && s.substr(7) == "world" && s.find("lo") == 3, "std::string");
    std::vector<int> v = { 5, 3, 9, 1, 7 };
    std::sort(v.begin(), v.end());
    CHECK((v == std::vector<int>{ 1, 3, 5, 7, 9 }), "std::sort on vector");
    std::map<std::string, int> m = { { "b", 2 }, { "a", 1 }, { "c", 3 } };
    std::string keys;
    for (auto &kv : m) keys += kv.first;
    CHECK(keys == "abc" && m["b"] == 2, "std::map ordered");
    std::unordered_map<int, std::string> um;
    for (int i = 0; i < 1000; i++) um[i] = std::to_string(i * 2);
    CHECK(um.size() == 1000 && um[500] == "1000", "std::unordered_map");
    auto it = std::find(v.begin(), v.end(), 7);
    CHECK(it != v.end() && it - v.begin() == 3, "std::find");
    CHECK(std::stoi("123") == 123 && std::to_string(4.5).substr(0, 3) == "4.5", "stoi/to_string");
}

static void test_streams()
{
    std::ostringstream os;
    os << "x=" << 42 << " y=" << 1.5 << ' ' << std::hex << 255;
    CHECK(os.str() == "x=42 y=1.5 ff", "ostringstream");
    std::istringstream is("10 20 thirty");
    int a = 0, b = 0;
    std::string c;
    is >> a >> b >> c;
    CHECK(a == 10 && b == 20 && c == "thirty", "istringstream");
    std::cout << "# printed with std::cout" << std::endl;
    CHECK(std::cout.good(), "std::cout works");
}

static void test_threads()
{
    std::atomic<int> counter{ 0 };
    std::mutex mu;
    long plain = 0;
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++)
        ts.emplace_back([&] {
            for (int k = 0; k < 10000; k++) {
                counter++;
                std::lock_guard<std::mutex> lk(mu);
                plain++;
            }
        });
    for (auto &t : ts) t.join();
    CHECK(counter == 40000 && plain == 40000, "std::thread + std::atomic + std::mutex");

    std::condition_variable cv;
    bool ready = false;
    int value = 0;
    std::thread prod([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> lk(mu);
        value = 99;
        ready = true;
        cv.notify_one();
    });
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return ready; });
    }
    prod.join();
    CHECK(value == 99, "std::condition_variable");

    std::exception_ptr from_thread;
    std::thread thrower([&] {
        try { throw std::runtime_error("in thread"); } catch (...) { from_thread = std::current_exception(); }
    });
    thrower.join();
    std::string w;
    try { if (from_thread) std::rethrow_exception(from_thread); } catch (const std::exception &e) { w = e.what(); }
    CHECK(w == "in thread", "exception inside a std::thread");
    CHECK(std::thread::hardware_concurrency() >= 1, "hardware_concurrency");

    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    CHECKF(t1 > t0 && ms >= 25, "steady_clock monotonic, sleep_for", "%lld ms", (long long)ms);
}

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual const char *name() const { return "shape"; }
};
struct Rect : Shape {
    double w, h;
    Rect(double a, double b) : w(a), h(b) {}
    double area() const override { return w * h; }
    const char *name() const override { return "rect"; }
};
struct Square : Rect {
    explicit Square(double a) : Rect(a, a) {}
    const char *name() const override { return "square"; }
};
struct Circle : Shape {
    double r;
    explicit Circle(double x) : r(x) {}
    double area() const override { return 3.0 * r * r; }
};

static void test_rtti()
{
    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.push_back(std::make_unique<Rect>(2, 3));
    shapes.push_back(std::make_unique<Square>(2));
    shapes.push_back(std::make_unique<Circle>(1));
    double total = 0;
    std::string names;
    for (auto &s : shapes) { total += s->area(); names += s->name(); names += ' '; }
    CHECK(total == 13.0 && names == "rect square shape ", "virtual functions");
    CHECK(dynamic_cast<Rect *>(shapes[1].get()) != nullptr, "dynamic_cast to base of the dynamic type");
    CHECK(dynamic_cast<Square *>(shapes[0].get()) == nullptr, "failed dynamic_cast is nullptr");
    CHECK(dynamic_cast<Circle *>(shapes[2].get()) != nullptr, "dynamic_cast sideways");
    bool thrown = false;
    try { (void)dynamic_cast<Circle &>(*shapes[0]); } catch (const std::bad_cast &) { thrown = true; }
    CHECK(thrown, "failed reference dynamic_cast throws bad_cast");
    CHECK(typeid(*shapes[1]) == typeid(Square) && typeid(*shapes[0]) != typeid(Square), "typeid comparison");
    std::string tn = typeid(Square).name();
    CHECKF(tn.find("Square") != std::string::npos, "typeid name", "name=%s", tn.c_str());
}

static void test_filesystem()
{
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    std::string cs = cwd.string();
    CHECK(!ec && !cs.empty(), "filesystem::current_path");
    CHECKF_LSIW(cs.size() >= 2 && (cs[0] == 'C' || cs[0] == 'c') && cs[1] == ':', "current_path starts with C:",
                "cwd=%s", cs.c_str());
    fs::path tmp = fs::temp_directory_path(ec);
    CHECKF(!ec && fs::is_directory(tmp), "temp_directory_path is a folder", "tmp=%s", tmp.string().c_str());
    fs::path base = tmp / ("lsiw_t_cpp_" + std::to_string(GetCurrentProcessId()));
    fs::path deep = base / "a" / "b" / "c";
    CHECK(fs::create_directories(deep, ec) && !ec && fs::is_directory(deep), "create_directories");
    {
        std::ofstream f((deep / "file.txt").string());
        f << "data\n";
    }
    CHECK(fs::exists(deep / "file.txt") && fs::file_size(deep / "file.txt") >= 5, "file inside the new folders");
    int entries = 0;
    for (auto &e : fs::recursive_directory_iterator(base)) { (void)e; entries++; }
    CHECKF(entries == 4, "recursive_directory_iterator", "entries=%d", entries);
    auto removed = fs::remove_all(base, ec);
    CHECKF(!ec && removed == 5 && !fs::exists(base), "remove_all", "removed=%llu", (unsigned long long)removed);
}

int main()
{
    t_init("t_cpp");
    test_exceptions();
    test_containers();
    test_streams();
    test_rtti();
    test_filesystem();
    test_threads();
    return t_done();
}
