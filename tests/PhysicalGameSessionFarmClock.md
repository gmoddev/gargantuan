# Farm32 in-band clock calibration

The qualified 32-client fixture uses the existing ScaleFunction reliable
RemoteFunction echo for four sequential calibration calls per client before
each measured phase. The five calibration epochs occur during initial warmup
and in explicit bounded gaps between measured phases. Each client first
receives `calibrating`, locally stops its measured callback and producer,
and sends a unique quiescence ACK. Only after all 32 ACKs does the server
enable the clock RPCs. All 32 clients must finish an epoch before the server
publishes the next phase. The existing
per-phase traffic mix, minimum 13-second phase duration, admission, GameSession
message format, and GNS transport policy are unchanged.

The farm-only native GNS sink records a request's GnsBefore (t1, client),
GnsReceive (t2, server), reply GnsBefore (t3, server), and reply
GnsReceive (t4, client), plus successful GnsQueued results. The existing
Remote request ID, Remote ObjectId, server peer connection generation, and
run-scoped nonce identify one exchange. No payload is retained. The sink is
installed only while the replicated ScaleClockActive marker is true and is
removed before a measured phase begins. It has a fixed 4,096-record cap per
process; the expected maximum server volume is 1,920 records, leaving 2,176
record slots for diagnostically visible duplication. Overflow, decode
exceptions, duplicates, and incomplete evidence fail reconciliation. The
GNS diagnostic filters non-clock frames before backend status/config reads.

These timestamps bracket native submission and receive, not actual packet
transmission or arrival. For client-minus-server clock offset at one exchange,
causality yields the interval [t1 - t2, t4 - t3]. The interval needs no
assumption of symmetric network delay. It does not prove phase-long oscillator
drift or exact one-way latency. A later cross-host due-to-observed gate must
carry this uncertainty and require a separately justified drift bound; it
cannot treat the midpoint as a synchronized timestamp.

tools/physical-qualifier/farm_clock_exchange.py joins the sealed role logs
after the run. It requires all 32 run-scoped identities, five 32-client
calibration barriers, four complete requests per client and epoch, accepted
native sends, ordered local timestamps, a preceding 32-client quiescence
barrier, zero trace overflow or decode errors, and server-side
probe timestamps preceding the corresponding measured phase. Its
PhaseLongOffset and OneWayLatency fields remain NOT_MEASURED.

The indexed-log command is:

```text
python tools/physical-qualifier/farm_clock_exchange.py <server/evidence-sha256.json> <clients/evidence-sha256.json> <run-manifest.json>
```

It verifies the manifest's 32 nonce identities and both evidence indices, then
hashes all 33 role logs while retaining only bounded clock/readiness/phase
metadata. Log bytes remain subject to the supervisor's 16-MiB maximum; native
metadata remains subject to its 4,096-record cap plus bounded lifecycle records.
The receipt includes all input hashes and the analyzer hash. Complete evidence
requires exactly 640 distinct client/epoch/probe exchanges. Completely absent
historical clock metadata is explicitly `NOT_MEASURED`; partial, modified or
foreign-run metadata fails closed.

`PhysicalFarmClockEvidence.ps1` connects this receipt to the role reconciler.
The cross-provider acceptance tool independently replays both providers' indexed
logs and compares the complete receipt with each reconciliation, including the
input/analyzer hashes and every offset interval. A missing historical receipt
is accepted only when replay finds no clock metadata. The overall provider and
Foundation verdicts remain `INCOMPLETE`; probe-scoped correlation is not a
phase-long clock synchronization or one-way latency claim.

Focused offline checks:

    python -m unittest tools.physical-qualifier.tests.test_farm_clock_exchange
    python -m unittest discover -s tools/physical-qualifier/tests -p "test_farm_clock*.py"
    pwsh -NoProfile -File tests/PhysicalGameSessionFarmReconcileTests.ps1
    pwsh -NoProfile -File tests/PhysicalGameSessionFarmAcceptanceTests.ps1
    pwsh -NoProfile -File tests/ContentScaleGameplayLuauSyntaxTests.ps1
