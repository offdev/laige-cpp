# Allocation watch (`laige::allocWatch*`)

The process-wide heap-allocation counter behind the G-R1
zero-allocation guardrail (M1-ALLOC-01; PRD §8.1, §9.3, `budgets.json`
`sim_heap_allocs` target 0, AGENTS PERF-003, CORE-001, FR-12.3).
Public header: `src/laige-core/include/laige/alloc_watch.h`;
counting backend: `src/laige-core/alloc_watch.cpp` (compiled in every
non-sanitizer build tree — the `LAIGE_ALLOC_WATCH=1` public definition
marks exactly those trees). Unit suite: `ctest -R zero_alloc`
(`tests/laige-sim/zero_alloc_tests.cpp`); the engine-side consumer is
the per-tick check in
[api/game_loop.md](game_loop.md#the-zero-allocation-check-m1-alloc-01-g-r1).

G-R1: the simulation's steady-state tick path MUST allocate nothing.
The budgets table (PRD §9.3) names the enforcement: **"Debug:
allocation counter + assert; Release: pool overflow → logged
degradation"**. This header is the debug side; the release side
already exists (the pool overflow path, [api/pools.md](pools.md);
the per-frame `simAllocs` delta, [api/profiler.md](profiler.md)) —
nothing new ships there.

## The armed-window model

The watch has one process-wide state: an **armed window**.

```cpp
laige::allocWatchArm();          // start a fresh window
// ... the region under test (one tick, one loop run, one API call) ...
laige::AllocWatchReading r = laige::allocWatchRead();
// r.allocs      heap allocations since the arm
// r.firstSite   the call site of the FIRST offending allocation
//               (nullptr while none)
```

- `allocWatchArm()` resets the window's count and clears the first
  site, then arms. One armed window at a time; a window stays live
  until the next arm. O(1), no allocation.
- `allocWatchRead()` reads the count plus the first offending call
  site. O(1), no allocation.
- `allocWatchLive()` is true in every tree where the counting backend
  is compiled in (every non-sanitizer tree); false in the sanitizer
  trees, where the watch degrades to inline no-ops (see the scope
  section).

**First-site semantics:** the first allocation after the arm is
recorded (the caller's return address of the allocating call —
`__builtin_return_address` on GCC/Clang, `_ReturnAddress()` on MSVC);
later offenders are counted but keep the first site (a relaxed CAS
that fails once the first site is recorded). The first site is the
actionable one: it is where the invariant broke first (FR-12.3).

## The per-tick assertion (the engine consumer)

In **debug builds**, `GameLoop::runOneTick()`
([api/game_loop.md](game_loop.md)) arms a fresh window **before** the
tick body and reads it **after a completed tick**. A completed tick
with a nonzero window count fails with:

| Condition | Event | Severity |
|---|---|---|
| a completed tick allocated (debug only) | `alloc/sim_tick_allocation` | Error |

Event fields: `tick`, `allocs` (the window's count), `site` (the first
offending call site, hex). The event fires once per offending tick
(the rate limiter never suppresses the first event of an episode —
and the assert follows it immediately, so a second event never
matters), then the debug assert breaks the build run with the event's
fix text in the condition string.

The window covers **everything the tick runs that is the sim
loop's own work**: the systems, the `onTick` hook, the replay
recorder, engine storage growth (archetype column doublings, table
growth), and any other heap use of the tick (a system's local
`std::vector`, a pool's backing store). A **failed** tick is not
checked (the profiler's "a failed tick is not recorded" contract,
api/profiler.md): a tick whose systems did not complete ran no user
work to blame, and its validation error is already actionable.

**Attribution — what is NOT the sim loop's heap:** G-R1's budget is
`sim_heap_allocs` (budgets.json) — the sim loop's own heap: storage,
systems, pools. The diagnostic subsystem's memory is a separate
subsystem with its own contract (LOG-003 gates hot-path logging by
construction; LOG-004 rate-limits repeated failures), so the
logging facade's emit path wraps its work in
`laige::detail::LoggingAllocationGuard` (the `LAIGE_LOG` macro;
`Logger::record`): the field value strings, the rate-state, and the
sink's message formatting are attributed to the diagnostic
subsystem, not the tick's window. Consequence: the engine's
documented in-tick degradations — a G-R5 budget-overrun
warn/critical, a replay write failure, a guardrail warn — still log
(actionable, rate-limited) and never trip G-R1; a tick that
allocates for any other reason still fails at its call site.

This is the **standing hot-path guardrail for every later sim/render
step** (roadmap README §6, "Global invariants"): any M2/M3 step that
adds an allocation inside a tick — a new system, a new recorder, a
new pool's backing store — fails the debug build immediately, at the
allocating call site, instead of silently eroding the 0-allocs
budget.

## Release builds

Release compiles the entire check out (the arm/read and the assert —
`#if !NDEBUG`): no assertion, no crash (CPP-012). A game system that
allocates in release degrades through the already-logged pool
accounting (pool overflow, pools.md) and shows up in the per-frame
`simAllocs` delta (M1-PROF-01/02 frame report) — never a silent
success, never an assert. The counting backend itself is still
compiled in (it is not `NDEBUG`-gated), so release tools and tests
can read the watch; only the tick-boundary enforcement is
debug-only.

## Scope of the counting backend

The backend is a **strong definition of the global
`operator new`/`new[]`** (plus the nothrow and sized-deallocation
variants) in `laige-core`, linked ahead of the CRT's weak defaults:

- **Static build trees (the default, every P0 OS):** the overrides sit
  in the executable's link — an armed window sees **every** heap
  allocation in the process: the engine, the pools, game systems,
  test frameworks.
- **Shared build trees:** the overrides live inside the laige-core
  image. On POSIX, dynamic linking interposes them process-wide (an
  executable's `operator new` call resolves to the library's
  definition); on Windows there is no cross-image interposition, so an
  armed window sees the allocations made **inside the engine images**
  — the engine allocators and the pools, which is the sim loop's
  storage — but not allocations made in the executable itself. The
  canonical (static) trees give full process coverage everywhere.
- **Sanitizer trees (`LAIGE_ASAN` / `LAIGE_TSAN`):** the sanitizer
  runtimes own `operator new`/`delete`, so the counting backend is
  **not** compiled in and the header degrades to inline no-ops
  (`allocWatchLive()` is false; arm/read return zero). There, the
  zero-allocation property is verified by the leak-free sanitizer run
  of the same loop plus the pool reservation-delta assertion — the
  established fallback pattern (tests/laige-core, M0-CORE-02/05;
  tests/laige-sim, M1-ECS-03/07; the `LAIGE_ALLOC_COUNTER` test
  definition gates the test-side probes on the same trees).

The `LAIGE_ALLOC_WATCH=1` public compile definition is set on
laige-core in exactly the trees where the backend is compiled in, so
every consumer (the sim module, tools, test executables) sees the same
live state. The test-side `LAIGE_ALLOC_COUNTER` definition
(`tests/**/logging_alloc_counter.h`) uses the same gate, so the test
probes and the engine's assertion always agree about whether the
watch is live.

## Cost (PERF-003, DBG-004)

| Path | Cost |
|---|---|
| Per allocation, window **disarmed** | one logging-depth load + one armed-flag load + two branches (no counter traffic) |
| Per allocation, window **armed**, outside a diagnostic emit | one logging-depth load, one armed-flag load, one `fetch_add`, one CAS that fails once the first site is recorded |
| Per allocation, inside a diagnostic emit (the attribution contract) | one logging-depth load + one branch (the emission's own work, not counted) |
| Per completed tick, debug builds | one arm (three atomic stores: first-site, count, armed flag) + one read (two atomic loads) |
| Release builds | the tick check is compiled out; the disarmed allocation path remains |

No allocation, no logging, no lock on any healthy path (LOG-003,
DBG-004). Windows are short and allocation-free by contract, so the
armed path's extra atomics are paid only on the rare allocation that
should not happen.

## Threading (CONC-001)

The armed window has exactly one owner: the **sim owner thread**
(the simulation is single-threaded, PRD §10.2). The loop's tick path
is the only caller that arms, so windows never nest. The counters are
relaxed atomics: an allocation from another thread during an armed
window is counted (it happened during the tick — the diagnostic says
so) but it is **observation data, not simulation state** (ARCH-009):
it never enters the tick count, the state hash, or a replay.

## Misuse warnings

- **Arming from more than one owner** breaks the single-window
  contract (the two owners' windows interleave; the reads are
  meaningless). The only intended armer is the per-tick check;
  tests arm deliberately and single-threaded.
- **Reading a disarmed window** returns whatever the last window
  held (the count is not reset on disarm — disarming is not an
  operation; the window ends by the next arm). Arm before the region
  you measure.
- **Trusting the count in a shared Windows build:** see the scope
  section — outside-image allocations are invisible there.
- **Expecting the watch in sanitizer trees:** it is a no-op by
  design (the runtimes own the allocators); use the leak-free
  sanitizer run + the reservation delta there.

## Example

```cpp
laige::allocWatchArm();
runOneSimTick();  // must allocate nothing (G-R1)
laige::AllocWatchReading r = laige::allocWatchRead();
// r.allocs == 0, r.firstSite == nullptr — otherwise the tick broke
// G-R1 and the engine's per-tick check has already logged
// alloc/sim_tick_allocation with the first offending site.
```

The engine's per-tick check is exactly this shape, repeated around
every completed tick in debug builds ([api/game_loop.md](game_loop.md));
the test-side probes wrap the same API
(`tests/**/logging_alloc_counter.h`).
