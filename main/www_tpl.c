/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — {{TOKEN}} substitution. See www_tpl.h.
 */
#include <stdbool.h>
#include <string.h>

#include "www_tpl.h"

#define TOK_OPEN  "{{"
#define TOK_CLOSE "}}"
#define TOK_MAX   32

/*
 * A ZERO-LENGTH CHUNK TERMINATES A CHUNKED RESPONSE.
 *
 * httpd_resp_send_chunk() with buf_len 0 emits the "0\r\n\r\n" terminator, so
 * one empty token value silently truncates the page from that point on. This
 * cost a debugging round: an unconfigured {{MQTTURI}} ended the portal right
 * before the Save button, and the page simply arrived without its bottom half.
 * Every send below goes through these guards.
 */
static void send_len(httpd_req_t *req, const char *buf, size_t len)
{
    if (len > 0) {
        httpd_resp_send_chunk(req, buf, (ssize_t)len);
    }
}

static void send_str(httpd_req_t *req, const char *s)
{
    if (s != NULL && s[0] != '\0') {
        httpd_resp_sendstr_chunk(req, s);
    }
}

void tpl_send(httpd_req_t *req, const char *tpl,
              const struct tpl_var *vars, size_t nvars, void *user)
{
    const char *p = tpl;

    while (*p) {
        const char *open = strstr(p, TOK_OPEN);
        if (!open) {
            break;
        }
        const char *close = strstr(open, TOK_CLOSE);
        size_t name_len = close ? (size_t)(close - open - 2) : 0;
        if (!close || name_len == 0 || name_len >= TOK_MAX) {
            /* Not a token after all -- keep the "{{" and carry on, so literal
             * braces in CSS or script survive untouched. */
            send_len(req, p, (size_t)(open - p + 2));
            p = open + 2;
            continue;
        }

        send_len(req, p, (size_t)(open - p));

        char name[TOK_MAX];
        memcpy(name, open + 2, name_len);
        name[name_len] = '\0';

        bool matched = false;
        for (size_t i = 0; i < nvars; i++) {
            if (strcmp(vars[i].name, name) != 0) {
                continue;
            }
            matched = true;
            if (vars[i].value) {
                send_str(req, vars[i].value);
            } else if (vars[i].emit) {
                vars[i].emit(req, user);
            }
            break;
        }
        if (!matched) {
            /* Emit the token verbatim: a typo should be visible on the page,
             * not a silently missing field. */
            send_len(req, open, (size_t)(close - open + 2));
        }
        p = close + 2;
    }

    send_str(req, p);
    httpd_resp_sendstr_chunk(req, NULL);   /* the intended terminator */
}
