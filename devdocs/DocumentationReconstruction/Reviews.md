# Independent documentation review

Reviewed on 2026-11-14 against integrated Engine
`2a4c1d2b03e40178b97334bb6f1f2eeedd51fe9f`. Three independent GPT-6 High
reviewers inspected another author's work after their own reconstruction slice.
The parent inspected source, build files, complete document inventory and the
combined patch. These are source reviews, not additional native qualification.

| Required review area | Independent reviewer | Evidence and result |
| --- | --- | --- |
| Architecture accuracy | Network/Studio and renderer/physics reviewers | Engine::Step order, Main authority, identities and projections checked; no new ownership system |
| Public API accuracy | Network/Studio reviewer | Declarations, native bindings and negative-test contexts checked; corrected destroy, query and asset-input behavior |
| Renderer and physics | API reviewer | Shadow alpha, swapchain/offscreen formats and physics dirty properties corrected; tested mesh failure contract preserved |
| Networking and F1 | Renderer/physics reviewer | D03 core qualification distinguished from historical physical F1 failures and deferred Node/provider scopes |
| Studio/LUI/EditorHost | Renderer/physics reviewer | Corrected primary v2 mailbox and current Instance writer v5; LUI/UI, Host and Engine authority kept separate |
| Build and contribution | Network/Studio and renderer/physics reviewers | Commands/options checked against CMake, .justfile, workflows and PackagerMain; removed nonexistent cube command |
| Historical evidence | Renderer/physics reviewer | Original receipts/statuses preserved; sibling evidence links retain recorded SHAs and dirty-state caveats |
| Navigation and usability | Network/Studio and renderer/physics reviewers | Corrected index-directory link routing, repo owner, sample naming and missing source path; built HTML verifier checks routes |
| CI and examples | Network/Studio reviewer and parent | Checked extraction coverage and source contexts; compiler validates five examples; checker and link-transformer regression tests exercise failure cases |

## Resolved findings

- `ClearAllChildren` destroys children rather than merely unparenting them.
- Cast misses return nil; overlap misses return an empty read-only result table.
- Asset references with wrong types or excessive length raise errors before
  bounded malformed/unknown references can return nil.
- FileLink source mounting is startup import, not general live two-way sync.
- Script `StepPhysics` is a generated capability/Main-gated binding, not absent;
  the read-only Studio denial test does not prove runtime unavailability. Its
  optional Instance argument is currently ignored; normal frames already step.
- Missing anchored StaticMesh content rejects initial admission or retains the
  prior body on update. A proposed Box-fallback correction was rejected after
  reading the full function and explicit tests; unanchored fallback is separate.
- Current shadow silhouettes do not sample alpha; runtime swapchain format is
  queried from SDL while offscreen output is fixed UNORM.
- Physics tracked-property list includes kinematic/filter/damping/gravity/sleep,
  CCD and MeshPart asset/collision changes.
- LatestFrameMailbox v2/BGRA8 is the Windows primary negotiated viewport path;
  ring v1/RGB8 and Base64 are compatibility/fallback paths.
- Current Instance serialization writes v5; this does not renumber EditorHost,
  package, source-binding or network protocols.
- Accepted ADRs remain normative constraints, including D03 amendments;
  historical proposal status does not silently override accepted decisions.
- Site directory/index links remain on the site, source links route to GitHub,
  and literal source targets and navigation entries are checked.
- Example commands now use the existing `baseplate.luau`. README describes
  source-matched Studio launch requirements and separate integration pins.
- Studio's canonical owner is confirmed as `GantriaEngine/gargantuan-studio`
  by its Git remote and GitHub repository metadata; old gmoddev links redirect.
- Final CI review confirmed the pinned Luau compiler target/mode and fresh
  checkout contract. Two silent coverage gaps were corrected: every content
  page must be emitted, and historical-link report drift requires review.
  Catalog source hashes normalize checkout newlines for Windows/Linux agreement.

No unresolved concrete source-review finding is accepted as current guidance.
Legacy documents classified Unverified retain that status; the inventory does
not certify every old sentence. Final integrated-source reconciliation remains
blocked on the separate Engine integration and is required before PR promotion.

## Validation scope

Local checks: Python integrity/checker regressions; Node link-transformer
regressions; real Luau compilation of five examples without execution; locked
Astro build; generated HTML route, anchor and repository-target validation;
`git diff --check` and documentation-only path review.

No Engine/native/GPU/physical-network performance suite was rerun. Existing
qualification evidence retains its original hashes and verdicts. PR and
post-merge checks are separate evidence and will be recorded after execution.
