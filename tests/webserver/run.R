## Test driver for the webserver package. It tests the installed package,
## nothing is built or installed. Requires permission to bind local TCP ports.
##
## Usage: Rscript run.R [options] [pattern]
##   --suites=spec,http,rqueue,rfork   suites to run (default: all)
##   --modes=threads,single,rfunfork,connfork
##                    server modes for the http suite (default: all)
##   --threads=<n>    processing threads of the servers (default: 4),
##                    the rqueue and rfork suites also run with 0 threads
##   --lib=<path>     load webserver from this library (also in the servers)
##   --slow           include slow tests
##   pattern          only run tests whose name, including the "[suite] "
##                    prefix, matches this regular expression
##
## Example: Rscript run.R --suites=http --modes=single chunked
args <- commandArgs(TRUE)
opt <- function(name, default=NULL) {
    a <- grep(paste0("^--", name, "(=|$)"), args, value=TRUE)
    if (!length(a)) default else if (grepl("=", a[1])) sub("^--[^=]*=", "", a[1]) else TRUE
}
split.opt <- function(x) strsplit(x, ",", fixed=TRUE)[[1]]
known <- c("suites", "modes", "threads", "lib", "slow")
bad <- setdiff(sub("^--([^=]*).*", "\\1", grep("^--", args, value=TRUE)), known)
if (length(bad)) stop("unknown option(s): ", paste0("--", bad, collapse=", "))
SUITES  <- split.opt(opt("suites", "spec,http,rqueue,rfork"))
MODES   <- split.opt(opt("modes", "threads,single,rfunfork,connfork"))
THREADS.OPT <- as.integer(opt("threads", 4L))
LIB     <- opt("lib")
SLOW    <- isTRUE(opt("slow", FALSE))
PATTERN <- paste(grep("^--", args, value=TRUE, invert=TRUE), collapse="|")

HERE <- local({
    a <- grep("^--file=", commandArgs(FALSE), value=TRUE)
    if (length(a)) dirname(normalizePath(sub("^--file=", "", a[1]))) else getwd()
})
if (!is.null(LIB)) {
    LIB <- normalizePath(LIB, mustWork=TRUE)
    .libPaths(c(LIB, .libPaths()))
}
suppressMessages(library(webserver))
cat("Testing webserver ", format(packageVersion("webserver")), " from ", dirname(find.package("webserver")),
    "\nR ", R.version$major, ".", R.version$minor, " (", R.version$platform, ")\n", sep="")

source(file.path(HERE, "helpers.R"))

## names of the tests in the files, so that suites without any tests matching
## the pattern don't have to start a server
matches <- function(label, files) {
    if (!nzchar(PATTERN)) return(TRUE)
    names <- unlist(lapply(files, function(f)
        regmatches(l <- readLines(file.path(HERE, f), warn=FALSE), regexpr('(?<=^test\\(")[^"]+', l, perl=TRUE))))
    any(grepl(PATTERN, paste0("[", label, "] ", names)))
}

## runs the test files with a fresh server started from the fixture (if any);
## each suite gets its own environment holding `vars' (e.g. MODE, THREADS)
run.suite <- function(label, files, fixture=NULL, args=character(), skip=NULL, vars=list()) {
    if (!matches(label, files)) return(invisible())
    cat("\n==", label, "==\n")
    if (!is.null(skip)) {
        cat("skipped (", skip, ")\n", sep="")
        return(invisible())
    }
    CONTEXT <<- paste0("[", label, "] ")
    on.exit({ kill.server(); srv <<- NULL })
    if (!is.null(fixture)) {
        err <- tryCatch({ start.server(fixture, args); NULL }, error=function(e) conditionMessage(e))
        if (!is.null(err)) {
            cat(err, "\n")
            results[paste0(CONTEXT, "server start")] <<- FALSE
            return(invisible())
        }
    }
    env <- list2env(vars, envir=new.env(parent=globalenv()))
    for (f in files) {
        err <- tryCatch({ source(file.path(HERE, f), local=env); NULL }, error=function(e) conditionMessage(e))
        if (!is.null(err)) {
            cat("Error in ", f, ": ", err, "\n", sep="")
            results[paste0(CONTEXT, f)] <<- FALSE
        }
    }
}

no.fork <- if (WIN) "needs fork(), not available on Windows"
DOCROOT <- make.docroot()
tryCatch({
    if ("spec" %in% SUITES)
        run.suite("spec", "test-spec.R")
    if ("http" %in% SUITES) for (mode in MODES) {
        threads <- if (mode == "single") 0L else THREADS.OPT
        run.suite(paste0("http/", mode), c("test-http.R", "test-conc.R"), "server.R",
                  c(paste0("--mode=", mode), paste0("--threads=", threads)),
                  skip=if (mode %in% c("rfunfork", "connfork")) no.fork,
                  vars=list(MODE=mode, THREADS=threads))
    }
    for (suite in intersect(c("rqueue", "rfork"), SUITES))
        for (threads in unique(c(THREADS.OPT, 0L)))
            run.suite(paste0(suite, "/", threads), paste0("test-", suite, ".R"), paste0(suite, ".R"),
                      paste0("--threads=", threads), skip=if (suite == "rfork") no.fork,
                      vars=list(THREADS=threads))
}, finally={
    kill.server()
    unlink(dirname(DOCROOT), recursive=TRUE)
})

n.fail <- sum(!results)
cat(sprintf("\n%d tests, %d failed%s\n", length(results), n.fail,
            if (skipped) sprintf(", %d skipped", skipped) else ""))
if (n.fail) cat("Failed:\n", paste0("  ", names(results)[!results], "\n"), sep="")
quit(status=if (n.fail) 1L else 0L)
