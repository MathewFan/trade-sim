# TradeSim benchmark evidence

Recorded 2026-10-30T15:32:13.070958+00:00. Three local release-build trials, source fingerprint `91ece10bc9eb64bb6539e984447a83e55c6b2a5ec7d5b22da99ae474752de484`. Compiler: Apple LLVM 21.0.0 (clang-2100.1.1.101); 10 hardware threads; platform: macOS-26.6.2-arm64-arm-64bit.

## Workload and completion scope

The C++ harness submits deterministic integer unit orders across 64 symbols. Buy/sell totals balance, producing one fill per pair and empty terminal books. Each producer keeps up to 64 completion futures outstanding, then observes their completions in order. Timing includes producer threads, admission, queueing, matching, and completion observation, but excludes engine startup, terminal snapshot checks, and teardown. Observed latency includes head-of-line observation delay. No networking, trade observer callback, replication, or user traffic is measured.

The memory modes each execute 999,936 commands per trial (the requested million rounds down to a multiple of 128). The per-command fsync sample uses 1,024 commands. Each row reports the median of three trials; it is not a long-running production capacity test. CPU frequency, OS scheduling, and concurrent machine activity can affect results.

| Mode | Shards | Producers | Commands/trial | Median commands/sec | Median observed p99, µs |
| --- | ---: | ---: | ---: | ---: | ---: |
| memory | 1 | 1 | 999,936 | 2,422,657 | 28.1 |
| memory | 1 | 4 | 999,936 | 1,051,228 | 446.5 |
| memory | 4 | 4 | 999,936 | 1,184,640 | 282.3 |
| wal-fsync | 1 | 1 | 1,024 | 45,504 | 1,817.8 |

Do not quote memory-mode throughput as durable or WebSocket throughput. Four shards improve the contended four-producer case on this run, while a single producer on one shard is faster than either multi-producer scenario. That reflects overhead and workload balance; sharding is not a universal speedup.

## Additional checks and measurements

- Every scenario checks completed command count, expected fill count, and empty terminal books for all 64 symbols.
- Replay of the 1,024-command fsynced journal reconstructed the terminal state; median startup/replay time was **1.397 ms**. Startup currently verifies and scans the journal twice.
- Random cancellation of 999,936 resting orders sharing one price level measured a median **2,847,858 cancels/sec**. Population/allocation is excluded from that timed cancellation loop. This is a focused iterator-unlink test, not end-to-end API cancellation throughput.
- The recorded fsync workload uses the local OS/filesystem/device. It is not a power-loss certification or a distributed commit measurement. The test that abruptly exits a process covers process restart, not physical storage failure.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
python3 scripts/benchmark.py --orders 1000000 --trials 3 --output benchmark-results.json
```

The helper records raw trials, medians/ranges, compiler, platform, hardware thread count, and a fingerprint of the engine/app/build source. Different compilers or machines may produce substantially different numbers. Compare equivalent workloads and durability settings.

See [raw data](benchmark-results.json) and [design limitations](DESIGN.md). CI runs a 1,024-command correctness smoke benchmark, not a throughput threshold on shared runners.
