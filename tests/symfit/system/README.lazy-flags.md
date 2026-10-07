# Lazy flag boundary regression tests

Build the configured `x86_64-softmmu` target in `symfit-dev` with host toolchain
variables removed. From the repository root:

```sh
distrobox enter --clean-path symfit-dev -- env -i HOME=/home/pratik PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin make -C build/symfit -j8 x86_64-softmmu/all
distrobox enter --clean-path symfit-dev -- env -i HOME=/home/pratik PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin make -C build/symfit/x86_64-softmmu check-lazy-flags
distrobox enter --clean-path symfit-dev -- env -i HOME=/home/pratik PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin python3 tests/symfit/system/lazy_flags_fixture.py
mkdir -p build/smoke-tmp
distrobox enter --clean-path symfit-dev -- env -i HOME=/home/pratik PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin TMPDIR="$PWD/build/smoke-tmp" python3 tests/symfit/system/symfit_system_smoke.py --arch x86_64 --json
```

For initial setup use `./build.sh`; these commands reuse its configured build.
The unit target is available only for the instrumented x86_64 system build.
It links the same objects as the emulator and wraps the program entry with a
test main. It uses real QOM CPU creation, reset, debugger writes, CPU VMState
load hooks, serialization and flag computation. Sentinel labels are opaque
and never passed to SymSan for lookup.

Coverage: zero, partial and full EFLAGS update masks; architectural CC/DF and
preserved bits; arithmetic ADD/SUB and explicit-EFLAGS lazy tuples; eight
successive exit/entry cycles; first entry; marker consumption; reset; debugger
replacement including an identical-value write; and CPU VMState round-trip
into an object containing old labels and a valid reuse marker. No label or
reuse marker is serialized. Three compile-time checks guard TCG shadow offsets.

The boot-sector fixture runs continuous and interrupted CMP/Jcc cases, then
concrete POPF/Jcc and a supported symbolic comparison control. It checks
concrete register results and structured `pc`, `label`, `op`, `taken`, and
`exportable` branch fields. Generated images, logs and records live under
`build/lazy-flags/`. It waits for the QMP VM stop after an IA match before
requesting the next resume, since IA publishes its match asynchronously.

An interrupted CMP/Jcc can route through a concrete flag-computation helper
and lose symbolic branch recording despite preserving the tuple. The fixture
reports whether that branch was recorded; only the continuous comparison and
the fresh comparison control must be symbolic. Unit tests independently require
complete tuple and label preservation. Symbolic helper modeling is separate.

## Verified on the live serializer branch

The focused tests, fixture and existing x86 system smoke pass. The system build
has existing format/redeclaration and libunwind link warnings. Negative checks
using the original HEAD boundary implementations with the maintained unit
tests fail deterministically: concrete reload retains sentinel labels; original
entry replaces the arithmetic tuple instead of preserving it. Generated
negative-check source and binaries are under `build/` and are not installed.

These results establish bounded shadow hygiene and concrete non-regression.
They do not explain a historical phantom constraint. Relaxation and replay
safeguards remain unchanged.
