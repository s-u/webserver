\name{simple}
\alias{simple}
\alias{webstart}
\alias{hasTLS}
\title{Web Server}
\description{
  \code{simple} starts a simple webserver (HTTP) serving static files
  and (optionally) using an R function as a handler.

  \code{webstart} starts a webserver based on currently registered
  content handlers, listening on one or more ports or sockets using
  HTTP or HTTPS.

  \code{hasTLS} returns \code{TRUE} if the package was built with
  TLS support, i.e., HTTPS can be used.
}
\usage{
simple(listen = 8080L, path = ".", prefix = "/", index = "index.html",
       start = TRUE, Rfun = get0(".http.request"), Rfun.prefix = "/",
       parse.body = TRUE, ...)

webstart(listen = 8080L, threads = 4L, fork = FALSE,
         tls.cert = NULL, tls.key = tls.cert, tls.ca = NULL,
         tls.client.ca = NULL, tls.client.cn = NULL)

hasTLS()
}
\arguments{
\item{listen}{where to listen for connections: a port number or a
  character vector, each element specifying one listener (see Details).}
\item{path}{string, path to serve static content from (must exist and be
  a directory) or \code{NULL} if no static content should be served.}
\item{prefix}{string, URL path prefix to match for the static handler.}
\item{index}{string, name of the file to look for if the URL path
  matches a directory or \code{NULL}.}
\item{start}{logical, if \code{TRUE} then the server is started
  immediately and all handlers are removed upon exit.}
\item{Rfun}{function to call as R handler or \code{NULL} if no R handler
  is desired.}
\item{Rfun.prefix}{string, URL path prefix to match for the R
  handler. If both prefixes match, the static handler is evaluated first
  and will proceed to the R handler if no static file was found.}
\item{parse.body}{passed to \code{\link{addHandler}} for the R handler:
  \code{TRUE} (default) decodes URL encoded forms and JSON request
  bodies (see \code{\link{defaultBodyParsers}}), \code{FALSE} passes
  the body as a raw vector.}
\item{threads}{integer, number of threads for the server to use.}
\item{fork}{logical, if \code{TRUE} then a new process is forked for
  each connection. Mutually exclusive with \code{threads > 0}, so
  \code{threads} defaults to 0 in that case (with a warning if
  \code{threads > 0} is specified explicitly). Not supported on
  Windows. Forked children inherit the RNG state of the parent, so
  remove \code{.Random.seed} from the global environment before
  starting the server if independent random streams are needed.}
\item{tls.cert}{file name of the server certificate (PEM format),
  required for HTTPS. The file may also contain intermediate
  certificates following the server certificate, they are sent to the
  clients along with it (making \code{tls.ca} below unnecessary).}
\item{tls.key}{file name of the private key of the server certificate
  (PEM format). Defaults to \code{tls.cert} (also for \code{NA} values),
  i.e., the key can be stored in the same file as the certificate.
  Encrypted keys are not supported.}
\item{tls.ca}{optional file name of further certificates (PEM format)
  sent to the clients along with the server certificate, typically the
  intermediate certificate(s) of the CA that issued it, and only needed
  if not already included in \code{tls.cert}. They are sent after
  those in \code{tls.cert}, so the intermediate certificates can also
  be split between the two files.}
\item{tls.client.ca}{optional file name of CA certificates (PEM
  format). If specified, clients must present a valid certificate
  issued by one of these CAs, otherwise the connection is rejected.}
\item{tls.client.cn}{optional character vector of allowed common names
  of client certificates (or a list of such vectors, see Details). A
  name can start with \code{*} to match any common name ending with the
  rest (e.g., \code{"*.example.com"}) or end with \code{*} to match
  any common name starting with the rest. Requires
  \code{tls.client.ca}.}
\item{\dots}{additional parameters passed to \code{webstart}}
}
\details{
  Each element of \code{listen} is one of
  \describe{
    \item{\code{<port>}}{a port number, the same as
      \code{"http://localhost:<port>"}}
    \item{\code{"http://<host>:<port>"}}{HTTP on the given port}
    \item{\code{"https://<host>:<port>"}}{HTTPS on the given port}
    \item{\code{"unix:<path>"}}{HTTP on a local (unix) socket, not
      supported on Windows. An existing socket at \code{<path>} is
      replaced.}
  }
  where \code{<host>} is \code{localhost} to only accept connections
  from the local machine, \code{*} to accept connections on all IPv4
  interfaces or a numeric IPv4 address of the machine (such as
  \code{127.0.0.1}, \code{0.0.0.0} (all IPv4 interfaces) or the address of
  one of its interfaces) to only accept connections on that address.
  IPv6 addresses are enclosed in brackets (such as \code{[::1]}) and
  require IPv6 support enabled at build time. IPv6 listeners only accept
  IPv6 connections, so \code{c("https://*:8443", "https://[::]:8443")}
  accepts connections on all IPv4 and IPv6 interfaces. Host names other than
  \code{localhost} are not supported. If the port is omitted, it is 80
  for HTTP and 443 for HTTPS. Port 0 means any free port, which is
  reported when the server is started (e.g.,
  \code{Listening on http://localhost:54321}).

  The \code{tls.*} arguments apply to the HTTPS listeners and are
  recycled over them: each argument is either a single value used by all
  HTTPS listeners or has one value for each HTTPS listener (in the order
  they appear in \code{listen}, HTTP listeners are not counted) where
  \code{NA} means not set. Since a single value of \code{tls.client.cn}
  is already a character vector (of allowed names), different values per
  listener are specified as a list of such vectors, with \code{NULL} for
  listeners that don't check names, e.g.,
  \code{tls.client.cn = list(NULL, c("alice", "*.example.com"))}.

  HTTPS requires TLS 1.2 or higher. All listeners are served by the same
  handlers. The certificates and keys are read when the server is
  started, so the server has to be restarted to use renewed
  certificates.

  \code{webstart()} runs the server in the current R session and returns
  once it is stopped by a user interrupt (SIGINT, e.g., Ctrl-C) or, on
  unix, by SIGTERM or SIGHUP. The registered handlers are kept, so the
  server can be started again with another \code{webstart()} call.
}
\value{
  \code{simple}: \code{TRUE}, invisibly.

  \code{webstart}: \code{NULL} once the server is stopped.

  \code{hasTLS}: logical, \code{TRUE} if TLS (HTTPS) is supported.
}
\author{
  Simon Urbanek
}
\examples{
\dontrun{
## webstart() runs the server until it is interrupted (e.g., by Ctrl-C
## or the Stop button), so each call below is a separate alternative

## local HTTP server on port 8080
webstart()

## HTTP for local clients and HTTPS for everyone
webstart(c(8080, "https://*:8443"), tls.cert="server.crt", tls.key="server.key")

## two HTTPS listeners with different certificates, the second
## one requires client certificates issued by the corporate CA
webstart(c("https://*:443", "https://*:8443"),
         tls.cert = c("public.pem", "internal.pem"),
         tls.client.ca = c(NA, "corp-ca.pem"),
         tls.client.cn = list(NULL, "*.corp.example.com"))
}
}
\keyword{IO}
