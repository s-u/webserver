## compiles R functions into a API specs
## The functions can define required and optional arguments ar follows:
## 1) arg           - required, no default, no conversion
## 2) arg=value     - optional with default, no conversion
## 3) arg=required  - same as 1)
## 4) arg=required(conv)
##                  - required, type enforced (see below)
## 5) arg=optional  - optional, default NULL, no conversion
## 5) arg=optional(default)
##                  - same as arg=default
## 6) arg=optional(default, conv)
##                  - optional with default, type enforced
##
## conv: can be one of:
## "*"      - any type allowed, no conversion
## function - function will be used to check/coerce values
## "<type>" - as(arg, "<type>") will be used for coercion
## value    - as(arg, class(value)[1]) is used
##
## It is recommended to use the explicit forms, e.g.:
##   a=required(as.numeric), b=optional(1, as.numeric)
## others are only provided for convenience.
##
## Important: the defaults are currently evaluated at definition
## time, so they may not depend on other arguments.
##
## The following arguments can be listed as pass-through from the
## low-level API: .path, .query, .body, .headers
##
## Example:
## function(key=required, n=optional(1, as.numeric), foo="bar")
## - key is a required argument, no conversion
## - n is optional and values will be passed to as.numeric
## - foo is optional with default "bar"
##
## Specification consists of the following entries:
## .args - a named list, each entry corresponds to one argument.
##         Each element is a list of three entries:
##   - required - logical
##   - default  - default value (only used if required=FALSE)
##   - convert  - function to use on input values
## .pass - character vector with named of low-level args to pass
## .fn   - the user function to call with all args resolved

env2spec <- function(e, names, rho=parent.frame()) {
    if (missing(names)) {
        if (is.environment(e))
            names <- ls(e)
        else if (is.list(e))
            names <- base::names(e)
        else
            stop("Not a list nor environment")
    }
    s <- new.env(TRUE, emptyenv())
    for (n in names) {
        fn <- e[[n]]
        if (is.function(fn)) {
            f <- formals(fn)
            nd <- grep("^[.]", names(f), invert=TRUE)
            l <- lapply(nd, function(n) {
                opt <- FALSE
                default <- NULL
                convert <- as.vector
                if (!identical(f[[n]], formals(identity)[[1]])) {
                    arg <- f[[n]]
                    sym <- if (is.symbol(arg)) arg else arg[[1]]
                    if (identical(sym, quote(required)) ||
                        (identical(sym, quote(optional)) && (opt <- TRUE))) {
                        ## have type spec?
                        if (length(arg) > (1 + opt)) {
                            ty <- eval(arg[[2 + opt]], rho)
                            if (!identical(ty, "*")) {
                                if (identical(ty, ""))
                                    ty <- as.character
                                if (!is.function(ty) && !is.character(ty))
                                    ty <- class(ty)[1L]
                                if (is.character(ty)) {
                                    cf <- function(x) NULL
                                    body(cf) <- bquote(as(x, .(ty)))
                                    environment(cf) <- baseenv()
                                    ty <- cf
                                }
                                convert <- ty
                            }
                        }
                        if (length(arg) > 1 && opt)
                            default <- eval(arg[[2]], rho)
                    } else { ## has default
                        opt <- TRUE
                        default <- eval(arg, rho)
                    }
                }
                list(required=!opt, default=default, convert=convert)
            })
            names(l) <- names(f)[nd]
            pass.list <- c(".path", ".query", ".body", ".headers")
            pass <- pass.list[pass.list %in% names(f)]
            e <- new.env(FALSE, baseenv())
            e$.args <- l
            e$.pass <- pass
            e$.fn <- compiler::cmpfun(fn)
            ## NOTE: we could improve preformance by using something like
            ## l <- add.pairlist(l, key, val) and do.call without list conversion
            ## since l[key] <- list(val) is inefficient
            ff <- function(.path, .query, .body, .headers, ...) {
                .l <- list()
                if (length(.args)) {
                    .qn <- names(.query)
                    for (.a in names(.args)) {
                        if (.a %in% .qn)
                            .l[.a] <- list(.args[[.a]]$convert(.query[.a]))
                        else if (.args[[.a]]$required)
                            stop("Required parameter `", .a, "' is missing")
                        else
                            .l[.a] <- list(.args[[.a]]$default)
                    }
                }
                for (.a in .pass)
                    .l[.a] <- list(get(.a))
                do.call(.fn, .l)
            }
            environment(ff) <- e
            s[[n]] <- compiler::cmpfun(ff)
        }
    }
    s
}
