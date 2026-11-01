#include "ru_http.h"

#include <string.h>

static bool ci_eq_n(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
    if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
    if (x != y) return false;
  }
  return true;
}
static bool tchar(unsigned char c) {
  if (c >= '0' && c <= '9') return true;
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}
static const uint8_t *find_crlf(const uint8_t *p, size_t n) {
  for (size_t i = 0; i + 1 < n; i++)
    if (p[i] == '\r' && p[i + 1] == '\n') return p + i;
  return NULL;
}
static const uint8_t *find_hdr_end(const uint8_t *p, size_t n) {
  for (size_t i = 0; i + 3 < n; i++)
    if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') return p + i;
  return NULL;
}

void ru_http_init(ru_http_conn_t *c, uint32_t now_ms, uint32_t timeout_ms) {
  memset(c, 0, sizeof *c);
  c->start_ms = now_ms;
  c->timeout_ms = timeout_ms;
  c->st = RU_HTTP_NEED_MORE;
}

/* Parses the request line in [p, p+n). */
static ru_http_status_t parse_reqline(ru_http_conn_t *c, const char *p, size_t n) {
  if (n > RU_HTTP_REQLINE_MAX) return RU_HTTP_ERR_URI_TOO_LONG;
  const char *sp1 = memchr(p, ' ', n);
  if (!sp1 || sp1 == p) return RU_HTTP_ERR_BAD;
  size_t ml = (size_t)(sp1 - p);
  for (size_t i = 0; i < ml; i++)
    if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= 'a' && p[i] <= 'z'))) return RU_HTTP_ERR_BAD;
  const char *t = sp1 + 1;
  const char *end = p + n;
  const char *sp2 = memchr(t, ' ', (size_t)(end - t));
  if (!sp2 || sp2 == t) return RU_HTTP_ERR_BAD;
  const char *v = sp2 + 1;
  size_t vl = (size_t)(end - v);
  /* validate target bytes: visible ASCII only */
  for (const char *q = t; q < sp2; q++)
    if ((unsigned char)*q < 0x21 || (unsigned char)*q > 0x7e) return RU_HTTP_ERR_BAD;
  if (*t != '/') return RU_HTTP_ERR_BAD; /* origin-form only: no absolute URI, no '*', no authority */
  if (vl < 5 || memcmp(v, "HTTP/", 5) != 0) return RU_HTTP_ERR_BAD;
  if (!((vl == 8) && (!memcmp(v, "HTTP/1.1", 8) || !memcmp(v, "HTTP/1.0", 8)))) return RU_HTTP_ERR_VERSION;
  if (ml == 3 && !memcmp(p, "GET", 3)) c->method = RU_HTTP_GET;
  else if (ml == 4 && !memcmp(p, "POST", 4)) c->method = RU_HTTP_POST;
  else return RU_HTTP_ERR_METHOD;
  size_t pl = 0;
  while (t + pl < sp2 && t[pl] != '?' && t[pl] != '#') pl++;
  if (pl >= RU_HTTP_PATH_MAX) return RU_HTTP_ERR_URI_TOO_LONG;
  memcpy(c->path, t, pl);
  c->path[pl] = 0;
  return RU_HTTP_NEED_MORE; /* "no error yet" */
}

static ru_http_status_t parse_all(ru_http_conn_t *c) {
  const uint8_t *b = c->buf;
  size_t n = c->len;
  const uint8_t *hend = find_hdr_end(b, n);
  const uint8_t *l1 = find_crlf(b, n);
  /* early rejections that do not need the whole header block */
  if (!l1) {
    if (n > RU_HTTP_REQLINE_MAX + 1) return RU_HTTP_ERR_URI_TOO_LONG;
    for (size_t i = 0; i < n; i++)
      if (b[i] == '\n' || b[i] == 0) return RU_HTTP_ERR_BAD;
    return RU_HTTP_NEED_MORE;
  }
  if (!hend) {
    if (n > RU_HTTP_HDR_TOTAL_MAX) return RU_HTTP_ERR_HEADERS;
  } else if ((size_t)(hend - b) + 4 > RU_HTTP_HDR_TOTAL_MAX) {
    return RU_HTTP_ERR_HEADERS;
  }
  ru_http_status_t r = parse_reqline(c, (const char *)b, (size_t)(l1 - b));
  if (r != RU_HTTP_NEED_MORE) return r;
  if (!hend) return RU_HTTP_NEED_MORE;

  size_t hb = (size_t)(hend - b) + 4;
  const uint8_t *p = l1 + 2;
  unsigned count = 0;
  bool have_cl = false;
  size_t cl = 0;
  c->form_ct = false;
  while (p < hend + 2) { /* hend+2 is the final blank line's CRLF position+... iterate lines until the blank line */
    const uint8_t *e = find_crlf(p, (size_t)(b + hb - p));
    if (!e) return RU_HTTP_ERR_BAD;
    if (e == p) break; /* blank line */
    size_t ll = (size_t)(e - p);
    if (ll > RU_HTTP_HDR_LINE_MAX) return RU_HTTP_ERR_HEADERS;
    if (++count > RU_HTTP_HDR_MAX_COUNT) return RU_HTTP_ERR_HEADERS;
    if (*p == ' ' || *p == '\t') return RU_HTTP_ERR_BAD; /* obs-fold */
    const uint8_t *colon = memchr(p, ':', ll);
    if (!colon || colon == p) return RU_HTTP_ERR_BAD;
    for (const uint8_t *q = p; q < colon; q++)
      if (!tchar(*q)) return RU_HTTP_ERR_BAD;
    const char *name = (const char *)p;
    size_t nl = (size_t)(colon - p);
    const uint8_t *v = colon + 1, *ve = e;
    for (const uint8_t *q = v; q < ve; q++)
      if (*q == '\r' || *q == '\n' || *q == 0 || (*q < 0x20 && *q != '\t') || *q == 0x7f) return RU_HTTP_ERR_BAD;
    while (v < ve && (*v == ' ' || *v == '\t')) v++;
    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
    size_t vl = (size_t)(ve - v);
    if (nl == 14 && ci_eq_n(name, "content-length", 14)) {
      if (have_cl || vl == 0 || vl > 10) return RU_HTTP_ERR_BAD;
      size_t val = 0;
      for (size_t i = 0; i < vl; i++) {
        if (v[i] < '0' || v[i] > '9') return RU_HTTP_ERR_BAD;
        val = val * 10 + (size_t)(v[i] - '0');
      }
      if (val > RU_HTTP_BODY_MAX) return RU_HTTP_ERR_TOO_LARGE;
      have_cl = true;
      cl = val;
    } else if (nl == 17 && ci_eq_n(name, "transfer-encoding", 17)) {
      return RU_HTTP_ERR_NOT_IMPL;
    } else if (nl == 12 && ci_eq_n(name, "content-type", 12)) {
      static const char ft[] = "application/x-www-form-urlencoded";
      size_t fl = sizeof ft - 1;
      if (vl >= fl && ci_eq_n((const char *)v, ft, fl) && (vl == fl || v[fl] == ';' || v[fl] == ' ')) c->form_ct = true;
    }
    p = e + 2;
  }
  if (c->method == RU_HTTP_GET) {
    if (have_cl && cl != 0) return RU_HTTP_ERR_BAD;
    cl = 0;
  } else if (!have_cl) {
    return RU_HTTP_ERR_LENGTH_REQ;
  }
  c->body_off = hb;
  c->body_len = cl;
  if (n < hb + cl) return RU_HTTP_NEED_MORE;
  if (n > hb + cl) return RU_HTTP_ERR_BAD; /* one request per connection: no pipelining */
  return RU_HTTP_DONE;
}

ru_http_status_t ru_http_poll(ru_http_conn_t *c, uint32_t now_ms) {
  if (c->st == RU_HTTP_NEED_MORE && (uint32_t)(now_ms - c->start_ms) > c->timeout_ms) c->st = RU_HTTP_ERR_TIMEOUT;
  return c->st;
}

ru_http_status_t ru_http_feed(ru_http_conn_t *c, const void *data, size_t n, uint32_t now_ms) {
  if (c->st == RU_HTTP_DONE) { c->st = RU_HTTP_ERR_BAD; return c->st; }
  if (c->st != RU_HTTP_NEED_MORE) return c->st;
  if (ru_http_poll(c, now_ms) != RU_HTTP_NEED_MORE) return c->st;
  if (!data && n) { c->st = RU_HTTP_ERR_BAD; return c->st; }
  bool overflow = false;
  size_t room = RU_HTTP_BUF_MAX - c->len;
  if (n > room) { n = room; overflow = true; }
  if (n) memcpy(c->buf + c->len, data, n);
  c->len += n;
  c->buf[c->len] = 0;
  ru_http_status_t r = parse_all(c);
  if (overflow) {
    if (r == RU_HTTP_NEED_MORE) r = find_hdr_end(c->buf, c->len) ? RU_HTTP_ERR_TOO_LARGE : RU_HTTP_ERR_HEADERS;
    else if (r == RU_HTTP_DONE) r = RU_HTTP_ERR_BAD;
  }
  c->st = r;
  return r;
}

int ru_http_status_code(ru_http_status_t s) {
  switch (s) {
    case RU_HTTP_NEED_MORE: return 0;
    case RU_HTTP_DONE: return 200;
    case RU_HTTP_ERR_BAD: return 400;
    case RU_HTTP_ERR_TIMEOUT: return 408;
    case RU_HTTP_ERR_METHOD: return 405;
    case RU_HTTP_ERR_LENGTH_REQ: return 411;
    case RU_HTTP_ERR_TOO_LARGE: return 413;
    case RU_HTTP_ERR_URI_TOO_LONG: return 414;
    case RU_HTTP_ERR_HEADERS: return 431;
    case RU_HTTP_ERR_NOT_IMPL: return 501;
    case RU_HTTP_ERR_VERSION: return 505;
  }
  return 400;
}

const char *ru_http_body(const ru_http_conn_t *c) { return (const char *)c->buf + c->body_off; }

/* ---------------- forms ---------------- */
static int hexv(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}
/* Decodes [s, s+n) into out; returns 0 ok, 1 bad escape/NUL, 2 too long. out may be NULL (validate only). */
static int decode(const char *s, size_t n, char *out, size_t cap, size_t *outlen) {
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    char ch = s[i];
    if (ch == '+') ch = ' ';
    else if (ch == '%') {
      if (i + 2 >= n) return 1;
      int h = hexv(s[i + 1]), l = hexv(s[i + 2]);
      if (h < 0 || l < 0) return 1;
      ch = (char)(h * 16 + l);
      i += 2;
    }
    if (ch == 0) return 1;
    if (out) {
      if (o + 1 >= cap) return 2;
      out[o] = ch;
    }
    o++;
  }
  if (out) out[o] = 0;
  if (outlen) *outlen = o;
  return 0;
}

static const char *next_field(const char *p, const char *end, const char **fe) {
  const char *q = p;
  while (q < end && *q != '&') q++;
  *fe = q;
  return p;
}

bool ru_form_check(const char *body, size_t len) {
  if (!body) return false;
  const char *p = body, *end = body + len;
  unsigned count = 0;
  if (len == 0) return true;
  for (;;) {
    const char *fe;
    next_field(p, end, &fe);
    if (fe > p) { /* empty fields (a&&b) are ignored */
      if (++count > RU_FORM_MAX_FIELDS) return false;
      const char *eq = memchr(p, '=', (size_t)(fe - p));
      const char *ke = eq ? eq : fe;
      if ((size_t)(ke - p) > RU_FORM_KEY_MAX * 3) return false;
      size_t kl;
      char kb[RU_FORM_KEY_MAX + 1];
      int r = decode(p, (size_t)(ke - p), kb, sizeof kb, &kl);
      if (r) return false;
      if (eq && decode(eq + 1, (size_t)(fe - eq - 1), NULL, 0, NULL)) return false;
    }
    if (fe >= end) break;
    p = fe + 1;
  }
  return true;
}

ru_form_err_t ru_form_get(const char *body, size_t len, const char *key, char *out, size_t cap) {
  if (!out || !cap) return RU_FORM_BAD;
  out[0] = 0;
  if (!body || !key) return RU_FORM_BAD;
  size_t klen = strlen(key);
  const char *p = body, *end = body + len;
  if (len == 0) return RU_FORM_NOT_FOUND;
  for (;;) {
    const char *fe;
    next_field(p, end, &fe);
    if (fe > p) {
      const char *eq = memchr(p, '=', (size_t)(fe - p));
      const char *ke = eq ? eq : fe;
      char kb[RU_FORM_KEY_MAX + 1];
      size_t kl = 0;
      if ((size_t)(ke - p) <= RU_FORM_KEY_MAX * 3 && decode(p, (size_t)(ke - p), kb, sizeof kb, &kl) == 0 && kl == klen &&
          memcmp(kb, key, klen) == 0) {
        if (!eq) { out[0] = 0; return RU_FORM_OK; }
        int r = decode(eq + 1, (size_t)(fe - eq - 1), out, cap, NULL);
        if (r) { memset(out, 0, cap); return r == 2 ? RU_FORM_TOO_LONG : RU_FORM_BAD; }
        return RU_FORM_OK;
      }
    }
    if (fe >= end) break;
    p = fe + 1;
  }
  return RU_FORM_NOT_FOUND;
}
