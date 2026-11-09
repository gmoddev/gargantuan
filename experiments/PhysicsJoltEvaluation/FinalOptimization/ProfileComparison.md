# Diagnostic Jolt nested work

40 frames per case, including 10 warmup frames. Both binaries have native profiling enabled. These scopes overlap and sum worker work; they are not wall time or a critical path. Only the separate unprofiled B/C receipts rank frame timing. Captured source is native `7e59df5f`, guarded Engine `3c864c58`, group size 1024; the worker restored external profiling OFF and all three qualification receipts exited zero.

| Case | Scope | Old 256 count | Final count | Old work ms | Final work ms | Work change |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| cloth16k-W1 | SoftSimulate | 640 | 640 | 7,962.9 | 7,722.2 | -3.0% |
| cloth16k-W1 | SoftEdges | 416,000 | 108,800 | 7,357.3 | 7,371.9 | +0.2% |
| cloth16k-W1 | SoftVolumes | 416,000 | 108,800 | 47.7 | 13.0 | -72.7% |
| cloth16k-W1 | SoftFinishIteration | 6,400 | 6,400 | 765.4 | 446.7 | -41.6% |
| cloth16k-W1 | SoftIntegrate | 3,200 | 3,200 | 114.3 | 101.8 | -10.9% |
| cloth16k-W1 | Barrier | 80 | 80 | 0.1 | 0.1 | +19.4% |
| cloth16k-W1 | ClearLambdas | 0 | 0 | 0.0 | 0.0 | — |
| cloth16k-W8 | SoftSimulate | 5,120 | 5,120 | 21,993.8 | 17,469.8 | -20.6% |
| cloth16k-W8 | SoftEdges | 416,000 | 108,800 | 8,978.9 | 8,654.2 | -3.6% |
| cloth16k-W8 | SoftVolumes | 416,000 | 108,800 | 55.3 | 16.9 | -69.4% |
| cloth16k-W8 | SoftFinishIteration | 6,400 | 6,400 | 1,519.3 | 892.8 | -41.2% |
| cloth16k-W8 | SoftIntegrate | 3,200 | 3,200 | 344.8 | 221.0 | -35.9% |
| cloth16k-W8 | Barrier | 27,341 | 27,335 | 2,815.2 | 2,249.6 | -20.1% |
| cloth16k-W8 | ClearLambdas | 0 | 0 | 0.0 | 0.0 | — |
| rubber4k-W8 | SoftSimulate | 5,120 | 5,120 | 17,936.6 | 18,305.9 | +2.1% |
| rubber4k-W8 | SoftEdges | 108,800 | 108,800 | 2,250.8 | 2,279.9 | +1.3% |
| rubber4k-W8 | SoftVolumes | 108,800 | 108,800 | 2,939.0 | 2,937.2 | -0.1% |
| rubber4k-W8 | SoftFinishIteration | 6,400 | 6,400 | 1,656.8 | 1,710.0 | +3.2% |
| rubber4k-W8 | SoftIntegrate | 3,200 | 3,200 | 90.0 | 63.9 | -29.1% |
| rubber4k-W8 | Barrier | 27,316 | 27,355 | 2,307.9 | 2,353.4 | +2.0% |
| rubber4k-W8 | ClearLambdas | 0 | 0 | 0.0 | 0.0 | — |

The final 1024-group cloth cases issue fewer edge/volume scope calls. The eight-worker cloth FinishIteration work also falls; this supports the reduced serial-group cost hypothesis, while the nested profile alone cannot allocate a wall-time gain. Rubber keeps group size 256, so its scope counts should stay equal.

## Exact 40-frame state checks

| Profile case | Reference | Frames | Result |
| --- | --- | ---: | --- |
| cloth16k-W8 | C-Long-20261109T070355/cloth16k.jsonl | 40 | exact |
| cloth16k-W8 | Final-Guards-20261109T072412/cloth16k-W8.jsonl | 40 | exact |
| rubber4k-W8 | C-Long-20261109T070355/rubber4k.jsonl | 40 | exact |

