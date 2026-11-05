# KI-011 deterministic soft-body lifecycle qualification

The original Windows MSVC 19.44 Release run at `942c4f8d9` failed the destroy/reuse
stale count assertion in 0.55 s; one isolated repetition passed in 0.36 s. Its
fixed data was 256x256 High cloth with no attachments, gravity (0,-20,0), and a
1/60 s step; shutdown used 128x128 High cloth. There was no random seed. The fixture
polled up to 1000 yields, then slept 2 ms before mutation. Original evidence remains in
the LUI input integration receipt and its preserved full-run/isolated logs.

## Demonstrated first faulty boundary

`StepActive` is set at admission, before collider/body snapshot construction.
Thus even observing `HasInFlightStep=true` does not establish that the old body
was captured. A completed step can also finish between polling and mutation.

The private native factory in `src/physics/SoftBodyTestHooks.hpp` now allows an
admission barrier and a pre-merge barrier; both run outside the backend mutex
and inside Step's existing exception cleanup. Holding admission, destroying the
old body and reusing its slot deterministically produces this real trace:

```text
AdmittedOnly InFlightObserved=1 StaleResults=0 CapturedReplacement=1 OldStaleAssertionWouldFail=1
```

The replacement alone is captured, the old ID is invalid, generations differ,
and there is no resurrection. This reproduces a legal schedule under which the
old test predicate fails despite correct generation behavior. The historical
run did not record its internal phase, so its exact schedule is not recoverable.
The defect demonstrated here is the test's unproven premise; no production
generation defect was found.

## Corrected regression

The lifecycle cases wait until the actual job group has completed and Step is
held before merge. They verify one dispatched job, 786432 result bytes for the
65536-vertex old cloth (196608 bytes for the 16384-vertex shutdown cloth), and an
active step. The controlling thread then disables/reconfigures/destroys/reuses
or starts shutdown before releasing the barrier. Existing `StaleResults>=1`
assertions remain; accepted result states must additionally be empty, and the
current replacement positions/revision must remain unchanged. Backpressure is
tested while the barrier guarantees that admission remains active.

The five-second synchronization bound is a hang guard, not a performance gate.
RAII releases and joins on assertion/exception paths. A throwing admission hook
proves that Step's in-flight flag is retired on failure. Hooked observation and
the ordinary no-hook factory produce byte-identical solver positions. Normal
construction owns no hook allocation; it adds only an absent native pointer
and two step-boundary null checks. There is no particle-loop instrumentation,
new executor, public schema/API, serialization, transport or solver change.
The small native overhead is not claimed as measured performance improvement.

## Qualification and scope

DockerPC reused `Workspaces/lui-animation-e682` and its `msvc-release` cache;
MSVC 19.44 x64 Release, Ninja, GNS OFF, NodeContent OFF and PCH OFF remained
unchanged. Two compile jobs built only the soft-body and rigid-physics targets.
Exact old Git blob/preimage checks and three candidate-file SHA256 checks
preceded overlay; source hashes were rechecked after execution.

- Serial CTest: rigid physics 0.06 s and soft-body 0.38 s, 2/2 passed.
- Fixed concurrency matrix: two independently launched soft-body processes both
  passed all lifecycle cases. Every barrier reported its captured premise;
  both reproduced the admission-only counterexample.
- No assertion, stale check, identity check, fixture topology, solver setting or
  acceptance threshold was relaxed. These runs are a prescribed serial/parallel
  matrix, not retry-until-success.

`SoftBodyQualification.json` pins sources and binaries. Full build, JUnit,
SerialLastTest.log and both concurrent stdout/stderr captures are preserved at
`C:/Sandbox/Codex/Artifacts/LUI-Primary-20261103/BlockerE/Attempt01` and copied to
the controller's matching task artifacts. This is focused Windows qualification;
new hosted/sanitizer results remain separate and must be read against their
actual source. It does not rewrite the original 82-test result.

The prior PR12 head `624135e`, merge checkout `e797950`, independently passed
106/106 Windows CTests, Linux Clang 19 ASan/UBSan and GNS ASan/UBSan/LSan. Windows then
failed the separate recovery-fixture extractor, which only read the first raw
literal. Follow-up `183e6d119` reconstructs explicit raw-literal addition and
preserves both payload hashes; see `RecoveryFixtureExtraction.json`. That
earlier CI result does not qualify this later lifecycle fix.
