---
status: current
owner: qualification
last_verified: 2026-10-02
related_code:
  - tests/foundation3l_acceptance.py
  - tests/test_foundation3l_acceptance.py
  - tests/PhysicalGameSessionFarmAcceptance.ps1
---

# Final Foundation 3L evidence conjunction

`foundation3l_acceptance.py` is an offline final conjunction for the fixed
canonical workload in `PhysicalFundingGateReview3L.md`. It does not start a
process on either physical endpoint, change acceptance bounds, close KI-006,
start 3M, or merge a pull request. Its only subprocesses are local Git inspection
and the repository's existing PowerShell offline farm replayer.

Run from a clean checkout of the full candidate commit:

Freeze the native/analyzer execution candidate A before its build. Dispatch CI
and the qualified-scale artifact on A, build the four-client probe and owned
source archive from A, and preserve that clean A checkout for final replay.
A later child B may record the newly measured qualifier artifact pins without
changing A's native execution source. Qualify B's adapter/pin changes separately;
the four-client native manifest and both official farm packages still identify
A. This avoids a build/hash self-reference cycle. A final PR documentation or
pin receipt commit does not retrospectively relabel an A executable as B. This
checker proves CI and physical execution provenance for the explicit A source;
it does not waive required separate validation of the B qualifier.

```powershell
python tests/foundation3l_acceptance.py `
  --source-commit <40-lowercase-hex> `
  --farm-inputs <raw-farm-arguments.json> `
  --powershell-path <absolute-pwsh.exe-path> --powershell-sha256 <runtime-sha256> `
  --four-client-inputs <raw-four-client-arguments.json> `
  --ci-index <ci-index.json> --ci-index-sha256 <independently-retained-sha256> `
  --provenance-index <provenance.json> --provenance-index-sha256 <independently-retained-sha256> `
  --output <new-output-path.json>
```

The output path must be new and outside the immutable farm roots. Exit 0 means
every required typed gate passed; exit 2 means an incomplete or failed
conjunction. Malformed, stale, mismatched or explicitly rejected raw evidence
throws and cannot produce PASS. Missing optional evidence inputs produce
INCOMPLETE. The command does not accept a four-client PASS flag or an existing
farm summary as evidence. The four-client argument map is passed only to the
fixed tracked `tests/foundation3l_four_client.py:Replay(Inputs, ExpectedCommit)`
implementation. Until that implementation and its complete inputs are present,
the fresh four-client gate remains unmeasured.
PowerShell is selected by its explicit absolute executable path and hash,
following the existing analysis-inventory/runtime pin pattern. There is no
`pwsh` PATH fallback and no caller-selected replay script. The fixed farm
script's `#requires -Version 7.0` remains enforced. Windows subprocess creation
uses `CREATE_NO_WINDOW`.

## Provider replay

The farm argument JSON contains the named parameters of
`PhysicalGameSessionFarmAcceptance.ps1`, excluding `OutputPath`. The six raw
Local/Node report/root parameters are required. Real Node TLS, owned Node
process resources, both provider capture index pairs, outer capture receipts
and coordinator results are required to obtain a final PASS. The fixed
PowerShell replayer is executed in this invocation and independently checks
all indexed raw files. Its deliberately INCOMPLETE outer observation is not a
failure and cannot itself authorize final PASS.

The final typed conjunction requires each provider's:

- five-phase, all-32-client workload and exact terminal conservation;
- generation-scoped F1 native completion and sticky failure evidence;
- ACK-gated grant lifecycle, recorded wait bound and fixed-workload fairness;
- accepted Character state chain, due-service cadence and all-window ordinary
  successful-send demand (including forced Character traffic);
- native probe clock calibration, work-tick limits and designated producer
  RPC/Event cadence;
- all-role logical cleanup and Remote ownership release;
- nonce-bound 32-tuple capture, complete manifests and lifecycle reconciliation;
- fixed service recovery, immutable exact W, C3/C5 finite-prefix convergence,
  and sampled bounded journal retention;
- role-local complete process sweeps, actual host/NIC observations and bounded
  evidence inventories.

Node additionally requires the actual negotiated authenticated TLS RPC and its
independently owned process resource receipt. Cross-provider exact workload and
deployment pins must match. General infinite saturated-source fairness, an
invented CPU/RSS/NIC percentage SLA, 31 additional Remote producers, and
cross-host one-way timing are not new gates. First-send F1, ordinary demand,
delivery, publication, recovery and packet reserve remain distinct evidence.

## Exact-head CI inventory

Retain original GitHub REST run metadata, the complete jobs response and each
artifact metadata response, plus the original downloaded ZIP bytes. The index
has this shape (all paths are relative to its directory):

```json
{
  "Format": "GargantuanFoundation3LCI",
  "Version": 1,
  "Jobs": [
    {
      "Name": "Windows x64 / MSVC 19.50+ / Release",
      "Run": {"Path": "native-run.json", "Sha256": "<sha256>"},
      "Jobs": {"Path": "native-jobs.json", "Sha256": "<sha256>"},
      "ArtifactMetadata": {"Path": "native-artifact.json", "Sha256": "<sha256>"},
      "Archive": {"Path": "native-ci-diagnostics.zip", "Sha256": "<sha256>"}
    }
  ]
}
```

There must be exactly three entries: Windows Release, Ubuntu headless ASan/UBSan
and Ubuntu GNS ASan/UBSan/LSan, with their exact current workflow job names.
The two native jobs must belong to the same run attempt. Required CTest steps
must have completed successfully, as must their jobs and whole workflow runs.
Only `push` and `workflow_dispatch` runs are accepted. PR merge runs cannot
substitute for an exact candidate checkout.

Each diagnostics ZIP must match GitHub's artifact digest and size, belong to
the expected run/head, and have been created during the selected job attempt.
The bounded JUnit reader requires the relevant native/F1/funded-ACK tests,
rejects failures and missing/skipped required tests, and permits existing
optional platform skips. It also requires `qualified-source-commit.txt` from
the actual CI checkout. Successful diagnostic uploads use `if: always()`;
their file lists and artifact retention policy are otherwise unchanged.

These are offline checks over externally collected GitHub metadata. The
independently retained inventory pin is the collection trust boundary, not a
signature invented by this script. No network access or credential is used.

## Package provenance and gate order

The separate pinned provenance index is:

```json
{
  "Format": "GargantuanFoundation3LProvenance",
  "Version": 1,
  "QualifiedPackage": {
    "ArtifactMetadata": {"Path": "qualified-artifact.json", "Sha256": "<sha256>"},
    "Archive": {"Path": "qualified-scale.zip", "Sha256": "<sha256>"}
  },
  "LocalClientStage": {"Path": "local-client-stage.json", "Sha256": "<sha256>"},
  "NodeClientStage": {"Path": "node-client-stage.json", "Sha256": "<sha256>"},
  "LocalHostTerminal": {"Path": "local-host.terminal.json", "Sha256": "<sha256>"}
}
```

The qualified package must come from the verified Windows dispatch job as
`qualified-scale-<candidate>`. Verification streams its original ZIP without
extraction: at most 1 GiB compressed, 2 GiB expanded, 22,000 entries and 256 MiB
per entry. It rejects redirected/duplicate/extra/missing entries and validates
every file against both role deployment inventories. Actual binary, package,
content-manifest and deployment hashes must equal the independently replayed
Local/Node run manifests. Matching a self-reported source SHA alone is
insufficient.
The builder's two retained source-fixture files and canonical workload
descriptor are the only allowed files outside the two deployment inventories;
they remain bound by the original GitHub ZIP digest and bounded separately.

Retain the root-owned CLIENT campaign stage configurations. Their RunId,
SourceCommit and ManifestSha256 must bind the same indexed provider manifests.
The fresh four-client replay must expose an evidence-backed `CompletedUtc`
with `CompletionClockDomain = CONTROLLING_HOST_UTC`. Both provider stage
`CreatedUtc` values must follow that completion. The controlling-host
`farm_outer_endpoint.py` Local `host.terminal.json` must additionally prove
`Outcome=COMPLETED`, `ChildExitCode=0`, and `ChildTreeReaped=true` for that exact
Local RunId. The separately replayed Local coordinator must succeed with the
stage's CoordinatorRunId. Enforce Local stage <= Local host `EndedUtc` <= Node
stage: a later successful Node receipt cannot repair overlapping or reversed
campaign order. This is broad session ordering
from one controller's UTC records, not cross-host native timestamp subtraction.
If either required completion receipt is absent, order remains NOT_MEASURED.

## Validation and remaining real inputs

The fixture suite mutates every provider conjunct and independently exercises
CI failures, missing tests, stale attempts, digest changes, PR-merge source
mismatches, package inventory corruption and premature provider staging. It
creates only small temporary local files and never claims physical results.

Real final qualification still needs the fresh four-client raw replay,
successful post-change exact-head CI ZIPs, the actual qualified-scale ZIP,
complete Local and Node raw campaigns with new F1/ordinary-demand records, real
TLS/resource/capture/lifecycle evidence and retained controller stage records.
Historical missing fields stay unmeasured; this tool never rewrites receipts.
