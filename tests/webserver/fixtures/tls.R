## Fixture for test-tls.R: one HTTP and three HTTPS listeners on consecutive ports.
## Usage: Rscript tls.R --port=<port> --docroot=<dir> --certs=<dir> [--threads=4] [--mode=threads] [--lib=<lib>]
## mode: threads (default), single (threads=0), connfork (fork per connection)
##   port    HTTP (bound to 127.0.0.1 given as an address)
##   port+1  HTTPS, certificate + intermediate CA via tls.ca
##   port+2  HTTPS, certificate chain and key in one file, client certificates
##           from the client CA with CN alice or *.example.org required
##   port+3  HTTPS, certificate chain file, any client certificate from the client CA
args <- commandArgs(TRUE)
arg <- function(name, default=NULL) {
    a <- grep(paste0("^--", name, "="), args, value=TRUE)
    if (length(a)) sub("^--[^=]*=", "", a[1]) else default
}
if (!is.null(arg("lib"))) .libPaths(c(arg("lib"), .libPaths()))
cat("PID", Sys.getpid(), "\n"); flush(stdout()) # the driver needs the pid of the R process
port    <- as.integer(arg("port"))
docroot <- arg("docroot")
certs   <- arg("certs")
threads <- as.integer(arg("threads", 4L))
mode    <- arg("mode", "threads")
if (mode == "single") threads <- 0L

library(webserver)
cf <- function(f) ifelse(is.na(f), NA, file.path(certs, f))

handler <- function(url, query, body, headers, method) {
    if (url == "/echo")
        return(list(paste0("method=", method, ";body=", length(body), ";sum=", sum(as.integer(body))),
                    "text/plain"))
    if (url == "/big")
        return(list(as.raw(rep(1:250, length.out=5e6)), "application/octet-stream"))
    if (url == "/pid") return(list(as.character(Sys.getpid()), "text/plain"))
    if (url == "/whoami") ## <proto>;<client CN or ->;<number of client CN headers>
        return(list(paste(headers$request.proto, if (is.null(headers$request.client.cn)) "-" else headers$request.client.cn,
                          sum(names(headers) == "request.client.cn"), sep=";"), "text/plain"))
    FALSE
}
invisible(addHandler("static", "/static/", path=paste0(docroot, "/"), index="index.html", stop=FALSE))
invisible(addHandler("rfun", "/", handler))
webstart(c(paste0("http://127.0.0.1:", port), paste0("https://localhost:", port + 1:3)),
         threads=if (mode == "connfork") 0L else threads, fork=(mode == "connfork"),
         tls.cert=cf(c("server.pem", "server-combined.pem", "server-chain.pem")),
         tls.key=cf(c("server.key", NA, "server.key")),
         tls.ca=cf(c("inter.pem", NA, NA)),
         tls.client.ca=cf(c(NA, "client-ca.pem", "client-ca.pem")),
         tls.client.cn=list(NULL, c("alice", "*.example.org"), NULL))
