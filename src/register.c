#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

/* Rmain.c */
SEXP C_add_handler(SEXP args);
SEXP C_remove_all_handlers(void);
SEXP C_parse_headers(SEXP sWhat);
SEXP C_start(SEXP sPort, SEXP sSock, SEXP sThreads,
             SEXP sFork, SEXP sIPv6, SEXP sLocal);
/* r_util.c */
SEXP C_fds2file(SEXP sFile, SEXP sWhich, SEXP sAppend,
                SEXP sMode, SEXP sClose);

static const R_CallMethodDef CallEntries[] = {
    {"C_add_handler",         (DL_FUNC) &C_add_handler,          1},
    {"C_remove_all_handlers", (DL_FUNC) &C_remove_all_handlers,  0},
    {"C_start",               (DL_FUNC) &C_start,                6},
    {"C_parse_headers",       (DL_FUNC) &C_parse_headers,        1},
    {"C_fds2file",            (DL_FUNC) &C_fds2file,             5},
    {NULL, NULL, 0}
};

void R_init_webserver(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
}
