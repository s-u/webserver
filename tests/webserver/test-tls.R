## HTTPS listeners alongside HTTP: certificate chains, client certificates,
## TLS with non-blocking I/O (pipelining, large transfers) and robustness.
## Fixture: fixtures/tls.R, srv$port is HTTP, srv$port + 1..3 are HTTPS (see there).
## The client is the openssl command line tool (OPENSSL, see run.R).

HTTPS  <- srv$port + 1L  # certificate + intermediate via tls.ca
CLIENT <- srv$port + 2L  # client certificates alice or *.example.org required
ANYCLI <- srv$port + 3L  # any client certificate from the client CA

## sends req (string or raw) to port with openssl s_client (verifying the
## server against the test root CA), returns list(status, out (raw), err)
tls <- function(port, req, cert=NULL, timeout=30) {
    f <- c(tempfile("in"), tempfile("out"), tempfile("err")); on.exit(unlink(f))
    writeBin(if (is.character(req)) charToRaw(req) else req, f[1])
    args <- c("s_client", "-quiet", "-connect", paste0(HOST, ":", port), "-servername", "localhost",
              "-CAfile", shQuote(file.path(CERTS, "ca.pem")), "-verify_return_error",
              if (!is.null(cert)) c("-cert", shQuote(file.path(CERTS, cert)), "-key", shQuote(file.path(CERTS, cert))))
    status <- suppressWarnings(system2(OPENSSL, args, stdin=f[1], stdout=f[2], stderr=f[3], timeout=timeout))
    list(status=status, out=if (file.exists(f[2])) readBin(f[2], "raw", file.size(f[2])) else raw(),
         err=paste(if (file.exists(f[3])) readLines(f[3], warn=FALSE), collapse="\n"))
}
## all responses in the output of tls()
responses <- function(x) {
    con <- rawConnection(x$out); on.exit(close(con))
    r <- list()
    while (!is.na((p <- read.resp(con))$status)) r[[length(r) + 1L]] <- p
    r
}
req.str <- function(path, method="GET", close=TRUE, body=NULL, hdr=NULL)
    c(charToRaw(paste0(method, " ", path, " HTTP/1.1\r\nHost: localhost\r\n", if (length(hdr)) paste0(hdr, "\r\n", collapse=""),
                       if (!is.null(body)) paste0("Content-Length: ", length(body), "\r\n"),
                       if (close) "Connection: close\r\n", "\r\n")), body)
## a single request, returns the response (status NA if there was none)
tget <- function(port, path, cert=NULL, ...) {
    x <- tls(port, req.str(path, ...), cert)
    r <- responses(x)
    if (length(r)) r[[1L]] else list(status=NA, text="", err=x$err)
}
rejected <- function(port, cert=NULL) {
    r <- tget(port, "/echo", cert)
    expect(is.na(r$status), paste("got a response:", format(r)))
}
https.healthy <- function() { r <- tget(HTTPS, "/static/hello.txt"); expect(isTRUE(r$status == 200), r$err) }

test("HTTP listener (explicit address) next to HTTPS", { r <- get("/static/hello.txt"); expect(st(r) == "200 hello world\n", st(r)) })
test("HTTPS: static file, chain completed by tls.ca verifies", {
    r <- tget(HTTPS, "/static/hello.txt"); expect(st(r) == "200 hello world\n", c(st(r), r$err)) })
test("HTTPS: R handler", { r <- tget(HTTPS, "/echo"); expect(st(r) == "200 method=GET;body=0;sum=0", c(st(r), r$err)) })
test("HTTPS: chain file with key in the same file (tls.key default)", {
    r <- tget(CLIENT, "/static/hello.txt", "alice.pem"); expect(st(r) == "200 hello world\n", c(st(r), r$err)) })
test("HTTPS: chain file with separate key", {
    r <- tget(ANYCLI, "/static/hello.txt", "carol.pem"); expect(st(r) == "200 hello world\n", c(st(r), r$err)) })

test("HTTPS: 1MB request body", {
    b <- as.raw(rep(0:255, length.out=1e6))
    r <- tget(HTTPS, "/echo", method="POST", body=b)
    expect(st(r) == paste0("200 method=POST;body=1000000;sum=", sum(as.integer(b))), c(st(r), r$err)) })
test("HTTPS: 5MB response", {
    r <- tget(HTTPS, "/big")
    expect(r$status == 200 && length(r$body) == 5e6 && identical(r$body[1:3], as.raw(1:3)), length(r$body)) })
## the requests arrive in one TLS record, so most of them are only in the TLS
## buffers (not the socket) once the first one has been read
test("HTTPS: 20 pipelined requests on one connection", {
    paths <- rep(c("/static/hello.txt", "/echo"), 10)
    x <- tls(HTTPS, do.call(c, lapply(seq_along(paths), function(i) req.str(paths[i], close=(i == 20)))))
    r <- responses(x)
    expect(length(r) == 20, paste(length(r), "responses", x$err))
    expect(all(vapply(r, function(o) isTRUE(o$status == 200), TRUE)))
    expect(identical(vapply(r, `[[`, "", "text"), rep(c("hello world\n", "method=GET;body=0;sum=0"), 10))) })

## the end of a large body and the next request share a TLS record: the body
## read takes only the rest of the body, the next request stays in the TLS
## buffer and the socket shows no more data
test("HTTPS: requests pipelined after large bodies", {
    sizes <- c(20000L, 40000L, 100000L)
    x <- tls(HTTPS, c(do.call(c, lapply(sizes, function(n) req.str("/echo", "POST", FALSE, as.raw(rep(1:7, length.out=n))))),
                      req.str("/static/hello.txt")))
    r <- responses(x)
    expect(length(r) == 4, paste(length(r), "responses", x$err))
    expect(identical(vapply(r, `[[`, "", "text"),
                     c(paste0("method=POST;body=", sizes, ";sum=", vapply(sizes, function(n) sum(rep(1:7, length.out=n)), 1)),
                       "hello world\n")), vapply(r, `[[`, "", "text")) })

test("client certificate alice accepted", {
    r <- tget(CLIENT, "/echo", "alice.pem"); expect(isTRUE(r$status == 200), r$err) })
test("client certificate *.example.org accepted", {
    r <- tget(CLIENT, "/echo", "bob.pem"); expect(isTRUE(r$status == 200), r$err) })
test("client certificate with other CN rejected", rejected(CLIENT, "carol.pem"))
test("client certificate from other CA rejected", rejected(CLIENT, "rogue.pem"))
test("missing client certificate rejected", rejected(CLIENT))
test("any CN accepted without tls.client.cn", {
    r <- tget(ANYCLI, "/echo", "carol.pem"); expect(isTRUE(r$status == 200), r$err) })
test("client certificate from other CA rejected (any CN)", rejected(ANYCLI, "rogue.pem"))
test("missing client certificate rejected (any CN)", rejected(ANYCLI))
test("client certificate ignored without tls.client.ca", {
    r <- tget(HTTPS, "/echo", "rogue.pem"); expect(isTRUE(r$status == 200), r$err) })

## the client identity is passed to handlers in the Request-Client-CN header
whoami <- function(port, cert=NULL, ...) { r <- tget(port, "/whoami", cert, ...); if (is.na(r$status)) r$err else r$text }
test("Request-Proto and Request-Client-CN headers", {
    expect(identical(r <- get("/whoami")$text, "http;-;0"), r)
    expect(identical(r <- whoami(HTTPS), "https;-;0"), r)
    expect(identical(r <- whoami(HTTPS, "alice.pem"), "https;-;0"), r) # not requested without tls.client.ca
    expect(identical(r <- whoami(CLIENT, "alice.pem"), "https;alice;1"), r)
    expect(identical(r <- whoami(CLIENT, "bob.pem"), "https;bob.example.org;1"), r)
    expect(identical(r <- whoami(ANYCLI, "carol.pem"), "https;carol;1"), r) })
test("Request-Client-CN cannot be set by the client", {
    fake <- c("Request-Client-CN: admin", "Request-Proto: ftp")
    expect(identical(r <- get("/whoami", headers=list("Request-Client-CN"="admin", "Request-Proto"="https"))$text, "http;-;0"), r)
    expect(identical(r <- whoami(HTTPS, hdr=fake), "https;-;0"), r)
    expect(identical(r <- whoami(CLIENT, "alice.pem", hdr=fake), "https;alice;1"), r) })

test("plain HTTP on the HTTPS port fails, server unaffected", {
    con <- socketConnection(HOST, HTTPS, blocking=TRUE, open="r+b", timeout=10); on.exit(close(con))
    send(con, "GET /echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    r <- read.resp(con)
    expect(!isTRUE(r$status == 200), format(r))
    https.healthy() })
test("garbage and aborted handshakes, server unaffected", {
    for (i in 1:5) {
        con <- socketConnection(HOST, HTTPS, blocking=TRUE, open="r+b", timeout=10)
        send(con, if (i %% 2) as.raw(c(0x16, 0x03, 0x01, 0x00, 0x10, 1:4)) else as.raw(sample(0:255, 300, TRUE)))
        close(con)
    }
    https.healthy()
    r <- get("/static/hello.txt"); expect(r$status == 200) })
test("idle connection does not block HTTPS", skip=if (MODE == "single") "connections are served one at a time", {
    con <- socketConnection(HOST, HTTPS, blocking=TRUE, open="r+b", timeout=10); on.exit(close(con))
    t0 <- Sys.time()
    https.healthy()
    expect(elapsed(t0) < 10, elapsed(t0)) })

## runs openssl <args> in the background with its own input and output files.
## On Windows system2() opens the files without sharing (so they could neither
## be used by several processes nor read while it runs), a redirection by cmd
## allows that (see rscript.bg())
openssl.bg <- function(args, input, output) {
    if (WIN)
        system2("cmd", c("/c", paste0('"', paste(shQuote(OPENSSL), paste(args, collapse=" "),
                                                 "<", shQuote(input), ">", shQuote(output), "2>NUL"), '"')), wait=FALSE)
    else
        system2(OPENSSL, args, stdin=input, stdout=output, stderr=FALSE, wait=FALSE)
}
test("10 parallel HTTPS clients", {
    n <- 10
    f.in <- paste0(tempfile("in"), 1:n); f.out <- paste0(tempfile("out"), 1:n); on.exit(unlink(c(f.in, f.out)))
    args <- c("s_client", "-quiet", "-connect", paste0(HOST, ":", HTTPS), "-CAfile", shQuote(file.path(CERTS, "ca.pem")))
    for (i in 1:n) {
        writeBin(req.str("/echo"), f.in[i])
        openssl.bg(args, f.in[i], f.out[i])
    }
    done <- function() sum(vapply(f.out, function(f)
        isTRUE(tryCatch(grepl("method=GET;body=0;sum=0", rawToChar(readBin(f, "raw", 1e4)), fixed=TRUE),
                        error=function(e) FALSE, warning=function(w) FALSE)), TRUE))
    expect(wait.for(done() == n, 30), paste(done(), "of", n, "completed")) })

test("server alive", { expect(server.alive()); https.healthy() })
