# Engineering decisions and failure boundaries

## Separate instruments, not just workers

A shard is a concurrency partition, not a shared order book. The original code attached a single book to each shard, allowing unrelated symbols with the same routing destination to match. Each shard now owns a symbol-to-book map. One worker owns that map, all matching state, its active-ID routing index, sequence, and journal.

FNV-1a routing is specified in code rather than depending on implementation-defined `std::hash<string>`. A generated ID is `serial * shard_count + shard_index`. Cancellation routes to exactly one shard; the active-ID index finds the symbol. The shard count belongs to the persisted topology and cannot change in place. A future rebalancing protocol must explicitly migrate state and IDs.

## Define the arrival order and ownership boundary

Concurrent producers are serialized before publishing into each acquire/release SPSC ring. A timed producer mutex plus bounded queue wait produces explicit overload errors. The worker never contends for that producer mutex. Admission order—not wall-clock timestamp or globally allocated ID—is FIFO arrival order. Cross-shard execution has no global total order.

The old raw-pointer arena retained every submitted order. Books now own only active orders; lists hold stable pointers, and an ID-to-iterator index gives constant-time FIFO unlink. Cancellation still includes a price-map lookup, so its total complexity is expected hash lookup plus O(log price levels), not universally O(1). Allocations are still present: this is not a preallocated or allocation-free engine.

DAY limit residuals rest; market residuals do not. IOC drops unfilled quantity. FOK first subtracts aggregate quantities from its remaining need, avoiding an overflow-prone running sum and avoiding one pass over every individual resting order. Failed FOK makes no changes. Input quantity and active-order limits keep aggregate quantities representable.

## Distinguish admission, execution, and durability

`submit_new` returns an admission ID. `submit` returns an ID plus a completion future. A successful completion means the command has been applied; in WAL sync mode its immutable command was written and fsynced first. The WebSocket ACK uses the completion future, not queue admission. Memory-mode ACKs explicitly identify that mode.

A file write failure may leave a partial final frame. The engine fails later commands and exposes failures through completion futures/barriers; it does not guess whether to continue. Recovery verifies the log and rejects partial tails. Operators must preserve the original log and use a trusted checkpoint/explicit repair policy rather than silently discarding bytes. Book allocation failure after a successful journal append similarly leaves an uncertain acknowledgment; replay is authoritative for that input prefix.

The observer callback is isolated from matching exceptions, but still runs on a worker. A blocking observer can stall that shard. The server's callback only pushes into a bounded bridge. Trade broadcasts are a best-effort presentation feed, not the durable event source. Completion acknowledgments and resynchronization semantics remain separate. There is no exactly-once client retry guarantee.

## Replay commands and verify the right object

The legacy journal serialized a post-match order (sometimes zero residual quantity), omitted sufficient identity data for replay, and restarted hash chaining from zero when appending. V2 records the complete original command before mutation. Framing is fixed-endian and bounded; record sequence must be contiguous. The topology is hashed through the chain's seed. Every file has a nonblocking exclusive writer lock.

A state hash covers the symbol and all live resting IDs, sides, prices, remaining quantities, and FIFO order, canonically serialized. It deliberately excludes wall time and per-shard sequence; identical visible books hash identically. The journal tip covers the operation history and differs even when commands leave the same final book. Neither is a Merkle proof or a signed attestation.

Cryptographic integrity is relative to a trusted expected tip. An internally consistent chain can be replaced, and a complete suffix can be removed without an internal checksum error. `wal_verify FILE EXPECTED_TIP` catches that only when the expected tip is independently trusted. Empty/missing journals cannot be distinguished from a new deployment without external metadata. A partially present shard set is rejected, but loss of every file requires external detection.

Replay occurs before worker startup, restores outstanding quantities and cancels, suppresses historical event broadcasts, and resumes the ID allocator above replayed IDs. Startup currently scans each WAL once for writer verification and once to apply commands. Replay is O(history), not bounded by active book size. Checkpointing, authenticated manifests, compaction, and replicated consensus are not implemented.

## Bound network work and own descriptors once

The server uses a single nonblocking poll loop rather than detached client threads and a competing broadcaster that both closed the same descriptor. That eliminates the descriptor reuse/double-close race and isolates slow socket writes from workers. Handshakes and frames are incrementally parsed; lengths are rejected before allocating payload storage. Output offsets handle short writes.

Per-client limits bound receive buffers, pending execution futures, and unsent output. A global event bridge has a fixed cap and an explicit drop counter. Protocol errors disconnect; invalid commands return a generic error. The parser deliberately implements a limited ASCII/flat-object contract rather than pretending a substring extractor is a general JSON parser. The WebSocket implementation intentionally rejects fragmented/binary frames and is not a complete RFC implementation.

A full depth hash is O(active orders), even though the network view displays ten levels. Repeated snapshots can interfere with matching. No snapshot-specific rate limiter, reserved CPU, fair scheduler, TLS, auth, or per-user authorization is provided. This is an inspectable local simulator, not a deployable public exchange.

## Validate invariants independently

- The core test drives 10,000 deterministic new/cancel commands against an independent vector-based matcher, comparing fills and remaining quantities after each step. It checks price priority, FIFO, IOC, FOK, market residuals, positive quantities, level aggregates, ID/iterator consistency, and an uncrossed book.
- Eight concurrent producers target one shard. Separate checks force unrelated symbols into the same shard. A blocked observer saturates all 4,095 queue slots to exercise overload and then recovery; another test checks stop drains admitted work. Observer exceptions are isolated.
- Recovery tests preserve a partially filled order and cancellation across restarts, resume IDs, reject writer/topology conflicts and missing shard files, and kill a child through `_exit` after completed durable commands. They test every truncation point of a sample record and every single-byte corruption, plus the standard SHA-256 `abc` vector. A complete-header prefix is explicitly treated as a valid prefix, not falsely claimed detectable without a checkpoint.
- Protocol tests check the RFC handshake vector, every split point of short and extended frames, hostile length headers, masking/fragmentation rules, duplicate JSON keys, numeric overflow, and invalid command fields. A real loopback integration test sends split data and commands from eight clients and verifies ACK/depth/shutdown behavior.
- Address/undefined-behavior sanitizers and ThreadSanitizer exercise these paths. They improve confidence in observed workloads; they are not a proof of race freedom or filesystem power-loss behavior.

## Interpret performance carefully

Memory matching, concurrent producer contention, shard parallelism, durable fsync, and cancellation are different workloads. The benchmark reports each rather than using a single high throughput figure for all of them. Four workers can improve a contended four-producer case while still losing to a single producer on one worker because of scheduling, atomics, and queue overhead.

Completion latency is observed through batches of futures. It includes queueing and head-of-line observation delay, and a closed-loop producer slows when the service slows. It does not characterize open-loop overload or coordinated omission. Fixed synthetic unit orders across 64 symbols do not model real market skew, bursts, cancels-to-fills ratios, deep books, network clients, observers, or production storage. Benchmark results should always retain the workload and durability mode.
