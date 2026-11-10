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
## (no handlers are registered, so webstart() also warns about that)
test("webstart rejects invalid socket", expect.error(suppressWarnings(webstart(socket=1:2))))
