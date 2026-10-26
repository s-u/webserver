\name{handlers}
\alias{handlers}
\alias{addHandler}
\alias{rmAllHandlers}
\title{Webserver Content Handlers}
\description{
  \code{addHandler} adds a content handler to be used with the
  webserver.

  \code{rmAllHandlers} removes all currently registered handlers.
}
\usage{
addHandler(type, prefix, ...)
rmAllHandlers()
}
\arguments{
\item{type}{handler type}
\item{prefix}{string, prefix at which the handler will be registered.}
\item{\dots}{parameters for the handler, see details below.}
}
\value{
undefined (currently returns \code{TRUE} invisibly, but that is not
guaranteed for the future).
}
\details{
For each webserver request the content handlers are matched in sequence
against the request's path and each matching handler gets a chance to
provide the result for the request.

Most commonly used handlers are \code{"static"} which serves files from
the file system, \code{"rfun"} which registers an R function to be
called with the request details and \code{"rewrite"} which modifies the
request path according to specified rules.
}
\section{Static Handler}{
Syntax: \code{addHandler("static", prefix, path, index=NULL,
  stop=FALSE, gzip=FALSE)}

When the \code{prefix} is matched, it is removed from the request path
and the remaining path is appended to \code{path} to look up
files. If the target matches a directory and \code{index} is a string,
then \code{index} is appended to the path, unless the trailing slash is
missing in which case a re-direct is returned to add the slash (this
makes sure that relative paths in the index stay valid).
If the resulting target is a file, it is served as the request result.
Otherwise either 404 Not Found response is sent (if \code{stop=TRUE})
or the processing is passed to the next handler.

The static handler supports conditional GET requests based on last
modified timestamp. It also supports HTTP range requests. If
\code{gzip=TRUE} and the client has requested gzip compression then the
handler checks if a file \code{<target>.gz} exists and is not older than
\code{<target>} then the compressed file is served instead. This saves
bandwidth for compressible static content.

Example:
\code{addHandler("static", "/", path=".", index="index.html", stop=TRUE)}

Serves files from the current directory at the root of URLs. It will
stop and not evaluate any furhter handlers even if the file is not
found.
}
\section{R Function Handler}{
  This handler calls an R function with the same API as the internal R
  server and Rserve.

  \code{addHandler("rfun", prefix, FUN, parse.query=TRUE,
    parse.headers=TRUE, fork=FALSE)}

  \itemize{
    \item{FUN}{function to call for a request (see below for details)}
    \item{parse.query}{logical, if \code{TRUE} then the query string is
      parsed into a named character vector, otherwise it is just one
      string (see below)}
    \item{parse.headers}{logical, if \code{TRUE} then the headers are
      parsed (see below), otherwise just a raw vector}
    \item{fork}{logical, if \code{TRUE} then the R processing will be
      delegated to forked workers, one per each I/O thread (not
      available on Widnows). Otherwise the R code is evaluated in the
      current session.}
  }

  The function \code{FUN} should have the following signature:

  \code{function(path, query, body, headers, ...)}

  \itemize{
    \item{path}{string, path of the request}
    \item{query}{query string part of the request. If
      \code{parse.query=TRUE} (the default) then the query string is
      parsed into a named character vector, so, e.g., the query string
      \code{"a=1&b=foo"} will be parsed into the R object
      \code{c(a="1", b="foo")}. Otherwise it is just the string itself.}
    \item{body}{body part of the request or \code{NULL} if there is no
      body. If \code{parse.body=TRUE} then an attempt is made to decode
      known body types such as multipart messages (file
      uploads) and JSON. Otherwise it is a raw vector.}
    \item{headers}{request headers. It is a raw vector unless
      \code{parse.headers=TRUE} in which case the headers are parsed
      into a named list where the header names are lowercase and the
      \code{"-"} character is replaced by \code{"."} so, for example,
      \code{User-Agent} is stored in the \code{user.agent} element of
      the list. Note that the parsing is experimental, since the
      interpretation can vary among header entries. Note that at the
      very least the \code{request.method} entry is always present and
      contains the request method name (\code{GET}, \code{POST}, etc.).}
    \item{...}{reserved for future extensions}
  }

  The result is expected to have the following form:

  \code{list(result, content.type, headers, http.code)}

  Only the first list element is mandatory, all others are optional.

  \itemize{
    \item{result}{Can be either a string or a raw vector containing the
      body of the result to return to the client. As a special case, the
      result can be a named character vector of length one with either
      \code{file} or \code{tempfile} name in which case the result is
      interpreted as a path to a file whose contents will be served.
      In the case of \code{tempfile} the file will be removed once read.}
    \item{content.type}{string, MIME type of the result, defaults to
      \code{"text/html"}}
    \item{headers}{character vector (unnamed) of any additional headers
      to send. Note that it may not contain \code{Content-Type} nor
      \code{Content-Length}.}
    \item{http.code}{integer, valid HTTP code (100..999), defaults to
      200L}
  }

  One exception is that returning \code{FALSE} indicates that the
  handler has refused to process the request and so the search should
  continue to the next handler. Note that due to the relatively
  expensive nature of some evaluation methods (such as worker
  communication) it is recommended to consolidate R handlers instead of
  relying on each to signal its unwillingness to process the request.

  If the returned object has any other form (e.g. if it is a character
  vector), it is assumed to be an error and it will result in 500 HTTP
  error code.

  Example: \code{addHandler("rfun", "/api/v1/add",
    function(path, query, ...)
      list(as.numeric(query["a"]) + as.numeric(query["b"])))}
}
\section{Rewrite Handler}{
  This handler modifies the request based on simple substitution rules.

  \code{addHandler("rewrite", prefix, replacement, stop=FALSE)}

  The \code{prefix} can contain the \code{?} character as a wildcard
  which will match any number of characters other than \code{/}. The
  matched content is then avaiable in the form of \code{$n} variables in
  the \code{replacement} where \code{n} is one of the integers 1..9.
  If the request prefix is matched then it is replaced by the
  \code{replacement}.

  Example:
  \code{addHandler("rewrite", "/api/?/?", "/r?name=$1&par=$2", stop=TRUE)}

  Will match request \code{/api/copy/this?a=1} and rewrite it to
  \code{/r?a=1&name=copy&par=this}. Note that if the replacement
  contains a query string, it will be appended to the request query
  string.

  The \code{stop} parameter only applies to subsequent rewrite requests,
  because the rewrite handler is not an actual content handler, it will
  still proceed to other content handlers regardless of the value of
  \code{stop}, but setting \code{stop=TRUE} will make sure that no more
  rewrite handlers will be consulted.
}
\author{Simon Urbanek}
\keyword{IO}
