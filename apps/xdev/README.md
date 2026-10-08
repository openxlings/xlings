# xdev

Build the development tool with `mcpp build -p xdev`. It discovers C++ tests
through `mcpp test --list --message-format json`, then reads each built test's
XTEST registry. Missing compiled metadata is reported; `--require-metadata`
makes it an error. Legacy commands remain in `tests/suites.toml`.

```sh
xdev ci plan --lane pr --changed origin/main --shards 4 --require-metadata
xdev test unit --lane main --shard 1/4 -j 4 --out target/xdev/run-1
xdev test e2e --no-build --fixture tests/http-fixture --out target/xdev/http-run
xdev report --in target/xdev/run-1 --fail-unverified
```

The plan writes GitHub matrix JSON to stdout. Plan and execution share the
same selection, platform, lane, affected-module and shard rules. Timings may
be supplied with `--timings FILE`: a JSON object mapping test program IDs to
positive milliseconds. Platform-qualified keys such as `linux:xdev:test_cli`
override an unqualified key. Without a timing record, unit programs estimate one
second and e2e programs five seconds. `--write-discovery FILE` exports the
compiled metadata for another runner; `--discovery FILE` consumes it. Actual
execution refreshes metadata from the selected artifact.

`-j N` runs up to N test programs concurrently, after their builds and metadata
reads finish serially. Each program owns its result files under `workers/N/`;
xdev merges them in selection order. Legacy suite commands execute serially
and participate in the same resource locks. A program holds the union of the
resources of its selected cases for its entire execution. Declare resources
with `XTEST(..., .resources = {"port:8080", "cpu:4"})`, or in a script header:

```sh
# xtest: covers=REQ-ID resources=port:8080,cpu:4
```

Resource names allow ASCII letters, digits, `.`, `_`, `-` and `:`. `port:N`
accepts 1–65535. `cpu`, `cpu:N` (positive N), and `cpu:all` all acquire the
**same exclusive CPU mutex**; N does not grant or account for CPU capacity.
Other valid names are opaque exclusive mutexes, including `sandbox`.
Locks use the kernel's file locking and are acquired in sorted order. They
coordinate independent xdev processes sharing the same lock directory;
`--lock-dir DIR` can share locks between separate checkouts. The default is
`target/xdev/resource-locks` within the checkout. Lock files persist: a normal
exit or process termination releases the kernel lock, and xdev never deletes
lock files or infers ownership from their age. A lock wait expires after
30 minutes and records a failed test. These locks coordinate cooperating
runners; they do not reserve CPU cores or prevent unrelated programs from
binding a declared port.

`--fixture DIR` starts an HTTP server for the duration of the run and passes
`XLINGS_TEST_FIXTURE_URL=http://127.0.0.1:PORT` to test programs and scripts.
The OS reserves an ephemeral port while the listener is open, so concurrent
fixture owners cannot claim the same port. The platform package owns the
POSIX/Winsock boundary. The server supports GET and HEAD of ordinary files,
including binary payloads, beneath DIR; it rejects traversal, malformed
escapes and symlinks leaving the root. Requests and socket operations have
bounded waits, responses close the connection, and the owning server joins
its worker and closes its listener at shutdown. It serves up to 64 MiB per
file. It binds exclusively to IPv4 loopback and never fetches external URLs.

Tests currently opt in to that URL explicitly. Automatic recipe/index
rewriting and testkit `Home::isolated()` defaulting to the HTTP fixture remain
integration work; starting this server alone does not prove an existing
network-dependent scenario is offline.

`xdev report --trend PREVIOUS/trend.json --write NEXT --timings-out NEXT/timings.json`
retains up to 64 distinct executions per platform, lane and test. The report
compares the current successful duration with the prior successful median,
flags tests with both passing and failing executions, and writes reusable
per-platform median weights for `ci plan --timings`. Failures and skips never
supply a timing weight; re-reporting an execution does not add a sample.
A conflicting or unreadable history is an error. CI retains Linux reports as
90-day artifacts; main also caches its history for subsequent reports and
root-scenario shard plans. PR runs read history and attach their own report.
The intermittent-result flag describes observed outcomes and does not decide
whether a failure is an infrastructure issue or a product regression.
