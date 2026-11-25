# SIEGEBREAKER

<img src="assets/siegebreaker-duck.png" width="200" alt="A rubber duck wearing a colander as a helmet and holding a stopwatch in its beak.">

SIEGEBREAKER is a C11 command-line tool for running constant-rate HTTP/1.1 load tests against a local service on Linux. Give it a GET endpoint, a request rate and a duration. It writes a JSON report showing how much traffic the client scheduled, how much it sent, how many responses it received and how long they took.

## Why this project exists

If you ask a load generator for 1,000 requests per second and see 997 responses per second, where did the other three go? That question came up while testing Juggernaut, a Spring Boot API. In one ten-second run, SIEGEBREAKER scheduled 10,000 requests. Its workers missed 26 scheduled slots. All 9,974 requests it emitted received HTTP 200. The gap was measurable on the client before those requests could reach the API. [See the recorded run](benchmarks/juggernaut-20261121-110229/1000-1.json).

The project makes that distinction visible during backend development. It schedules requests against a monotonic clock, independently of previous responses. When a worker wakes up late or runs out of available connections, it counts the lost opportunities. For completed responses, it records latency from both admission and the original scheduled time. Together, these measurements help answer whether a service handled the intended workload and how much confidence to place in its reported p99.

## Build and run

You need 64-bit Linux, a C11 compiler, CMake 3.16 or newer, pthreads and libcurl 7.85 or newer with development headers. The tests and example server use Python 3. The repository includes the HdrHistogram_c core.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Start the example server in one terminal:

```sh
python3 examples/local_server.py
```

In another terminal, run a five-second test and save the report:

```sh
./build/siegebreaker barrage \
  --url http://127.0.0.1:8080/ \
  --rps 100 --duration 5s --workers 2 --connections 16 > result.json
```

To see client backpressure for yourself, try the same command with `--url http://127.0.0.1:8080/slow --workers 1 --connections 1`. This handler waits 200 ms per request. A single in-flight request occupies the only slot across several scheduled arrivals. Look at `pool_full` alongside `completed_rps`.

## Configuration

| Option | Default | Accepted values |
|---|---|---|
| `--url` | `http://localhost:8080/` | HTTP on localhost, 127.0.0.1 or [::1] |
| `--rps` | 100 | 1–20,000 across all workers |
| `--duration` | 10s | Whole seconds or minutes, up to one hour |
| `--workers` | 2 | 1–16 |
| `--connections` | 16 | At least one per worker, up to 512 in total |
| `--timeout-ms` | 2,000 | 1–30,000 per transfer |

## Read a report

| Fields | Interpretation |
|---|---|
| `planned`, `admitted`, `emitted`, `completed` | Scheduled slots, transfers accepted by the engine, transfers for which libcurl reported an outgoing request header, and fully received HTTP responses. Each transfer counts at most once in each stage. |
| `missed_schedule`, `pool_full` | Slots lost to late scheduling and to a full worker pool. In a normal finished run, these two counts plus admissions equal the planned total. |
| `scheduler_lag_us`, `service_us`, `scheduled_us` | Delay from due time to admission, latency from admission to completion, and latency from due time to completion. |
| `http`, `http_errors`, `transport_errors`, `timeouts` | Counts by HTTP status, completed 4xx/5xx responses, incomplete transfers, and the timeout subset of transport errors. |
| `emitted_rps`, `completed_rps`, `elapsed_s` | Rates averaged over the measurement window including any time needed to drain outstanding requests. |
| `new_connections`, `body_bytes`, `cancelled` | New successful connections reported by libcurl, received body bytes from finished transfers, and active transfers removed after an internal worker failure. |

Latency histograms include completed HTTP error responses. Dropped slots and transport failures have no HTTP completion latency. Read their counters alongside the percentiles, especially when the client misses its target rate. Histograms use integer microseconds and three significant digits of HDR resolution. An empty distribution is reported as `null`.

The process writes diagnostics to stderr. Exit code 0 means the measurement finished, even if the workload produced HTTP errors or timeouts. Codes 1 and 2 indicate an engine failure and an invalid configuration, respectively. SIGINT and SIGTERM stop admissions and allow in-flight requests to finish within their timeouts, returning 130 and 143.

## Implementation and scope

Each POSIX worker owns a libcurl multi handle, an epoll loop, a monotonic timerfd, a fixed pool of request handles and private histograms. Workers take turns on a shared schedule. A late worker attempts only its most recent due slot, which avoids catch-up bursts. Libcurl handles HTTP framing and connection reuse. The engine merges the histograms after the workers finish.

The current release implements the **Barrage** mode for loopback HTTP GET. The [local example report](benchmarks/local.json) uses the included fixture. The archived [Juggernaut](benchmarks/juggernaut-20261121-110229/) and [Mirvin](benchmarks/mirvin-20261121-191545/) runs used a temporary Bearer-enabled build. Those authenticated runs cover specific endpoints and short windows on a shared host. Adaptive capacity searches, HTTPS, scenario files and long-running stability analysis are planned extensions.

## Tests and license

The unit suite covers the scheduler, CLI parser and histogram aggregation. Integration tests use a local HTTP server to check connection reuse, reconnection, HTTP errors, timeouts, pool exhaustion, multiple workers and signal handling. For an instrumented build, configure a separate build directory with `-DSANITIZER=address`, `undefined` or `thread`, then build and run CTest there.

SIEGEBREAKER is distributed under the [MIT License](LICENSE). The vendored histogram implementation includes its [upstream licensing information](vendor/hdr/LICENSE.txt).
