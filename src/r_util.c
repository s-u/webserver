#ifndef NO_R

#include <Rinternals.h>

#include <unistd.h>
#include <fcntl.h>

SEXP C_fds2file(SEXP sFile, SEXP sWhich, SEXP sAppend, SEXP sMode, SEXP sClose) {
    SEXP res;
    int app = Rf_asInteger(sAppend), mode = Rf_asInteger(sMode),
	cl  = Rf_asInteger(sClose);
    const char *fn;
    int fd, i = 0;
    if (TYPEOF(sFile) != STRSXP || LENGTH(sFile) != 1)
	Rf_error("Invalid filename, must be a string");
    if (TYPEOF(sWhich) != INTSXP)
	Rf_error("Invalid list of descriptors to redirect");
    fn = CHAR(STRING_ELT(sFile, 0));
    fd = open(fn, O_WRONLY | O_CREAT | (app ? O_APPEND : 0), mode);
    if (fd == -1)
	Rf_error("Unable to write to '%s'", fn);
    res = PROTECT(Rf_allocVector(LGLSXP, LENGTH(sWhich)));
    while (i < LENGTH(sWhich)) {
	LOGICAL(res)[i] = (dup2(fd, INTEGER(sWhich)[i]) != -1);
	if (cl)
	    close(INTEGER(sWhich)[i]);
	i++;
    }
    UNPROTECT(1);
    return res;
}

#endif
