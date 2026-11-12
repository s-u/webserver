## Network-free tests for the declarative API compiler (R/spec.R) and R helpers.
## Evaluated in the driver's R session, no server is started.
ns <- asNamespace("webserver")
## the spec compiler references these as symbols in formals, they are never called
required <- function(...) NULL
optional <- function(...) NULL
args.of <- function(fn) environment(ns$env2spec(list(f=fn))$f)$.args

a <- args.of(function(a=required, b=required(as.numeric), c=optional(5, as.numeric), d="x", e=1L, f) NULL)
test("bare `arg` (no default) is required", expect(a$f$required))
test("arg=required is required", expect(a$a$required))
test("arg=required(conv) is required", expect(a$b$required))
test("arg=optional(def, conv) is optional w/ default", expect(!a$c$required && identical(a$c$default, 5)))
test("arg=\"x\" is optional with default \"x\"", expect(!a$d$required && identical(a$d$default, "x")))
test("arg=1L is optional with default 1L", expect(!a$e$required && identical(a$e$default, 1L)))
test("optional(conv) conversion applied", expect(identical(a$c$convert("7"), 7)))

## end-to-end through spec() (which needs jsonlite)
if (requireNamespace("jsonlite", quietly=TRUE)) {
    api <- ns$spec(list(add=function(x=required(as.numeric), y=optional(10, as.numeric)) x + y,
                        greet=function(name="world") paste("hello", name),
                        err=function() stop("nope")))
    ## results must be rfun responses: list(payload, content-type[, headers[, status]])
    json <- function(x) list(as.character(jsonlite::toJSON(x, auto_unbox=TRUE)), "application/json")
    test("spec: unknown path -> 404 response", expect(
           identical(api("/missing")[[4]], 404L) && identical(api("/missing")[[2]], "application/json")))
    test("spec: required param missing is reported as JSON", expect(
           grepl("Required parameter", api("/add", NULL, NULL, NULL)[[1]]) &&
           identical(api("/add", NULL, NULL, NULL)[[2]], "application/json")))
    test("spec: query args converted and added", expect(
           identical(api("/add", c(x="1", y="2"), NULL, NULL), json(3))))
    test("spec: default used when arg omitted", expect(
           identical(api("/greet", NULL, NULL, NULL), list("hello world", "text/plain"))))
    test("spec: error becomes a JSON response, not an R error", expect(
           identical(api("/err", NULL, NULL, NULL)[[2]], "application/json") &&
           grepl('"success":false', api("/err", NULL, NULL, NULL)[[1]])))
    api2 <- ns$spec(list(lst=function() list(a=1, b="x"),
                         js=function() jsonlite::toJSON(list(z=TRUE), auto_unbox=TRUE),
                         raw=function() I(list("low-level", "text/csv", character(), 202L))))
    test("spec: list result -> JSON", expect(identical(api2("/lst", NULL, NULL, NULL), json(list(a=1, b="x")))))
    test("spec: json result sent as-is", expect(identical(api2("/js", NULL, NULL, NULL), list('{"z":true}', "application/json"))))
    test("spec: I() result passed through as response", expect(
           identical(api2("/raw", NULL, NULL, NULL), list("low-level", "text/csv", character(), 202L))))
    test("spec: JSON body arguments", expect(
           identical(api("/add", NULL, structure(charToRaw('{"x":2,"y":5}'), "content-type"="application/json"), NULL), json(7))))
} else cat("(jsonlite not installed: skipping end-to-end spec tests)\n")

## fds2file / R.run helpers are unix-only conveniences; just check argument validation
test("addHandler rejects unknown type", expect.error(addHandler("bogus", "/")))
test("addHandler rejects non-string prefix", expect.error(addHandler("static", 1, path=".")))
test("addHandler static requires path", expect.error(addHandler("static", "/")))
rmAllHandlers()
test("simple() rejects nonexistent path", expect.error(simple(path="/definitely/not/here", start=FALSE)))
rmAllHandlers()
test("listen: schemes are case-insensitive", {
    l <- webserver:::parse.listen(c("HTTPS://*:8443", "Http://localhost:8080"))
    expect(identical(vapply(l, `[[`, "", "proto"), c("https", "http"))) })
test("listen: unix socket path stays relative", skip=if (WIN) "no unix sockets on Windows",
     expect(identical(webserver:::parse.listen("UNIX:rel/s")[[1]]$socket, "rel/s")))
test("addHandler static rejects nonexistent path", expect.error(addHandler("static", "/", "/definitely/not/here")))
test("addHandler static rejects NA index", expect.error(addHandler("static", "/", ".", index=NA)))
rmAllHandlers()

## body parsers: the wrapper created for parse.body is called directly
pb <- function(body, ct=NULL, parsers=defaultBodyParsers()) {
    if (is.character(body)) body <- charToRaw(body)
    if (!is.null(body)) attr(body, "content-type") <- ct
    webserver:::body.parsing.handler(function(path, query, body, headers, ...) body, parsers)("/", NULL, body, NULL, "POST")
}
is.400 <- function(r) is.list(r) && identical(r[[4L]], 400L)
test("parse.body: form", expect(identical(pb("a=1&b=x+y%21", "application/x-www-form-urlencoded; charset=utf-8"),
                                          c(a="1", b="x y!"))))
test("parse.body: form with NUL -> 400", expect(is.400(pb(as.raw(c(0x61, 0x3d, 0, 0x62)), "application/x-www-form-urlencoded"))))
test("parse.body: no body", expect(is.null(pb(NULL))))
test("parse.body: no content type -> raw", { r <- pb("abc"); expect(is.raw(r) && identical(rawToChar(r), "abc")) })
test("parse.body: unmatched type -> raw with content type", {
    r <- pb("abc", "application/octet-stream")
    expect(is.raw(r) && identical(attr(r, "content-type"), "application/octet-stream")) })
test("parse.body: exact match before patterns, patterns in order", {
    p <- list("*/plain"=function(b, ct) "suffix", "text/*"=function(b, ct) "prefix", "text/plain"=function(b, ct) "exact")
    expect(identical(pb("x", "text/plain", p), "exact"))
    expect(identical(pb("x", "text/html", p), "prefix"))
    expect(identical(pb("x", "image/plain", p), "suffix")) })
test("parse.body: names are case-insensitive", {
    expect(identical(pb("x", "TEXT/csv", list("Text/CSV"=function(b, ct) ct)), "TEXT/csv")) })
test("parse.body: failing parser -> 400", expect(is.400(pb("x", "text/plain", list("text/plain"=function(b, ct) stop("bad"))))))
test("parse.body: function name looked up at request time", {
    assign(".ws.test.fn", function(...) "by name", envir=.GlobalEnv); on.exit(rm(".ws.test.fn", envir=.GlobalEnv))
    expect(identical(webserver:::body.parsing.handler(quote(.ws.test.fn), list())("/", NULL, NULL, NULL, "GET"), "by name")) })
if (requireNamespace("jsonlite", quietly=TRUE)) {
    test("parse.body: JSON", expect(identical(pb('{"a":1,"b":[1,2]}', "application/json"), list(a=1L, b=1:2))))
    test("parse.body: +json suffix", expect(identical(pb('{"a":1}', "application/vnd.api+json"), list(a=1L))))
    test("parse.body: malformed JSON -> 400", expect(is.400(pb('{"a":', "application/json"))))
}
test("parse.body: invalid values rejected", {
    expect.error(addHandler("rfun", "/x", function(...) 1, parse.body="yes"))
    expect.error(addHandler("rfun", "/x", function(...) 1, parse.body=list(function(b, ct) 1)))
    expect.error(addHandler("rfun", "/x", function(...) 1, parse.body=list(a=1))) })
rmAllHandlers()

## webstart() argument errors. A call that does not fail would start a server
## and never return, so they are all evaluated in a separate R process (with a
## timeout) which reports the error message of each one
CERTS <- normalizePath(file.path(HERE, "..", "tls"), mustWork=FALSE)
start.errors <- function(exprs) {
    script <- tempfile(fileext=".R"); on.exit(unlink(script))
    writeLines(c(if (!is.null(LIB)) sprintf(".libPaths(c(%s, .libPaths()))", deparse(LIB)),
                 "suppressMessages(library(webserver))",
                 sprintf("CERTS <- %s; cf <- function(f) file.path(CERTS, f)", deparse(CERTS)),
                 sprintf("cat('<<%s>>', gsub('\\n', ' ', tryCatch({ suppressWarnings(%s); 'NO ERROR' }, error=function(e) conditionMessage(e))), '\\n')",
                         names(exprs), exprs)), script)
    out <- suppressWarnings(system2(file.path(R.home("bin"), "Rscript"), shQuote(script), stdout=TRUE, stderr=TRUE, timeout=60))
    m <- regmatches(out, regexec("^<<([^>]+)>> (.*)$", out))
    m <- m[lengths(m) == 3]
    setNames(trimws(vapply(m, `[`, "", 3)), vapply(m, `[`, "", 2))
}
## errors that are raised in R and need no TLS support
E <- start.errors(c(
    proto  = 'webstart("ftp://localhost:8080")',
    host   = 'webstart("http://example.com:8080")',
    port   = 'webstart(70000)',
    octal  = 'webstart("http://127.0.0.010:8080")',
    badip  = 'webstart("http://256.0.0.1:8080")',
    badip6 = 'webstart("http://[zzz]:8080")',
    noaddr = 'webstart("http://192.0.2.1:8080")',
    badnum = 'webstart(8080.5)',
    na     = 'webstart(c(8080, NA))',
    empty  = 'webstart(character())',
    unixdir= 'webstart(paste0("unix:", tempdir()))',
    tlshttp= 'webstart(8080, tls.cert=cf("server.pem"))',
    cnhttp = 'webstart(8080, tls.client.cn="alice")'))
expect.msg <- function(name, pattern) expect(isTRUE(grepl(pattern, E[name])), if (is.na(E[name])) "no result" else E[name])
test("listen: unsupported protocol", expect.msg("proto", "invalid listener 'ftp://localhost:8080': must be a port number"))
test("listen: unsupported host", expect.msg("host", "host must be"))
test("listen: invalid port", expect.msg("port", "invalid port"))
test("listen: no leading zeros in IPv4 addresses (octal)", expect.msg("octal", "host must be"))
test("listen: invalid IPv4 address", expect.msg("badip", "host must be"))
test("listen: invalid IPv6 address", expect.msg("badip6", "invalid IPv6 address|IPv6 support is not enabled"))
## 192.0.2.0/24 is reserved for documentation, so no machine has that address
test("listen: address not on this machine, reason reported", expect.msg("noaddr", "failed to start server on http://192.0.2.1:8080: unable to bind to 192.0.2.1 port 8080: "))
test("listen: non-integer port", expect.msg("badnum", "invalid listener '8080.5'"))
test("listen: NA", expect.msg("na", "cannot contain NA"))
test("listen: empty", expect.msg("empty", "must be a port number"))
test("listen: unix socket path is a directory", skip=if (WIN) "no unix sockets on Windows",
     expect.msg("unixdir", "exists and is not a socket"))
test("listen: unix sockets rejected on Windows", skip=if (!WIN) "Windows only",
     expect.msg("unixdir", "not supported on Windows"))
test("tls.* without https listener", expect.msg("tlshttp", "only allowed with https"))
test("tls.client.cn without https listener", expect.msg("cnhttp", "only allowed with https"))

if (hasTLS()) {
    E <- start.errors(c(
        nocert = 'webstart("https://localhost:8443")',
        nofile = 'webstart("https://localhost:8443", tls.cert=cf("nonexistent.pem"))',
        length = 'webstart(c("https://localhost:8443", "https://localhost:8444", 8080), tls.cert=cf(c("server.pem", "server.pem", "server.pem")))',
        cnca   = 'webstart("https://localhost:8443", tls.cert=cf("server-combined.pem"), tls.client.cn="alice")',
        cnpat  = 'webstart("https://localhost:8443", tls.cert=cf("server-combined.pem"), tls.client.ca=cf("client-ca.pem"), tls.client.cn="a*b")',
        cnlist = 'webstart(c("https://localhost:8443", "https://localhost:8444"), tls.cert=cf("server-combined.pem"), tls.client.ca=cf("client-ca.pem"), tls.client.cn=list("a", "b", "c"))',
        enckey = 'webstart("https://localhost:8443", tls.cert=cf("server.pem"), tls.key=cf("server-enc.key"))',
        wrongkey = 'webstart("https://localhost:8443", tls.cert=cf("server.pem"), tls.key=cf("alice.pem"))',
        badcert = 'webstart("https://localhost:8443", tls.cert=cf("make-certs.sh"), tls.key=cf("server.key"))',
        badca  = 'webstart("https://localhost:8443", tls.cert=cf("server-combined.pem"), tls.ca=cf("make-certs.sh"))',
        badcli = 'webstart("https://localhost:8443", tls.cert=cf("server-combined.pem"), tls.client.ca=cf("make-certs.sh"))'))
    test("https requires tls.cert", expect.msg("nocert", "requires tls.cert to be set"))
    test("missing certificate file", expect.msg("nofile", "does not exist"))
    test("tls.* recycled over https listeners only", expect.msg("length", "3 elements, but there are 2 https listeners"))
    test("tls.client.cn requires tls.client.ca", expect.msg("cnca", "requires tls.client.ca to be set"))
    test("tls.client.cn: * only at the beginning or end", expect.msg("cnpat", "only have a \\* at the beginning or the end"))
    test("tls.client.cn list length", expect.msg("cnlist", "3 elements, but there are 2 https listeners"))
    test("encrypted key rejected", expect.msg("enckey", "encrypted private keys are not supported"))
    test("key not matching the certificate", expect.msg("wrongkey", "does not match the certificate"))
    test("invalid certificate file", expect.msg("badcert", "cannot load certificate"))
    test("invalid tls.ca file", expect.msg("badca", "cannot read certificates"))
    test("invalid tls.client.ca file", expect.msg("badcli", "cannot load client CA"))
} else {
    E <- start.errors(c(notls = 'webstart("https://localhost:8443", tls.cert=cf("server.pem"))'))
    test("https without TLS support", expect.msg("notls", "TLS support is not available"))
}
