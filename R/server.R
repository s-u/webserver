addHandler <- function(type, prefix, ...)
    UseMethod("addHandler")

addHandler.character <- function(type, prefix, ...)
    .External(C_add_handler, type, prefix, ...)

rmAllHandlers <- function()
    .Call(C_remove_all_handlers)

simple <- function(port=8080L, path=".", prefix="/", index="index.html", start=TRUE,
                   Rfun=get0(".http.request"), Rfun.prefix="/",
                   threads = 4L, ...) {
    if (is.character(path)) {
        path <- path.expand(path)
        if (!dir.exists(path))
            stop("path '", path, "' does not exist")
        wd <- getwd()
        setwd(path)
        path <- getwd()
        setwd(wd)
        addHandler("static", prefix, path=paste0(path,"/"), index=index)
    }
    if (is.function(Rfun))
        addHandler("rfun", Rfun.prefix, Rfun)
    if (start) {
        on.exit(rmAllHandlers())
        webstart(port, threads, ...)
    }
    invisible(TRUE)
}

webstart <- function(port=8080L, threads=4L, fork=FALSE, socket, ipv6=FALSE, local=TRUE)
    .Call("C_start", port, if (missing(socket)) NULL else socket, threads, fork, ipv6, local)
