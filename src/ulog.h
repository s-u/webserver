#ifndef ULOG_H__
#define ULOG_H__

/* debug output - change the DBG(X) X to enable debugging output */
#ifndef DBG
#ifdef RSERV_DEBUG
#define DBG(X) X
#else
#define DBG(X) while(0) {}
#endif
#endif

/* levels, compatible with syslog */
#define UL_PANIC 0
#define UL_ALERT 1
#define UL_CRIT  2
#define UL_ERROR 3
#define UL_WARN  4
#define UL_NOTE  5
#define UL_INFO  6
#define UL_DEBUG 7

/* convenience macros */
#if (defined (__STDC_VERSION__)) && (__STDC_VERSION__ >= 199900L)
#define ulog_info(...)  ulog(UL_INFO, __VA_ARGS__)
#define ulog_warn(...)  ulog(UL_WARN, __VA_ARGS__)
#define ulog_debug(...) ulog(UL_DEBUG, __VA_ARGS__)
#define ulog_note(...)  ulog(UL_NOTE, __VA_ARGS__)
#define ulog_error(...) ulog(UL_ERROR, __VA_ARGS__)
#else /* no vararg macros supported */
#define ulog_info(...)  do { ulog_begin(UL_INFO);  ulog_add(...); ulog_end(); } while(0)
#define ulog_warn(...)  do { ulog_begin(UL_WARN);  ulog_add(...); ulog_end(); } while(0)
#define ulog_debug(...) do { ulog_begin(UL_DEBUG); ulog_add(...); ulog_end(); } while(0)
#define ulog_note(...)  do { ulog_begin(UL_NOTE);  ulog_add(...); ulog_end(); } while(0)
#define ulog_error(...) do { ulog_begin(UL_ERROR); ulog_add(...); ulog_end(); } while(0)
#endif

#ifdef NO_ULOG
/* no logging at all - this is for non-main modules only */
#define ulog(...) while(0) {}
#define ulog_begin(L) while(0) {}
#define ulog_add(...) while(0) {}
#define ulog_end() while(0) {}
#else

/* --- main app API - used by top-level code --- */

/* set target, app name and facility */
void ulog_set_path(const char *path);
void ulog_set_app_name(const char *name);
void ulog_set_facility(int fac);

/* returns 1 if path/target has been set, 0 otherwise */
int  ulog_enabled(void);

/* reset the logging connection. This must be called
   if the path has been changed to force re-connect. */
void ulog_reset(void);

/* --- logging API - used by code that wants to log --- */

/* logging commands */
void ulog_begin(int level);
void ulog_add(const char *format, ...);
void ulog_end(void);
void ulog(int level, const char *format, ...);

#endif /* NO_ULOG */

#endif
