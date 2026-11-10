## Concurrency and lifecycle tests.
## Fixture: fixtures/server.R (run once for each server mode)

## ---- concurrency ----------------------------------------------------------
## without threads a connection is served until it is closed, so anything
## that needs a second connection served meanwhile cannot work by design
single <- if (MODE == "single") "single-threaded server"
test("conc: 16 parallel clients x 50 requests", {
    res <- par.lapply(1:16, function(i) {
        for (j in 1:25) {
            r <- get(sprintf("/echo?i=%d&j=%d", i, j))
            if (r$status != 200 || !has(r$text, sprintf("i=%d&j=%d", i, j))) return(FALSE)
            if (!identical(get("/static/hello.txt")$text, "hello world\n")) return(FALSE)
        }
        TRUE })
    expect(all(unlist(res)), "some parallel requests failed") })
test("conc: half-open connection does not block others", {
    slow <- conn(); on.exit(close(slow)); send(slow, "GET /echo HTTP/1.1\r\nHost: x\r\nX-Slow: ")
    t0 <- Sys.time(); for (i in 1:10) expect(get("/echo")$status == 200)
    expect(difftime(Sys.time(), t0, units="secs") < 5, "stalled") }, skip=single)
test("conc: more idle keep-alive connections than threads", {
    idle <- lapply(seq_len(THREADS * 3), function(i) { s <- conn(); send(s, "GET /echo HTTP/1.1\r\nHost: x\r\n\r\n"); read.resp(s); s })
    on.exit(for (s in idle) close(s))
    t0 <- Sys.time(); expect(get("/echo")$status == 200)
    expect(difftime(Sys.time(), t0, units="secs") < 5, "starved") }, skip=single)
test("conc: slow R handler does not block static files", {
    marker <- file.path(DOCROOT, "slow-started"); unlink(marker); on.exit(unlink(marker))
    slow <- conn(30); on.exit(close(slow), add=TRUE); send(slow, "GET /sleep?m=slow-started HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    expect(wait.for(file.exists(marker)), "the slow handler did not start")
    t0 <- Sys.time(); expect(get("/static/hello.txt")$status == 200)
    dt <- as.numeric(difftime(Sys.time(), t0, units="secs"))
    r <- read.resp(slow)
    expect(dt < 0.8, paste("static request waited", round(dt, 2), "s behind an R handler"))
    expect(identical(r$text, "slept")) }, skip=single)
test("conc: clients that stop reading do not block the server forever", {
    ## each blocks a processing thread until the send timeout (30s) expires
    stuck <- lapply(seq_len(THREADS + 1), function(i) {
        s <- conn(); send(s, "GET /static/big.bin HTTP/1.1\r\nHost: x\r\n\r\n"); s })
    on.exit(for (s in stuck) close(s))
    Sys.sleep(1)
    t0 <- Sys.time(); r <- get("/echo", timeout=60)
    expect(r$status == 200, list("server did not recover", r))
    cat(sprintf("    (served after %.1fs)\n", as.numeric(difftime(Sys.time(), t0, units="secs")))) },
    skip=if (!SLOW) "slow, use --slow" else if (MODE != "threads") "slow, run in threads mode only")
test("conc: connection churn", {
    for (i in 1:300) {
        con <- tryCatch(conn(), error=function(e) stop("connect #", i, " failed: ", conditionMessage(e)))
        close(con)
    }
    for (i in 1:100) { r <- get("/echo"); expect(r$status == 200, list(paste0("request #", i, ":"), r)) }
    healthy() })

test("lifecycle: server still running", expect(server.alive(), server.log(20)))

