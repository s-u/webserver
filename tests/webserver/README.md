# webserver test suite

Black-box tests for the `webserver` package. Most suites start a real server
in a separate R process and talk to it over raw sockets, so the tests need
permission to bind local TCP ports.

The suite tests the **installed** package. It does not build or install
anything, so build and install the version you want to test first, e.g.

```sh
R CMD build webserver
R CMD INSTALL -l mylib webserver_*.tar.gz
```

## Running

```sh
Rscript tests/webserver/run.R [options] [pattern]
```

With no arguments all suites are run in all configurations (about 5 minutes).
The exit status is non-zero if any test failed, and the failed tests are
listed at the end.

| Option | Default | Description |
|---|---|---|
| `--suites=<list>` | `spec,http,rqueue,rfork` | suites to run (comma-separated) |
| `--modes=<list>` | `threads,single,rfunfork,connfork` | server modes for the `http` suite |
| `--threads=<n>` | `4` | processing threads of the servers; `rqueue` and `rfork` also run with 0 threads |
| `--lib=<path>` | | load `webserver` from this library (in the driver and in the servers) |
| `--slow` | off | include slow tests (e.g. waiting for the 30s send timeout) |
| `pattern` | | only run tests whose name, including the `[suite/config] ` prefix, matches this regular expression |

Examples:

```sh
# everything, testing the package installed in ./mylib
Rscript tests/webserver/run.R --lib=mylib

# only the chunked encoding tests of the single-threaded server
Rscript tests/webserver/run.R --suites=http --modes=single chunked

# all tests of the rqueue suite with 4 threads
Rscript tests/webserver/run.R '^\[rqueue/4\]'
```

## Suites

| Suite | Files | Fixture | Description |
|---|---|---|---|
| `spec` | `test-spec.R` | none | the declarative API compiler and argument checks, no network |
| `http` | `test-http.R`, `test-conc.R` | `fixtures/server.R` | static files, R handlers, rewrites, HTTP protocol, malformed requests, concurrency; run once per mode |
| `rqueue` | `test-rqueue.R` | `fixtures/rqueue.R` | R handlers evaluated in the server's R session: serialization, errors, C stack, shutdown |
| `rfork` | `test-rfork.R` | `fixtures/rfork.R` | R handlers evaluated in forked workers: worker life cycle, payloads, shutdown |

Server modes of the `http` suite:

- `threads` - R evaluated in-process on the processing threads,
- `single` - no threads (`threads=0`), connections are served one at a time,
- `rfunfork` - the R handler delegates evaluation to forked R workers,
- `connfork` - a forked process per connection.

`rfunfork`, `connfork` and the `rfork` suite need `fork()` and are skipped on
Windows. The `rfork` suite also uses `ps` to inspect the worker processes.

## Writing tests

`helpers.R` provides the infrastructure shared by all test files:

- `test(name, expr, skip=NULL, restart=TRUE)` - runs `expr` as the body of a
  function (so `on.exit()` works and variables are local). `skip` is the
  reason why the test cannot run in the current configuration. If the server
  dies during a test, the test fails and a fresh server is started for the
  following tests; tests that stop the server on purpose use `restart=FALSE`.
- `expect(cond, info)`, `expect.in(x, set)`, `expect.error(expr)`, `healthy()`
- HTTP client: `get(path, headers=, ...)`, `request(method, path, headers, body, ...)`,
  `rawreq(string)`, `conn()`, `send(con, x)`, `read.resp(con)`, `closed(con)`,
  `par.get(paths, conc)` for concurrent requests.
- `srv` is the current server (`port`, `pid`, `log`, ...), `DOCROOT` the
  directory with the static files; in the `http` suite `MODE` and `THREADS`
  describe the configuration, in `rqueue` and `rfork` `THREADS`.

Fixtures are plain R scripts that take `--name=value` arguments (`--port`,
`--docroot`, `--lib` and fixture-specific ones) and print `PID <pid>` as their
first line.
