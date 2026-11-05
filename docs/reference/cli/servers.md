# Server tools

Run the example chat servers and the stress tools that drive them.

## Chat server

Run the default best-effort server:

```sh
./server 7777
```

Run a lossless server:

```sh
./server --lossless 7777
./server_lossless 7777
LLAM_CHAT_LOSSLESS=1 ./server 7777
```

| Option | Meaning |
| --- | --- |
| `--public` | Bind on the public interface instead of the local default. |
| `--lossless` | Apply producer backpressure when a client outbox is full. |
| `--best-effort` | Drop per-client deliveries when a client outbox is full. |
| `-h`, `--help` | Print usage. |
| `port` | TCP port. Defaults to `7777`. |

Use best-effort mode for throughput ceilings. Use lossless mode when every
expected chat delivery must arrive.

`server` is intentionally a bounded-outbox chat backend. Under high-rate fanout,
slow clients can overflow their outbox and lose per-client deliveries;
`server_flood` reports those drops through the server stats file.

The chat reader groups up to 64 complete input lines before waking writers,
flushing at the end of each read. Writers use ordered vectored writes for the
queued batch. Statistics still count individual messages and deliveries.

## Native server flood

`server_flood` starts a target server, drives nonblocking TCP clients, and
reports both inbound message rate and observed broadcast delivery rate.

```sh
make server-flood
./server_flood --server ./server --server-best-effort \
  --clients 16 --duration 60 --message-bytes 8 --batch 64 --target-mps 0.30
```

The `mps` options and report fields count millions per second. Inbound and delivery
rates differ: in chat fanout one inbound message can produce `clients - 1` peer
deliveries, so 16 clients at `--target-mps 0.30` can reach 4.5 million deliveries per
second.

| Option | Default | Meaning |
| --- | --- | --- |
| `--server <path>` | `./server` | Server binary to launch. |
| `--host <ip>` | `127.0.0.1` | Bind/connect host. |
| `--clients <n>` | `4` | TCP clients. Valid range is `2..4096`. |
| `--duration <sec>` | `3.0` | Main send duration. Must be positive. |
| `--drain-sec <sec>` | `1.0` | Time to read late fanout after sending stops. |
| `--message-bytes <n>` | `8` | Message size. Valid range is `2..1024`. |
| `--batch <n>` | `128` | Per-client send batch. Valid range is `1..65536`. |
| `--target-mps <m>` | `0.0` | Optional target inbound rate in millions of messages/sec, so `0.30` is 300,000/sec. `0` means unthrottled. |
| `--min-delivery-mps <m>` | `0.0` | Fail if the observed delivery rate, in millions of deliveries/sec, is below this value. `1.3` is 1,300,000/sec. `0` disables the check. |
| `--min-delivery-ratio <r>` | `0.0` | Fail if observed/expected broadcast ratio is below this value. |
| `--shutdown-timeout <sec>` | `30.0` | Server shutdown wait before forced cleanup. |
| `--server-lossless` | off | Start the target server with `--lossless`. |
| `--server-best-effort` | on | Start the target server with `--best-effort`. |
| `--allow-forced-stop` | off | Do not fail when cleanup must force-stop the server. |
| `--fail-on-forced-stop` | on | Fail when cleanup force-stops the server. |
| `--allow-missing-stats` | off | Do not fail if the server stats file is missing. |
| `--fail-on-missing-stats` | on | Fail if the server stats file is missing. |
| `-h`, `--help` | | Print usage. |

The same defaults can be set with `LLAM_SERVER_FLOOD_*` environment variables;
see [Environment variables](../environment.md#example-server-harness).

## Exact server stress

Use `stress_server.py` when correctness matters more than raw throughput. It
starts the server, connects clients, sends deterministic payloads, and waits
for the expected fanout.

```sh
make server-stress
python3 scripts/stress_server.py --clients 64 --messages 16 --payload-bytes 64
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--server <path>` | `./server` | Server binary to launch. |
| `--host <host>` | `127.0.0.1` | Bind/connect host. |
| `--clients <n>` | `16` | Client count. Must be at least `2`. |
| `--messages <n>` | `8` | Messages per client. |
| `--payload-bytes <n>` | `48` | Random payload bytes appended to each line. |
| `--timeout <sec>` | `20` | Wait limit for all expected broadcasts. |
| `--connect-spread-ms <ms>` | `5` | Delay between connects to avoid testing only backlog behavior. |
| `--seed <n>` | unset | Deterministic payload seed. |

## Composite server stress

Use `stress_server_composite.py` for the full chat-server operational gate. It
combines exact fanout checks, native flood, payload variation, churn, slow
receivers, half-close/reset behavior, RSS sampling, and fd sampling.

```sh
make server-stress-composite
make server-stress-composite-quick
make server-stress-composite-hour
python3 scripts/stress_server_composite.py --quick
python3 scripts/stress_server_composite.py --quick --seed 1234
python3 scripts/stress_server_composite.py --soak-hour
```

Edge logs split client socket failures into expected churn/cleanup errors and
`unexpected_client_errors`; the latter must stay at zero. Pass `--seed` to replay
randomized correctness payloads and edge churn behavior from a previous run.

`--quick` is intended for hosted CI runners. It keeps the exact-delivery correctness
checks but uses lower absolute flood thresholds than the standard profile:

| Phase | Delivery requirement |
| --- | --- |
| Correctness matrix | Every expected fanout line, in both profiles. |
| Lossless flood (`lossless-8b`, `lossless-1kb`) | Observed/expected ratio of at least `0.999`, in both profiles. |
| Best-effort flood (`throughput-8b`, `throughput-64b`) | No ratio floor. Minimum delivery rate is 0.10 and 0.08 million/sec with `--quick`, and 1.3 and 1.0 million/sec in the standard profile. |

The one-hour profile runs the same classes of checks with a long soak layout: 30
minutes of main flood, two 5-minute payload flood phases, and 20 minutes of mixed
edge stress. It keeps the high-rate best-effort flood load, but treats the run as
a stability and accounting soak rather than a throughput benchmark. Zero traffic,
missing stats, accounting gaps, closed-outbox drops, forced server stop,
resource-limit violations, and unexpected edge client errors still fail the run.
Absolute delivery-rate regression checks are left to the standard Stress workflow
and scheduled benchmark jobs.

| Option | Default | Meaning |
| --- | --- | --- |
| `--server <path>` | `./server` | Server binary for correctness and edge phases. |
| `--server-flood <path>` | `./server_flood` | Native flood binary. |
| `--host <host>` | `127.0.0.1` | Bind/connect host. |
| `--correctness-matrix <cases>` | `8:8:16,16:4:64,32:2:1024` | Comma-separated `clients:messages:payload_bytes` cases. |
| `--correctness-timeout <sec>` | `30` | Per-correctness-case timeout. |
| `--flood-duration <sec>` | `60` | Main native flood duration. |
| `--flood-min-delivery-ratio <r>` | `0.0` | Minimum observed/expected broadcast ratio for flood phases. Above `0`, it replaces each phase's floor; `0.0` keeps the floors above. |
| `--allow-flood-forced-stop` | off | Allow native flood cleanup to force-stop the server. |
| `--payload-flood-duration <sec>` | `10` | Duration for payload-size flood phases. |
| `--edge-duration <sec>` | `60` | Edge/churn phase duration. |
| `--edge-clients <n>` | `24` | Clients used by edge phases. |
| `--churn-threads <n>` | `4` | Concurrent churn workers. |
| `--slow-threads <n>` | `4` | Slow receiver workers. |
| `--resource-interval <sec>` | `1.0` | RSS/fd sampling interval. |
| `--shutdown-timeout <sec>` | `30` | Server shutdown timeout. |
| `--command-timeout-padding <sec>` | `30` | Extra timeout before process-tree cleanup. |
| `--max-rss-mb <n>` | `2048` | Fail above this resident-set size. |
| `--max-fds <n>` | `4096` | Fail above this file descriptor count. |
| `--max-unexpected-client-errors <n>` | `0` | Allowed unexpected edge client errors; `-1` disables the check. |
| `--seed <n>` | unset | Deterministic edge payload/churn seed. |
| `--skip-correctness` | off | Skip exact fanout checks. |
| `--skip-flood` | off | Skip native flood phases. |
| `--skip-edge` | off | Skip edge/churn phases. |
| `--quick` | off | Short smoke profile. Mutually exclusive with `--soak-hour`. |
| `--soak-hour` | off | Long one-hour profile. Mutually exclusive with `--quick`. |
