<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
See LICENSE.
-->

# I/O watch index validation and measurement

The runtime now keeps a private hash index beside each node's poll, accept and
receive watch ownership lists. The index selects a bucket by descriptor; every
existing descriptor/device/inode/event/local-address and lifecycle predicate is
still checked before returning a watch. It accelerates lookup, not FD identity
validation or the close/rehome list walks.

## Ownership and failure behavior

All index reads, insertions, growth and removals use the existing `watch_lock`.
No new production atomics or lock order are introduced. Watch addresses stay
stable across growth, and rehashing preserves newest-first precedence among
entries sharing one descriptor. The ownership list remains responsible for
cleanup and migration traversal. A destroy-pending watch can remain indexed
while pins/backend references drain; lookup rejects it. Physical destruction
unlinks its entry before freeing storage. Shutdown releases the index only after
workers/control operations have drained.

There is never a partial index. If the first bucket allocation fails, all lookups
use the original list until that table empties. If growth fails, the complete old
table accepts the new entry with a higher load factor. Optional allocation does
not create a new public failure or overwrite errno. Buckets are released when
the last watch disappears and at shutdown; capacity otherwise retains its peak.

On the measured arm64 build each watch grows by 24 bytes and `llam_node_t` grows
from 832 to 896 bytes. Buckets consume eight bytes per slot, initially 16 slots
and growing geometrically at load factor one. At 4,096 poll watches, entries and
buckets add 128 KiB beyond the old watch storage, plus node padding/metadata.
Hash lookup is expected constant time; collision chains and retained generations
can still make an individual lookup linear.

## Tests

```sh
make -j4 CC=clang test_io_watch_index test_connect_io test_io_buffers \
  test_runtime_io_dump test_runtime_shutdown_internal
./test_io_watch_index
./test_connect_io
./test_io_buffers
./test_runtime_io_dump
./test_runtime_shutdown_internal
```

`test_io_watch_index` checks all three watch kinds across growth, collisions,
first-allocation and resize failure, event/identity mismatch and non-list-order
destruction. It then exercises close/finalize/FD-reuse with a retained source
pin: two forced orders and 500 concurrent pthread trials. It calls production
close/finalize/forward helpers with two detached nodes and real socket
file descriptors, including `close`/`dup2`. It verifies that late forwarding
cannot target the reused descriptor's fresh watch and that releasing the old
pin cannot remove the new index entry.

This fixture isolates index/lifetime transitions; it does not emulate every
kernel completion interleaving. The native I/O suite separately exercises real
backend I/O, listener reuse, close cleanup, and cancellation. It is not a formal
proof of all schedules. The test skips native Windows, whose IOCP lookup path
is not changed. The new regression is registered in Make, CMake/CTest and the
existing `make test-asan` suite. Test allocation failure hooks are absent from
production artifacts.

## Benchmark

Build the same benchmark source against the pre-change and indexed runtime,
keeping compiler, flags and existing local changes matched. The baseline must
represent the pre-index working tree, not an unrelated clean Git revision.
Capture its executable before rebuilding the indexed runtime.

```sh
make -j4 CC=clang bench_watch_index
python3 -B scripts/bench_watch_index.py --baseline /path/to/frozen/baseline-bench \
  --candidate ./bench_watch_index --samples 7 --warmup 1 \
  --output research/watch-index-new-run.json
```

CMake exposes `bench_watch_index` and `test_io_watch_index` as well. The driver
uses fresh processes and alternates baseline/indexed order. It refuses to
overwrite results and retains every per-process measurement, command, binary
hash and exact nanosecond/count histogram. p99 is nearest-rank over individual
operations, not a percentile over seven process durations. All inherited
`LLAM_*` overrides are removed.

- `lookup`: pre-create one poll watch per real socket and repeatedly call the
  production get-or-create lookup under `watch_lock`. Identity `fstat` and lock
  acquisition are included; setup/destruction are excluded. The default is
  32,768 timed operations.
- `io`: one managed poll/read/echo task per socketpair. The native client waits
  until every connection has an actual attached poll waiter, then sends one
  request at a time in a permutation of descriptors and checks every reply.
  Throughput counts completed echoes, and latency is client write-to-reply RTT.
  The default is 8,192 requests, rounded to complete per-connection rounds.

Connection counts are 1, 16, 128, 1,024 and 4,096. All workloads use one processor
and one native watch node. Child-process soft FD limits are raised when needed
within the existing hard limit. This is local AF_UNIX steady-state I/O, not TCP,
open-loop saturation, multi-node scaling or connection-churn performance. The
lookup workload and the full I/O workload support different performance claims.

A longer I/O confirmation run is available without repeating the lookup cases:

```sh
python3 -B scripts/bench_watch_index.py --baseline /path/to/frozen/baseline-bench \
  --modes io --samples 7 --warmup 1 --io-operations 32768 \
  --output research/watch-index-io-confirmation.json
```

The recorded October 27 results, validation limits, and reversible task patch
are in the experiment report (`research/watch-index-2026-10-27/REPORT.md`, local output).
