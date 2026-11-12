## Minimal smoke test run by R CMD check: starts a server in a separate R
## process and checks that it serves static files and R handlers.
## The full test suite is in tests/webserver (see the README there).
library(webserver)

## argument checks (no server needed)
stopifnot(inherits(try(addHandler("bogus", "/"), silent=TRUE), "try-error"))
rmAllHandlers()
stopifnot(is.logical(hasTLS()))
## HTTPS is tested if supported, the client is curlGetHeaders() (without
## verification since the certificate is not trusted)
tls <- hasTLS() && capabilities("libcurl")
certs <- normalizePath("tls", mustWork=FALSE)
if (hasTLS() && !tls) cat("libcurl is not available, skipping the HTTPS test\n")

dir <- tempfile("wstest")
dir.create(file.path(dir, "root"), recursive=TRUE)
writeBin(charToRaw("hello world\n"), file.path(dir, "root", "hello.txt"))
log <- file.path(dir, "server.log")
script <- file.path(dir, "server.R")
writeLines(c(
    'args <- commandArgs(TRUE)',
    'cat("PID", Sys.getpid(), "\\n"); flush(stdout())',
    'library(webserver)',
    'addHandler("static", "/static/", path=paste0(args[2], "/"))',
    'addHandler("rfun", "/echo", function(url, query, body, headers, method)',
    '    list(paste0("method=", method, ";query=", paste(names(query), query, sep="=", collapse="&"),',
    '                ";body=", if (length(body)) rawToChar(body) else ""), "text/plain"))',
    'if (args[3] == "NA") webstart(as.integer(args[1]), threads=2L) else',
    '    webstart(c(args[1], paste0("https://localhost:", args[3])), threads=2L,',
    '             tls.cert=file.path(args[4], "server-chain.pem"), tls.key=file.path(args[4], "server.key"))'), script)

## minimal HTTP client (no proxies or libcurl involved)
http <- function(port, req) {
    con <- socketConnection("127.0.0.1", port, blocking=TRUE, open="r+b", timeout=10)
    on.exit(close(con))
    writeBin(charToRaw(req), con); flush(con)
    res <- raw()
    while (length(r <- readBin(con, "raw", 65536))) res <- c(res, r)
    res <- rawToChar(res)
    list(status=as.integer(sub("^HTTP/1\\.[01] ([0-9]+).*", "\\1", res)),
         body=sub("^.*?\r\n\r\n", "", res))
}

## a random port we can listen on and connect to via loopback, NA if there is none
## (some build/check environments don't allow that)
free.port <- function() {
    for (i in 1:5) {
        p <- sample(20000:60000, 1)
        if (!exists("serverSocket")) return(p) # R < 4.0: cannot check
        s <- tryCatch(serverSocket(p), error=function(e) NULL, warning=function(w) NULL)
        if (is.null(s)) next
        ok <- tryCatch({ close(socketConnection("127.0.0.1", p, open="r+b", timeout=2)); TRUE },
                       error=function(e) FALSE, warning=function(w) FALSE)
        close(s)
        if (ok) return(p)
    }
    NA
}
connects <- function(port)
    tryCatch({ close(socketConnection("127.0.0.1", port, open="r+b", timeout=1)); TRUE },
             error=function(e) FALSE, warning=function(w) FALSE)

pid <- NA
port <- free.port()
if (is.na(port)) {
    cat("cannot listen on a loopback port, skipping the server tests\n")
} else tryCatch({
    started <- FALSE
    for (i in 1:5) { # the port may have been taken in the meantime, so retry with another one
        if (i > 1 && is.na(port <- free.port())) break
        rscript <- file.path(R.home("bin"), "Rscript")
        tls.port <- if (tls) free.port() else NA
        if (tls && (is.na(tls.port) || tls.port == port)) next
        args <- c(shQuote(script), port, shQuote(file.path(dir, "root")), tls.port, shQuote(certs))
        ## on Windows system2() opens the log without sharing, so it could not be
        ## read while the server runs; a redirection by cmd allows that
        if (.Platform$OS.type == "windows")
            system2("cmd", c("/c", paste0('"', paste(shQuote(rscript), paste(args, collapse=" "),
                                                     ">", shQuote(log), "2>&1"), '"')), wait=FALSE)
        else
            system2(rscript, args, stdout=log, stderr=log, wait=FALSE)
        for (k in 1:100) {
            Sys.sleep(0.1)
            l <- if (file.exists(log)) grep("^PID ", readLines(log, warn=FALSE), value=TRUE) else character()
            if (length(l)) pid <- as.integer(sub("^PID ([0-9]+).*", "\\1", l[1]))
            if (!is.na(pid) && connects(port)) {
                started <- TRUE
                break
            }
        }
        if (started) break
        if (!is.na(pid)) tools::pskill(pid)
        pid <- NA
    }
    if (!started) stop("cannot start the server:\n", paste(readLines(log, warn=FALSE), collapse="\n"))

    r <- http(port, "GET /static/hello.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    stopifnot(r$status == 200, identical(r$body, "hello world\n"))
    r <- http(port, "GET /static/missing.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    stopifnot(r$status == 404)
    r <- http(port, "GET /echo?a=1&b=x%20y HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    stopifnot(r$status == 200, identical(r$body, "method=GET;query=a=1&b=x y;body="))
    r <- http(port, "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello")
    stopifnot(r$status == 200, identical(r$body, "method=POST;query=;body=hello"))
    if (tls) {
        Sys.setenv(no_proxy="*", NO_PROXY="*")
        h <- curlGetHeaders(paste0("https://127.0.0.1:", tls.port, "/static/hello.txt"), verify=FALSE)
        stopifnot(attr(h, "status") == 200)
        cat("HTTPS OK\n")
    }
    cat("all OK\n")
}, finally={
    if (!is.na(pid)) tools::pskill(pid)
})
unlink(dir, recursive=TRUE)
