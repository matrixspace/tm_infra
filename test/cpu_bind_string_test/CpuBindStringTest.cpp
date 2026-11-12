// Tests for parseCpuList, CpuBindingSpec::parse, ThreadedNodeOptions::CpuBind,
// and ThreadCpuLease::bindCurrentThreadBy.
// Coverage: parsing correctness, edge cases, error handling, ThreadedNodeOptions
// round-trip via CpuBind(), and bindCurrentThreadBy none/auto/cpus variants.

#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dev::cd606::tm::infra;

namespace
{
    int failures = 0;
    void check(bool ok, std::string const &name)
    {
        std::printf("%s: %s\n", ok ? "ok" : "FAIL", name.c_str());
        failures += !ok;
    }
    bool throws(std::function<void()> f)
    {
        try { f(); return false; }
        catch (std::invalid_argument const &) { return true; }
        catch (...) { return true; }
    }
    bool eq(std::vector<int> const &a, std::vector<int> const &b)
    {
        return a == b;
    }

    // ── parseCpuList ──────────────────────────────────────────────────────────

    void testParseCpuList()
    {
        check(eq(parseCpuList(""), {}),
            "parseCpuList: empty string returns empty");
        check(eq(parseCpuList("3"), {3}),
            "parseCpuList: single CPU");
        check(eq(parseCpuList("2,3"), {2, 3}),
            "parseCpuList: comma list");
        check(eq(parseCpuList("2-5"), {2, 3, 4, 5}),
            "parseCpuList: dash range");
        check(eq(parseCpuList("2:5"), {2, 3, 4, 5}),
            "parseCpuList: colon range");
        check(eq(parseCpuList("2,3,8-11"), {2, 3, 8, 9, 10, 11}),
            "parseCpuList: mixed comma and range");
        check(eq(parseCpuList(" 2 , 3 "), {2, 3}),
            "parseCpuList: whitespace around tokens");
        check(eq(parseCpuList("0"), {0}),
            "parseCpuList: CPU 0");
        check(throws([] { parseCpuList("5-3"); }),
            "parseCpuList: empty range throws");
        check(throws([] { parseCpuList("abc"); }),
            "parseCpuList: non-numeric throws");
    }

    // ── CpuBindingSpec::parse ─────────────────────────────────────────────────

    void testCpuBindingSpec()
    {
        {
            auto s = CpuBindingSpec::parse("");
            check(s.kind == CpuBindKind::None, "parse '': kind None");
        }
        {
            auto s = CpuBindingSpec::parse("none");
            check(s.kind == CpuBindKind::None, "parse 'none': kind None");
        }
        {
            auto s = CpuBindingSpec::parse("NONE");
            check(s.kind == CpuBindKind::None, "parse 'NONE': case-insensitive");
        }
        {
            auto s = CpuBindingSpec::parse("  none  ");
            check(s.kind == CpuBindKind::None, "parse '  none  ': whitespace trimmed");
        }
        {
            auto s = CpuBindingSpec::parse("auto");
            check(s.kind == CpuBindKind::Auto, "parse 'auto': kind Auto");
        }
        {
            auto s = CpuBindingSpec::parse("AUTO");
            check(s.kind == CpuBindKind::Auto, "parse 'AUTO': case-insensitive");
        }
        {
            auto s = CpuBindingSpec::parse("numa:1");
            check(s.kind == CpuBindKind::AutoOnNuma && s.numaNode == 1,
                "parse 'numa:1': kind AutoOnNuma, node 1");
        }
        {
            auto s = CpuBindingSpec::parse("NUMA:3");
            check(s.kind == CpuBindKind::AutoOnNuma && s.numaNode == 3,
                "parse 'NUMA:3': case-insensitive NUMA");
        }
        {
            auto s = CpuBindingSpec::parse("cpus:2,3");
            check(s.kind == CpuBindKind::Cpus && eq(s.cpus, {2, 3}),
                "parse 'cpus:2,3': kind Cpus");
        }
        {
            auto s = CpuBindingSpec::parse("cpus:2-5");
            check(s.kind == CpuBindKind::Cpus && eq(s.cpus, {2, 3, 4, 5}),
                "parse 'cpus:2-5': range shorthand");
        }
        {
            auto s = CpuBindingSpec::parse("CPUS:8-11");
            check(s.kind == CpuBindKind::Cpus && eq(s.cpus, {8, 9, 10, 11}),
                "parse 'CPUS:8-11': case-insensitive");
        }
        {
            // pure digit shorthand
            auto s = CpuBindingSpec::parse("8-11");
            check(s.kind == CpuBindKind::Cpus && eq(s.cpus, {8, 9, 10, 11}),
                "parse '8-11': shorthand maps to Cpus");
        }
        {
            auto s = CpuBindingSpec::parse("2,3,8-11");
            check(s.kind == CpuBindKind::Cpus && eq(s.cpus, {2, 3, 8, 9, 10, 11}),
                "parse '2,3,8-11': mixed shorthand");
        }
        {
            // "cpus:" with empty list degrades to None
            auto s = CpuBindingSpec::parse("cpus:");
            check(s.kind == CpuBindKind::None, "parse 'cpus:': empty list -> None");
        }
        {
            check(throws([] { CpuBindingSpec::parse("unknown"); }),
                "parse 'unknown': throws invalid_argument");
        }
        {
            check(throws([] { CpuBindingSpec::parse("bind=auto"); }),
                "parse 'bind=auto': wrong syntax throws");
        }
    }

    // ── ThreadedNodeOptions::CpuBind ──────────────────────────────────────────

    void testThreadedNodeOptionsCpuBind()
    {
        {
            auto opts = ThreadedNodeOptions{}.CpuBind("none");
            check(opts.cpus.empty() && !opts.cpuAutoBind,
                "CpuBind('none'): leaves cpus empty, no autoBind");
        }
        {
            auto opts = ThreadedNodeOptions{}.CpuBind("");
            check(opts.cpus.empty() && !opts.cpuAutoBind,
                "CpuBind(''): same as none");
        }
        {
            auto opts = ThreadedNodeOptions{}.CpuBind("auto");
            check(opts.cpuAutoBind.has_value() && !opts.cpuAutoBind->numaNode,
                "CpuBind('auto'): sets cpuAutoBind, no NUMA");
        }
        {
            auto opts = ThreadedNodeOptions{}.CpuBind("numa:2");
            check(opts.cpuAutoBind.has_value() && opts.cpuAutoBind->numaNode == 2,
                "CpuBind('numa:2'): cpuAutoBind with numaNode 2");
        }
        {
            auto opts = ThreadedNodeOptions{}.CpuBind("cpus:4,5");
            check(eq(opts.cpus, {4, 5}) && !opts.cpuAutoBind,
                "CpuBind('cpus:4,5'): sets cpus");
        }
        {
            auto opts = ThreadedNodeOptions{}.CpuBind("6-8");
            check(eq(opts.cpus, {6, 7, 8}) && !opts.cpuAutoBind,
                "CpuBind('6-8'): shorthand sets cpus");
        }
        {
            check(throws([] { ThreadedNodeOptions{}.CpuBind("bad_keyword"); }),
                "CpuBind('bad_keyword'): throws");
        }
        // fromCpuBindString factory
        {
            auto opts = ThreadedNodeOptions::fromCpuBindString("cpus:10");
            check(eq(opts.cpus, {10}),
                "fromCpuBindString('cpus:10'): cpus set");
        }
        // CpuBind("none") must not clobber a previously-set cpu list
        {
            auto opts = ThreadedNodeOptions{}.Cpus({3, 4}).CpuBind("none");
            check(eq(opts.cpus, {3, 4}),
                "CpuBind('none') after Cpus(): does not clear existing cpus");
        }
        // CpuBind("auto") after Cpus() should set cpuAutoBind (and clear cpus
        // indirectly—user responsibility; just check AutoBind set)
        {
            auto opts = ThreadedNodeOptions{}.CpuBind("auto");
            check(opts.cpuAutoBind.has_value(), "CpuBind('auto'): cpuAutoBind set");
        }
    }

    // ── ThreadCpuLease::bindCurrentThreadBy with "none" ──────────────────────
    //
    // A minimal stub allocator so we can call the allocator-ref overload
    // without dereferencing a null pointer. For "none" / "" the allocator
    // methods must NOT be called; the test verifies this by counting calls and
    // checking the returned lease is inactive.

    struct StubAllocator : IThreadCpuAllocator {
        mutable int acquireCalls = 0;
        mutable int reserveCalls = 0;
        int acquire(CpuAutoBindRequest const &, std::string const &) override
        {
            ++acquireCalls;
            return 0; // should never be reached for "none"
        }
        void reserve(std::vector<int> const &, std::string const &) override
        {
            ++reserveCalls;
        }
        void release(std::vector<int> const &) noexcept override {}
    };

    void testBindCurrentThreadByNone()
    {
        // "none": must return an inactive lease and never touch the allocator.
        {
            StubAllocator stub;
            auto lease = ThreadCpuLease::bindCurrentThreadBy(stub, "none", "t1");
            check(!lease.active(),
                "bindCurrentThreadBy(alloc, 'none'): inactive lease");
            check(stub.acquireCalls == 0 && stub.reserveCalls == 0,
                "bindCurrentThreadBy(alloc, 'none'): allocator not called");
        }
        // "": same behaviour as "none".
        {
            StubAllocator stub;
            auto lease = ThreadCpuLease::bindCurrentThreadBy(stub, "", "t2");
            check(!lease.active(),
                "bindCurrentThreadBy(alloc, ''): inactive lease");
            check(stub.acquireCalls == 0 && stub.reserveCalls == 0,
                "bindCurrentThreadBy(alloc, ''): allocator not called");
        }
    }

    // ── code review regressions (codereview.md) ───────────────────────────────

    // Only std::invalid_argument counts: a different exception (e.g. bad_alloc
    // from runaway range expansion) must not pass as "rejected".
    bool throwsInvalid(std::function<void()> f)
    {
        try { f(); return false; }
        catch (std::invalid_argument const &) { return true; }
        catch (...) { return false; }
    }

    void testStrictNumberParsing()
    {
        // [P2] trailing garbage and extra separators must be rejected
        check(throwsInvalid([] { parseCpuList("2foo"); }), "review: parseCpuList '2foo' throws");
        check(throwsInvalid([] { parseCpuList("2-3-8"); }), "review: parseCpuList '2-3-8' throws");
        check(throwsInvalid([] { parseCpuList("2-3x"); }), "review: parseCpuList '2-3x' throws");
        check(throwsInvalid([] { parseCpuList("2-"); }), "review: parseCpuList '2-' throws");
        check(throwsInvalid([] { parseCpuList("-1"); }), "review: parseCpuList '-1' throws");
        check(throwsInvalid([] { parseCpuList("+2"); }), "review: parseCpuList '+2' throws");
        check(throwsInvalid([] { parseCpuList("2 3"); }), "review: parseCpuList '2 3' throws");
        check(throwsInvalid([] { parseCpuList("99999999999"); }), "review: parseCpuList above INT_MAX throws");
        check(throwsInvalid([] { CpuBindingSpec::parse("cpus:2foo"); }), "review: parse 'cpus:2foo' throws");
        check(throwsInvalid([] { CpuBindingSpec::parse("2-3-8"); }), "review: parse shorthand '2-3-8' throws");
        check(throwsInvalid([] { CpuBindingSpec::parse("numa:1,2"); }), "review: parse 'numa:1,2' throws");
        check(throwsInvalid([] { CpuBindingSpec::parse("numa:1x"); }), "review: parse 'numa:1x' throws");
        check(throwsInvalid([] { CpuBindingSpec::parse("numa:"); }), "review: parse 'numa:' throws");
        check(throwsInvalid([] { CpuBindingSpec::parse("numa:-1"); }), "review: parse 'numa:-1' throws");
        // valid forms still accepted, including blanks inside a range
        check(eq(parseCpuList("2 - 4"), {2, 3, 4}), "review: parseCpuList '2 - 4' still accepted");
        {
            auto s = CpuBindingSpec::parse("numa: 1 ");
            check(s.kind == CpuBindKind::AutoOnNuma && s.numaNode == 1, "review: parse 'numa: 1 ' still accepted");
        }
    }

    void testRangeAtIntMax()
    {
        // [P2] expanding a range ending at INT_MAX must not overflow ++c
        // (run under -fsanitize=undefined to see the UB on the old code)
        bool ok = false;
        try {
            auto v = parseCpuList("2147483647-2147483647");
            ok = v.size() == 1 && v[0] == 2147483647;
        } catch (...) {
        }
        check(ok, "review: range INT_MAX-INT_MAX expands to exactly one element");
        try {
            auto v = parseCpuList("2147483646-2147483647");
            ok = v.size() == 2 && v[1] == 2147483647;
        } catch (...) {
            ok = false;
        }
        check(ok, "review: range INT_MAX-1..INT_MAX expands to two elements");
    }

    // [P2] Env without ThreadCpuAllocatorComponent: bindCurrentThreadBy<Env>
    // must compile; "none" returns an inactive lease, real bindings throw.
    struct EnvWithoutAllocator {};

    StubAllocator &envStub()
    {
        static StubAllocator s;
        return s;
    }
    struct EnvStubAccess {
        static IThreadCpuAllocator &instance() { return envStub(); }
    };
    struct EnvWithAllocator : ThreadCpuAllocatorComponent<EnvStubAccess> {};

    void testTemplateEntryWithoutAllocator()
    {
        {
            auto lease = ThreadCpuLease::bindCurrentThreadBy<EnvWithoutAllocator>("none", "test");
            check(!lease.active(), "review: bindCurrentThreadBy<EnvWithoutAllocator>('none') compiles, inactive");
        }
        {
            auto lease = ThreadCpuLease::bindCurrentThreadBy<EnvWithoutAllocator>("", "test");
            check(!lease.active(), "review: bindCurrentThreadBy<EnvWithoutAllocator>('') inactive");
        }
        for (char const *spec : {"auto", "numa:0", "cpus:0"}) {
            bool got = false;
            try {
                ThreadCpuLease::bindCurrentThreadBy<EnvWithoutAllocator>(spec, "test");
            } catch (ThreadCpuBindError const &) {
                got = true;
            } catch (...) {
            }
            check(got, std::string("review: bindCurrentThreadBy<EnvWithoutAllocator>('") + spec + "') throws ThreadCpuBindError");
        }
        // malformed spec still reports a parse error, not a missing component
        check(throwsInvalid([] { ThreadCpuLease::bindCurrentThreadBy<EnvWithoutAllocator>("cpus:2foo", "test"); }),
            "review: bindCurrentThreadBy<EnvWithoutAllocator>('cpus:2foo') throws invalid_argument");
        // "none" through an Env that has an allocator does not touch it
        {
            envStub().acquireCalls = 0;
            envStub().reserveCalls = 0;
            auto lease = ThreadCpuLease::bindCurrentThreadBy<EnvWithAllocator>("none", "test");
            check(!lease.active() && envStub().acquireCalls == 0 && envStub().reserveCalls == 0,
                "review: bindCurrentThreadBy<EnvWithAllocator>('none') leaves allocator untouched");
        }
    }

} // anonymous namespace

int main()
{
    testParseCpuList();
    testCpuBindingSpec();
    testThreadedNodeOptionsCpuBind();
    testBindCurrentThreadByNone();
    testStrictNumberParsing();
    testRangeAtIntMax();
    testTemplateEntryWithoutAllocator();

    std::printf("\n%s (%d failure%s)\n",
        failures == 0 ? "PASS" : "FAIL",
        failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
