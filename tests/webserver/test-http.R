## Static files, R handlers, rewrites and HTTP protocol handling.
## Fixture: fixtures/server.R (run once for each server mode)

## ---- static ---------------------------------------------------------------
test("static: file", {
    r <- get("/static/hello.txt")
    expect(r$status == 200, r); expect(identical(r$text, "hello world\n"))
    expect(identical(r$headers[["content-type"]], "text/plain"))
    expect(identical(r$headers[["content-length"]], "12"))
    expect(!is.null(r$headers[["last-modified"]]) && !is.null(r$headers[["date"]]))
})
test("static: UTF-8 file name", { r <- get("/static/caf%C3%A9.txt", timeout=5); expect(r$status == 200 && identical(r$text, "utf8\n"), r) })
test("static: control characters in the URL do not hang the server", { for (p in c("/static/hello%01.txt", "/static/%1f", "/static/caf%C3%A9-none")) expect(!is.na(get(p, timeout=5)$status), p) })
test("static: HEAD has no body", {
    r <- request("HEAD", "/static/hello.txt")
    expect(r$status == 200 && r$headers[["content-length"]] == "12" && !length(r$body))
})
test("static: index at root", { r <- get("/static/"); expect(r$status == 200 && has(r$text, "root"), r) })
test("static: index in subdir with slash", { r <- get("/static/sub/"); expect(r$status == 200 && has(r$text, "sub"), r) })
test("static: subdir without slash serves index or redirects", {
    r <- get("/static/sub"); expect.in(r$status, c(200, 301, 302, 307, 308), r) })
test("static: dir without index is not listed", expect.in(get("/static/emptydir/")$status, c(403, 404)))
test("static: missing -> 404", expect(get("/static/nope.txt")$status == 404))
test("static: unmatched path -> 404", expect(get("/no-such-thing")$status == 404))
test("static: prefix is segment aligned", expect(get("/static_hello.txt")$status == 404))
test("static: overlong path -> 414", {
    r <- get(paste0("/static/", strrep("a", 600))); expect(r$status == 414, r) })
test("static: range", {
    r <- get("/static/digits.txt", headers=list(Range="bytes=2-5"))
    expect(r$status == 206, r); expect(identical(r$text, "2345"))
    expect(identical(r$headers[["content-range"]], "bytes 2-5/10"))
    expect(identical(r$headers[["content-length"]], "4"))
})
test("static: open-ended range", {
    r <- get("/static/digits.txt", headers=list(Range="bytes=7-")); expect(r$status == 206 && r$text == "789", r) })
test("static: range past EOF -> 416", expect(get("/static/digits.txt", headers=list(Range="bytes=50-60"))$status == 416))
test("static: conditional GET -> 304", {
    lm <- get("/static/hello.txt")$headers[["last-modified"]]
    r <- get("/static/hello.txt", headers=list("If-Modified-Since"=lm))
    expect(r$status == 304, r); expect(!length(r$body)); expect(is.null(r$headers[["content-length"]]))
})
test("static: conditional GET, old date -> 200",
    expect(get("/static/hello.txt", headers=list("If-Modified-Since"="Thu, 01 Jan 1970 00:00:01 GMT"))$status == 200))
test("static: 304 keeps keep-alive framing", {
    lm <- get("/static/hello.txt")$headers[["last-modified"]]
    con <- conn(); on.exit(close(con))
    for (i in 1:2) {
        send(con, paste0("GET /static/hello.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: ", lm, "\r\n\r\n"))
        expect(read.resp(con)$status == 304)
    }
})

## ---- traversal ------------------------------------------------------------
not.served <- function(p) { r <- get(p); expect(r$status != 200, paste("served:", p)) }
test("traversal: ..", not.served("/static/../secret-outside.txt"))
test("traversal: encoded ..", { not.served("/static/%2e%2e/secret-outside.txt"); not.served("/static/%2E%2E%2Fsecret-outside.txt") })
test("traversal: deep ..", not.served("/static/sub/../../secret-outside.txt"))
test("traversal: backslash (Windows separator)", { not.served("/static/..\\secret-outside.txt"); not.served("/static/%2e%2e%5csecret-outside.txt"); not.served("/static/sub\\..\\..\\secret-outside.txt") })
test("traversal: NUL byte", expect.in(get("/static/hello.txt%00.png")$status, c(400, 404)))
test("traversal: double slash absolute", not.served("/static//etc/passwd"))

## ---- R handler ------------------------------------------------------------
test("rfun: GET with query", {
    r <- get("/echo?a=1&b=hello%20world&c=x%2By")
    expect(r$status == 200, r); expect(has(r$text, "method=GET")); expect(has(r$text, "a=1"))
    expect(has(r$text, "b=hello world")); expect(has(r$text, "c=x+y"), r$text)
})
test("rfun: POST body", {
    r <- request("POST", "/echo", list("Content-Type"="text/plain"), "payload")
    expect(r$status == 200, r); expect(has(r$text, "method=POST") && has(r$text, "body=7"))
    expect(has(r$text, "bodytxt=payload") && has(r$text, "ct=text/plain"), r$text)
})
test("rfun: POST 1MB body", {
    r <- request("POST", "/echo", list("Content-Type"="application/octet-stream"), as.raw(rep(65:74, 1e5)))
    expect(r$status == 200 && has(r$text, "body=1000000"), r)
})
test("rfun: empty POST with Content-Type does not crash", {
    r <- request("POST", "/echo", list("Content-Type"="text/plain", "Content-Length"="0"))
    expect(r$status == 200, r); healthy()
})
test("rfun: GET with Content-Type does not crash", { r <- get("/echo", headers=list("Content-Type"="text/plain")); expect(r$status == 200, r); healthy() })
test("rfun: form-urlencoded body", {
    r <- request("POST", "/echo", list("Content-Type"="application/x-www-form-urlencoded"), "a=1&b=2")
    expect(r$status == 200 && has(r$text, "bodytxt=a=1&b=2"), r) })
test("rfun: other methods", for (m in c("PUT", "DELETE", "PATCH", "OPTIONS")) {
    r <- request(m, "/echo", body=if (m != "OPTIONS") ""); expect(r$status == 200 && has(r$text, paste0("method=", m)), list(m, r)) })
test("rfun: request headers visible", {
    r <- get("/hdr", headers=list("X-Foo"="bar", "User-Agent"="tester")); low <- gsub("[._]", "-", tolower(r$text))
    expect(has(low, "x-foo: bar") && has(low, "user-agent: tester"), r$text) })
## /parsed decodes the body with defaultBodyParsers() plus "text/*" (upper case)
post.parsed <- function(ct, body) request("POST", "/parsed", headers=list("Content-Type"=ct), body=body)
test("rfun: parse.body form", {
    r <- post.parsed("application/x-www-form-urlencoded", "a=1&b=x+y%21")
    expect(r$status == 200 && identical(r$text, 'character:c(a = "1", b = "x y!")'), r) })
test("rfun: parse.body JSON", {
    r <- post.parsed("application/json; charset=utf-8", '{"a":1,"b":[1,2]}')
    expect(r$status == 200 && identical(r$text, "list:list(a = 1L, b = 1:2)"), r) })
test("rfun: parse.body malformed JSON -> 400", {
    r <- post.parsed("application/json", '{"a":'); expect(r$status == 400, r); healthy() })
test("rfun: parse.body custom parser by prefix", {
    r <- post.parsed("Text/Plain", "abc"); expect(r$status == 200 && identical(r$text, 'character:"ABC"'), r) })
test("rfun: parse.body unmatched type passed raw", {
    r <- post.parsed("application/octet-stream", "abc"); expect(r$status == 200 && startsWith(r$text, "raw:"), r) })
## the request headers as "name: value" lines (/hdr reflects them)
hdr.lines <- function(r) strsplit(r$text, "\n", fixed=TRUE)[[1L]]
test("rfun: Request-Method and Request-Proto set by the server", {
    r <- get("/hdr"); l <- grep("^request[.]", hdr.lines(r), value=TRUE)
    expect(identical(l, c("request.method: GET", "request.proto: http")), r$text) })
test("rfun: Request-* headers sent by the client are dropped", {
    r <- get("/hdr", headers=list("Request-Method"="FAKE", "request-proto"="https",
                                  "REQUEST-Client-CN"="admin", "Request-Other"="x", "X-Request-Id"="7"))
    l <- hdr.lines(r)
    expect(identical(grep("^request[.]", l, value=TRUE), c("request.method: GET", "request.proto: http")), r$text)
    expect("x.request.id: 7" %in% l, r$text) })
test("rfun: custom response headers", {
    r <- get("/custom-headers"); expect(r$status == 200, r)
    expect(identical(r$headers[["x-test"]], "1") && identical(r$headers[["x-other"]], "two"), r) })
test("rfun: status codes", for (code in c(201, 204, 301, 400, 401, 403, 404, 405, 418, 500, 503)) {
    r <- get(paste0("/code/", code)); expect(r$status == code, list(code, r)) })
test("rfun: status line has reason phrase", expect(grepl("^HTTP/1\\.[01] 418 \\S", get("/code/418")$line)))
test("rfun: binary roundtrip", {
    r <- get("/raw"); expect(r$status == 200 && identical(r$body, as.raw(0:255)))
    expect(identical(r$headers[["content-type"]], "application/octet-stream")) })
test("rfun: empty payload", { r <- get("/empty"); expect(r$status == 200 && identical(r$headers[["content-length"]], "0")) })
test("rfun: 5MB response", {
    r <- get("/big"); expect(r$status == 200 && length(r$body) == 5e6 && identical(r$body[1:5], as.raw(1:5)), length(r$body)) })
test("rfun: UTF-8", { r <- get("/utf8"); expect(identical(r$body, charToRaw("héllo"))) })
test("rfun: file payload", { r <- get("/file"); expect(r$status == 200 && identical(r$text, "hello world\n"), r) })
test("rfun: tmpfile payload is removed", {
    r <- get("/tmpfile"); expect(r$status == 200 && identical(r$text, "temporary\n"), r)
    f <- r$headers[["x-tmpfile"]]; expect(!is.null(f), "no X-Tmpfile header")
    expect(wait.for(!file.exists(f)), "tmpfile not unlinked") })
test("rfun: missing file payload is well-formed", {
    con <- conn(3); on.exit(close(con))
    send(con, "GET /missing-file HTTP/1.1\r\nHost: x\r\n\r\n")
    r <- read.resp(con); expect.in(r$status, c(200, 404, 500), r)
    if (r$status == 200) expect(identical(r$headers[["content-length"]], "0"))
    ## no stray bytes after the body: the next response on the connection must parse
    send(con, "GET /echo HTTP/1.1\r\nHost: x\r\n\r\n")
    expect(read.resp(con)$status == 200, "connection desynchronised by stray bytes")
})
test("rfun: R error -> 500, server survives", {
    r <- get("/error"); expect(r$status == 500 && has(r$text, "boom"), r); healthy() })
test("rfun: NULL result -> 200", expect(get("/null")$status == 200))
test("rfun: FALSE falls through -> 404", expect(get("/refuse")$status == 404))
test("rfun: incomplete request is not passed to the handler", {
    con <- conn(3); send(con, "GET /touch?f=partial HTTP/1.1\r\nHost: x\r\n"); Sys.sleep(0.2); close(con)
    settle(); expect(!file.exists(file.path(DOCROOT, "partial")), "handler called"); healthy() })
test("rfun: truncated body is not passed to the handler", {
    con <- conn(3); send(con, "POST /touch?f=truncated HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\nshort")
    Sys.sleep(0.2); close(con)
    settle(); expect(!file.exists(file.path(DOCROOT, "truncated")), "handler called"); healthy() })
test("rewrite: placeholders into the query", {
    a <- strrep("x", 300); b <- strrep("y", 300)
    r <- get(paste0("/rw/", a, "/", b, "?q=1"))
    expect(r$status == 200 && has(r$text, paste0("query=q=1&a=", a, "&b=", b, ";")), r)
    r <- get(paste0("/rw/", a, "/", b)); expect(r$status == 200 && has(r$text, paste0("query=a=", a, "&b=", b, ";")), r)
    healthy() })
test("rewrite: fixed rewrite with query keeps the rest of the path", {
    r <- get("/rwf/hello.txt"); expect(r$status == 200 && identical(r$text, "hello world\n"), r) })
test("path: '+' is literal in the path", expect(identical(get("/a+b")$text, "plus")))
test("query: '+' is space", expect(has(get("/echo?a=x+y")$text, "a=x y")))
test("path: percent decoding", expect(get("/code/%34%31%38")$status == 418))
test("query: malformed percent escapes", { expect(get("/echo?a=%zz&b=%4")$status == 200); healthy() })
test("url: fragment tolerated", expect(get("/echo?a=1#frag")$status == 200))

## ---- protocol -------------------------------------------------------------
test("proto: HTTP/1.0 closes", {
    con <- conn(3); on.exit(close(con)); send(con, "GET /static/hello.txt HTTP/1.0\r\n\r\n")
    r <- read.resp(con); expect(r$status == 200, r); expect(closed(con), "HTTP/1.0 connection left open") })
test("proto: HTTP/1.1 requires Host", expect(rawreq("GET /static/hello.txt HTTP/1.1\r\n\r\n")$status == 400))
test("proto: keep-alive, 20 requests", {
    con <- conn(); on.exit(close(con))
    for (i in 1:20) { send(con, sprintf("GET /echo?i=%d HTTP/1.1\r\nHost: x\r\n\r\n", i))
        r <- read.resp(con); expect(r$status == 200 && has(r$text, paste0("i=", i)), list(i, r)) } })
test("proto: keep-alive mixed static/rfun", {
    con <- conn(); on.exit(close(con))
    for (p in c("/static/hello.txt", "/echo", "/static/digits.txt", "/raw", "/static/hello.txt")) {
        send(con, sprintf("GET %s HTTP/1.1\r\nHost: x\r\n\r\n", p)); expect(read.resp(con)$status == 200, p) } })
test("proto: Connection: close honoured", {
    con <- conn(3); on.exit(close(con)); send(con, "GET /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    read.resp(con); expect(closed(con)) })
test("proto: Connection header is case-insensitive", {
    con <- conn(3); on.exit(close(con)); send(con, "GET /echo HTTP/1.1\r\nHost: x\r\nCONNECTION: Close\r\n\r\n")
    read.resp(con); expect(closed(con)) })
test("proto: LF-only line endings", expect(rawreq("GET /echo HTTP/1.1\nHost: x\nConnection: close\n\n")$status == 200))
test("proto: byte-at-a-time request", {
    req <- charToRaw("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello")
    con <- conn(); on.exit(close(con))
    for (b in seq_along(req)) { send(con, req[b]); Sys.sleep(0.002) }
    r <- read.resp(con); expect(r$status == 200 && has(r$text, "bodytxt=hello"), r) })
test("proto: request split between CR and LF", {
    con <- conn(); on.exit(close(con))
    send(con, "GET /echo HTTP/1.1\r"); Sys.sleep(0.2); send(con, "\nHost: x\r\nConnection: close\r\n\r\n")
    expect(read.resp(con)$status == 200) })
test("proto: headers and body in separate packets", {
    con <- conn(); on.exit(close(con))
    send(con, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n"); Sys.sleep(0.2)
    send(con, "01234"); Sys.sleep(0.2); send(con, "56789")
    r <- read.resp(con); expect(r$status == 200 && has(r$text, "bodytxt=0123456789"), r) })
test("proto: Expect: 100-continue does not stall", {
    con <- conn(10); on.exit(close(con))
    send(con, "POST /echo HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n")
    ## a compliant client waits only briefly for 100, then sends the body anyway
    Sys.sleep(0.5); send(con, "hello")
    r <- read.resp(con)
    if (isTRUE(r$status == 100L)) r <- read.resp(con)
    expect(r$status == 200, r) })
test("proto: request pipelined after a POST", {
    con <- conn(5); on.exit(close(con))
    send(con, paste0("POST /echo?n=1 HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc",
                     "GET /echo?n=2 HTTP/1.1\r\nHost: x\r\n\r\n"))
    r1 <- read.resp(con); expect(has(r1$text, "n=1") && has(r1$text, "bodytxt=abc"), r1)
    r2 <- read.resp(con); expect(has(r2$text, "n=2"), "request after the POST not answered") })
test("proto: Expect: 100-continue gets 100 before the body is sent", {
    con <- conn(3); on.exit(close(con))
    send(con, "POST /echo HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n")
    r <- read.resp(con); expect(isTRUE(r$status == 100L), r)
    send(con, "hello"); r <- read.resp(con); expect(r$status == 200 && has(r$text, "bodytxt=hello"), r) })
test("proto: pipelined requests", {
    con <- conn(5); on.exit(close(con))
    send(con, "GET /echo?n=1 HTTP/1.1\r\nHost: x\r\n\r\nGET /echo?n=2 HTTP/1.1\r\nHost: x\r\n\r\n")
    r1 <- read.resp(con); expect(has(r1$text, "n=1"))
    r2 <- read.resp(con); expect(has(r2$text, "n=2"), "second pipelined request not answered") })
test("proto: truncated body does not hurt the server", {
    con <- conn(3)
    send(con, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\nshort")
    close(con); settle(); healthy() })

## ---- chunked request bodies -----------------------------------------------
chunked <- function(body, method="POST", path="/echo", extra="")
    rawreq(paste0(method, " ", path, " HTTP/1.1\r\nHost: x\r\n", extra, "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n", body))
test("chunked: single chunk", {
    r <- chunked("5\r\nhello\r\n0\r\n\r\n"); expect(r$status == 200 && has(r$text, "body=5;") && has(r$text, "bodytxt=hello"), r) })
test("chunked: several chunks, extensions and trailer", {
    r <- chunked("3;foo=bar\r\nhel\r\nA\r\nlo world!!\r\n0\r\nX-Trailer: 1\r\n\r\n")
    expect(r$status == 200 && has(r$text, "bodytxt=hello world!!"), r) })
test("chunked: empty body", { r <- chunked("0\r\n\r\n"); expect(r$status == 200 && has(r$text, "body=0;"), r) })
test("chunked: case-insensitive, trailing space", {
    r <- rawreq("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: Chunked \r\nConnection: close\r\n\r\n2\r\nok\r\n0\r\n\r\n")
    expect(r$status == 200 && has(r$text, "bodytxt=ok"), r) })
test("chunked: split across packets", {
    con <- conn(); on.exit(close(con))
    for (p in c("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", "5\r", "\nhel", "lo\r\n",
                "6\r\n world", "\r\n0\r", "\n\r\n")) { send(con, p); Sys.sleep(0.1) }
    r <- read.resp(con); expect(r$status == 200 && has(r$text, "bodytxt=hello world"), r) })
test("chunked: request pipelined after a chunked request", {
    con <- conn(5); on.exit(close(con))
    send(con, paste0("POST /echo?n=1 HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n0\r\n\r\n",
                     "GET /echo?n=2 HTTP/1.1\r\nHost: x\r\n\r\n"))
    r1 <- read.resp(con); expect(has(r1$text, "n=1") && has(r1$text, "bodytxt=ab"), r1)
    r2 <- read.resp(con); expect(has(r2$text, "n=2"), r2) })
test("chunked: Expect: 100-continue", {
    con <- conn(3); on.exit(close(con))
    send(con, "POST /echo HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nTransfer-Encoding: chunked\r\n\r\n")
    r <- read.resp(con); expect(isTRUE(r$status == 100L), r)
    send(con, "2\r\nhi\r\n0\r\n\r\n"); r <- read.resp(con); expect(r$status == 200 && has(r$text, "bodytxt=hi"), r) })
test("chunked: 3MB binary body in two chunks", {
    b <- as.raw(rep(1:255, length.out=3e6)) # no NULs, the fixture echoes the body as a string
    con <- conn(30); on.exit(close(con))
    send(con, "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
    send(con, sprintf("%x\r\n", 1e6)); send(con, b[1:1e6]); send(con, "\r\n")
    send(con, sprintf("%x\r\n", 2e6)); send(con, b[1e6 + 1:2e6]); send(con, "\r\n0\r\n\r\n")
    r <- read.resp(con)
    expect(r$status == 200 && identical(tail(r$body, 3e6), b), list(r$status, length(r$body))) })
test("chunked: invalid chunk size -> 400", { expect(chunked("zz\r\nhello\r\n0\r\n\r\n")$status == 400); healthy() })
test("chunked: missing CRLF after chunk data -> 400", { expect(chunked("3\r\nhello\r\n0\r\n\r\n")$status == 400); healthy() })
test("chunked: huge chunk size -> 413", { expect(chunked("FFFFFFFFFFFFFFFFFFFF\r\n")$status == 413); healthy() })
test("chunked: with Content-Length -> 400", { expect(chunked("5\r\nhello\r\n0\r\n\r\n", extra="Content-Length: 5\r\n")$status == 400); healthy() })
test("chunked: in HTTP/1.0 -> 400", { expect(rawreq("POST /echo HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")$status == 400); healthy() })
test("chunked: GET with chunked body -> 400", { expect(chunked("0\r\n\r\n", method="GET")$status == 400); healthy() })
test("chunked: other transfer codings -> 501", {
    expect(rawreq("POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\nConnection: close\r\n\r\n0\r\n\r\n")$status == 501)
    expect(chunked("0\r\n\r\n", extra="Transfer-Encoding: chunked\r\n")$status == 501, "repeated Transfer-Encoding")
    healthy() })
test("chunked: truncated body is not passed to the handler", {
    con <- conn(3); send(con, "POST /touch?f=chunktrunc HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n10\r\nshort")
    Sys.sleep(0.2); close(con)
    settle(); expect(!file.exists(file.path(DOCROOT, "chunktrunc")), "handler called"); healthy() })

## ---- framing and connection handling -----------------------------------------
test("proto: whitespace before colon -> 400 (no request smuggling)", {
    for (h in c("Content-Length : 41", "Transfer-Encoding : chunked")) {
        con <- conn(3)
        send(con, paste0("POST /echo HTTP/1.1\r\nHost: x\r\n", h, "\r\n\r\nGET /touch?f=smuggled HTTP/1.1\r\nHost: x\r\n\r\n"))
        r <- read.resp(con); close(con); expect(r$status == 400, list(h, r))
    }
    settle(); expect(!file.exists(file.path(DOCROOT, "smuggled")), "smuggled request was processed"); healthy() })
test("proto: duplicate Host -> 400", expect(rawreq("GET /echo HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n")$status == 400))
test("proto: 204 and 304 have no body and keep the connection usable", {
    con <- conn(5); on.exit(close(con))
    for (code in c(204, 304)) {
        send(con, sprintf("GET /code/%d HTTP/1.1\r\nHost: x\r\n\r\n", code))
        r <- read.resp(con); expect(r$status == code && is.null(r$headers[["content-length"]]), list(code, r))
        send(con, "GET /echo?after=1 HTTP/1.1\r\nHost: x\r\n\r\n")
        r <- read.resp(con); expect(r$status == 200 && has(r$text, "after=1"), list(code, "next request:", r))
    } })
test("proto: Connection: close is announced in the response", {
    r <- get("/echo"); expect(identical(tolower(r$headers[["connection"]]), "close"), r)
    con <- conn(3); on.exit(close(con)); send(con, "GET /echo HTTP/1.1\r\nHost: x\r\n\r\n")
    r <- read.resp(con); expect(is.null(r$headers[["connection"]]), list("keep-alive response:", r)) })
test("proto: Connection: keep-alive, close is honoured", {
    con <- conn(3); on.exit(close(con)); send(con, "GET /echo HTTP/1.1\r\nHost: x\r\nConnection: keep-alive, close\r\n\r\n")
    read.resp(con); expect(closed(con)) })
test("proto: handler can close the connection", {
    con <- conn(3); on.exit(close(con)); send(con, "GET /conn-close HTTP/1.1\r\nHost: x\r\n\r\n")
    r <- read.resp(con); expect(r$status == 200 && identical(r$text, "bye"), r); expect(closed(con), "connection left open") })

## ---- malformed ------------------------------------------------------------
test("bad: binary garbage", { r <- request(raw=as.raw(c(1, 2, 3, charToRaw("garbage"), 13, 10, 13, 10))); expect(is.na(r$status) || r$status == 400, r); healthy() })
test("bad: no HTTP version", { expect(rawreq("GET /echo\r\n\r\n")$status == 400); healthy() })
test("bad: HTTP/2.0", { expect.in(rawreq("GET /echo HTTP/2.0\r\nHost: x\r\n\r\n")$status, c(400, 505)); healthy() })
test("bad: negative Content-Length", { expect.in(rawreq("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: -5\r\nConnection: close\r\n\r\n")$status, c(400, 413)); healthy() })
test("bad: huge Content-Length -> 413", { expect(rawreq("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999\r\nConnection: close\r\n\r\n")$status == 413); healthy() })
test("bad: non-numeric Content-Length", { expect.in(rawreq("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\nConnection: close\r\n\r\n")$status, c(200, 400, 411)); healthy() })
test("bad: conflicting Content-Length", {
    r <- rawreq("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nContent-Length: 5\r\nConnection: close\r\n\r\nabcde")
    expect(r$status == 400, r); healthy() })
## the server rejects these without reading the whole request, so closing the
## connection resets it and on Windows the client then loses the response (NA)
expect.reject <- function(status, codes) if (!(WIN && is.na(status))) expect.in(status, codes)
test("bad: 100KB request line", { expect.reject(rawreq(paste0("GET /", strrep("a", 1e5), " HTTP/1.1\r\nHost: x\r\n\r\n"))$status, c(400, 413, 414)); healthy() })
test("bad: 100KB header", { expect.reject(request("GET", "/echo", list("X-Big"=strrep("a", 1e5)))$status, c(400, 413, 431)); healthy() })
test("bad: 2000 headers", { h <- setNames(as.list(rep(strrep("v", 50), 2000)), paste0("X-H", 1:2000)); expect.in(request("GET", "/echo", h)$status, c(200, 400, 413, 431)); healthy() })
test("headers > 1KB are all delivered", {
    h <- setNames(as.list(rep(strrep("v", 100), 40)), paste0("X-H", 0:39))
    r <- get("/hdr", headers=h); low <- gsub("[._]", "-", tolower(r$text)); expect(r$status == 200, r)
    expect(has(low, paste0("x-h39: ", strrep("v", 100))) && has(low, "x-h0: "), substr(r$text, 1, 200)) })
test("bad: folded header line -> 400", {
    for (h in c("X-A: 1\r\n folded", " X-A: 1", "X-A: 1\r\n\tRequest-Client-CN: admin"))
        expect(rawreq(paste0("GET /echo HTTP/1.1\r\nHost: x\r\n", h, "\r\nConnection: close\r\n\r\n"))$status == 400, h)
    healthy() })
test("bad: header without colon", { expect.in(rawreq("GET /echo HTTP/1.1\r\nHost: x\r\nNoColonHere\r\nConnection: close\r\n\r\n")$status, c(200, 400)); healthy() })
test("bad: overlong method", { expect.in(rawreq("VERYLONGMETHODNAME /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")$status, c(200, 400, 501)); healthy() })
test("bad: GET with body", { expect.in(request("GET", "/echo", body="abc")$status, c(200, 400)); healthy() })
test("bad: abrupt disconnects", {
    for (p in c("", "GET", "GET /echo HTTP/1.1\r\n", "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nab")) {
        con <- conn(); if (nzchar(p)) send(con, p); close(con) }
    settle(); healthy() })
test("bad: client closes during large response (SIGPIPE)", {
    con <- conn(); send(con, "GET /big HTTP/1.1\r\nHost: x\r\n\r\n"); rd(con, 1000); close(con)
    settle(); healthy() })
test("bad: odd URLs", {
    for (p in c("/static//hello.txt", "/static/./hello.txt", "/static/hello.txt/", "//static/hello.txt", "*")) {
        r <- rawreq(sprintf("GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", p)); expect(!is.na(r$status), p) }
    healthy() })
test("bad: absolute-form URI", { expect.in(rawreq("GET http://localhost/echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")$status, c(200, 400, 404)); healthy() })

