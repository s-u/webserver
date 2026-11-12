## Fixture for test-http.R and test-conc.R.
## Usage: Rscript server.R --port=<port> --docroot=<dir> [--threads=4] [--mode=threads] [--lib=<lib>]
## mode: threads (default; R evaluated in-process behind a mutex), single (threads=0),
##       rfunfork (rfun handler delegates to forked R workers), connfork (fork per connection)
args <- commandArgs(TRUE)
arg <- function(name, default=NULL) {
    a <- grep(paste0("^--", name, "="), args, value=TRUE)
    if (length(a)) sub("^--[^=]*=", "", a[1]) else default
}
if (!is.null(arg("lib"))) .libPaths(c(arg("lib"), .libPaths()))
cat("PID", Sys.getpid(), "\n"); flush(stdout()) # the driver needs the pid of the R process
port    <- as.integer(arg("port"))
docroot <- arg("docroot")
threads <- as.integer(arg("threads", 4L))
mode    <- arg("mode", "threads")
fork    <- mode == "rfunfork"
if (mode == "single") threads <- 0L

library(webserver)

## creates the file named by the m query parameter in the docroot, so the
## tests can tell when a handler has started
mark <- function(query) if ("m" %in% names(query)) file.create(file.path(docroot, basename(query[["m"]])))

handler <- function(url, query, body, headers, method) {
    if (url == "/echo") {
        q <- if (is.null(query)) "" else paste(names(query), query, sep="=", collapse="&")
        ct <- attr(body, "content-type")
        return(list(paste0("method=", method, ";query=", q, ";body=", length(body),
                           ";ct=", if (is.null(ct)) "" else ct, ";bodytxt=",
                           if (length(body)) rawToChar(body) else ""),
                    "text/plain"))
    }
    if (url == "/hdr")        # reflect request headers
        return(list(paste(names(headers), vapply(headers, function(x) paste(x, collapse=","), ""), sep=": ", collapse="\n"), "text/plain"))
    if (url == "/custom-headers")
        return(list("ok", "text/plain", c("X-Test: 1", "X-Other: two")))
    if (grepl("^/code/", url))
        return(list("code", "text/plain", character(), as.integer(sub("^/code/", "", url))))
    if (url == "/raw")
        return(list(as.raw(0:255), "application/octet-stream"))
    if (url == "/empty")
        return(list(raw(0), "text/plain"))
    if (url == "/big")
        return(list(as.raw(rep(1:250, length.out=5e6)), "application/octet-stream"))
    if (url == "/file")
        return(list(file=file.path(docroot, "hello.txt"), "text/plain"))
    if (url == "/tmpfile") {
        f <- tempfile(); writeBin(charToRaw("temporary\n"), f) # no CRLF on Windows
        return(list(tmpfile=f, "text/plain", paste0("X-Tmpfile: ", f)))
    }
    if (url == "/pid") return(list(as.character(Sys.getpid()), "text/plain"))
    ## leaves a trace in the file system, so the tests can tell whether the
    ## handler was called (also works with forked workers)
    if (url == "/touch") {
        file.create(file.path(docroot, basename(query[["f"]])))
        return(list("touched", "text/plain"))
    }
    if (url == "/missing-file")
        return(list(file="/nonexistent/file.txt", "text/plain"))
    if (url == "/a+b") return(list("plus", "text/plain"))
    if (url == "/error")  stop("boom")
    if (url == "/null")   return(NULL)
    if (url == "/refuse") return(FALSE)
    if (url == "/utf8")
        return(list("héllo", "text/plain; charset=utf-8"))
    if (url == "/sleep") { mark(query); Sys.sleep(1); return(list("slept", "text/plain")) }
    if (url == "/conn-close") return(list("bye", "text/plain", "Connection: close"))
    FALSE
}

## rewrites must come first
webserver::addHandler("rewrite", "/rw/?/?", "/echo?a=$1&b=$2")
webserver::addHandler("rewrite", "/rwf/", "/static/?k=v")
webserver::addHandler("static", "/static/", path=docroot, index="index.html", stop=FALSE)
## reflects the (decoded) body: <class>:<deparsed value>
webserver::addHandler("rfun", "/parsed", function(url, query, body, ...)
    list(paste0(class(body)[1L], ":", paste(deparse(body), collapse="")), "text/plain"),
    parse.body=c(webserver::defaultBodyParsers(),
                 list("text/*"=function(body, content.type) toupper(rawToChar(body)))),
    fork=fork)
webserver::addHandler("rfun", "/", handler, fork=fork)
cat("READY\n", file=stderr())
flush(stderr())
if (mode == "connfork") {
    webserver::webstart(port, threads=0L, fork=TRUE)
} else
    webserver::webstart(port, threads=threads, fork=FALSE)
