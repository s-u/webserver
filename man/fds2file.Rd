\name{fds2file}
\alias{fds2file}
\title{Internal utilities}
\description{
\code{fds2file} re-directs specified file descriptors into a file.
}
\usage{
fds2file(filename, fds, append=TRUE, mode="666", close=FALSE)
}
\arguments{
\item{filename}{string, name of the file (will be expanded)}
\item{fds}{integer vector, list of file descriptors to re-direct
  (1L=stdout, 2L=stderr, so 1:2 will re-direct both)}
\item{append}{logical, if \code{TRUE} then content is appended,
  otherwise any existing file is truncated}
\item{mode}{octmode, more of the file to create}
\item{close}{logical, if \code{TRUE} then the original \code{fds} are
  closed (re-direct) otherwise the putput will appear on both in the
  file and the descriptors}
}
\value{
logical vector of the same length as \code{fds} with \code{TRUE} for
each successful redirect and \code{FALSE} if re-direct failed. Note that
the function will raise an error if the file cannot be created, so
failure to redirect is likely casued by invalid \code{fds}.
}
\author{Simon Urbanek}
\keyword{internal}
