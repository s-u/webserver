## Fixture for test-rqueue.R: R handlers evaluated in-process (no forking).
## Usage: Rscript rqueue.R --port=<port> --docroot=<dir> --threads=<n> [--lib=<lib>]
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
## AST evaluation uses (much) more C stack than byte code, so this makes the
## recursion tests exercise the C stack of the thread running the handler
compiler::enableJIT(0)
options(expressions=500000)

inflight <- FALSE   # set while a handler body runs
overlaps <- 0L      # handler bodies that found another one running
served   <- 0L
## creates the file named by the m query parameter in the docroot, so the
## tests can tell when a handler has started
mark <- function(query) if ("m" %in% names(query)) file.create(file.path(docroot, basename(query[["m"]])))
rec <- function(n) if (n <= 0) 0 else 1 + rec(n - 1)

handler <- function(url, query, body, headers, method) {
    if (inflight) overlaps <<- overlaps + 1L
    inflight <<- TRUE
    on.exit(inflight <<- FALSE)
    served <<- served + 1L
    n <- if (!is.null(query) && "n" %in% names(query)) query[["n"]] else ""
    switch(url,
           ## echoes n after yielding the CPU, so an overlapping evaluation would be noticed
           "/id"      = { Sys.sleep(0.002); list(paste0("id=", n), "text/plain") },
           "/sleep"   = { mark(query); Sys.sleep(as.numeric(n)); list("slept", "text/plain") },
           "/err"     = stop("err ", n),
           "/warn"    = { warning("w ", n); list("warned", "text/plain") },
           "/gc"      = { x <- lapply(1:2000, function(i) runif(100)); invisible(gc()); list(as.character(length(x)), "text/plain") },
           "/recurse" = list(as.character(rec(as.integer(n))), "text/plain"),
           "/stats"   = list(paste0("overlaps=", overlaps, ";served=", served), "text/plain"),
           "/refuse"  = FALSE,
           FALSE)
}

addHandler("static", "/static/", path=paste0(docroot, "/"))
addHandler("rfun", "/", handler)
cat("cstack before:", Cstack_info()[["size"]], "\n")
cat("READY\n")
webstart(port, threads=threads)
## webstart() returns after SIGINT; the session must be fully usable afterwards
cat("cstack after:", Cstack_info()[["size"]], "\n")
r <- tryCatch(rec(1e6), error=function(e) conditionMessage(e))
cat("deep recursion after server stopped:", r, "\n")
cat("DONE\n")
