#define _GNU_SOURCE
#include "dz_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const char *dz_net_err_name(dz_net_err_t e) {
  switch (e) {
    case DZ_NET_OK: return "ok";
    case DZ_NET_TIMEOUT: return "timeout";
    case DZ_NET_CONNECT: return "connect";
    case DZ_NET_TLS: return "tls";
    case DZ_NET_TOO_LARGE: return "too-large";
  }
  return "?";
}

void dz_transport_destroy(dz_transport_t *t) {
  if (t && t->destroy) t->destroy(t);
}

static int64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Header values must not contain control characters (header injection, config injection). */
static int headers_ok(const char *const *h) {
  for (; h && *h; h++)
    for (const char *p = *h; *p; p++)
      if ((unsigned char)*p < 0x20 || *p == 0x7f) return 0;
  return 1;
}

/* ---------- plain socket ---------- */

static dz_net_err_t wait_fd(int fd, short ev, int64_t deadline) {
  for (;;) {
    int64_t left = deadline - mono_ms();
    if (left <= 0) return DZ_NET_TIMEOUT;
    struct pollfd p = {fd, ev, 0};
    int r = poll(&p, 1, (int)(left > 1000000 ? 1000000 : left));
    if (r > 0) return DZ_NET_OK;
    if (r < 0 && errno != EINTR) return DZ_NET_CONNECT;
  }
}

/* Splits http://host[:port]/path. Returns 0 on success; -1 bad, -2 https. */
static int split_url(const char *url, char *host, size_t hcap, char *port, size_t pcap, const char **path) {
  if (strncmp(url, "http://", 7) != 0) return strncmp(url, "https://", 8) == 0 ? -2 : -1;
  const char *h = url + 7, *e = h;
  while (*e && *e != '/' && *e != ':') e++;
  if (e == h || (size_t)(e - h) >= hcap) return -1;
  memcpy(host, h, (size_t)(e - h));
  host[e - h] = 0;
  if (*e == ':') {
    const char *ps = ++e;
    while (*e && *e != '/') e++;
    if (e == ps || (size_t)(e - ps) >= pcap) return -1;
    memcpy(port, ps, (size_t)(e - ps));
    port[e - ps] = 0;
  } else {
    snprintf(port, pcap, "80");
  }
  *path = *e ? e : "/";
  return 0;
}

typedef struct {
  char *buf;
  size_t cap, len;
  int too_large;
} sink_t;

static void sink_put(sink_t *s, const char *d, size_t n) {
  size_t room = s->cap - 1 - s->len;
  if (n > room) { n = room; s->too_large = 1; }
  memcpy(s->buf + s->len, d, n);
  s->len += n;
  s->buf[s->len] = 0;
}

static dz_net_err_t sock_do(const char *url, const char *const *headers, const dz_post_t *post, uint32_t timeout_ms,
                            char *buf, size_t cap, dz_http_result_t *out) {
  dz_http_result_t dummy;
  if (!out) out = &dummy;
  out->status = 0;
  out->len = 0;
  if (post && post->hdr_out && post->hdr_cap) post->hdr_out[0] = 0;
  if (!buf || cap < 1) return DZ_NET_CONNECT;
  buf[0] = 0;
  char host[128], port[8];
  const char *path;
  int sr = split_url(url, host, sizeof host, port, sizeof port, &path);
  if (sr == -2) return DZ_NET_TLS;
  if (sr < 0 || !headers_ok(headers) || strpbrk(url, "\r\n ")) return DZ_NET_CONNECT;

  int64_t deadline = mono_ms() + timeout_ms;
  struct addrinfo hints, *ai = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai) return DZ_NET_CONNECT;
  int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd < 0) { freeaddrinfo(ai); return DZ_NET_CONNECT; }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  dz_net_err_t err = DZ_NET_OK;
  int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
  freeaddrinfo(ai);
  if (r < 0 && errno != EINPROGRESS) { close(fd); return DZ_NET_CONNECT; }
  if (r < 0) {
    err = wait_fd(fd, POLLOUT, deadline);
    int soe = 0;
    socklen_t sl = sizeof soe;
    if (err == DZ_NET_OK && (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soe, &sl) < 0 || soe)) err = DZ_NET_CONNECT;
    if (err != DZ_NET_OK) { close(fd); return err; }
  }

  char req[2048];
  int n;
  if (post)
    n = snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHost: %s:%s\r\nConnection: close\r\nContent-Type: %s\r\nContent-Length: %zu\r\n",
                 path, host, port, post->content_type ? post->content_type : "application/json", post->len);
  else
    n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%s\r\nConnection: close\r\nAccept: application/json\r\n", path,
                 host, port);
  for (const char *const *h = headers; h && *h && n > 0 && (size_t)n < sizeof req; h++)
    n += snprintf(req + n, sizeof req - (size_t)n, "%s\r\n", *h);
  if (n > 0 && (size_t)n < sizeof req - 2) { req[n++] = '\r'; req[n++] = '\n'; } else { close(fd); return DZ_NET_CONNECT; }
  for (int off = 0; off < n;) {
    if ((err = wait_fd(fd, POLLOUT, deadline)) != DZ_NET_OK) { close(fd); return err; }
    ssize_t w = send(fd, req + off, (size_t)(n - off), MSG_NOSIGNAL);
    if (w < 0 && errno != EAGAIN && errno != EINTR) { close(fd); return DZ_NET_CONNECT; }
    if (w > 0) off += (int)w;
  }
  for (size_t off = 0; post && off < post->len;) {
    if ((err = wait_fd(fd, POLLOUT, deadline)) != DZ_NET_OK) { close(fd); return err; }
    ssize_t w = send(fd, post->body + off, post->len - off, MSG_NOSIGNAL);
    if (w < 0 && errno != EAGAIN && errno != EINTR) { close(fd); return DZ_NET_CONNECT; }
    if (w > 0) off += (size_t)w;
  }

  /* Read headers (bounded 8 KB), then body per Content-Length / chunked / until close. */
  char hdr[8192];
  size_t hl = 0, body_off = 0;
  int have_hdr = 0;
  sink_t sink = {buf, cap, 0, 0};
  long clen = -1;
  int chunked = 0;
  /* chunk decoder state */
  enum { C_SIZE, C_DATA, C_CRLF, C_TRAILER, C_DONE } cs = C_SIZE;
  size_t csize = 0, cgot = 0;
  char szline[24];
  size_t szn = 0;
  long body_total = 0;
  int eof = 0;
  for (;;) {
    char tmp[4096];
    const char *data = NULL;
    size_t dn = 0;
    if (!have_hdr) {
      if (hl >= sizeof hdr - 1) { err = DZ_NET_CONNECT; break; }
    }
    if ((err = wait_fd(fd, POLLIN, deadline)) != DZ_NET_OK) break;
    size_t want = sizeof tmp;
    if (!have_hdr && sizeof hdr - 1 - hl < want) want = sizeof hdr - 1 - hl;
    ssize_t rd = recv(fd, tmp, want, 0);
    if (rd < 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      err = DZ_NET_CONNECT;
      break;
    }
    if (rd == 0) eof = 1;
    if (!have_hdr) {
      memcpy(hdr + hl, tmp, (size_t)rd);
      hl += (size_t)rd;
      hdr[hl] = 0;
      char *he = strstr(hdr, "\r\n\r\n");
      if (!he) {
        if (eof) { err = DZ_NET_CONNECT; break; }
        continue;
      }
      have_hdr = 1;
      body_off = (size_t)(he - hdr) + 4;
      if (strncmp(hdr, "HTTP/1.", 7) != 0 || hdr[7] < '0' || hdr[7] > '1' || hdr[8] != ' ') { err = DZ_NET_CONNECT; break; }
      out->status = atoi(hdr + 9);
      if (out->status < 100 || out->status > 599) { out->status = 0; err = DZ_NET_CONNECT; break; }
      *he = 0;
      for (char *p = strchr(hdr, '\n'); p; p = strchr(p + 1, '\n')) {
        p++;
        if (!strncasecmp(p, "content-length:", 15)) {
          char *e;
          long v = strtol(p + 15, &e, 10);
          if (v >= 0 && v < 1L << 30) clen = v;
        } else if (!strncasecmp(p, "transfer-encoding:", 18)) {
          if (strcasestr(p, "chunked")) chunked = 1;
        } else if (post && post->capture_header && post->hdr_out && post->hdr_cap) {
          size_t hn = strlen(post->capture_header);
          if (!strncasecmp(p, post->capture_header, hn) && p[hn] == ':') {
            const char *v = p + hn + 1;
            while (*v == ' ') v++;
            size_t vl = strcspn(v, "\r\n");
            if (vl >= post->hdr_cap) vl = post->hdr_cap - 1;
            memcpy(post->hdr_out, v, vl);
            post->hdr_out[vl] = 0;
          }
        }
      }
      data = hdr + body_off;
      dn = hl - body_off;
    } else {
      data = tmp;
      dn = (size_t)rd;
    }
    /* feed body bytes */
    if (chunked) {
      for (size_t i = 0; i < dn && cs != C_DONE; i++) {
        char c = data[i];
        switch (cs) {
          case C_SIZE:
            if (c == '\n') {
              szline[szn] = 0;
              char *e;
              unsigned long v = strtoul(szline, &e, 16);
              if (szn == 0 || (*e && *e != ';' && *e != '\r') || v > (1UL << 30)) { err = DZ_NET_CONNECT; cs = C_DONE; break; }
              csize = v;
              cgot = 0;
              szn = 0;
              cs = csize ? C_DATA : C_TRAILER;
            } else if (szn < sizeof szline - 1) {
              szline[szn++] = c;
            } else {
              err = DZ_NET_CONNECT; cs = C_DONE;
            }
            break;
          case C_DATA: {
            size_t take = dn - i < csize - cgot ? dn - i : csize - cgot;
            sink_put(&sink, data + i, take);
            cgot += take;
            i += take - 1;
            if (cgot == csize) cs = C_CRLF;
            break;
          }
          case C_CRLF:
            if (c == '\n') cs = C_SIZE;
            break;
          case C_TRAILER:
            if (c == '\n') cs = C_DONE; /* tolerate no trailers; stop at first blank-ish line end */
            break;
          case C_DONE: break;
        }
      }
      if (err != DZ_NET_OK || cs == C_DONE) break;
    } else {
      sink_put(&sink, data, dn);
      body_total += (long)dn;
      if (clen >= 0 && body_total >= clen) break;
    }
    if (eof) {
      /* closed early: fine without framing, an error when framing promised more */
      if ((chunked && cs != C_DONE) || (!chunked && clen >= 0 && body_total < clen)) err = DZ_NET_CONNECT;
      break;
    }
  }
  close(fd);
  out->len = sink.len;
  if (err == DZ_NET_OK && sink.too_large) err = DZ_NET_TOO_LARGE;
  if (err != DZ_NET_OK && err != DZ_NET_TOO_LARGE) {
    /* a half-received body is not a response */
    out->len = 0;
    buf[0] = 0;
    if (post && post->hdr_out && post->hdr_cap) post->hdr_out[0] = 0;
  }
  return err;
}

static dz_net_err_t sock_get(dz_transport_t *t, const char *url, const char *const *headers, uint32_t timeout_ms,
                             char *buf, size_t cap, dz_http_result_t *out) {
  (void)t;
  return sock_do(url, headers, NULL, timeout_ms, buf, cap, out);
}

static dz_net_err_t sock_post(dz_transport_t *t, const char *url, const char *const *headers, const dz_post_t *p,
                              uint32_t timeout_ms, char *buf, size_t cap, dz_http_result_t *out) {
  (void)t;
  if (!p || !p->body) return DZ_NET_CONNECT;
  return sock_do(url, headers, p, timeout_ms, buf, cap, out);
}

static void plain_destroy(dz_transport_t *t) { free(t); }

dz_transport_t *dz_transport_socket_create(void) {
  dz_transport_t *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->get = sock_get;
  t->post = sock_post;
  t->destroy = plain_destroy;
  return t;
}

/* ---------- curl CLI (dev only) ---------- */

static int cfg_quote(char *dst, size_t cap, size_t *n, const char *s) {
  if (*n + 2 >= cap) return -1;
  dst[(*n)++] = '"';
  for (; *s; s++) {
    if (*n + 3 >= cap) return -1;
    if (*s == '"' || *s == '\\') dst[(*n)++] = '\\';
    dst[(*n)++] = *s;
  }
  dst[(*n)++] = '"';
  return 0;
}

/* The curl config (URL, body, Authorization header) holds the token: curl_do owns the buffer and wipes it with a volatile loop on
 * EVERY exit path of curl_do_inner (cfg_quote overflow, pipe() / fork() failure, normal return). Residual risk accepted: the ESP
 * transport (dz_transport_esp.c) hands the header to esp_http_client, which keeps its own heap copy until the client is cleaned
 * up; that copy is freed, not wiped. Dev-only host code below. */
static dz_net_err_t curl_do_inner(char *cfg, size_t cfg_cap, const char *url, const char *const *headers, const dz_post_t *post,
                                  uint32_t timeout_ms, char *buf, size_t cap, dz_http_result_t *out) {
  dz_http_result_t dummy;
  if (!out) out = &dummy;
  out->status = 0;
  out->len = 0;
  if (post && post->hdr_out && post->hdr_cap) post->hdr_out[0] = 0;
  if (!buf || cap < 1) return DZ_NET_CONNECT;
  buf[0] = 0;
  if (!headers_ok(headers) || strpbrk(url, "\r\n \"\\")) return DZ_NET_CONNECT;
  /* the captured header name goes into curl's -w format: letters, digits and '-' only */
  char wfmt[96];
  bool cap_hdr = post && post->capture_header && post->hdr_out && post->hdr_cap;
  if (cap_hdr) {
    size_t cl = strlen(post->capture_header);
    if (!cl || cl > 40) return DZ_NET_CONNECT;
    for (size_t i = 0; i < cl; i++) {
      char c = post->capture_header[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return DZ_NET_CONNECT;
    }
    snprintf(wfmt, sizeof wfmt, "\n%%header{%s}\n%%{http_code}", post->capture_header);
  } else {
    snprintf(wfmt, sizeof wfmt, "\n%%{http_code}");
  }
  if (post && (!post->body || post->len > 3000)) return DZ_NET_CONNECT;
  if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return DZ_NET_CONNECT;

  /* config on stdin: nothing secret on argv */
  size_t n = 0;
  n += (size_t)snprintf(cfg, cfg_cap, "url = ");
  if (cfg_quote(cfg, cfg_cap, &n, url)) return DZ_NET_CONNECT;
  cfg[n++] = '\n';
  if (post) {
    char body[3001];
    memcpy(body, post->body, post->len);
    body[post->len] = 0;
    if (strpbrk(body, "\r\n")) return DZ_NET_CONNECT;
    n += (size_t)snprintf(cfg + n, cfg_cap - n, "request = \"POST\"\nheader = \"Content-Type: %s\"\ndata-binary = ",
                          post->content_type ? post->content_type : "application/json");
    int qr = cfg_quote(cfg, cfg_cap, &n, body);
    if (qr) return DZ_NET_CONNECT;
    cfg[n++] = '\n';
  }
  for (const char *const *h = headers; h && *h; h++) {
    n += (size_t)snprintf(cfg + n, cfg_cap - n, "header = ");
    if (cfg_quote(cfg, cfg_cap, &n, *h)) return DZ_NET_CONNECT;
    cfg[n++] = '\n';
  }
  cfg[n] = 0;

  char tmo[16];
  snprintf(tmo, sizeof tmo, "%u", (unsigned)((timeout_ms + 999) / 1000 ? (timeout_ms + 999) / 1000 : 1));
  int in[2], outp[2];
  if (pipe(in) < 0) return DZ_NET_CONNECT;
  if (pipe(outp) < 0) { close(in[0]); close(in[1]); return DZ_NET_CONNECT; }
  signal(SIGPIPE, SIG_IGN);
  pid_t pid = fork();
  if (pid < 0) { close(in[0]); close(in[1]); close(outp[0]); close(outp[1]); return DZ_NET_CONNECT; }
  if (pid == 0) {
    dup2(in[0], 0);
    dup2(outp[1], 1);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) dup2(dn, 2);
    close(in[0]); close(in[1]); close(outp[0]); close(outp[1]);
    execlp("curl", "curl", "-q", "-sS", /* -q FIRST: never read ~/.curlrc */ "--max-time", tmo, "-K", "-", "-o", "-", "-w", wfmt, (char *)NULL);
    _exit(127);
  }
  close(in[0]);
  close(outp[1]);
  /* config is far smaller than a pipe buffer: a blocking write cannot deadlock */
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(in[1], cfg + off, n - off);
    if (w <= 0) break;
    off += (size_t)w;
  }
  close(in[1]);
  for (volatile char *z = cfg; z < cfg + cfg_cap; z++) *z = 0; /* held the token: non-elidable wipe (curl_do wipes again on every path) */

  /* body followed by "\n<code>"; keep the last 8 bytes aside */
  sink_t sink = {buf, cap, 0, 0};
  char tail[193]; /* 192-byte window + NUL: "\n<header value>\n<code>" */
  size_t tl = 0;
  int64_t deadline = mono_ms() + (int64_t)timeout_ms + 3000;
  for (;;) {
    char tmp[4096];
    if (wait_fd(outp[0], POLLIN, deadline) != DZ_NET_OK) { kill(pid, SIGKILL); break; }
    ssize_t r = read(outp[0], tmp, sizeof tmp);
    if (r <= 0) break;
    for (ssize_t i = 0; i < r; i++) {
      if (tl == 192) { sink_put(&sink, tail, 1); memmove(tail, tail + 1, tl - 1); tl--; }
      tail[tl++] = tmp[i];
    }
  }
  close(outp[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  if (code != 0) {
    out->len = 0;
    buf[0] = 0;
    if (code == 28 || code == -1) return DZ_NET_TIMEOUT;
    if (code == 35 || code == 51 || code == 53 || code == 54 || code == 58 || code == 59 || code == 60 || code == 64 || code == 66 || code == 77 || code == 80 || code == 82 || code == 83)
      return DZ_NET_TLS;
    return DZ_NET_CONNECT; /* 6, 7, 56, 127 (curl missing), ... */
  }
  /* tail = "\nNNN" (or "\n<header>\nNNN" for a capture) possibly preceded by body bytes */
  tail[tl] = 0;
  char *nl = NULL;
  for (size_t i = tl; i-- > 0;)
    if (tail[i] == '\n') { nl = tail + i; break; }
  if (!nl) return DZ_NET_CONNECT;
  if (cap_hdr) {
    char *nl2 = NULL;
    for (size_t i = (size_t)(nl - tail); i-- > 0;)
      if (tail[i] == '\n') { nl2 = tail + i; break; }
    if (!nl2) return DZ_NET_CONNECT;
    size_t vl = (size_t)(nl - nl2 - 1);
    if (vl >= post->hdr_cap) vl = post->hdr_cap - 1;
    memcpy(post->hdr_out, nl2 + 1, vl);
    post->hdr_out[vl] = 0;
    char *code = nl + 1;
    sink_put(&sink, tail, (size_t)(nl2 - tail));
    out->status = atoi(code);
    out->len = sink.len;
    if (out->status < 100 || out->status > 599) { out->status = 0; out->len = 0; buf[0] = 0; post->hdr_out[0] = 0; return DZ_NET_CONNECT; }
    return sink.too_large ? DZ_NET_TOO_LARGE : DZ_NET_OK;
  }
  /* bytes of tail before the newline belong to the body */
  sink_put(&sink, tail, (size_t)(nl - tail));
  out->status = atoi(nl + 1);
  out->len = sink.len;
  if (out->status < 100 || out->status > 599) { out->status = 0; out->len = 0; buf[0] = 0; return DZ_NET_CONNECT; }
  return sink.too_large ? DZ_NET_TOO_LARGE : DZ_NET_OK;
}

static dz_net_err_t curl_do(const char *url, const char *const *headers, const dz_post_t *post, uint32_t timeout_ms, char *buf,
                            size_t cap, dz_http_result_t *out) {
  char cfg[8192];
  dz_net_err_t e = curl_do_inner(cfg, sizeof cfg, url, headers, post, timeout_ms, buf, cap, out);
  for (volatile char *z = cfg; z < cfg + sizeof cfg; z++) *z = 0;
  return e;
}

static dz_net_err_t curl_get(dz_transport_t *t, const char *url, const char *const *headers, uint32_t timeout_ms,
                             char *buf, size_t cap, dz_http_result_t *out) {
  (void)t;
  return curl_do(url, headers, NULL, timeout_ms, buf, cap, out);
}

static dz_net_err_t curl_post(dz_transport_t *t, const char *url, const char *const *headers, const dz_post_t *p,
                              uint32_t timeout_ms, char *buf, size_t cap, dz_http_result_t *out) {
  (void)t;
  if (!p || !p->body) return DZ_NET_CONNECT;
  return curl_do(url, headers, p, timeout_ms, buf, cap, out);
}

dz_transport_t *dz_transport_curl_create(void) {
  dz_transport_t *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->get = curl_get;
  t->post = curl_post;
  t->destroy = plain_destroy;
  return t;
}
