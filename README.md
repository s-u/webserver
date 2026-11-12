# webserver
## Fast, multi-threaded HTTP/HTTPS server for R

This package provides a high-performance web server for R.
It serves static files and R functions over HTTP and HTTPS (TLS),
including client certificate authentication. It supports TCP ports
(IPv4 and IPv6) and local (unix) sockets.
Requests are processed by multiple threads and
on unix R handlers can be evaluated in forked worker processes
(Windows only supports evaluation in the same R process).

[![CRAN](https://rforge.net/do/cransvg/webserver)](https://cran.r-project.org/package=webserver)
[![RForge](https://rforge.net/do/versvg/webserver)](https://RForge.net/webserver)
[![check](https://github.com/s-u/webserver/actions/workflows/check.yml/badge.svg)](https://github.com/s-u/webserver/actions/workflows/check.yml)

## Examples

Serve the current directory on `http://localhost:8080` (akin to `python -m http.server`):

```
Rscript -e 'webserver::simple()'
```

or on all interfaces on port 8000:

```
Rscript -e 'webserver::simple("http://*:8000")'
```

R functions can be served as well, e.g., `http://localhost:8080/?name=World` with:

```
webserver::simple(path=NULL, Rfun=function(path, query, ...)
    list(paste("Hello,", query["name"]), "text/plain"))
```

The function has the same interface as the HTTP handlers of Rserve and the
internal R help server, and `simple()` uses the `.http.request()` function by
default (if it exists) just like Rserve, so existing handlers can be used as-is
(use `parse.body=FALSE` for raw request bodies).

For more control, register the handlers explicitly and start the server.
Handlers are tried in the order they were added, so rewrite handlers (which
modify the request) go first, e.g., to also serve `http://localhost:8080/hello/World`:

```
library(webserver)
addHandler("rewrite", "/hello/?", "/hello?name=$1")
addHandler("rfun", "/hello", function(path, query, ...)
    list(paste("Hello,", query["name"]), "text/plain"))
addHandler("static", "/", path=".", index="index.html")
webstart(8080)
```

See [`?webstart`](https://rforge.net/webserver/docs/reference/simple.html) and [`?addHandler`](https://rforge.net/webserver/docs/reference/handlers.html) for details.

## Installation

To install the latest release from CRAN, use simply

```
install.packages("webserver")
```

For installation of the latest development version, use

```
install.packages("webserver", repos="https://rforge.net")
```
