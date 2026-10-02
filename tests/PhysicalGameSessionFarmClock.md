# Farm32 in-band clock calibration

The qualified 32-client fixture uses the existing ScaleFunction reliable
RemoteFunction echo for four sequential calibration calls per client before
each measured phase. The five calibration epochs occur during initial warmup
and in explicit bounded gaps between measured phases. All 32 clients must
finish an epoch before the server publishes the next phase. The existing
per-phase traffic mix, minimum 13-second phase duration, admission, GameSession
message format, and GNS transport policy are unchanged.

The farm-only native GNS sink records a request's GnsBefore (t1, client),
GnsReceive (t2, server), reply GnsBefore (t3, server), and reply
GnsReceive (t4, client), plus successful GnsQueued results. The existing
Remote request ID, Remote ObjectId, server peer connection generation, and
run-scoped nonce identify one exchange. No payload is retained. The sink is
installed only while the replicated ScaleClockActive marker is true and is
removed before a measured phase begins. It has a fixed 4,096-record cap per
process; overflow and incomplete evidence fail reconciliation.

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
native sends, ordered local timestamps, zero trace overflow, and server-side
probe timestamps preceding the corresponding measured phase. Its
PhaseLongOffset and OneWayLatency fields remain NOT_MEASURED.

Focused offline checks:

    python -m unittest tools.physical-qualifier.tests.test_farm_clock_exchange
    pwsh -NoProfile -File tests/ContentScaleGameplayLuauSyntaxTests.ps1
