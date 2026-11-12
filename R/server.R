addHandler <- function(type, prefix, ...)
    UseMethod("addHandler")

addHandler.character <- function(type, prefix, ...) {
    if (identical(type, "static"))
        add.static.handler(prefix, ...)
    else
        .External(C_add_handler, type, prefix, ...)
}

## the static handler appends the rest of the URL path to its path, so it
## must end with a slash and be absolute (the working directory may change)
add.static.handler <- function(prefix, path, ...) {
    if (!is.character(path) || length(path) != 1L || is.na(path))
        stop("path must be a string")
    path <- path.expand(path)
    if (!dir.exists(path))
        stop("path '", path, "' does not exist")
    ## we need the absolute path. normalizePath() has issues on Windows
    ## and we don't really care about canonicalization so going through
    ## setwd+getwd is the best way.
    wd <- setwd(path)
    path <- tryCatch(getwd(), finally=setwd(wd))
    .External(C_add_handler, "static", prefix, paste0(sub("/$", "", path), "/"), ...)
}

rmAllHandlers <- function()
    .Call(C_remove_all_handlers)

simple <- function(listen=8080L, path=".", prefix="/", index="index.html", start=TRUE,
                   Rfun=get0(".http.request"), Rfun.prefix="/", ...) {
    if (is.character(path))
        addHandler("static", prefix, path, index=index)
    if (is.function(Rfun))
        addHandler("rfun", Rfun.prefix, Rfun)
    if (start) {
        on.exit(rmAllHandlers())
        webstart(listen, ...)
    }
    invisible(TRUE)
}

hasTLS <- function()
    .Call(C_has_tls)

## parses the listen= specification of webstart() into a list of
## listeners as expected by C_start
parse.listen <- function(listen) {
    if (!length(listen) || !(is.character(listen) || is.numeric(listen)))
        stop("listen must be a port number or a character vector of listener specifications")
    lapply(as.character(listen), function(s) {
        if (is.na(s))
            stop("listen cannot contain NA")
        bad <- function(why) stop("invalid listener '", s, "': ", why, call.=FALSE)
        ## this is the list of all available specs with their defaults
        l <- list(spec=s, proto="http", port=NA_integer_, local=TRUE, ipv6=FALSE, address=NULL, socket=NULL, tls=NULL)
        ## just plain port -> http localhost listen
        if (grepl("^[0-9]+$", s)) {
            port <- s
            l$spec <- paste0("http://localhost:", s)
        } else if (grepl("^unix:", s, ignore.case=TRUE)) { ## unix socket
            if (.Platform$OS.type == "windows")
                bad("unix sockets are not supported on Windows")
            path <- path.expand(sub("^unix:", "", s, ignore.case=TRUE))
            if (!nzchar(path))
                bad("missing socket path")
            ## NOTE: we leave the path relative to avoid path limit issues (~100 chars
            ##       on most systems), and it is not used otherwise
            if (utils::file_test("-f", path) || dir.exists(path))
                bad("the path exists and is not a socket")
            l$proto <- "unix"
            l$port <- 0L
            l$socket <- path
            return(l)
        } else {
            ## <proto>://<host>[:<port>][/]  where <host> can be [<IPv6>]
            m <- regmatches(s, regexec("^(https?)://(\\[[^]]*\\]|[^]/:[]*)(:([0-9]*))?/?$", s,
                                       ignore.case=TRUE))[[1L]]
            if (!length(m))
                bad("must be a port number, http://<host>:<port>, https://<host>:<port> or unix:<path>")
            l$proto <- tolower(m[2L])
            port <- if (nzchar(m[5L])) m[5L] else if (l$proto == "https") "443" else "80"
            host <- tolower(m[3L])
            if (host == "*")
                l$local <- FALSE
            else if (host != "localhost") { ## a numeric address (IPv4 or 6)
                l$local <- FALSE
                if (grepl("^\\[.*\\]$", host)) {
                    l$ipv6 <- TRUE
                    l$address <- substr(host, 2L, nchar(host) - 1L)
                } else if (grepl("^(0|[1-9][0-9]{0,2})(\\.(0|[1-9][0-9]{0,2})){3}$", host) &&
                           all(as.integer(strsplit(host, ".", fixed=TRUE)[[1L]]) <= 255L))
                    l$address <- host  ## only if it is a valid IPv4 address
                else
                    bad(paste("the host must be localhost (local connections only), * (all interfaces),",
                              "a numeric IPv4 address or an IPv6 address in [] (host names are not supported)"))
            }
        }
        port <- suppressWarnings(as.integer(port))
        if (is.na(port) || port > 65535L)
            bad("invalid port")
        l$port <- port
        l
    })
}

webstart <- function(listen=8080L, threads=4L, fork=FALSE,
                     tls.cert=NULL, tls.key=tls.cert, tls.ca=NULL,
                     tls.client.ca=NULL, tls.client.cn=NULL) {
    l <- parse.listen(listen)

    ## check for https listeners are they need TLS specs
    https <- which(vapply(l, function(o) o$proto == "https", TRUE))
    nh <- length(https)

    ## options for the different files we accept for the TLS specs
    ## we have to expand them, check presence etc.
    files <- list(cert=tls.cert, key=tls.key, ca=tls.ca, client.ca=tls.client.ca)

    if (!nh) { ## no TLS listeners requested
        if (!all(vapply(files, is.null, TRUE)) || !is.null(tls.client.cn))
            stop("tls.* arguments are only allowed with https:// listeners")
    } else { ## at least one TLS listener
        if (!hasTLS())
            stop("TLS support is not available in this build of webserver (it requires OpenSSL 1.1.0 or higher)")

        ## the tls.* arguments are recycled over the https listeners
        recycle <- function(x, name) {
            if (!length(x) %in% c(1L, nh))
                stop(name, " has ", length(x), " elements, but there ",
                     if (nh == 1L) "is 1 https listener" else paste("are", nh, "https listeners"), call.=FALSE)
            rep(x, length.out=nh)
        }
        ## for each of the argument recycle it, expand and check presence
        for (n in names(files)) {
            name <- paste0("tls.", n)
            f <- files[[n]]
            files[[n]] <- if (is.null(f))
                rep(NA_character_, nh)
            else {
                if (!is.character(f) && !all(is.na(f)))
                    stop(name, " must be a character vector of file names")
                f <- recycle(as.character(f), name)
                f[!is.na(f)] <- path.expand(f[!is.na(f)])
                if (any(miss <- !is.na(f) & !file.exists(f)))
                    stop("file '", f[miss][1L], "' specified in ", name, " does not exist")
                f
            }
        }

        ## the key defaults to the certificate file (also per listener)
        files$key[is.na(files$key)] <- files$cert[is.na(files$key)]

        ## handle the client cert spec (not a file)
        cn <- tls.client.cn
        if (!is.list(cn))
            cn <- list(cn)
        cn <- recycle(cn, "tls.client.cn")

        ## construct the specs for each https listener
        for (i in seq_len(nh)) {
            spec <- l[[https[i]]]$spec
            if (is.na(files$cert[i]))
                stop("https listener ", spec, " requires tls.cert to be set")
            ci <- cn[[i]]
            if (!is.null(ci)) {
                if (!is.character(ci) || !length(ci) || anyNA(ci) || !all(nzchar(ci)))
                    stop("tls.client.cn must be a character vector of names (or a list of those)")
                if (!all(!grepl("*", ci, fixed=TRUE) | grepl("^\\*[^*]*$", ci) | grepl("^[^*]+\\*$", ci)))
                    stop("names in tls.client.cn can only have a * at the beginning or the end")
                if (is.na(files$client.ca[i]))
                    stop("tls.client.cn requires tls.client.ca to be set (https listener ", spec, ")")
                ci <- enc2utf8(ci)
            }
            l[[https[i]]]$tls <- list(cert=files$cert[i], key=files$key[i], ca=files$ca[i],
                                      client.ca=files$client.ca[i], client.cn=ci)
        }
    }
    if (isTRUE(fork)) {
        if (.Platform$OS.type == "windows")
            stop("fork=TRUE is not supported on Windows")
        if (missing(threads))
            threads <- 0L
    }
    .Call(C_start, l, threads, fork)
}
