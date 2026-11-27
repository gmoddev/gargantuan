# Construction Yard SQLite recovery

On November 27, 2026, the Windows x64 Development Dedicated Server at Engine `88cb9fab0` recovered a 64-edit Construction Yard history across six fresh processes. The package used the original Yard server script and an additional server-side test script. The test script submitted ordinary `DataStoreService` writes and checked their returned revisions. On each restart, the Yard script replayed the saved events over the authored scene. Multiplayer input and door travel have a separate test in `tests/SpatialConstructionYardMultiplayer.py`.

| Fresh process | Events replayed at startup | Events in SQLite at exit | History bytes | Database bytes |
| --- | ---: | ---: | ---: | ---: |
| 1 | 0 | 16 | 562 | 12,288 |
| 2 | 16 | 32 | 1,084 | 20,480 |
| 3 | 32 | 48 | 1,606 | 20,480 |
| 4 | 48 | 64 | 2,128 | 20,480 |
| 5 and 6 | 64 | 64 | 2,128 | 20,480 |

Each successful append was read back through the public document API. Eight pairs of overlapping writes used the same expected revision. Each pair produced one success and one conflict. Sixteen subsequent duplicate writes and four stale conditional deletes returned conflicts. Each writing process also created and deleted one scratch record. The final database contained exactly one record at document revision 70 and provider revision 72. Its five 4 KiB pages included one free page. Each post-exit read-only inspection passed SQLite integrity checking, matched the complete stored document against an independent binary encoder, and found zero journal sidecars.

The final two processes each recovered 24 exterior bricks, 24 workshop bricks and 16 excavations. For both grids, the observer checked every candidate cell and cast a ray at it through its own WorldRoot. The two processes returned byte-identical terrain density/material data for 4,000 samples and byte-identical hit flags and float heights for 100 terrain rays. The final serialized document SHA-256 was `f4c3d21ad6e7b903d88244aac4d98cf12b055f8aad4a7849b6ea8d1cb7009e69`. [Process observations and geometry digests](SpatialYardRecoveryBoots.csv)

Across 64 appends, elapsed time from submitting the write until the server script observed its completion had a median of 16.628 ms, p95 of 23.044 ms and maximum of 24.064 ms. With 64 observations, nearest-rank p99 is the maximum. The measurement includes normal server scheduling and polling. The eight pairs include both operations. A separate run of the same writes before full geometry capture measured a 16.494 ms median and a 133.906 ms maximum. The two final read-only processes in the recorded run reached verified replay 1.234 and 1.250 seconds after launch. [Individual operations](SpatialYardRecoveryWrites.csv)

The driver terminated the first four processes after their writes and readbacks completed. It terminated the fifth after its readback and let the sixth shut down normally. Power loss during a SQLite transaction, storage across different hosts and long-lived allocation growth remain outside this run.

## Reproduction

Run from the repository with a SQLite-enabled Windows product suite. Use a short, unused absolute output directory on Windows. The driver retains the authored project, relocated package, database, unmodified process logs, binary terrain/ray exports, CSVs and JSON summary.

```powershell
python -B tests/SpatialYardRecoverySoak.py --engine "$Suite/gargantuan.exe" --packager "$Suite/gargantuan-packager.exe" --server-runtime "$Suite/ServerRuntimeDistribution" --output C:/Sandbox/YardRecoveryRun
```

The recorded suite used EditorHost SHA-256 `388938b3a6c49a9f4d6edd9acc7b7170c0971cbb2857712060c54bd69ce3e927`, Dedicated Server `dca6a01075d7c40d8babec984142681d1cc1dfc56e745cff9e7b7b0ff49eca34` and package manifest `0e9f19279803dd1f66784a6d811f461b8d8efc7de062ed275214cd702d0fec77`. The two public test sources are [the Python driver](../../tests/SpatialYardRecoverySoak.py) and [the server observer](../../tests/SpatialYardRecoverySoak.luau).
