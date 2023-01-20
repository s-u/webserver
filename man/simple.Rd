\name{simple}
\alias{simple}
\alias{webstart}
\title{Web Server}
\description{
  \code{simple} starts a simple webserver (HTTP) serving static files
  and (optionally) using an R function as a handler.

  \code{webstart} starts a webserver based on currently registered
  content handlers.
}
\usage{
simple(port=8080L, path=".", prefix="/", index="index.html", start=TRUE,
       Rfun=get0(".http.request"), Rfun.prefix="/", threads=4L, ...)

webstart(port = 8080L, threads = 4L)
}
\arguments{
\item{port}{integer, port on which to listen}
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
\item{threads}{integer, number of threads for the server to use.}
\item{\dots}{additonal parameters passed to \code{webstart}}
}
\value{\code{TRUE}, invisibly}
\author{
  Simon Urbanek
}
\keyword{IO}
