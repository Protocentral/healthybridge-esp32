/* Minimal shim so www_tpl.c compiles on the host. */
#ifndef SHIM_ESP_HTTP_SERVER_H
#define SHIM_ESP_HTTP_SERVER_H
#include <stddef.h>
#include <sys/types.h>
typedef int esp_err_t;
typedef struct httpd_req httpd_req_t;
#define HTTPD_RESP_USE_STRLEN (-1)
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len);
#define httpd_resp_sendstr_chunk(r, s) httpd_resp_send_chunk((r), (s), HTTPD_RESP_USE_STRLEN)
#endif
