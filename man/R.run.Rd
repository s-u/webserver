\name{R.run}
\alias{R.run}
\title{Run R}
\description{
  \code{R.run} evaluates \code{expr} in a new R process.
}
\usage{
R.run(expr, out, where=parent.frame(), bquote=TRUE)
}
\arguments{
\item{expr}{expression to evaluate}
\item{out}{string, filename to re-direct stdout/stderr to or \code{NULL}
  if not re-direction is desired.}
\item{where}{environment to evaluate dot-quoted expressions in}
\item{bquote}{logical, if \code{TRUE} then \code{expr} is treated like
  \code{bquote()} which means only expressions inside \code{.()} are
  evaluated locally and everyting else remotely. Otherwise \code{expr}
  is passed as-is.}
}
\author{Simon Urbanek}
\keyword{utilities}
