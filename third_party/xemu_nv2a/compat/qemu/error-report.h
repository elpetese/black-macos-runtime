#ifndef XEMU_COMPAT_ERROR_REPORT_H
#define XEMU_COMPAT_ERROR_REPORT_H
#include <stdio.h>
#define error_report(...) (fprintf(stderr, "[nv2a] " __VA_ARGS__), fputc('\n', stderr))
#define warn_report(...)  (fprintf(stderr, "[nv2a] warning: " __VA_ARGS__), fputc('\n', stderr))
#define info_report(...)  (fprintf(stderr, "[nv2a] " __VA_ARGS__), fputc('\n', stderr))
#define warn_report_once(...) do { static int _w; if (!_w++) warn_report(__VA_ARGS__); } while (0)
#define error_report_once(...) do { static int _w; if (!_w++) error_report(__VA_ARGS__); } while (0)
#endif
