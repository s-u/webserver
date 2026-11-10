## Fixture for test-rfork.R: R handler evaluated in forked worker processes.
## Usage: Rscript rfork.R --port=<port> --docroot=<dir> --threads=<n> [--lib=<lib>]
args <- commandArgs(TRUE)
arg <- function(name, default=NULL) {
    a <- grep(paste0("^--", name, "="), args, value=TRUE)
    if (length(a)) sub("^--[^=]*=", "", a[1]) else default
}
if (!is.null(arg("lib"))) .libPaths(c(arg("lib"), .libPaths()))
cat("PID", Sys.getpid(), "\n"); flush(stdout()) # the driver needs the pid of the R process
port    <- as.integer(arg("port"))
docroot <- arg("docroot")
threads <- as.integer(arg("threads"))

library(webserver)
compiler::enableJIT(0) # see rqueue_server.R
options(expressions=500000)

state <- "parent"
rec <- function(n) if (n <= 0) 0 else 1 + rec(n - 1)

## evaluated in the server's own process: lets the tests look at the parent
parent <- function(url, query, body, headers, method)
    switch(url,
           "/parent/pid"   = list(as.character(Sys.getpid()), "text/plain"),
           "/parent/state" = list(state, "text/plain"),
           FALSE)

worker <- function(url, query, body, headers, method) {
    n <- if (!is.null(query) && "n" %in% names(query)) query[["n"]] else ""
    switch(url,
           "/pid"     = list(as.character(Sys.getpid()), "text/plain"),
           "/id"      = list(paste0("id=", n, ";pid=", Sys.getpid()), "text/plain"),
           "/echo"    = list(paste0("method=", method, ";len=", length(body), ";ct=",
                                    if (is.null(attr(body, "content-type"))) "" else attr(body, "content-type"),
                                    ";sum=", sum(as.integer(body))), "text/plain"),
           "/big"     = list(as.raw(rep(1:250, length.out=as.numeric(n))), "application/octet-stream"),
           "/hdr"     = list("ok", "text/plain", c("X-Worker: yes"), 201L),
           "/set"     = { state <<- n; list(state, "text/plain") },
           "/state"   = list(state, "text/plain"),
           "/sleep"   = { Sys.sleep(as.numeric(n)); list("slept", "text/plain") },
           "/err"     = stop("err ", n),
           "/die"     = tools::pskill(Sys.getpid(), tools::SIGKILL),
           "/recurse" = list(as.character(rec(as.integer(n))), "text/plain"),
           "/tmpfile" = { f <- tempfile(); writeLines("temporary", f)
                          list(tmpfile=f, "text/plain", paste0("X-Tmpfile: ", f)) },
           FALSE)
}

addHandler("rfun", "/parent/", parent)
addHandler("rfun", "/", worker, fork=TRUE)
## only reached if the worker refuses the request
addHandler("static", "/static/", path=paste0(docroot, "/"))
webstart(port, threads=threads)
cat("STOPPED\n")
## the tests check that the workers are gone and the port is free while we are still alive
Sys.sleep(2)
r <- tryCatch(rec(1e6), error=function(e) conditionMessage(e))
cat("deep recursion after server stopped:", r, "\n")
cat("DONE\n")
