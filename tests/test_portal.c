/* ru_portal: HTTP parser (truncation, mutation fuzz, limits, slowloris), forms, DNS, QR, password generator, flow, CSRF,
 * escaping, token secrecy, rate limit, one client, timeouts. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_test.h"
#include "ru_portal.h"

static uint32_t rng_state = 12345;
static uint32_t rnd32(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

/* ============================ HTTP parser ============================ */
static ru_http_status_t parse_all_at(const char *s, size_t n, ru_http_conn_t *c) {
  ru_http_init(c, 1000, 5000);
  return ru_http_feed(c, s, n, 1000);
}
static ru_http_status_t parse_str(const char *s) { static ru_http_conn_t c; return parse_all_at(s, strlen(s), &c); }

static const char *GET1 = "GET /generate_204?x=1 HTTP/1.1\r\nHost: 192.168.4.1\r\nUser-Agent: t\r\nAccept: */*\r\n\r\n";
static const char *POST1 = "POST /wifi HTTP/1.1\r\nHost: a\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 12\r\n\r\nssid=a&p=%41";
static const char *POST2 = "POST /domoticz HTTP/1.0\r\ncontent-length: 0\r\n\r\n";

static void test_http_basic(void) {
  static ru_http_conn_t c;
  CHECK(parse_all_at(GET1, strlen(GET1), &c) == RU_HTTP_DONE);
  CHECK(c.method == RU_HTTP_GET && !strcmp(c.path, "/generate_204") && c.body_len == 0);
  const char *p1 = "POST /wifi HTTP/1.1\r\nHost: a\r\nContent-Type: application/x-www-form-urlencoded; charset=UTF-8\r\nContent-Length: 12\r\n\r\nssid=a&p=%41";
  CHECK(parse_all_at(p1, strlen(p1), &c) == RU_HTTP_DONE);
  CHECK(c.method == RU_HTTP_POST && c.form_ct && c.body_len == 12 && !memcmp(ru_http_body(&c), "ssid=a&p=%41", 12));
  CHECK(parse_all_at(POST2, strlen(POST2), &c) == RU_HTTP_DONE && !c.form_ct && c.body_len == 0);
  /* feeding after DONE is a second request on the connection: refused */
  parse_all_at(GET1, strlen(GET1), &c);
  CHECK(ru_http_feed(&c, "G", 1, 1001) == RU_HTTP_ERR_BAD);
  /* byte by byte gives the same result */
  ru_http_init(&c, 0, 5000);
  ru_http_status_t r = RU_HTTP_NEED_MORE;
  for (size_t i = 0; i < strlen(p1); i++) {
    CHECK(r == RU_HTTP_NEED_MORE);
    r = ru_http_feed(&c, p1 + i, 1, 10);
  }
  CHECK(r == RU_HTTP_DONE && c.body_len == 12);
  CHECK(ru_http_status_code(RU_HTTP_ERR_HEADERS) == 431 && ru_http_status_code(RU_HTTP_ERR_TIMEOUT) == 408);
}

static void test_http_truncation(void) {
  const char *reqs[] = {GET1, POST1, POST2,
                        "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello",
                        "GET / HTTP/1.1\r\n\r\n"};
  static ru_http_conn_t c;
  for (size_t k = 0; k < sizeof reqs / sizeof *reqs; k++) {
    size_t n = strlen(reqs[k]);
    for (size_t t = 0; t < n; t++) { /* every strict prefix: never DONE, never a crash */
      ru_http_status_t r = parse_all_at(reqs[k], t, &c);
      CHECK(r != RU_HTTP_DONE);
    }
    CHECK(parse_all_at(reqs[k], n, &c) == RU_HTTP_DONE);
    /* one extra byte after the request is refused */
    char extra[512];
    snprintf(extra, sizeof extra, "%sX", reqs[k]);
    CHECK(parse_str(extra) == RU_HTTP_ERR_BAD);
  }
}

static void test_http_limits(void) {
  char big[RU_HTTP_BUF_MAX * 2];
  char line[600];
  /* request line too long */
  memset(line, 'a', sizeof line - 1); line[sizeof line - 1] = 0;
  snprintf(big, sizeof big, "GET /%s HTTP/1.1\r\n\r\n", line);
  CHECK(parse_str(big) == RU_HTTP_ERR_URI_TOO_LONG);
  /* ... also when no CRLF ever arrives */
  snprintf(big, sizeof big, "GET /%s", line);
  CHECK(parse_str(big) == RU_HTTP_ERR_URI_TOO_LONG);
  /* path longer than 95 but request line within 256 */
  memset(line, 'a', 120); line[120] = 0;
  snprintf(big, sizeof big, "GET /%s HTTP/1.1\r\n\r\n", line);
  CHECK(parse_str(big) == RU_HTTP_ERR_URI_TOO_LONG);
  /* a long query is dropped, not stored */
  snprintf(big, sizeof big, "GET /a?%s HTTP/1.1\r\n\r\n", line);
  CHECK(parse_str(big) == RU_HTTP_DONE);
  /* header line too long */
  memset(line, 'b', 300); line[300] = 0;
  snprintf(big, sizeof big, "GET / HTTP/1.1\r\nX: %s\r\n\r\n", line);
  CHECK(parse_str(big) == RU_HTTP_ERR_HEADERS);
  /* 17 headers */
  strcpy(big, "GET / HTTP/1.1\r\n");
  for (int i = 0; i < 17; i++) strcat(big, "A: b\r\n");
  strcat(big, "\r\n");
  CHECK(parse_str(big) == RU_HTTP_ERR_HEADERS);
  strcpy(big, "GET / HTTP/1.1\r\n");
  for (int i = 0; i < 16; i++) strcat(big, "A: b\r\n");
  strcat(big, "\r\n");
  CHECK(parse_str(big) == RU_HTTP_DONE);
  /* total header block over 1536 (each line ok) */
  strcpy(big, "GET / HTTP/1.1\r\n");
  memset(line, 'c', 200); line[200] = 0;
  for (int i = 0; i < 9; i++) { strcat(big, "H: "); strcat(big, line); strcat(big, "\r\n"); }
  strcat(big, "\r\n");
  CHECK(parse_str(big) == RU_HTTP_ERR_HEADERS);
  /* endless headers without the blank line */
  strcpy(big, "GET / HTTP/1.1\r\nA: b\r\n");
  { static ru_http_conn_t c; ru_http_init(&c, 0, 99999); ru_http_status_t r = RU_HTTP_NEED_MORE; int rounds = 0;
    r = ru_http_feed(&c, big, strlen(big), 0);
    while (r == RU_HTTP_NEED_MORE && rounds++ < 1000) r = ru_http_feed(&c, "Z: yyyyyyyy\r\n", 13, 0);
    CHECK(r == RU_HTTP_ERR_HEADERS); }
  /* body limits */
  CHECK(parse_str("POST / HTTP/1.1\r\nContent-Length: 2049\r\n\r\n") == RU_HTTP_ERR_TOO_LARGE);
  CHECK(parse_str("POST / HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("POST / HTTP/1.1\r\nContent-Length: 18446744073709551616\r\n\r\n") == RU_HTTP_ERR_BAD);
  {
    static char post[RU_HTTP_BUF_MAX + 64];
    int h = snprintf(post, sizeof post, "POST / HTTP/1.1\r\nContent-Length: 2048\r\n\r\n");
    memset(post + h, 'x', 2048);
    static ru_http_conn_t c;
    CHECK(parse_all_at(post, (size_t)h + 2048, &c) == RU_HTTP_DONE && c.body_len == 2048);
    CHECK(parse_all_at(post, (size_t)h + 2049, &c) == RU_HTTP_ERR_BAD); /* more than declared */
    CHECK(parse_all_at(post, (size_t)h + 2047, &c) == RU_HTTP_NEED_MORE);
    /* a flood far past any limit in one feed */
    memset(post, 'x', sizeof post);
    CHECK(parse_all_at(post, sizeof post, &c) != RU_HTTP_DONE && c.len <= RU_HTTP_BUF_MAX);
    CHECK(parse_all_at("POST / HTTP/1.1\r\nContent-Length: 2048\r\n\r\n", 0, &c) == RU_HTTP_NEED_MORE);
  }
  /* strictness */
  CHECK(parse_str("POST / HTTP/1.1\r\n\r\n") == RU_HTTP_ERR_LENGTH_REQ);
  CHECK(parse_str("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n") == RU_HTTP_ERR_NOT_IMPL);
  CHECK(parse_str("POST / HTTP/1.1\r\ntransfer-encoding: identity\r\nContent-Length: 0\r\n\r\n") == RU_HTTP_ERR_NOT_IMPL);
  CHECK(parse_str("PUT / HTTP/1.1\r\n\r\n") == RU_HTTP_ERR_METHOD);
  CHECK(parse_str("DELETE / HTTP/1.1\r\n\r\n") == RU_HTTP_ERR_METHOD);
  CHECK(parse_str("GET / HTTP/2.0\r\n\r\n") == RU_HTTP_ERR_VERSION);
  CHECK(parse_str("GET / HTTP/1.2\r\n\r\n") == RU_HTTP_ERR_VERSION);
  CHECK(parse_str("GET / FOO\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET http://evil/ HTTP/1.1\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET * HTTP/1.1\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET /a b HTTP/1.1\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET / HTTP/1.1\nHost: a\n\n") == RU_HTTP_ERR_BAD); /* bare LF */
  CHECK(parse_str("GET / HTTP/1.1\r\nHost: a\nX: b\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET / HTTP/1.1\r\nHost: a\rX: b\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET / HTTP/1.1\r\n Host: a\r\n\r\n") == RU_HTTP_ERR_BAD); /* obs-fold */
  CHECK(parse_str("GET / HTTP/1.1\r\nBad Name: a\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET / HTTP/1.1\r\nNoColon\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET / HTTP/1.1\r\n: x\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET / HTTP/1.1\r\nX: a\x01" "b\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET /\x01 HTTP/1.1\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET /\xc3\xa9 HTTP/1.1\r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("POST / HTTP/1.1\r\nContent-Length: +1\r\n\r\nx") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("POST / HTTP/1.1\r\nContent-Length: 0x1\r\n\r\nx") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("POST / HTTP/1.1\r\nContent-Length: \r\n\r\n") == RU_HTTP_ERR_BAD);
  CHECK(parse_str("GET / HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc") == RU_HTTP_ERR_BAD); /* smuggling shape */
  CHECK(parse_str("GET / HTTP/1.1\r\nContent-Length: 0\r\n\r\n") == RU_HTTP_DONE);
  CHECK(parse_str("GET / HTTP/1.1\r\nHost: a\r\n\r\nGET /2 HTTP/1.1\r\n\r\n") == RU_HTTP_ERR_BAD); /* pipelining */
  CHECK(parse_str("GET / HTTP/1.1\r\nContent-Length:   \t 0 \t\r\n\r\n") == RU_HTTP_DONE);
  CHECK(parse_str("") == RU_HTTP_NEED_MORE);
}

static void test_http_timeout(void) {
  static ru_http_conn_t c;
  /* slowloris: one byte every second, deadline 5 s measured from init */
  ru_http_init(&c, 100000, 5000);
  const char *s = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
  ru_http_status_t r = RU_HTTP_NEED_MORE;
  size_t i = 0;
  uint32_t t = 100000;
  while (r == RU_HTTP_NEED_MORE && i < strlen(s)) { t += 1000; r = ru_http_feed(&c, s + i, 1, t); i++; }
  CHECK(r == RU_HTTP_ERR_TIMEOUT && t == 106000 && i == 6);
  CHECK(ru_http_feed(&c, "x", 1, t + 1) == RU_HTTP_ERR_TIMEOUT); /* sticky */
  /* a complete request delivered after the deadline is still a timeout */
  ru_http_init(&c, 0, 5000);
  CHECK(ru_http_feed(&c, s, strlen(s), 5001) == RU_HTTP_ERR_TIMEOUT);
  /* poll without data */
  ru_http_init(&c, 0, 5000);
  CHECK(ru_http_feed(&c, "GET /", 5, 10) == RU_HTTP_NEED_MORE);
  CHECK(ru_http_poll(&c, 4999) == RU_HTTP_NEED_MORE);
  CHECK(ru_http_poll(&c, 5001) == RU_HTTP_ERR_TIMEOUT);
  /* exactly at the deadline is still fine, and clock wrap-around works */
  ru_http_init(&c, 0xFFFFFF00u, 5000);
  CHECK(ru_http_feed(&c, s, strlen(s), 0xFFFFFF00u + 4000u) == RU_HTTP_DONE);
  /* a partial body that never completes */
  ru_http_init(&c, 0, 3000);
  CHECK(ru_http_feed(&c, "POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc", 41, 100) == RU_HTTP_NEED_MORE);
  CHECK(ru_http_poll(&c, 3100) == RU_HTTP_ERR_TIMEOUT);
}

static void test_http_fuzz(void) {
  const char *seeds[] = {GET1, POST1, POST2};
  static ru_http_conn_t c;
  for (int it = 0; it < 30000; it++) {
    const char *sd = seeds[rnd32() % 3];
    char m[600];
    size_t n = strlen(sd);
    memcpy(m, sd, n);
    int muts = 1 + (int)(rnd32() % 4);
    for (int k = 0; k < muts; k++) {
      uint32_t op = rnd32() % 4;
      size_t pos = n ? rnd32() % n : 0;
      if (op == 0) m[pos] = (char)rnd32();
      else if (op == 1 && n > 1) { memmove(m + pos, m + pos + 1, n - pos - 1); n--; }
      else if (op == 2 && n + 1 < sizeof m) { memmove(m + pos + 1, m + pos, n - pos); m[pos] = (char)rnd32(); n++; }
      else if (op == 3) n = pos;
    }
    ru_http_status_t r = parse_all_at(m, n, &c);
    if (r == RU_HTTP_DONE) {
      CHECK(c.path[0] == '/' && strlen(c.path) < RU_HTTP_PATH_MAX);
      CHECK(c.body_len <= RU_HTTP_BODY_MAX && c.body_off + c.body_len == c.len && c.len == n);
      CHECK(c.method == RU_HTTP_GET || c.method == RU_HTTP_POST);
    }
    /* the same bytes in two chunks end in the same state */
    if (n > 2) {
      static ru_http_conn_t d;
      ru_http_init(&d, 1000, 5000);
      size_t h = 1 + rnd32() % (n - 1);
      (void)ru_http_feed(&d, m, h, 1000);
      ru_http_status_t r2 = ru_http_feed(&d, m + h, n - h, 1000);
      if (r == RU_HTTP_DONE) CHECK(r2 == RU_HTTP_DONE);
    }
  }
  /* pure random garbage */
  for (int it = 0; it < 3000; it++) {
    char g[300];
    size_t n = rnd32() % sizeof g;
    for (size_t i = 0; i < n; i++) g[i] = (char)rnd32();
    (void)parse_all_at(g, n, &c);
  }
}

/* ============================ forms ============================ */
static void test_form(void) {
  char o[64];
  const char *b = "a=1&b=hello+world&c=%41%62%7e&d=&e&a=2";
  CHECK(ru_form_check(b, strlen(b)));
  CHECK(ru_form_get(b, strlen(b), "a", o, sizeof o) == RU_FORM_OK && !strcmp(o, "1")); /* first wins */
  CHECK(ru_form_get(b, strlen(b), "b", o, sizeof o) == RU_FORM_OK && !strcmp(o, "hello world"));
  CHECK(ru_form_get(b, strlen(b), "c", o, sizeof o) == RU_FORM_OK && !strcmp(o, "Ab~"));
  CHECK(ru_form_get(b, strlen(b), "d", o, sizeof o) == RU_FORM_OK && !strcmp(o, ""));
  CHECK(ru_form_get(b, strlen(b), "e", o, sizeof o) == RU_FORM_OK && !strcmp(o, ""));
  CHECK(ru_form_get(b, strlen(b), "zz", o, sizeof o) == RU_FORM_NOT_FOUND && !o[0]);
  CHECK(ru_form_get(b, strlen(b), "A", o, sizeof o) == RU_FORM_NOT_FOUND); /* case sensitive */
  CHECK(ru_form_get("s=%3Cscript%3E", 14, "s", o, sizeof o) == RU_FORM_OK && !strcmp(o, "<script>"));
  CHECK(ru_form_get("%73sid=x", 8, "ssid", o, sizeof o) == RU_FORM_OK && !strcmp(o, "x")); /* encoded key */
  /* bad escapes */
  CHECK(!ru_form_check("a=%zz", 5) && !ru_form_check("a=%4", 4) && !ru_form_check("a=%", 3) && !ru_form_check("a=%00", 5));
  CHECK(ru_form_get("a=%4", 4, "a", o, sizeof o) == RU_FORM_BAD && !o[0]);
  CHECK(ru_form_get("a=%00x", 6, "a", o, sizeof o) == RU_FORM_BAD);
  CHECK(!ru_form_check("%zz=1", 5));
  /* bounded output */
  CHECK(ru_form_get("a=12345678", 10, "a", o, 8) == RU_FORM_TOO_LONG && !o[0]);
  CHECK(ru_form_get("a=1234567", 9, "a", o, 8) == RU_FORM_OK);
  CHECK(ru_form_get("a=%41%41%41%41%41%41%41%41", 26, "a", o, 8) == RU_FORM_TOO_LONG);
  /* field count and key length */
  char many[200] = "";
  for (int i = 0; i < 17; i++) strcat(many, "a=1&");
  CHECK(!ru_form_check(many, strlen(many)));
  many[0] = 0;
  for (int i = 0; i < 16; i++) strcat(many, "a=1&");
  CHECK(ru_form_check(many, strlen(many)));
  char key[80];
  memset(key, 'k', 40); strcpy(key + 40, "=1");
  CHECK(!ru_form_check(key, strlen(key)));
  CHECK(ru_form_check("", 0) && ru_form_get("", 0, "a", o, sizeof o) == RU_FORM_NOT_FOUND);
  /* every truncation of an encoded body: no crash, deterministic */
  for (size_t t = 0; t <= strlen(b); t++) { (void)ru_form_check(b, t); (void)ru_form_get(b, t, "c", o, sizeof o); }
  /* random garbage with exact-size buffers (ASAN) */
  for (int it = 0; it < 5000; it++) {
    size_t n = rnd32() % 64;
    char *g = malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) g[i] = "ab=&%+0F\x01"[rnd32() % 9];
    char *ob = malloc(8);
    (void)ru_form_check(g, n);
    (void)ru_form_get(g, n, "a", ob, 8);
    free(ob); free(g);
  }
}

/* ============================ DNS ============================ */
static size_t mkq(uint8_t *q, const char *name, unsigned qtype, unsigned flags, bool edns) {
  memset(q, 0, 64);
  q[0] = 0x12; q[1] = 0x34; q[2] = (uint8_t)(flags >> 8); q[3] = (uint8_t)flags; q[5] = 1; q[11] = edns ? 1 : 0;
  size_t i = 12;
  const char *p = name;
  while (*p) {
    const char *dot = strchr(p, '.');
    size_t l = dot ? (size_t)(dot - p) : strlen(p);
    q[i++] = (uint8_t)l;
    memcpy(q + i, p, l); i += l;
    p += l; if (*p == '.') p++;
  }
  q[i++] = 0;
  q[i++] = (uint8_t)(qtype >> 8); q[i++] = (uint8_t)qtype; q[i++] = 0; q[i++] = 1;
  if (edns) { const uint8_t opt[] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0}; memcpy(q + i, opt, sizeof opt); i += sizeof opt; }
  return i;
}
static void test_dns(void) {
  const uint8_t ip[4] = {192, 168, 4, 1};
  uint8_t q[64], out[512];
  size_t qn = mkq(q, "connectivitycheck.gstatic.com", 1, 0x0100, false);
  size_t n = ru_dns_answer(q, qn, ip, out, sizeof out);
  CHECK(n == qn + 16);
  CHECK(out[0] == 0x12 && out[1] == 0x34 && (out[2] & 0x80) && (out[2] & 0x01) && (out[3] & 0x0F) == 0);
  CHECK(out[6] == 0 && out[7] == 1 && !memcmp(out + n - 4, ip, 4));
  CHECK(out[n - 6] == 0 && out[n - 5] == 4 && out[qn] == 0xC0 && out[qn + 1] == 12);
  /* AAAA: NOERROR, no answer */
  qn = mkq(q, "example.com", 28, 0x0100, false);
  n = ru_dns_answer(q, qn, ip, out, sizeof out);
  CHECK(n == qn && out[7] == 0 && (out[3] & 0x0F) == 0 && (out[2] & 0x80));
  /* EDNS is accepted and dropped */
  qn = mkq(q, "a.b", 1, 0x0100, true);
  n = ru_dns_answer(q, qn, ip, out, sizeof out);
  CHECK(n > 0 && out[11] == 0 && !memcmp(out + n - 4, ip, 4));
  /* every strict prefix and every overlong variant is dropped, with an exact-size output buffer under ASAN */
  qn = mkq(q, "a.example.org", 1, 0x0100, false);
  for (size_t t = 0; t < qn; t++) {
    uint8_t *ex = malloc(qn + 16), *qq = malloc(t ? t : 1);
    memcpy(qq, q, t);
    CHECK(ru_dns_answer(qq, t, ip, ex, qn + 16) == 0);
    free(ex); free(qq);
  }
  for (size_t cap = 0; cap < qn + 16; cap++) { /* too small an output buffer: nothing written, nothing sent */
    uint8_t *ex = malloc(cap ? cap : 1);
    CHECK(ru_dns_answer(q, qn, ip, ex, cap) == 0);
    free(ex);
  }
  { uint8_t *ex = malloc(qn + 16); CHECK(ru_dns_answer(q, qn, ip, ex, qn + 16) == qn + 16); free(ex); }
  uint8_t bad[80];
  memcpy(bad, q, qn); bad[qn] = 0;                                  /* trailing garbage */
  CHECK(ru_dns_answer(bad, qn + 1, ip, out, sizeof out) == 0);
  memcpy(bad, q, qn); bad[2] |= 0x80;                               /* a response */
  CHECK(ru_dns_answer(bad, qn, ip, out, sizeof out) == 0);
  memcpy(bad, q, qn); bad[2] |= 0x08;                               /* opcode 1 */
  CHECK(ru_dns_answer(bad, qn, ip, out, sizeof out) == 0);
  memcpy(bad, q, qn); bad[5] = 2;                                   /* qdcount 2 */
  CHECK(ru_dns_answer(bad, qn, ip, out, sizeof out) == 0);
  memcpy(bad, q, qn); bad[5] = 0;
  CHECK(ru_dns_answer(bad, qn, ip, out, sizeof out) == 0);
  memcpy(bad, q, qn); bad[7] = 1;                                   /* ancount 1 */
  CHECK(ru_dns_answer(bad, qn, ip, out, sizeof out) == 0);
  memcpy(bad, q, qn); bad[12] = 0xC0;                               /* compression pointer in the question */
  CHECK(ru_dns_answer(bad, qn, ip, out, sizeof out) == 0);
  memcpy(bad, q, qn); bad[12] = 64;                                 /* label too long */
  CHECK(ru_dns_answer(bad, qn, ip, out, sizeof out) == 0);
  memcpy(bad, q, qn); bad[qn - 1] = 3;                              /* class CH */
  CHECK(ru_dns_answer(bad, qn, ip, out, sizeof out) == 0);
  memcpy(bad, q, qn); bad[11] = 2;                                  /* arcount 2 */
  CHECK(ru_dns_answer(bad, qn, ip, out, sizeof out) == 0);
  CHECK(ru_dns_answer(NULL, 20, ip, out, sizeof out) == 0 && ru_dns_answer(q, qn, NULL, out, sizeof out) == 0);
  CHECK(ru_dns_answer(q, 600, ip, out, sizeof out) == 0);
  { /* name over 255 bytes */
    uint8_t l[400]; memset(l, 0, 12); l[5] = 1;
    size_t i = 12;
    for (int k = 0; k < 6; k++) { l[i++] = 50; memset(l + i, 'a', 50); i += 50; }
    l[i++] = 0; l[i++] = 0; l[i++] = 1; l[i++] = 0; l[i++] = 1;
    CHECK(i > 255 && ru_dns_answer(l, i > 512 ? 0 : i, ip, out, sizeof out) == 0);
  }
  /* mutation fuzz, exact-size buffers */
  for (int it = 0; it < 30000; it++) {
    bool ed = rnd32() & 1;
    size_t m = mkq(q, (rnd32() & 1) ? "x.y.z" : "captive.apple.com", (rnd32() & 3) ? 1 : 28, 0x0100, ed);
    uint8_t mm[80];
    memcpy(mm, q, m);
    for (int k = 0, c = 1 + (int)(rnd32() % 3); k < c; k++) mm[rnd32() % m] = (uint8_t)rnd32();
    size_t ml = (rnd32() % 5 == 0) ? rnd32() % (m + 1) : m;
    uint8_t *ex = malloc(520);
    size_t r = ru_dns_answer(mm, ml, ip, ex, 520);
    CHECK(r == 0 || (r >= 12 && r <= 512 && (ex[2] & 0x80)));
    free(ex);
  }
}

/* ============================ captive paths, QR, password ============================ */
static bool fixed_rand(void *u, uint8_t *b, size_t n) { uint8_t *s = u; for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(*s + i * 7 + (*s) * i); (*s)++; return true; }
static bool bad_rand(void *u, uint8_t *b, size_t n) { (void)u; (void)b; (void)n; return false; }
static bool lcg_rand(void *u, uint8_t *b, size_t n) { (void)u; for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd32(); return true; }

static void test_captive(void) {
  CHECK(ru_captive_classify("/") == RU_PROBE_ROOT);
  CHECK(ru_captive_classify("/generate_204") == RU_PROBE_ANDROID && ru_captive_classify("/gen_204") == RU_PROBE_ANDROID);
  CHECK(ru_captive_classify("/hotspot-detect.html") == RU_PROBE_APPLE);
  CHECK(ru_captive_classify("/ncsi.txt") == RU_PROBE_WINDOWS && ru_captive_classify("/connecttest.txt") == RU_PROBE_WINDOWS &&
        ru_captive_classify("/redirect") == RU_PROBE_WINDOWS);
  CHECK(ru_captive_classify("/anything") == RU_PROBE_OTHER && ru_captive_classify("") == RU_PROBE_OTHER && ru_captive_classify(NULL) == RU_PROBE_OTHER);
  char u[40];
  CHECK(ru_portal_url("192.168.4.1", u, sizeof u) && !strcmp(u, "http://192.168.4.1/"));
  CHECK(!ru_portal_url("192.168.4.1", u, 19) && !u[0]);
  CHECK(ru_portal_url("192.168.4.1", u, 20));
  CHECK(!ru_portal_url("evil.com", u, sizeof u) && !ru_portal_url("1.2.3", u, sizeof u) && !ru_portal_url("1..2.3", u, sizeof u) &&
        !ru_portal_url("1.2.3.4/x", u, sizeof u) && !ru_portal_url(NULL, u, sizeof u));
}

static void test_qr(void) {
  char o[200];
  CHECK(ru_qr_wifi_payload("dzPanel-AB23", "pass word", o, sizeof o) && !strcmp(o, "WIFI:T:WPA;S:dzPanel-AB23;P:pass word;;"));
  CHECK(ru_qr_wifi_payload("a;b", "c,d", o, sizeof o) && !strcmp(o, "WIFI:T:WPA;S:a\\;b;P:c\\,d;;"));
  CHECK(ru_qr_wifi_payload("a:b", "x\\y", o, sizeof o) && !strcmp(o, "WIFI:T:WPA;S:a\\:b;P:x\\\\y;;"));
  CHECK(ru_qr_wifi_payload("q\"s", "p\"w", o, sizeof o) && !strcmp(o, "WIFI:T:WPA;S:q\\\"s;P:p\\\"w;;"));
  CHECK(ru_qr_wifi_payload("\\;,:\"", "\\;,:\"1", o, sizeof o) &&
        !strcmp(o, "WIFI:T:WPA;S:\\\\\\;\\,\\:\\\";P:\\\\\\;\\,\\:\\\"1;;"));
  CHECK(ru_qr_wifi_payload("abc123", "deadbeef", o, sizeof o) && !strcmp(o, "WIFI:T:WPA;S:\"abc123\";P:\"deadbeef\";;")); /* hex-looking values quoted */
  CHECK(ru_qr_wifi_payload("net", "", o, sizeof o) && !strcmp(o, "WIFI:T:nopass;S:net;;"));
  CHECK(ru_qr_wifi_payload("caf\xc3\xa9", "pw", o, sizeof o) && strstr(o, "caf\xc3\xa9"));
  /* rejects */
  CHECK(!ru_qr_wifi_payload("", "pw", o, sizeof o) && !o[0]);
  CHECK(!ru_qr_wifi_payload("a\nb", "pw", o, sizeof o));
  CHECK(!ru_qr_wifi_payload("net", "p\rw", o, sizeof o));
  CHECK(!ru_qr_wifi_payload("123456789012345678901234567890123", "pw", o, sizeof o)); /* 33 bytes */
  CHECK(ru_qr_wifi_payload("12345678901234567890123456789012x", "pw", o, sizeof o) == false);
  char longpw[80]; memset(longpw, 'z', 64); longpw[64] = 0;
  CHECK(!ru_qr_wifi_payload("net", longpw, o, sizeof o));
  /* exact-size output: never overflows */
  CHECK(ru_qr_wifi_payload("a;b", "c,d", o, sizeof o));
  size_t need = strlen(o) + 1;
  for (size_t cap = 0; cap < need; cap++) { char *e = malloc(cap ? cap : 1); CHECK(!ru_qr_wifi_payload("a;b", "c,d", e, cap)); free(e); }
  { char *e = malloc(need); CHECK(ru_qr_wifi_payload("a;b", "c,d", e, need)); free(e); }
}

static void test_password(void) {
  char a[40], b[40];
  uint8_t s1 = 1, s2 = 2;
  CHECK(ru_gen_password(fixed_rand, &s1, 12, a, sizeof a) && strlen(a) == 12);
  CHECK(ru_gen_password(fixed_rand, &s2, 12, b, sizeof b) && strcmp(a, b) != 0);
  for (const char *p = a; *p; p++) CHECK(strchr(RU_PW_ALPHABET, *p));
  CHECK(strlen(RU_PW_ALPHABET) == 32 && !strpbrk(RU_PW_ALPHABET, "0O1Il"));
  CHECK(!ru_gen_password(fixed_rand, &s1, 11, a, sizeof a) && !a[0]);
  CHECK(!ru_gen_password(fixed_rand, &s1, 33, a, sizeof a));
  CHECK(!ru_gen_password(fixed_rand, &s1, 12, a, 12));
  CHECK(!ru_gen_password(bad_rand, NULL, 12, a, sizeof a) && !a[0]);
  CHECK(!ru_gen_password(NULL, NULL, 12, a, sizeof a));
  /* uniform over the alphabet: every symbol shows up with a plausible count */
  int cnt[32] = {0};
  for (int i = 0; i < 500; i++) { char t[40]; CHECK(ru_gen_password(lcg_rand, NULL, 32, t, sizeof t)); for (int k = 0; k < 32; k++) cnt[strchr(RU_PW_ALPHABET, t[k]) - RU_PW_ALPHABET]++; }
  for (int k = 0; k < 32; k++) CHECK(cnt[k] > 350 && cnt[k] < 650);
  char ssid[16];
  CHECK(ru_gen_ap_ssid(lcg_rand, NULL, ssid, sizeof ssid) && strlen(ssid) == 12 && !strncmp(ssid, "dzPanel-", 8));
  CHECK(!ru_gen_ap_ssid(lcg_rand, NULL, ssid, 12) && !ru_gen_ap_ssid(bad_rand, NULL, ssid, sizeof ssid));
  char h[40];
  CHECK(ru_gen_hex(lcg_rand, NULL, 16, h, sizeof h) && strlen(h) == 32 && strspn(h, "0123456789abcdef") == 32);
  CHECK(!ru_gen_hex(lcg_rand, NULL, 16, h, 32) && !ru_gen_hex(bad_rand, NULL, 16, h, sizeof h));
  CHECK(ru_ct_streq("abc", "abc") && !ru_ct_streq("abc", "abd") && !ru_ct_streq("abc", "ab") && !ru_ct_streq("", "a") && ru_ct_streq("", ""));
}

/* ============================ portal flow ============================ */
static const char *TOKEN = "eyJhbGciOiJIUzI1NiJ9.TOKENMARKER0123456789.c2lnbmF0dXJl";
static const char *WIFIPW = "wifi-secret-pw-777";

static ru_kv_mem_t KV;
static ru_portal_t P;
static char LOG[4096];
static int join_calls, test_calls;
static bool join_ok;
static ru_cfg_test_t test_result;
static char seen_token[RU_CFG_TOKEN_MAX], seen_url[RU_CFG_URL_MAX], seen_join_ssid[40], seen_join_pass[80];
static ru_portal_net_t NETS[3];
static size_t NNETS;
static char ALLPAGES[200000];
static size_t allpages_n;

static size_t cb_scan(void *u, ru_portal_net_t *o, size_t max) { (void)u; size_t n = NNETS < max ? NNETS : max; memcpy(o, NETS, n * sizeof *o); return n; }
static bool cb_join(void *u, const char *s, const char *p, uint32_t t) {
  (void)u; (void)t; join_calls++;
  snprintf(seen_join_ssid, sizeof seen_join_ssid, "%s", s);
  snprintf(seen_join_pass, sizeof seen_join_pass, "%s", p);
  return join_ok;
}
static ru_cfg_test_t cb_test(void *u, const ru_cfg_t *c, uint32_t t, char *info, size_t cap) {
  (void)u; (void)info; (void)cap; test_calls++;
  CHECK(t == 15000);
  snprintf(seen_token, sizeof seen_token, "%s", c->token);
  snprintf(seen_url, sizeof seen_url, "%s", c->url);
  return test_result;
}
static void cb_log(void *u, const char *m) {
  (void)u;
  size_t n = strlen(LOG), l = strlen(m);
  if (n + l + 2 > sizeof LOG) { n = 0; LOG[0] = 0; } /* keep only recent lines */
  memcpy(LOG + n, m, l);
  LOG[n + l] = '\n';
  LOG[n + l + 1] = 0;
}
static uint8_t seedctr = 9;

static void setup(const ru_portal_cfg_t *cfg) {
  ru_kv_mem_init(&KV);
  LOG[0] = 0; join_calls = test_calls = 0; join_ok = true; test_result = RU_CFG_TEST_OK; allpages_n = 0; ALLPAGES[0] = 0;
  seen_token[0] = seen_url[0] = 0;
  strcpy(NETS[0].ssid, "Home"); NETS[0].rssi = -40; NETS[0].secured = true;
  strcpy(NETS[1].ssid, "<script>alert(1)</script>"); NETS[1].rssi = -50; NETS[1].secured = true;
  strcpy(NETS[2].ssid, "\"><img src=x onerror=1>&'"); NETS[2].rssi = -60; NETS[2].secured = false;
  NNETS = 3;
  ru_portal_ops_t ops = {0};
  ops.random = fixed_rand; ops.user = &seedctr; ops.scan = NULL; ops.wifi_join = cb_join; ops.log = cb_log;
  ops.cfg.test = cb_test;
  /* scan and join use user = &seedctr too: scan ignores it */
  ops.scan = cb_scan;
  ru_portal_init(&P, cfg, &ops, &KV.base);
}

static char BODY[RU_PORTAL_PAGE_MAX];
static ru_portal_resp_t resp(void) { ru_portal_resp_t r; memset(&r, 0, sizeof r); r.body = BODY; r.body_cap = sizeof BODY; return r; }
static ru_portal_resp_t RS;
static bool req(ru_http_method_t m, const char *path, const char *body, uint32_t client, uint32_t now) {
  ru_portal_req_t q = {m, path, body, body ? strlen(body) : 0, body != NULL, client};
  RS = resp();
  bool r = ru_portal_handle(&P, &q, now, &RS);
  if (RS.html && allpages_n + RS.body_len + 1 < sizeof ALLPAGES) { memcpy(ALLPAGES + allpages_n, BODY, RS.body_len); allpages_n += RS.body_len; ALLPAGES[allpages_n] = 0; }
  if (RS.html) { CHECK(strlen(BODY) == RS.body_len); }
  return r;
}
/* csrf as a browser would read it from the form */
static void page_csrf(char *out, size_t cap) {
  const char *k = "name=\"csrf\" value=\"";
  const char *p = strstr(BODY, k);
  out[0] = 0;
  if (!p) return;
  p += strlen(k);
  size_t n = strcspn(p, "\"");
  snprintf(out, cap, "%.*s", (int)n, p);
}
static void post_wifi(const char *csrf, const char *ssid, const char *manual, const char *pw, uint32_t client, uint32_t now) {
  char b[400];
  snprintf(b, sizeof b, "csrf=%s&ssid=%s&ssid_manual=%s&pass=%s", csrf, ssid, manual, pw);
  req(RU_HTTP_POST, "/wifi", b, client, now);
}
static void post_dz(const char *csrf, const char *url, const char *token, bool ack, uint32_t client, uint32_t now) {
  char b[1200];
  snprintf(b, sizeof b, "csrf=%s&url=%s&token=%s%s", csrf, url, token, ack ? "&ack=1" : "");
  req(RU_HTTP_POST, "/domoticz", b, client, now);
}
static void started(void) { CHECK(ru_portal_start(&P, RU_PORTAL_SRC_USER, 1000)); }

static void test_flow_happy(void) {
  setup(NULL);
  started();
  CHECK(req(RU_HTTP_GET, "/", NULL, 1, 2000) && RS.status == 200 && RS.html);
  CHECK(strstr(BODY, "Step 1 of 2") && strstr(BODY, "type=\"password\" name=\"pass\""));
  char csrf[64];
  page_csrf(csrf, sizeof csrf);
  CHECK(strlen(csrf) == 32 && !strcmp(csrf, P.csrf));
  post_wifi(csrf, "Home", "", WIFIPW, 1, 3000);
  CHECK(RS.status == 303 && !strcmp(RS.location, "http://192.168.4.1/") && join_calls == 1);
  CHECK(!strcmp(seen_join_ssid, "Home") && !strcmp(seen_join_pass, WIFIPW));
  CHECK(req(RU_HTTP_GET, "/", NULL, 1, 4000) && strstr(BODY, "Step 2 of 2") && strstr(BODY, "Connected to Wi-Fi <b>Home</b>"));
  CHECK(strstr(BODY, "name=\"token\"") && strstr(BODY, "type=\"password\" name=\"token\"") && strstr(BODY, "http://</b> is not encrypted"));
  CHECK(strstr(BODY, "type=\"checkbox\" name=\"ack\""));
  page_csrf(csrf, sizeof csrf);
  /* http without the box: refused, no test call, URL kept but token never echoed */
  post_dz(csrf, "192.168.1.5%3A8080", TOKEN, false, 1, 5000);
  CHECK(RS.status == 200 && strstr(BODY, "Plain http sends your token") && test_calls == 0 && !ru_cfg_is_set(&KV.base));
  CHECK(strstr(BODY, "value=\"192.168.1.5:8080\""));
  /* with the box */
  post_dz(csrf, "192.168.1.5%3A8080", TOKEN, true, 1, 6000);
  CHECK(RS.status == 303 && test_calls == 1 && ru_cfg_is_set(&KV.base));
  CHECK(!strcmp(seen_token, TOKEN) && !strcmp(seen_url, "http://192.168.1.5:8080"));
  ru_cfg_t c;
  CHECK(ru_cfg_load(&KV.base, &c) == RU_CFG_OK && !strcmp(c.token, TOKEN) && c.insecure_lan);
  CHECK(req(RU_HTTP_GET, "/", NULL, 1, 7000) && strstr(BODY, "<h2>Done</h2>"));
  CHECK(ru_portal_running(&P));
  CHECK(ru_portal_tick(&P, 6000 + 14999));
  CHECK(!ru_portal_tick(&P, 6000 + 15000) && P.last_stop == RU_PORTAL_STOP_SUCCESS);
  CHECK(!req(RU_HTTP_GET, "/", NULL, 1, 30000) && RS.status == 503);
  /* secrets gone from RAM state after stop */
  CHECK(P.ap_pass[0] == 0 && P.csrf[0] == 0);
  /* the token is in no page, no log */
  CHECK(!strstr(ALLPAGES, TOKEN) && !strstr(ALLPAGES, "TOKENMARKER") && !strstr(LOG, "TOKENMARKER") && !strstr(LOG, WIFIPW));
  CHECK(!strstr(ALLPAGES, WIFIPW));
  CHECK(strstr(LOG, "started") && strstr(LOG, "saved"));
}

static void test_https_and_errors(void) {
  char csrf[64];
  setup(NULL);
  started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000);
  page_csrf(csrf, sizeof csrf);
  /* step 2 before joining is refused (redirect back) */
  post_dz(csrf, "https%3A%2F%2Fdz.example", TOKEN, false, 1, 2100);
  CHECK(RS.status == 303 && test_calls == 0);
  /* join failure keeps step 1 */
  join_ok = false;
  post_wifi(csrf, "Home", "", WIFIPW, 1, 2200);
  CHECK(RS.status == 200 && strstr(BODY, "could not join") && strstr(BODY, "Step 1 of 2") && !strstr(BODY, WIFIPW));
  join_ok = true;
  /* bad input */
  post_wifi(csrf, "Home", "", "short", 1, 2300);
  CHECK(strstr(BODY, "not valid") && join_calls == 1);
  post_wifi(csrf, "", "", WIFIPW, 1, 2400);
  CHECK(strstr(BODY, "not valid"));
  /* manual SSID wins over the list; hidden-network entry */
  post_wifi(csrf, "Home", "Hidden+Net", WIFIPW, 1, 2500);
  CHECK(RS.status == 303 && !strcmp(seen_join_ssid, "Hidden Net"));
  req(RU_HTTP_GET, "/", NULL, 1, 2600);
  page_csrf(csrf, sizeof csrf);
  struct { ru_cfg_test_t t; const char *needle; } cases[] = {
      {RU_CFG_TEST_UNREACHABLE, "could not reach"}, {RU_CFG_TEST_TLS, "secure connection failed"},
      {RU_CFG_TEST_TOKEN_REJECTED, "rejected the token"}, {RU_CFG_TEST_NO_WIFI, "not connected to Wi-Fi"},
      {RU_CFG_TEST_BAD_RESPONSE, "not like a Domoticz"}};
  setup(NULL);
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
    ru_portal_cfg_t cfg = {0};
    cfg.max_failures = 50;
    setup(&cfg);
    started();
    req(RU_HTTP_GET, "/", NULL, 1, 2000); page_csrf(csrf, sizeof csrf);
    post_wifi(csrf, "Home", "", WIFIPW, 1, 2100);
    test_result = cases[i].t;
    post_dz(csrf, "https%3A%2F%2Fdz.example", TOKEN, false, 1, 2200);
    CHECK(RS.status == 200 && strstr(BODY, cases[i].needle) && !ru_cfg_is_set(&KV.base));
    CHECK(!strstr(BODY, "TOKENMARKER") && !strstr(ALLPAGES, "TOKENMARKER"));
    CHECK(P.step == RU_PORTAL_STEP_DOMOTICZ && P.failures == 1);
  }
  /* https saves with insecure_lan false and needs no tick */
  setup(NULL);
  started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000); page_csrf(csrf, sizeof csrf);
  post_wifi(csrf, "Home", "", WIFIPW, 1, 2100);
  post_dz(csrf, "HTTPS%3A%2F%2FDz.Example%2F", TOKEN, false, 1, 2200);
  ru_cfg_t c;
  CHECK(RS.status == 303 && ru_cfg_load(&KV.base, &c) == RU_CFG_OK && !c.insecure_lan && !strcmp(c.url, "https://dz.example"));
  /* invalid url / token variants never reach the connection test */
  { ru_portal_cfg_t lax = {0}; lax.max_failures = 50; setup(&lax); }
  started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000); page_csrf(csrf, sizeof csrf);
  post_wifi(csrf, "Home", "", WIFIPW, 1, 2100);
  const char *badurls[] = {"http%3A%2F%2Fuser%3Apw%40h", "http%3A%2F%2Fh%2F%3Fq%3D1", "ftp%3A%2F%2Fh", "", "http%3A%2F%2F"};
  for (size_t i = 0; i < sizeof badurls / sizeof *badurls; i++) { post_dz(csrf, badurls[i], TOKEN, true, 1, 2200); CHECK(strstr(BODY, "address is not valid")); }
  post_dz(csrf, "https%3A%2F%2Fh", "has+space", true, 1, 2300);
  CHECK(strstr(BODY, "token is not valid"));
  post_dz(csrf, "https%3A%2F%2Fh", "", true, 1, 2300);
  CHECK(strstr(BODY, "token is not valid") && test_calls == 0);
  /* a token with JSON-significant characters is passed through intact */
  post_dz(csrf, "https%3A%2F%2Fh", "a%22b%5Cc", true, 1, 2400);
  CHECK(test_calls == 1 && !strcmp(seen_token, "a\"b\\c"));
  /* oversized token */
  char big[1000]; memset(big, 'A', 900); big[900] = 0;
  setup(NULL); started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000); page_csrf(csrf, sizeof csrf);
  post_wifi(csrf, "Home", "", WIFIPW, 1, 2100);
  post_dz(csrf, "https%3A%2F%2Fh", big, true, 1, 2200);
  CHECK(strstr(BODY, "too long") && test_calls == 0);
}

static void test_escaping(void) {
  setup(NULL);
  started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000);
  CHECK(!strstr(BODY, "<script>") && !strstr(BODY, "<img") && !strstr(BODY, "onerror=1>"));
  CHECK(strstr(BODY, "&lt;script&gt;alert(1)&lt;/script&gt;"));
  CHECK(strstr(BODY, "&quot;&gt;&lt;img src=x onerror=1&gt;&amp;&#39;"));
  CHECK(strstr(BODY, "value=\"&lt;script&gt;") != NULL);
  CHECK(strstr(BODY, "<option value=\"Home\">Home</option>"));
  /* control characters and an over-long unterminated SSID */
  strcpy(NETS[0].ssid, "a\nb\r\"c\x01");
  memset(NETS[1].ssid, 'Z', 32); NETS[1].ssid[32] = 'Y'; /* no NUL inside the struct's 33 bytes: callback must not overrun */
  req(RU_HTTP_GET, "/", NULL, 1, 2100);
  CHECK(strstr(BODY, "a?b?&quot;c?"));
  CHECK(!strstr(BODY, "a\nb"));
  /* the joined SSID echoed on step 2 is escaped too */
  char csrf[64]; page_csrf(csrf, sizeof csrf);
  post_wifi(csrf, "x", "%3Cscript%3Ealert(1)%3C%2Fscript%3E", WIFIPW, 1, 2200);
  CHECK(RS.status == 303);
  req(RU_HTTP_GET, "/", NULL, 1, 2300);
  CHECK(strstr(BODY, "Connected to Wi-Fi <b>&lt;script&gt;alert(1)&lt;/script&gt;</b>") && !strstr(BODY, "<script"));
  /* hostile URL prefill */
  page_csrf(csrf, sizeof csrf);
  post_dz(csrf, "%22%3E%3Cscript%3Ex%3C%2Fscript%3E", TOKEN, true, 1, 2400);
  CHECK(!strstr(ALLPAGES, "<script") && !strstr(ALLPAGES, "\"><script"));
  /* no page contains script tags, external resources or inline handlers, ever */
  CHECK(!strstr(ALLPAGES, "<script") && !strstr(ALLPAGES, "src=\"http") && !strstr(ALLPAGES, "href=\"http") && !strstr(ALLPAGES, "<link") &&
        !strstr(ALLPAGES, "<iframe") && !strstr(ALLPAGES, " onclick") && !strstr(ALLPAGES, "javascript:"));
  CHECK(!strstr(ALLPAGES, "@import") && !strstr(ALLPAGES, "url("));
}

static void test_csrf(void) {
  char csrf[64];
  ru_portal_cfg_t lax = {0};
  lax.max_failures = 50;
  setup(&lax);
  started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000); page_csrf(csrf, sizeof csrf);
  post_wifi("", "Home", "", WIFIPW, 1, 2100);
  CHECK(RS.status == 403 && join_calls == 0);
  post_wifi("0123456789abcdef0123456789abcdef", "Home", "", WIFIPW, 1, 2100);
  CHECK(RS.status == 403 && join_calls == 0);
  char wrong[64]; snprintf(wrong, sizeof wrong, "%s", csrf); wrong[31] = wrong[31] == 'a' ? 'b' : 'a';
  post_wifi(wrong, "Home", "", WIFIPW, 1, 2100);
  CHECK(RS.status == 403 && join_calls == 0);
  post_wifi(csrf, "Home", "", WIFIPW, 1, 2100); /* the genuine one still works: 3 failures so far, limit 5 */
  CHECK(RS.status == 303 && join_calls == 1);
  /* missing field, wrong content type, no body, malformed escapes */
  ru_portal_req_t q = {RU_HTTP_POST, "/domoticz", "url=x&token=y", 13, true, 1};
  RS = resp(); ru_portal_handle(&P, &q, 2200, &RS); CHECK(RS.status == 403 && test_calls == 0);
  q.form_ct = false;
  RS = resp(); ru_portal_handle(&P, &q, 2200, &RS); CHECK(RS.status != 303 && test_calls == 0 && !ru_cfg_is_set(&KV.base));
  q.body = "csrf=%zz"; q.body_len = 8; q.form_ct = true;
  RS = resp(); ru_portal_handle(&P, &q, 2200, &RS); CHECK(test_calls == 0);
  /* csrf in the query string or a different field name does not count */
  post_dz("", "https%3A%2F%2Fh", TOKEN, true, 1, 2300);
  CHECK(RS.status == 403 && test_calls == 0);
  char b[300];
  snprintf(b, sizeof b, "xcsrf=%s&url=https%%3A%%2F%%2Fh&token=%s", csrf, TOKEN);
  req(RU_HTTP_POST, "/domoticz", b, 1, 2300);
  CHECK(test_calls == 0);
  /* an old session token is not valid after a restart */
  char old[64]; snprintf(old, sizeof old, "%s", csrf);
  ru_portal_stop(&P, RU_PORTAL_STOP_USER);
  CHECK(ru_portal_start(&P, RU_PORTAL_SRC_USER, 9000));
  CHECK(strcmp(old, P.csrf) != 0);
  post_wifi(old, "Home", "", WIFIPW, 1, 9100);
  CHECK(RS.status == 403);
  CHECK(!strstr(ALLPAGES, TOKEN));
}

static void test_rate_limit(void) {
  char csrf[64];
  setup(NULL);
  started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000); page_csrf(csrf, sizeof csrf);
  join_ok = false;
  for (int i = 0; i < 4; i++) { post_wifi(csrf, "Home", "", WIFIPW, 1, 2100 + (uint32_t)i); CHECK(RS.status == 200 && !P.locked); }
  post_wifi(csrf, "Home", "", WIFIPW, 1, 2200);
  CHECK(RS.status == 429 && P.locked && join_calls == 5);
  /* locked: even the correct credentials do nothing, pages answer 429 */
  join_ok = true;
  post_wifi(csrf, "Home", "", WIFIPW, 1, 2300);
  CHECK(RS.status == 429 && join_calls == 5);
  req(RU_HTTP_GET, "/", NULL, 1, 2400);
  CHECK(RS.status == 429);
  /* probes still redirect (they do nothing) */
  req(RU_HTTP_GET, "/generate_204", NULL, 1, 2500);
  CHECK(RS.status == 302);
  CHECK(strstr(LOG, "locked"));
  /* restart unlocks */
  ru_portal_stop(&P, RU_PORTAL_STOP_USER);
  CHECK(ru_portal_start(&P, RU_PORTAL_SRC_USER, 5000));
  CHECK(!P.locked && P.failures == 0);
  /* custom limit, step 2 failures count too */
  ru_portal_cfg_t cfg = {0}; cfg.max_failures = 2;
  setup(&cfg); started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000); page_csrf(csrf, sizeof csrf);
  post_wifi(csrf, "Home", "", WIFIPW, 1, 2100);
  test_result = RU_CFG_TEST_UNREACHABLE;
  post_dz(csrf, "https%3A%2F%2Fh", TOKEN, true, 1, 2200); CHECK(RS.status == 200);
  post_dz(csrf, "https%3A%2F%2Fh", TOKEN, true, 1, 2300); CHECK(RS.status == 429 && test_calls == 2);
  post_dz(csrf, "https%3A%2F%2Fh", TOKEN, true, 1, 2400); CHECK(RS.status == 429 && test_calls == 2);
}

static void test_client_and_timeouts(void) {
  char csrf[64];
  ru_portal_cfg_t cfg = {0};
  cfg.idle_ms = 5 * 60000; cfg.client_idle_ms = 30000;
  setup(&cfg);
  started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000); page_csrf(csrf, sizeof csrf);
  /* a second phone gets a busy page and cannot post */
  req(RU_HTTP_GET, "/", NULL, 2, 3000);
  CHECK(RS.status == 503 && strstr(BODY, "Another device") && !strstr(BODY, "csrf"));
  post_wifi(csrf, "Home", "", WIFIPW, 2, 3100);
  CHECK(RS.status == 503 && join_calls == 0);
  /* probes from anyone are redirected and do not steal the slot */
  req(RU_HTTP_GET, "/hotspot-detect.html", NULL, 2, 3200);
  CHECK(RS.status == 302 && P.owner == 1);
  /* the owner continues */
  post_wifi(csrf, "Home", "", WIFIPW, 1, 4000);
  CHECK(RS.status == 303);
  /* after the client was silent for client_idle_ms the slot is free */
  req(RU_HTTP_GET, "/", NULL, 2, 4000 + 29999);
  CHECK(RS.status == 503);
  req(RU_HTTP_GET, "/", NULL, 2, 4000 + 30000);
  CHECK(RS.status == 200);
  /* idle timeout: probes do not keep it alive */
  setup(&cfg);
  started();
  req(RU_HTTP_GET, "/", NULL, 1, 1000);
  ru_portal_status_t st;
  ru_portal_status(&P, 1000, &st);
  CHECK(st.running && st.remaining_min == 5 && st.remaining_ms == 300000);
  ru_portal_status(&P, 1000 + 60001, &st);
  CHECK(st.remaining_min == 4);
  for (uint32_t t = 1000; t < 1000 + 300000; t += 5000) { req(RU_HTTP_GET, "/generate_204", NULL, 1, t); }
  CHECK(!ru_portal_tick(&P, 1000 + 300000) && P.last_stop == RU_PORTAL_STOP_IDLE);
  ru_portal_status(&P, 400000, &st);
  CHECK(!st.running && st.remaining_min == 0 && !st.password[0] && !st.ssid[0] && st.last_stop == RU_PORTAL_STOP_IDLE);
  CHECK(!req(RU_HTTP_GET, "/", NULL, 1, 400000) && RS.status == 503);
  /* page requests refresh the idle timer; handle() itself applies the timeout */
  setup(&cfg); started();
  req(RU_HTTP_GET, "/", NULL, 1, 1000 + 250000);
  CHECK(ru_portal_running(&P));
  CHECK(req(RU_HTTP_GET, "/", NULL, 1, 1000 + 250000 + 299999));
  CHECK(!req(RU_HTTP_GET, "/", NULL, 1, 1000 + 250000 + 299999 + 300000) && !ru_portal_running(&P));
  /* tick wrap-around */
  setup(&cfg);
  CHECK(ru_portal_start(&P, RU_PORTAL_SRC_USER, 0xFFFFFF00u));
  CHECK(ru_portal_tick(&P, 0xFFFFFF00u + 1000) && !ru_portal_tick(&P, 0xFFFFFF00u + 300000));
}

static void test_never_unless_requested(void) {
  setup(NULL);
  CHECK(!ru_portal_running(&P));
  CHECK(!req(RU_HTTP_GET, "/", NULL, 1, 1000) && RS.status == 503 && !RS.html);
  CHECK(!req(RU_HTTP_POST, "/wifi", "csrf=&ssid=a&pass=b", 1, 1000) && join_calls == 0);
  CHECK(!ru_portal_tick(&P, 1000));
  CHECK(!ru_portal_start(&P, RU_PORTAL_SRC_NONE, 1000) && !ru_portal_running(&P));
  /* the Wi-Fi manager path starts it only from AP_REQUESTED */
  ru_wifi_sm_t sm;
  ru_wifi_sm_ops_t sops = {0};
  ru_wifi_sm_init(&sm, NULL, &sops);
  for (int s = RU_WIFI_IDLE; s <= RU_WIFI_AP_REQUESTED; s++) {
    sm.state = (ru_wifi_state_t)s;
    bool want = s == RU_WIFI_AP_REQUESTED;
    setup(NULL);
    CHECK(ru_portal_start_from_wifi(&P, &sm, 1000) == want && ru_portal_running(&P) == want);
  }
  setup(NULL);
  CHECK(!ru_portal_start_from_wifi(&P, NULL, 1000) && !ru_portal_running(&P));
  /* no random source, failing random source, bad config: no start */
  ru_portal_ops_t ops = {0};
  ru_portal_init(&P, NULL, &ops, &KV.base);
  CHECK(!ru_portal_start(&P, RU_PORTAL_SRC_USER, 0));
  ops.random = bad_rand;
  ru_portal_init(&P, NULL, &ops, &KV.base);
  CHECK(!ru_portal_start(&P, RU_PORTAL_SRC_USER, 0) && !ru_portal_running(&P));
  ru_portal_cfg_t cfg = {0}; cfg.pw_len = 8; ops.random = lcg_rand;
  ru_portal_init(&P, &cfg, &ops, &KV.base);
  CHECK(!ru_portal_start(&P, RU_PORTAL_SRC_USER, 0));
  cfg.pw_len = 16; cfg.ap_ip = "not-an-ip";
  ru_portal_init(&P, &cfg, &ops, &KV.base);
  CHECK(!ru_portal_start(&P, RU_PORTAL_SRC_USER, 0));
  /* double start is refused */
  setup(NULL); started();
  CHECK(!ru_portal_start(&P, RU_PORTAL_SRC_USER, 2000));
}

static void test_probes_and_status(void) {
  setup(NULL);
  started();
  const char *paths[] = {"/generate_204", "/gen_204", "/hotspot-detect.html", "/ncsi.txt", "/connecttest.txt", "/redirect", "/favicon.ico", "/anything/else", "/library/test/success.html", "/domoticz-evil"};
  for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) {
    CHECK(req(RU_HTTP_GET, paths[i], NULL, 7, 2000) && RS.status == 302 && !strcmp(RS.location, "http://192.168.4.1/") && !RS.html);
    CHECK(req(RU_HTTP_POST, paths[i], "a=b", 7, 2000) && RS.status == 302);
  }
  CHECK(P.failures == 0 && P.owner == 0); /* probes cost nothing and claim nothing */
  req(RU_HTTP_GET, "/domoticz", NULL, 1, 2000); CHECK(RS.status == 302);
  req(RU_HTTP_POST, "/", "a=b", 1, 2000); CHECK(RS.status == 405);
  /* status for the on-screen QR */
  ru_portal_status_t st;
  ru_portal_status(&P, 2000, &st);
  CHECK(st.running && st.step == RU_PORTAL_STEP_WIFI && !strncmp(st.ssid, "dzPanel-", 8) && strlen(st.password) == 12);
  CHECK(!strcmp(st.url, "http://192.168.4.1/") && st.remaining_min == 10);
  char q[200], u[80];
  CHECK(ru_portal_qr_wifi(&P, q, sizeof q) && !strncmp(q, "WIFI:T:WPA;S:dzPanel-", 21) && strstr(q, st.password));
  CHECK(ru_portal_qr_wifi(&P, q, sizeof q) && strstr(q, ";P:") && q[strlen(q) - 1] == ';');
  CHECK(ru_portal_qr_url(&P, u, sizeof u) && !strcmp(u, "http://192.168.4.1/") && !ru_portal_qr_url(&P, u, 5));
  CHECK(!strstr(LOG, st.password) && !strstr(LOG, st.ssid));
  CHECK(!strstr(ALLPAGES, st.password)); /* the AP password is never in a page either */
  /* password differs per session */
  char first[40]; snprintf(first, sizeof first, "%s", st.password);
  ru_portal_stop(&P, RU_PORTAL_STOP_USER);
  CHECK(!ru_portal_qr_wifi(&P, q, sizeof q) && !q[0]);
  CHECK(ru_portal_start(&P, RU_PORTAL_SRC_USER, 3000));
  ru_portal_status(&P, 3000, &st);
  CHECK(strcmp(first, st.password) != 0);
  /* response buffer too small: 500, never a truncated page */
  ru_portal_req_t rq = {RU_HTTP_GET, "/", NULL, 0, false, 1};
  char tiny[300];
  ru_portal_resp_t r = {0}; r.body = tiny; r.body_cap = sizeof tiny;
  CHECK(ru_portal_handle(&P, &rq, 3100, &r) && r.status == 500 && strlen(tiny) == r.body_len);
  char one[1]; r.body = one; r.body_cap = 1;
  CHECK(ru_portal_handle(&P, &rq, 3100, &r) && r.status == 500 && !r.html);
  char mid[RU_PORTAL_PAGE_MAX]; r.body = mid; r.body_cap = sizeof mid; NNETS = 3;
  CHECK(ru_portal_handle(&P, &rq, 3100, &r) && r.status == 200);
  CHECK(r.body_len < RU_PORTAL_PAGE_MAX - 512); /* headroom with the maximum scan list */
  NNETS = 3;
}

static void test_full_scan_page_fits(void) {
  setup(NULL);
  started();
  /* worst case: 16 networks, each SSID 32 chars of '"' (6 bytes each when escaped) */
  NNETS = 3;
  for (int i = 0; i < 3; i++) { memset(NETS[i].ssid, '"', 32); NETS[i].ssid[32] = 0; }
  req(RU_HTTP_GET, "/", NULL, 1, 2000);
  CHECK(RS.status == 200 || RS.status == 500);
  if (RS.status == 200) CHECK(strstr(BODY, "&quot;&quot;"));
}


static void test_portal_fuzz(void) {
  ru_portal_cfg_t lax = {0};
  lax.max_failures = 100000;
  setup(&lax);
  started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000);
  char csrf[64]; page_csrf(csrf, sizeof csrf);
  const char *alpha = "csrf=&ssidurlokentpas%+0123456789abcdefABCDEF<>\"'\\;,:";
  test_result = RU_CFG_TEST_UNREACHABLE;
  for (int it = 0; it < 20000; it++) {
    char b[300];
    size_t n = 0;
    if (rnd32() & 1) n += (size_t)snprintf(b, sizeof b, "csrf=%s&", csrf);
    size_t len = rnd32() % 200;
    for (size_t i = 0; i < len && n + 1 < sizeof b; i++) b[n++] = alpha[rnd32() % strlen(alpha)];
    b[n] = 0;
    const char *paths[] = {"/wifi", "/domoticz", "/", "/x"};
    (void)req((rnd32() & 1) ? RU_HTTP_POST : RU_HTTP_GET, paths[rnd32() % 4], (rnd32() & 3) ? b : NULL, 1, 2000 + (uint32_t)it);
    if (it % 4000 == 0) { allpages_n = 0; ALLPAGES[0] = 0; }
    if (!ru_portal_running(&P)) { ru_portal_start(&P, RU_PORTAL_SRC_USER, 2000 + (uint32_t)it); req(RU_HTTP_GET, "/", NULL, 1, 2000 + (uint32_t)it); page_csrf(csrf, sizeof csrf); }
    if (P.step == RU_PORTAL_DONE || P.locked) { ru_portal_stop(&P, RU_PORTAL_STOP_USER); }
  }
  CHECK(!strstr(LOG, "TOKENMARKER"));
}

/* ============================ review findings: guard, wifi validation, busy ============================ */
static void test_guard(void) {
  const char *ap = "192.168.4.1";
  /* good */
  CHECK(ru_portal_guard(ap, "192.168.4.1", "192.168.4.1", false) == RU_GUARD_ALLOW);
  CHECK(ru_portal_guard(ap, "192.168.4.1", "192.168.4.1", true) == RU_GUARD_ALLOW);
  CHECK(ru_portal_guard(ap, "192.168.4.1", "192.168.4.1:80", true) == RU_GUARD_ALLOW);
  /* request that arrived on the home-network address: refused whatever Host says */
  const char *locals[] = {"192.168.1.20", "10.0.0.5", "127.0.0.1", "0.0.0.0", "", "192.168.4.2", "192.168.4.10", "192.168.4.1.", " 192.168.4.1",
                          "::ffff:192.168.4.1", "[::ffff:192.168.4.1]", "::1", "192.168.4.1:80", "192.168.004.001"};
  for (size_t i = 0; i < sizeof locals / sizeof *locals; i++) {
    CHECK(ru_portal_guard(ap, locals[i], "192.168.4.1", false) == RU_GUARD_FORBIDDEN);
    CHECK(ru_portal_guard(ap, locals[i], "192.168.1.20", true) == RU_GUARD_FORBIDDEN);
  }
  CHECK(ru_portal_guard(ap, NULL, "192.168.4.1", false) == RU_GUARD_FORBIDDEN);
  CHECK(ru_portal_guard(NULL, "192.168.4.1", "192.168.4.1", false) == RU_GUARD_FORBIDDEN);
  CHECK(ru_portal_guard("", "", "", false) == RU_GUARD_FORBIDDEN);
  /* missing Host */
  /* GET without Host (HTTP/1.0 captive probes) is redirected, no longer 400; POST without Host stays 400; wrong local address stays 403 */
  CHECK(ru_portal_guard(ap, "192.168.4.1", NULL, false) == RU_GUARD_REDIRECT);
  CHECK(ru_portal_guard(ap, "192.168.4.1", "", false) == RU_GUARD_REDIRECT);
  CHECK(ru_portal_guard(ap, "192.168.4.1", "", true) == RU_GUARD_BAD_REQUEST);
  CHECK(ru_portal_guard(ap, "192.168.4.1", NULL, true) == RU_GUARD_BAD_REQUEST);
  CHECK(ru_portal_guard(ap, "192.168.1.20", NULL, false) == RU_GUARD_FORBIDDEN);
  CHECK(ru_portal_guard(ap, "192.168.1.20", "", true) == RU_GUARD_FORBIDDEN);
  /* glue address helpers */
  { char o[24]; const uint8_t v4[4] = {192, 168, 4, 1};
    uint8_t m[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 10, 1, 2, 1}; uint8_t n6[16] = {0}; n6[15] = 1; uint8_t cp[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xfe, 1, 2, 3, 4};
    ru_portal_local_text(4, v4, o, sizeof o); CHECK(!strcmp(o, "192.168.4.1"));
    ru_portal_local_text(6, m, o, sizeof o); CHECK(!strcmp(o, "10.1.2.1"));
    ru_portal_local_text(6, n6, o, sizeof o); CHECK(o[0] == 0);
    ru_portal_local_text(6, cp, o, sizeof o); CHECK(o[0] == 0);
    ru_portal_local_text(2, v4, o, sizeof o); CHECK(o[0] == 0);
    ru_portal_local_text(4, NULL, o, sizeof o); CHECK(o[0] == 0);
    char tiny[8]; ru_portal_local_text(4, v4, tiny, sizeof tiny); CHECK(strlen(tiny) < sizeof tiny);
    CHECK(!strcmp(ru_portal_host_arg(100, 48, "x"), "-") && !strcmp(ru_portal_host_arg(48, 48, "x"), "-"));
    CHECK(!strcmp(ru_portal_host_arg(11, 48, "192.168.4.1"), "192.168.4.1"));
    CHECK(ru_portal_host_arg(0, 48, "") == NULL && ru_portal_host_arg(5, 48, "") == NULL && ru_portal_host_arg(5, 48, NULL) == NULL);
    /* an over-long Host is foreign: GET redirects, POST refused (not treated as missing) */
    CHECK(ru_portal_guard(ap, "192.168.4.1", ru_portal_host_arg(60, 48, ""), true) == RU_GUARD_BAD_REQUEST);
  }
  /* wrong Host, port variants, rebinding names, mapped forms: never ALLOW; GET is redirected, POST refused */
  const char *hosts[] = {"evil.example", "192.168.4.1.evil.example", "evil.example:80", "192.168.4.1:8080", "192.168.4.1:", "192.168.4.1:0080", "192.168.4.1:80:80",
                         "192.168.4.1.", "192.168.4.10", "192.168.4.11", "192.168.4.", " 192.168.4.1", "192.168.4.1 ", "192.168.4.1\t", "192.168.4.1@evil.example",
                         "evil.example@192.168.4.1", "evil.example#192.168.4.1", "evil.example/192.168.4.1", "[::ffff:192.168.4.1]", "[::ffff:c0a8:401]", "::ffff:192.168.4.1",
                         "localhost", "127.0.0.1", "192.168.1.20", "0xc0a80401", "3232236545", "192.168.004.001", "192.168.4.1,evil.example", "192.168.4.1%00.evil.example",
                         "connectivitycheck.gstatic.com", "-", "192.168.4.1\r\nX: y", "192.168.4.1:80 ", "192.168.4.1:800"};
  for (size_t i = 0; i < sizeof hosts / sizeof *hosts; i++) {
    CHECK(ru_portal_guard(ap, "192.168.4.1", hosts[i], false) == RU_GUARD_REDIRECT);
    CHECK(ru_portal_guard(ap, "192.168.4.1", hosts[i], true) == RU_GUARD_BAD_REQUEST);
  }
  /* a custom AP address works the same way */
  CHECK(ru_portal_guard("10.1.2.1", "10.1.2.1", "10.1.2.1", false) == RU_GUARD_ALLOW && ru_portal_guard("10.1.2.1", "10.1.2.1", "192.168.4.1", false) == RU_GUARD_REDIRECT);
}

static bool busy_flag;
static bool cb_busy(void *u) { (void)u; return busy_flag; }
static void test_wifi_validation_and_busy(void) {
  char csrf[64];
  /* portal validates exactly like the USB path: 64 non-hex, non-ASCII, control bytes and bad SSIDs never reach the join */
  char p64[80];
  memset(p64, 'z', 64); p64[64] = 0;
  const char *bad[] = {p64, "p%C3%A4ssw%C3%B6rd1", "%FF%FE%FD%FC%FB%FA%F9%F8", "pass%01word", "pass%7Fword"};
  ru_portal_cfg_t cfg0 = {0};
  cfg0.max_failures = 50;
  setup(&cfg0);
  started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000);
  page_csrf(csrf, sizeof csrf);
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    post_wifi(csrf, "Home", "", bad[i], 1, 2100 + (uint32_t)i);
    CHECK(RS.status == 200 && strstr(BODY, "not valid") && join_calls == 0);
  }
  post_wifi(csrf, "%C3%28bad", "", WIFIPW, 1, 2200); /* invalid UTF-8 in the SSID */
  CHECK(RS.status == 200 && strstr(BODY, "not valid") && join_calls == 0);
  /* a real 64-hex PSK and an ASCII passphrase still pass */
  char hex[80];
  memset(hex, 'a', 64); hex[64] = 0;
  post_wifi(csrf, "Home", "", hex, 1, 2300);
  CHECK(RS.status == 303 && join_calls == 1);
  /* the same inputs on the USB path */
  {
    ru_cfgline_ops_t ops = {0};
    char r[RU_CFGLINE_REPLY_MAX], j[200];
    ru_kv_mem_t kv;
    ru_kv_mem_init(&kv);
    snprintf(j, sizeof j, "{\"v\":1,\"wifi\":{\"ssid\":\"Home\",\"password\":\"%s\"}}", p64);
    ru_cfgline_handle(&kv.base, &ops, j, strlen(j), r, sizeof r);
    CHECK(!strcmp(r, "RU+CFG ERR bad_wifi"));
    snprintf(j, sizeof j, "{\"v\":1,\"wifi\":{\"ssid\":\"Home\",\"password\":\"p\xc3\xa4ssw\xc3\xb6rd1\"}}");
    ru_cfgline_handle(&kv.base, &ops, j, strlen(j), r, sizeof r);
    CHECK(!strcmp(r, "RU+CFG ERR bad_wifi"));
  }
  /* busy: a refused join says so, is not a lockout strike, and a later attempt can still succeed */
  ru_portal_cfg_t cfg = {0};
  cfg.max_failures = 2;
  setup(&cfg);
  P.ops.wifi_busy = cb_busy;
  started();
  req(RU_HTTP_GET, "/", NULL, 1, 2000);
  page_csrf(csrf, sizeof csrf);
  join_ok = false;
  busy_flag = true;
  for (int i = 0; i < 4; i++) {
    post_wifi(csrf, "Home", "", WIFIPW, 1, 2100 + (uint32_t)i);
    CHECK(RS.status == 200 && strstr(BODY, "still busy") && !strstr(BODY, "could not join") && !P.locked);
  }
  busy_flag = false;
  post_wifi(csrf, "Home", "", WIFIPW, 1, 2200);
  CHECK(RS.status == 200 && strstr(BODY, "could not join") && P.failures == 1); /* an ordinary failure still counts */
  join_ok = true;
  post_wifi(csrf, "Home", "", WIFIPW, 1, 2300);
  CHECK(RS.status == 303);
}

int main(void) {
  test_http_basic();
  test_http_truncation();
  test_http_limits();
  test_http_timeout();
  test_http_fuzz();
  test_form();
  test_dns();
  test_captive();
  test_qr();
  test_password();
  test_flow_happy();
  test_https_and_errors();
  test_escaping();
  test_csrf();
  test_rate_limit();
  test_client_and_timeouts();
  test_never_unless_requested();
  test_probes_and_status();
  test_full_scan_page_fits();
  test_portal_fuzz();
  test_guard();
  test_wifi_validation_and_busy();
  MT_DONE();
}
