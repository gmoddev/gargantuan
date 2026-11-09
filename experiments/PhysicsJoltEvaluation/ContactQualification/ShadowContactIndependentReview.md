---
status: experimental-finding
authority: non-normative
date: 2026-11-09
---

# Double-shadow contact corpus and recovery

`ReviewShadowContact.py` assembles the 24 authored ES/GS contact cases from
three **separate preserved** evidence roots in the new correction campaign:
the 22 completed cases under `ShadowContact-20261109T120323`, the one
resource-deferred GS original128 −10 µm case under
`ShadowRemainder-20261109T122111`, and the losslessly repackaged GS
original128 +10 µm case under `ShadowRecovered`. It does not change or
copy the original evidence.

The script checked exact Engine
`7f2fd23cd3b3846e6486e851270af9b1069f734e` and native
`1691460cdcd9bd401c877cc219f38a89071a76ca` pins; each source's
actual manifest, receipt, stdout/stderr, fixture and compressed-trace
SHA-256; authored definitions against the original 12-case manifest; exact
source grid faces; and all 1,320 full float32 position and velocity frame
hashes. It reran the independent geometry reducer and obtained exact
`Verdict.json` equality for all 24 cases.

The recovered +10 µm case is tied to its original completed physics run:
its `Case.json`, `ContactFixtureV1.json`, and `Receipt.json` hashes equal
those in the incomplete original directory. The preserved original raw
trace SHA-256 is
`5f2f13ef651b2301e900e42b01a6b6d013055a0e270bd8d929c212e55`;
it equals the recovered uncompressed-trace source hash. Both the original
partial gzip and its preserved recovery copy hash to
`c379201bcd82b65d676fc3f669a1a7fb1ea4626ceebca85112513459f9621718`.
The new gzip hash and recovery receipt agree. This establishes lossless
packaging recovery, not a second +10 µm physics run. The −10 µm case was
run separately after resource cleanup with the same exact Engine/native
source and authored case definition.

## Geometry

| Mode | Pass / 12 | Remaining failures | Representative passing offset |
| --- | ---: | --- | --- |
| ES: triangle closest-point contact + double shadow | 10 | Exact-plane 17² axial 38.756%; original 128² axial 42.680% | Original 128² ±10 µm: axial 5.528%, sphere penetration 2.099 µm |
| GS: vertex kinematic-relative contact + double shadow | 3 | Face/edge 250 mm gap, offset 17² ~61 mm gap, exact-plane axial 38.693%, original 128² axial 42.653% | Original 128² ±10 µm: axial 6.075%, sphere penetration 0.9516 mm |

The GS ±10 µm original128 passing penetration is close to the authored
1 mm surface gate. Its pass does not establish broader contact robustness.
The ES and GS geometry outcomes are consistent with the earlier E/G
behavior: double-shadow precision corrects velocity and momentum drift but
does not supply missing face contact in G or remove the exact-planar axial
failure. The 128² offset traces report 6,347 ES and 7,917 GS projected
orientation flip **instances** across frames. A projected flip in 3D is not
by itself a triangle inversion or self-intersection; a complete 3D
self-intersection certificate remains absent for these large traces.

## Signed reflection

All six authored ± pairs (ES and GS: 17² ±10 µm, 17² ±1 cm, and 128²
±10 µm) match exact signed reflection of every published cloth position
and velocity float32 bit, except irrelevant signed-zero differences.
Across each pair there are zero XY bit mismatches, zero nonzero-Z sign-bit
mismatches, zero collider center/linear/angular velocity reflection bit
mismatches, and zero numerical reflection error. Frame times, sphere IDs,
orientation quaternions and velocity-method metadata are equal pairwise.
In particular, the recovered GS 128² ±10 µm traces have this exact
40-frame relationship despite originating in separate packaging roots.

The machine receipt is
`C:/Users/aiden/.codex/artifacts/jolt-cloth-contact-correction-20261109/ShadowContactIndependentReview.json`.
These geometry and symmetry results do not establish complete external
work, reciprocal rigid momentum, physical energy closure, or full 3D
self-intersection. The producer's overall physical qualification remains
`INCOMPLETE`.
