/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — {{TOKEN}} substitution for embedded HTML pages.
 *
 * Pages live as real .html files under main/www/ and are linked in by
 * EMBED_TXTFILES (see main/CMakeLists.txt), which NUL-terminates them and
 * exposes _binary_<name>_html_start. Keeping them as files rather than C string
 * literals means they can be opened in a browser, linted and diffed like markup
 * instead of reviewed as escaped soup -- which is how a chunk landed inside an
 * attribute value and rendered the network list into an <input>.
 *
 * The substituter streams the page straight to the response, so no copy of it
 * exists in RAM.
 */
#ifndef WWW_TPL_H
#define WWW_TPL_H

#include "esp_http_server.h"

/* Emits the replacement for one {{TOKEN}}. Write with httpd_resp_sendstr_chunk;
 * emit nothing for an empty value. */
typedef void (*tpl_emit_fn)(httpd_req_t *req, void *user);

struct tpl_var {
    const char *name;   /* token name without the braces, e.g. "SSID" */
    const char *value;  /* literal replacement, or NULL to use emit */
    tpl_emit_fn emit;   /* used when value is NULL */
};

/* Stream `tpl` to the response, replacing every {{NAME}} that appears in vars.
 * An unknown token is emitted verbatim so a typo is visible on the page rather
 * than silently blanking content. */
void tpl_send(httpd_req_t *req, const char *tpl,
              const struct tpl_var *vars, size_t nvars, void *user);

#endif /* WWW_TPL_H */
