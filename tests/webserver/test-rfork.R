## R handlers registered with fork=TRUE: each processing thread (or the main
## thread with THREADS = 0) delegates evaluation to its own forked worker.
## Unix only. Fixture: fixtures/rfork.R (run with --threads=THREADS and --threads=0)

nworkers <- max(THREADS, 1L) # one worker per thread

## ---- basics
test("request is evaluated in a worker process", {
    r <- get("/pid"); w <- as.integer(r$text)
    expect(r$status == 200 && !is.na(w) && w != srv$pid && pid.alive(w), list(r, "server:", srv$pid)) })
test("the worker is a child of the server", {
    w <- as.integer(get("/pid")$text); expect(w %in% children(srv$pid), list(w, "children:", children(srv$pid))) })
test("request method passed to the worker", { r <- st(get("/echo?x=1")); expect(startsWith(r, "200 method=GET;"), r) })
test("POST body and content type passed to the worker", {
    r <- request("POST", "/echo", list("Content-Type"="text/x-test"), "abcdefghij")
    expect(identical(r$text, "method=POST;len=10;ct=text/x-test;sum=1015"), r) })
test("status code and headers from the worker", {
    r <- get("/hdr"); expect(r$status == 201 && identical(r$headers[["x-worker"]], "yes"), r) })
test("R error in the worker -> 500 with its message", {
    r <- get("/err?n=7"); expect(r$status == 500 && has(r$text, "err 7"), r) })
test("FALSE in the worker falls through to the next handler", {
    r <- get("/static/hello.txt"); expect(r$status == 200 && identical(r$text, "hello world\n"), r) })
test("FALSE in the worker with no other handler -> 404", { r <- get("/nothing-here"); expect(r$status == 404, r) })

## ---- payloads
test("3MB request body reaches the worker intact", {
    b <- as.raw(sample(0:255, 3e6, TRUE))
    r <- request("POST", "/echo", list("Content-Type"="application/octet-stream"), b, timeout=30)
    expect(identical(r$text, paste0("method=POST;len=3000000;ct=application/octet-stream;sum=", sum(as.integer(b)))), r) })
test("20MB response from the worker", {
    r <- get("/big?n=20000000", timeout=30); expect(r$status == 200 && length(r$body) == 2e7, length(r$body)) })
test("tmpfile payload sent and removed", {
    r <- get("/tmpfile"); f <- r$headers[["x-tmpfile"]]
    expect(identical(r$text, "temporary\n") && !is.null(f) && wait.for(!file.exists(f)), list(r, f)) })

## ---- workers: reuse, state, parallelism
test("60 requests from 8 parallel clients answered correctly", {
    r <- par.get(paste0("/id?n=", 1:60), conc=8)
    bad <- sum(vapply(1:60, function(i) !startsWith(st(r[[i]]), paste0("200 id=", i, ";pid=")), TRUE))
    expect(bad == 0, paste(bad, "wrong/missing responses")) })
test("workers are reused (at most one per thread)", {
    pids <- unique(vapply(par.get(rep("/pid", 30), conc=8), function(r) st(r), ""))
    expect(length(pids) >= 1 && length(pids) <= nworkers && all(startsWith(pids, "200 ")),
           paste(length(pids), "distinct worker pids for", nworkers, "worker(s)")) })
test("no more worker processes than threads", {
    w <- children(srv$pid); expect(length(w) <= nworkers, list("workers:", w)) })
test("worker keeps its R state between requests", {
    get("/set?n=changed"); r <- st(get("/state")); expect(r == "200 changed", r) },
    skip=if (THREADS > 1) "requests may go to different workers")
test("worker R state does not leak into the server", { r <- st(get("/parent/state")); expect(r == "200 parent", r) })
test("workers evaluate in parallel (3 x 1s in < 1.8s)", {
    t0 <- Sys.time(); r <- par.get(rep("/sleep?n=1", 3), conc=3); dt <- elapsed(t0)
    expect(all(vapply(r, function(x) st(x) == "200 slept", TRUE)), lapply(r, st))
    expect(dt < 1.8, sprintf("took %.2fs", dt)) }, skip=if (THREADS < 2) "needs more than one thread")

## ---- C stack in the worker
test("recursion depth 2000 in the worker", { r <- st(get("/recurse?n=2000")); expect(r == "200 2000", r) })
test("runaway recursion in the worker -> R error", {
    r <- st(get("/recurse?n=1000000", timeout=60)); expect(startsWith(r, "500") && has(r, "C stack"), substr(r, 1, 200)) })

## ---- dying workers
test("worker dying during a request -> 500", { r <- get("/die"); expect(r$status == 500, r) })
test("next request gets a new worker", {
    r <- get("/pid"); expect(r$status == 200 && as.integer(r$text) != srv$pid, r) })
test("idle workers killed externally are replaced", {
    killed <- children(srv$pid)
    for (w in killed) tools::pskill(w, tools::SIGKILL)
    expect(wait.for(!any(killed %in% running(srv$pid))), list("still running:", killed))
    bad <- 0
    for (i in seq_len(nworkers * 3)) if (!startsWith(st(get(paste0("/id?n=", i))), paste0("200 id=", i, ";"))) bad <- bad + 1
    expect(bad == 0, paste(bad, "failed requests")) })
test("dead workers are reaped (no zombies)", { expect(wait.for(zombies(srv$pid) == 0), ps.table()[ps.table()$ppid == srv$pid, ]) })
test("server still alive", expect(server.alive(), server.log(5)))

## ---- shutdown
stopped <- function() any(grepl("^STOPPED", readLines(srv$log, warn=FALSE)))
test("webstart() returns promptly on SIGINT", {
    t0 <- Sys.time(); tools::pskill(srv$pid, tools::SIGINT)
    wait.for(stopped() || !server.alive(), 15)
    dt <- elapsed(t0)
    expect(stopped() && dt < 3, sprintf("took %.2fs; %s", dt, server.log(3))) }, restart=FALSE)
not.stopped <- function() if (!stopped()) "webstart() did not return"
## the fixture keeps R running for 2s after webstart() returned (after that its
## children would be gone anyway), so don't wait longer than that
test("all workers are gone after webstart() returns", {
    expect(wait.for(!length(children(srv$pid)), 1), list("left:", children(srv$pid))) }, skip=not.stopped(), restart=FALSE)
test("port is released after webstart() returns", expect(!port.open(srv$port)), skip=not.stopped(), restart=FALSE)
test("R session usable after webstart()", {
    wait.for(!server.alive(), 5)
    l <- readLines(srv$log, warn=FALSE)
    expect(any(grepl("^deep recursion after server stopped: .*C stack", l)) && any(grepl("^DONE", l)), server.log(3)) },
    skip=not.stopped(), restart=FALSE)
## workers share the parent's file descriptors (including the script Rscript is
## reading), so a worker exiting improperly can make the server re-read its input
test("server script ran to completion without errors", {
    l <- readLines(srv$log, warn=FALSE); e <- grep("^Error: unexpected|Execution halted", l)
    expect(!length(e), paste(l[unique(c(e, e + 1))], collapse="\n")) }, skip=not.stopped(), restart=FALSE)
