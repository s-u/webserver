fds2file <- function(filename, fds, append=TRUE, mode="666", close=FALSE)
    .Call(path.expand(filename), as.integer(fds), append, as.octmode(mode))

R.run <- function(expr, out, where=parent.frame(), bquote=TRUE) {
    ## the . part of bquote
    unquote <- function(e) {
        if (is.pairlist(e)) 
            as.pairlist(lapply(e, unquote))
        else if (is.call(e)) {
            if (is.name(e[[1L]]) && as.character(e[[1]]) == ".") 
                eval(e[[2L]], where)
            else as.call(lapply(e, unquote))
        } else e
    }
    if (bquote)
        expr <- unquote(substitute(expr))

    f <- pipe(paste(shQuote(file.path(R.home("bin"),"R")),
                    "--no-echo --no-restore -e 'webserver:::R.run.stdin()' >/dev/null 2>&1"), "w")
    on.exit(close(f))
    serialize(list(expr=expr, out=out), f, TRUE)
}

## Internal function acting as client code for R.run: reads
## serialized object from stdin and evaluates it accordingly.
R.run.stdin <- function() {
    f <- file("stdin", "r")
    l <- unserialize(f)
    if (is.character(l$out))
        fds2file(l$out, 1:2, TRUE, as.octmode("666"))
    expr <- l$expr
    close(f)
    on.exit(quit("no"))
    eval(expr)
}
