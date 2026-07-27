/*
 * SPDX-License-Identifier: MIT
 * Host test for the {{TOKEN}} substituter. Build:
 *   cc -std=c11 -Wall -Wextra -I main -I test test/test_www_tpl.c main/www_tpl.c \
 *      -o /tmp/tpl_test && /tmp/tpl_test
 *
 * The bug this exists to prevent: a ZERO-LENGTH CHUNK TERMINATES a chunked HTTP
 * response. One empty token value silently truncated the captive portal right
 * before its Save button, and the page arrived without its bottom half. The
 * stub below records a zero-length send as a hard failure, exactly as the real
 * httpd would treat it.
 */
#include <stdio.h>
#include <string.h>
#include "www_tpl.h"

char g_out[16384];
size_t g_len;
int g_zero_sends;
int g_terminated;

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len)
{
    (void)r;
    if (buf == NULL || len == 0) { g_terminated++; return 0; }
    if (len == HTTPD_RESP_USE_STRLEN) len = (ssize_t)strlen(buf);
    if (len == 0) { g_zero_sends++; g_terminated++; return 0; }
    memcpy(g_out + g_len, buf, (size_t)len);
    g_len += (size_t)len;
    g_out[g_len] = '\0';
    return 0;
}

static int fails;
static void ck(const char *what, int cond)
{
    printf(cond ? "  ok   %s\n" : "  FAIL %s\n", what);
    if (!cond) fails++;
}

static void emit_list(httpd_req_t *req, void *user)
{
    (void)user;
    httpd_resp_send_chunk(req, "<li>a</li><li>b</li>", HTTPD_RESP_USE_STRLEN);
}

static void reset(void){ g_len=0; g_out[0]=0; g_zero_sends=0; g_terminated=0; }

int main(void)
{
    /* 1. The regression: an EMPTY value must not truncate the page. */
    reset();
    {
        const struct tpl_var v[] = { { "URI", "", NULL }, { "SSID", "Zeus", NULL } };
        tpl_send(NULL, "A<input value=\"{{URI}}\">B{{SSID}}C", v, 2, NULL);
        ck("empty value does not truncate",
           strcmp(g_out, "A<input value=\"\">BZeusC") == 0);
        ck("no zero-length chunk emitted", g_zero_sends == 0);
        ck("terminated exactly once", g_terminated == 1);
    }

    /* 2. Adjacent tokens: the gap between them is zero bytes. */
    reset();
    {
        const struct tpl_var v[] = { { "A", "1", NULL }, { "B", "2", NULL } };
        tpl_send(NULL, "{{A}}{{B}}", v, 2, NULL);
        ck("adjacent tokens", strcmp(g_out, "12") == 0);
        ck("adjacent: no zero-length chunk", g_zero_sends == 0);
    }

    /* 3. Token at the very start and very end. */
    reset();
    {
        const struct tpl_var v[] = { { "X", "hello", NULL } };
        tpl_send(NULL, "{{X}}", v, 1, NULL);
        ck("token is the whole template", strcmp(g_out, "hello") == 0);
    }

    /* 4. Callback token. */
    reset();
    {
        const struct tpl_var v[] = { { "L", NULL, emit_list } };
        tpl_send(NULL, "<ul>{{L}}</ul>", v, 1, NULL);
        ck("callback token", strcmp(g_out, "<ul><li>a</li><li>b</li></ul>") == 0);
    }

    /* 5. An unknown token is emitted verbatim, not silently blanked -- a typo
     *    should be visible on the page. */
    reset();
    {
        const struct tpl_var v[] = { { "A", "1", NULL } };
        tpl_send(NULL, "x{{NOPE}}y", v, 1, NULL);
        ck("unknown token kept verbatim", strcmp(g_out, "x{{NOPE}}y") == 0);
    }

    /* 6. CSS/JS braces must survive: the page is full of them. */
    reset();
    {
        const struct tpl_var v[] = { { "A", "1", NULL } };
        tpl_send(NULL, "@media{*{a:b}}f(){}{{A}}", v, 1, NULL);
        ck("braces in css/js survive", strcmp(g_out, "@media{*{a:b}}f(){}1") == 0);
    }

    /* 7. A lone "{{" with no closer must not eat the rest of the page. */
    reset();
    {
        const struct tpl_var v[] = { { "A", "1", NULL } };
        tpl_send(NULL, "keep {{ this and this", v, 1, NULL);
        ck("unterminated {{ keeps the tail",
           strcmp(g_out, "keep {{ this and this") == 0);
    }

    /* 8. Template with no tokens at all. */
    reset();
    {
        const struct tpl_var v[] = { { "A", "1", NULL } };
        tpl_send(NULL, "<html>plain</html>", v, 1, NULL);
        ck("token-free template", strcmp(g_out, "<html>plain</html>") == 0);
    }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
