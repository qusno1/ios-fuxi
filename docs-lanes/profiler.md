# backboardd user-stack profiler

Add `sweetprof=1` to the FUXI boot arguments (11 bytes before a separating
space). With the argument absent or not exactly 1, no worker or table is
created. `sweetprof=N` (2..3600) delays the start by N seconds instead, e.g.
`sweetprof=130` to cover `fuxitouchscript=120`. The worker starts from the UFS initialization hook, sleeps 60 seconds,
then selects the first process named `backboardd`, holds a task reference, and
profiles it once for 60 seconds. It does not follow process restarts.

Sweeps target 2 ms spacing with 100 us timer leeway. Each sweep visits the task's
threads under `task_lock`, briefly takes each scheduler lock for state, then
reads saved user PC/LR/x16 and up to four frame-pointer return addresses through
`vm_map_read_user`, like SMP. Frame reads stop on faults, misalignment, backwards
links, or a 1 MiB span. No user pointers are dereferenced directly. There is no
per-sweep allocation or printing. Slow sweeps schedule the next wait 2 ms from
completion; missed deadlines never cause catch-up spinning.

At completion the output is:

```text
PROF backboardd[<pid>] slide 0x<slide> mh 0x<mach-header>
PROF backboardd t<tid> name=<name> total=<samples> dropped=<samples>
PROF backboardd t<tid> name=<name> n=<count>/<samples> state=<run|wait|ready|stop> x16=<signed-low32> wait=0x<event> wr=<wait-result> mixed=<0|1> pc 0x<pc> lr 0x<lr> bt 0x<frame1> ... 0x<frame8>
PROF done threads=<count> sweeps=<count> late=<count> skipped=<count> untracked=<count> truncated=<count>
```

Thread IDs are system-wide IDs, not enumeration indices. Names have whitespace
and nonprinting bytes replaced by `_`, with `-` for unnamed threads. `run` means
on a CPU, `wait` means off-CPU with TH_WAIT, `ready` means runnable off-CPU, and
`stop` covers other nonterminated states. `wait` is the raw kernel wait event,
not a decoded Mach port name; `wr` is the scheduler's wait result. Saved x16 is
useful as the syscall/Mach trap number at a syscall boundary; it is otherwise
an ordinary user register. This profiler does not decode IOConnect selectors.

A single fixed allocation holds 8192 hash buckets and 256 thread records. The
key is `(tid,state,pc,lr,frame1,frame2)`. Rows show the twelve most frequent keys
per thread, in descending count order. x16, wait event/result, and frames 3/4
are the first representative; `mixed=1` means one of those values differed in
later samples of that key. Missing frames are zero. Totals include dropped hash
insertions, and the denominator excludes unreadable/terminated thread samples.
`dropped` counts failed 64-probe insertions; `untracked` counts samples beyond
256 distinct IDs. `late` counts sweep deadline overruns requiring rescheduling.
`skipped` counts invalid saved states and terminated threads. At most 400 lines
are emitted including header/footer. All tracked thread summaries are retained;
the remaining row budget is divided evenly across threads, up to twelve each.
`truncated` counts threads whose top rows were cut by that budget. Top-twelve counts need not sum to
the total.

Symbolicate the captured text with the unchanged SMP-compatible tool:

```sh
python3 tools/sweet-crash.py CACHE ROOT profiler.txt
```

Use the matching local shared cache/root and keep the PROF slide/mh header
before its rows. Saved state and stacks are approximate snapshots: running
threads are not suspended, copyin can fault/sleep, and sampling/task-lock work
can perturb timing. The 2 ms cadence and 100 us leeway are targets, not realtime
guarantees. Device overhead and the actual CA wait attribution remain unmeasured.

Canonical code is in `xnu/fuxi-prof/`. `tools/sync-fuxi-prof.py` generates
`xnu/patches/0111-fuxi-user-profiler.patch` and `--check` validates source parity
and replays the relevant preceding hooks with `git apply`. It adds BSD and
osfmk build-list entries plus the FUXI UFS start hook. CI's
`tools/ci/prepare-kernel.py` stages every numeric patch as required;
`tools/ci/build.sh` runs `tools/test-fuxi-prof.py` (including the sync check) for
both kernel profiles before building. The host test compiles the actual C with
mocked tasks/time and ASan/UBSan. Optional `--xnu-buildtree BUILDTREE` adds
full-file ARM64 syntax checks using an existing saved XNU build tree, without
running its scripts or accessing a device.

## Registry and work-interval timing

Add the single token `fuxiregtrace=1` (14 bytes). UFS startup parses it once;
absent/zero/other values leave timing, process-name checks, locks and the worker
disabled. It filters exactly `backboardd`, across all threads/restarts. A worker
prints one line every ten seconds, including idle windows:

```text
REGTRACE backboardd span_us=<window> dropped=<n>/<total_us>/<max_us> wi=<n>/<total_us>/<max_us> [class=<class> key=<key> plane=<plane> options=0x<options> apis=0x<mask> n=<calls> total_us=<total> max_us=<max> bytes=<max-size> lookup_us=<lookup-total> serialize_us=<serialize-total> copy_us=<copy-total>] ...
```

Up to eight rows rank by total elapsed handler time among 128 tracked
`(starting-entry-class,key,plane,options)` tuples. Oversized names (128 bytes or
more) and new tuples after saturation contribute to `dropped`; existing tuples
keep accumulating. Empty/no plane prints `-`, whole-table key is `*`. Whitespace,
nonprinting bytes and `[`, `]`, `=` print as `_`; raw names stay distinct internally.
`apis` ORs 1=raw bytes, 2=XML property, 4=recursive XML property, 8=unsupported XML
table, 16=binary table, 32=binary property. Binary forwarding MIG calls are counted
once in their `_bin_buf` implementations. `bytes` is the maximum produced
serialized/raw length, even on copyout failure; zero also covers missing/denied
properties and serialization failures.

Times include waits/preemption, exclude final aggregation/printing, and belong
to completion windows; `span_us` is actual worker spacing. Lookup includes policy,
compatibility and key creation; serialize includes allocation; copy includes
copyout/releases. Whole-table binary snapshot/compatibility lookup is in serialize.
Early returns accrue to the current phase. `wi` covers all `work_interval_ctl`
operations/error returns via C exit cleanup. Under 80 KiB of fixed static storage
holds the table, top-eight snapshot and output buffer. Hooks allocate/print
nothing; the worker selects/resets under a mutex and prints after unlocking.

Inspection found no property overrides, computed values or large registry OSData
in AppleCLCD/IOMobileFramebuffer or SweetSurface. AppleCLCD's three published
properties are small startup values; its swap/vsync lock is separate from the
registry property lock. Optional FuxiBacklight reads do sampled logging (more with
startup trace); brightness dictionaries are immutable snapshots rebuilt on writes.
ARM64 enables `CONFIG_THREAD_GROUPS=1`. Work-interval NOTIFY validates the interval
and creator, obtains urgency, then calls the machine callback; the default
perfcontrol path returns immediately and our overlays register no replacement.
No display-value, locking or scheduler change was justified by inspection.

The supplied 60 s/2 ms samples observed about 7% in registry MIG and 2.2% in the
work-interval syscall. **REGTRACE device results, the per-frame key, the slow stage
and tracer overhead are not yet observed.** These hooks exclude Mach dispatch/reply
and user-library time; sampling alone does not establish the expensive property.

`xnu/fuxi-prof/` holds the canonical code and reviewed `regtrace-hooks.diff`.
`tools/sync-fuxi-regtrace.py` generates `0112-fuxi-registry-trace.patch`;
`--check --tree TREE` also validates application after 0111. The shared header is
an XNU-only export for BSD/IOKit builds. Both CI profiles run
`tools/test-fuxi-regtrace.py`: actual C/C++ with ASan/UBSan tests ordering, bounds,
filtering, once-only/default-off initialization, concurrency, phases/sizes and exit
cleanup. Optional `--xnu-buildtree BUILDTREE` checks the complete new BSD source
and patched syscall/IOUserClient translation units against saved ARM64 flags/headers.
