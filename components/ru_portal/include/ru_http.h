/* ru_http: strict HTTP/1.1 request parser for the setup portal, plus application/x-www-form-urlencoded decoding.
 * Pure C, no IDF, no allocation. One request per connection: GET and POST only, bounded everywhere.
 *
 * The parser is incremental and stateless between feeds (it re-parses the accumulated bytes), so a request truncated at
 * any byte yields RU_HTTP_NEED_MORE or an error, never RU_HTTP_DONE. The timeout is a TOTAL deadline for the whole
 * request measured from ru_http_init (not an idle timeout), which defeats slowloris-style trickling. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RU_HTTP_REQLINE_MAX 256   /* request line, excluding CRLF */
#define RU_HTTP_PATH_MAX 96       /* path incl. NUL (query and fragment are dropped) */
#define RU_HTTP_HDR_MAX_COUNT 16
#define RU_HTTP_HDR_LINE_MAX 256  /* one header line, excluding CRLF */
#define RU_HTTP_HDR_TOTAL_MAX 1536 /* request line + all headers + blank line */
#define RU_HTTP_BODY_MAX 2048
#define RU_HTTP_BUF_MAX (RU_HTTP_HDR_TOTAL_MAX + RU_HTTP_BODY_MAX)

typedef enum { RU_HTTP_GET = 1, RU_HTTP_POST = 2 } ru_http_method_t;

typedef enum {
  RU_HTTP_NEED_MORE = 0,
  RU_HTTP_DONE,
  RU_HTTP_ERR_BAD,          /* 400 malformed, or bytes after the end of the request */
  RU_HTTP_ERR_TIMEOUT,      /* 408 */
  RU_HTTP_ERR_METHOD,       /* 405 */
  RU_HTTP_ERR_LENGTH_REQ,   /* 411 POST without Content-Length */
  RU_HTTP_ERR_TOO_LARGE,    /* 413 body over RU_HTTP_BODY_MAX */
  RU_HTTP_ERR_URI_TOO_LONG, /* 414 */
  RU_HTTP_ERR_HEADERS,      /* 431 header line, count or total too large */
  RU_HTTP_ERR_NOT_IMPL,     /* 501 any Transfer-Encoding (chunked is not supported) */
  RU_HTTP_ERR_VERSION       /* 505 */
} ru_http_status_t;

typedef struct {
  uint8_t buf[RU_HTTP_BUF_MAX + 1];
  size_t len;
  uint32_t start_ms, timeout_ms;
  ru_http_status_t st;
  /* results, valid when st == RU_HTTP_DONE */
  ru_http_method_t method;
  char path[RU_HTTP_PATH_MAX];
  bool form_ct; /* Content-Type: application/x-www-form-urlencoded */
  size_t body_off, body_len;
} ru_http_conn_t;

void ru_http_init(ru_http_conn_t *c, uint32_t now_ms, uint32_t timeout_ms);
/* Appends n bytes and re-evaluates. Terminal results are sticky; feeding more after RU_HTTP_DONE gives RU_HTTP_ERR_BAD. */
ru_http_status_t ru_http_feed(ru_http_conn_t *c, const void *data, size_t n, uint32_t now_ms);
/* No new bytes: only the deadline is checked (call from the socket's poll timeout). */
ru_http_status_t ru_http_poll(ru_http_conn_t *c, uint32_t now_ms);
int ru_http_status_code(ru_http_status_t s); /* HTTP status to answer with; 200 for DONE, 0 for NEED_MORE */
const char *ru_http_body(const ru_http_conn_t *c); /* NUL terminated, body_len bytes (DONE only) */

/* ---- application/x-www-form-urlencoded ---- */
#define RU_FORM_MAX_FIELDS 16
#define RU_FORM_KEY_MAX 32
typedef enum { RU_FORM_OK = 0, RU_FORM_NOT_FOUND, RU_FORM_TOO_LONG, RU_FORM_BAD } ru_form_err_t;
/* Well-formed: at most RU_FORM_MAX_FIELDS fields, keys <= RU_FORM_KEY_MAX bytes, every %XX valid, no decoded NUL. */
bool ru_form_check(const char *body, size_t len);
/* Decodes the FIRST field called key ('+' is a space, %XX decoded) into out (always NUL terminated on OK; zeroed on
 * any error). TOO_LONG when the decoded value needs cap or more bytes. BAD on a malformed escape or a decoded NUL. */
ru_form_err_t ru_form_get(const char *body, size_t len, const char *key, char *out, size_t cap);
