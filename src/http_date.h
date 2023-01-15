#ifndef HTTP_DATE_H__
#define HTTP_DATE_H__

/* utilities to parse valid HTTP times
   and to generate RFC 822/1123 date 
   
   (C)Copyright 2014-22 Simon Urbanek

   License: MIT
*/

char *posix2http(double ts, char date_buf[32]);
double http2posix(const char *c);

#endif
