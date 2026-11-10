## R handlers evaluated in the server's own R session (no forking): with
## THREADS > 0 the handlers run on the processing threads, serialized by the
## R queue mutex; with THREADS = 0 they run on the main thread.
## Fixture: fixtures/rqueue.R (run with --threads=THREADS and --threads=0)

test("basic call", { r <- st(get("/id?n=1")); expect(r == "200 id=1", r) })
test("FALSE falls through -> 404", { r <- get("/refuse"); expect(r$status == 404, r) })

## every response must belong to its own request and no two handler evaluations may overlap
test("400 requests from 20 parallel clients, all answered correctly", {
    r <- par.get(paste0("/id?n=", 1:400), conc=20)
    bad <- sum(vapply(1:400, function(i) st(r[[i]]) != paste0("200 id=", i), TRUE))
    expect(bad == 0, paste(bad, "wrong/missing responses")) })
test("handler evaluations never overlap", { r <- st(get("/stats")); expect(startsWith(r, "200 overlaps=0;"), r) })

## R errors and warnings under load must not leave the R lock held
test("100 parallel R errors -> 500", {
    r <- par.get(paste0("/err?n=", 1:100), conc=10)
    n500 <- sum(vapply(r, function(x) isTRUE(x$status == 500), TRUE))
    expect(n500 == 100, paste(n500, "of 100 were 500"))
    expect(has(st(r[[42]]), "err 42"), st(r[[42]])) })
test("server still fine after errors, warnings and GC", {
    par.get(paste0("/warn?n=", 1:100), conc=10)
    par.get(rep("/gc", 40), conc=10)
    r <- st(get("/id?n=2")); expect(r == "200 id=2", r) })

## C stack: moderate recursion works as it does in a plain R session,
## runaway recursion is an R error and must not take the server down
test("recursion depth 2000", { r <- st(get("/recurse?n=2000")); expect(r == "200 2000", r) })
## (test() also fails it if the server dies). On Windows the C stack is so large
## that the pointer protection stack overflows first.
test("runaway recursion -> R error (C stack), server survives", {
    r <- st(get("/recurse?n=1000000", timeout=60))
    expect(startsWith(r, "500") && (has(r, "C stack") || (WIN && has(r, "protection stack overflow"))), substr(r, 1, 200))
    r <- st(get("/id?n=3")); expect(r == "200 id=3", r) })

## R evaluations are serialized, but static content is served in parallel
test("static file served while R is busy, R evaluations serialized", {
    marker <- file.path(DOCROOT, "rq-busy"); unlink(marker); on.exit(unlink(marker))
    t0 <- Sys.time()
    busy <- lapply(1:3, function(i) { s <- conn(20); send(s, "GET /sleep?n=0.5&m=rq-busy HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"); s })
    expect(wait.for(file.exists(marker)), "no handler started")
    ts <- Sys.time(); r <- get("/static/hello.txt"); dt <- elapsed(ts)
    for (s in busy) read.resp(s)
    total <- elapsed(t0)
    expect(identical(r$text, "hello world\n") && dt < 0.3, sprintf("%s in %.2fs", st(r), dt))
    expect(total >= 1.45, sprintf("3 x 0.5s took %.2fs", total)) },
    skip=if (THREADS < 2) "needs more than one thread")

## shutdown: SIGINT stops webstart(), which must return promptly and leave
## the R session with its stack checking intact
no.signals <- if (WIN) "needs signals"
test("shutdown on SIGINT within 3 s", {
    expect(server.alive(), paste("server died before shutdown:", server.log(5)))
    t0 <- Sys.time(); tools::pskill(srv$pid, tools::SIGINT)
    wait.for(!server.alive(), 15)
    dt <- elapsed(t0)
    expect(!server.alive(), "still running after 15 s")
    expect(dt < 3, sprintf("took %.2fs", dt)) }, skip=no.signals, restart=FALSE)
stopped <- function() if (server.alive()) "server did not stop" else no.signals
test("C stack limit restored after webstart()", {
    l <- readLines(srv$log, warn=FALSE)
    before <- sub("^cstack before: ", "", grep("^cstack before: ", l, value=TRUE))
    after  <- sub("^cstack after: ", "", grep("^cstack after: ", l, value=TRUE))
    expect(length(after) == 1 && identical(before, after), paste("before:", before, "after:", after)) }, skip=stopped(), restart=FALSE)
test("R session usable after webstart()", {
    l <- readLines(srv$log, warn=FALSE)
    expect(any(grepl("^deep recursion after server stopped: .*C stack", l)) && any(grepl("^DONE", l)), server.log(3)) },
    skip=stopped(), restart=FALSE)
