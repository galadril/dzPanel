#include "ru_captive.h"
#include "ru_cfg.h" /* ru_secure_zero */

#include <string.h>

/* ---------------- DNS ---------------- */
size_t ru_dns_answer(const uint8_t *q, size_t qlen, const uint8_t ip[4], uint8_t *out, size_t cap) {
  if (!q || !out || !ip || qlen < 12 || qlen > 512) return 0;
  uint16_t flags = (uint16_t)((q[2] << 8) | q[3]);
  unsigned qd = (unsigned)((q[4] << 8) | q[5]), an = (unsigned)((q[6] << 8) | q[7]);
  unsigned ns = (unsigned)((q[8] << 8) | q[9]), ar = (unsigned)((q[10] << 8) | q[11]);
  if (flags & 0x8000) return 0;           /* a response */
  if (((flags >> 11) & 0xF) != 0) return 0; /* opcode must be QUERY */
  if (flags & 0x0200) return 0;           /* truncated query makes no sense */
  if (qd != 1 || an != 0 || ns != 0 || ar > 1) return 0;
  size_t i = 12, nlen = 0;
  for (;;) {
    if (i >= qlen) return 0;
    uint8_t l = q[i];
    if (l == 0) { i++; nlen++; break; }
    if (l & 0xC0) return 0; /* compression or extended label in the question */
    if (l > 63 || i + 1 + l > qlen) return 0;
    nlen += (size_t)l + 1;
    if (nlen > 255) return 0;
    i += 1 + (size_t)l;
  }
  if (i + 4 > qlen) return 0;
  unsigned qtype = (unsigned)((q[i] << 8) | q[i + 1]), qclass = (unsigned)((q[i + 2] << 8) | q[i + 3]);
  if (qclass != 1) return 0;
  size_t qend = i + 4; /* end of the question */
  if (ar == 0 && qend != qlen) return 0; /* trailing garbage */
  if (ar == 1) { /* one additional record: must be well-formed, then it is dropped */
    size_t j = qend;
    if (j + 11 > qlen) return 0;
    if (q[j] != 0 && (q[j] & 0xC0) != 0xC0) return 0; /* EDNS OPT has the root name */
    j += (q[j] == 0) ? 1 : 2;
    if (j + 10 > qlen) return 0;
    size_t rdl = (size_t)((q[j + 8] << 8) | q[j + 9]);
    if (j + 10 + rdl != qlen) return 0;
  }
  bool answer = qtype == 1;
  size_t need = qend + (answer ? 16 : 0);
  if (need > cap || need > 512) return 0;
  memcpy(out, q, qend);
  out[2] = (uint8_t)(0x80 | 0x04 | (q[2] & 0x01)); /* QR, AA, copy RD */
  out[3] = 0x00;                                    /* RA=0, rcode NOERROR */
  out[6] = 0; out[7] = answer ? 1 : 0;
  out[8] = out[9] = out[10] = out[11] = 0;
  if (answer) {
    uint8_t *a = out + qend;
    a[0] = 0xC0; a[1] = 0x0C; /* name: pointer to the question */
    a[2] = 0; a[3] = 1;       /* A */
    a[4] = 0; a[5] = 1;       /* IN */
    a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 30; /* TTL */
    a[10] = 0; a[11] = 4;
    memcpy(a + 12, ip, 4);
  }
  return need;
}

/* ---------------- paths ---------------- */
ru_probe_t ru_captive_classify(const char *p) {
  if (!p) return RU_PROBE_OTHER;
  if (!strcmp(p, "/") || !strcmp(p, "/index.html")) return RU_PROBE_ROOT;
  if (!strcmp(p, "/generate_204") || !strcmp(p, "/gen_204")) return RU_PROBE_ANDROID;
  if (!strcmp(p, "/hotspot-detect.html") || !strcmp(p, "/library/test/success.html")) return RU_PROBE_APPLE;
  if (!strcmp(p, "/ncsi.txt") || !strcmp(p, "/connecttest.txt") || !strcmp(p, "/redirect")) return RU_PROBE_WINDOWS;
  return RU_PROBE_OTHER;
}

bool ru_portal_url(const char *ap_ip, char *out, size_t cap) {
  if (!out || !cap) return false;
  out[0] = 0;
  if (!ap_ip) return false;
  size_t n = strlen(ap_ip);
  if (n < 7 || n > 15) return false;
  unsigned dots = 0;
  for (size_t i = 0; i < n; i++) {
    if (ap_ip[i] == '.') dots++;
    else if (ap_ip[i] < '0' || ap_ip[i] > '9') return false;
  }
  if (dots != 3 || ap_ip[0] == '.' || ap_ip[n - 1] == '.' || strstr(ap_ip, "..")) return false;
  if (n + 9 > cap) return false; /* "http://" + ip + "/" + NUL */
  memcpy(out, "http://", 7);
  memcpy(out + 7, ap_ip, n);
  out[7 + n] = '/';
  out[8 + n] = 0;
  return true;
}

/* ---------------- QR ---------------- */
typedef struct { char *o; size_t cap, n; bool ok; } wr_t;
static void wc(wr_t *w, char c) { if (w->n + 1 < w->cap) w->o[w->n++] = c; else w->ok = false; }
static void ws(wr_t *w, const char *s) { while (*s) wc(w, *s++); }
static bool all_hex(const char *s) {
  if (!*s) return false;
  for (; *s; s++)
    if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) return false;
  return true;
}
static void wesc(wr_t *w, const char *s) {
  bool q = all_hex(s);
  if (q) wc(w, '"');
  for (; *s; s++) {
    if (*s == '\\' || *s == ';' || *s == ',' || *s == ':' || *s == '"') wc(w, '\\');
    wc(w, *s);
  }
  if (q) wc(w, '"');
}
bool ru_qr_wifi_payload(const char *ssid, const char *password, char *out, size_t cap) {
  if (!out || !cap) return false;
  out[0] = 0;
  if (!ssid || !password) return false;
  size_t sl = strlen(ssid), pl = strlen(password);
  if (sl == 0 || sl > 32 || pl > 63) return false;
  for (size_t i = 0; i < sl; i++) if ((unsigned char)ssid[i] < 0x20 || ssid[i] == 0x7f) return false;
  for (size_t i = 0; i < pl; i++) if ((unsigned char)password[i] < 0x20 || password[i] == 0x7f) return false;
  wr_t w = {out, cap, 0, true};
  ws(&w, pl ? "WIFI:T:WPA;S:" : "WIFI:T:nopass;S:");
  wesc(&w, ssid);
  if (pl) { ws(&w, ";P:"); wesc(&w, password); }
  ws(&w, ";;");
  if (!w.ok) { ru_secure_zero(out, cap); return false; }
  out[w.n] = 0;
  return true;
}

/* ---------------- random ---------------- */
bool ru_gen_password(ru_rand_fn rnd, void *user, unsigned len, char *out, size_t cap) {
  if (!out || !cap) return false;
  out[0] = 0;
  if (!rnd || len < RU_PW_MIN || len > RU_PW_MAX || cap < (size_t)len + 1) return false;
  uint8_t r[RU_PW_MAX];
  if (!rnd(user, r, len)) { ru_secure_zero(r, sizeof r); return false; }
  for (unsigned i = 0; i < len; i++) out[i] = RU_PW_ALPHABET[r[i] & 31];
  out[len] = 0;
  ru_secure_zero(r, sizeof r);
  return true;
}
bool ru_gen_ap_ssid(ru_rand_fn rnd, void *user, char *out, size_t cap) {
  if (!out || !cap) return false;
  out[0] = 0;
  if (!rnd || cap < 13) return false;
  uint8_t r[4];
  if (!rnd(user, r, 4)) return false;
  memcpy(out, "RoundOS-", 8);
  for (int i = 0; i < 4; i++) out[8 + i] = RU_PW_ALPHABET[r[i] & 31];
  out[12] = 0;
  return true;
}
bool ru_gen_hex(ru_rand_fn rnd, void *user, unsigned n, char *out, size_t cap) {
  if (!out || !cap) return false;
  out[0] = 0;
  if (!rnd || n == 0 || n > 32 || cap < 2 * (size_t)n + 1) return false;
  uint8_t r[32];
  if (!rnd(user, r, n)) { ru_secure_zero(r, sizeof r); return false; }
  static const char hx[] = "0123456789abcdef";
  for (unsigned i = 0; i < n; i++) { out[2 * i] = hx[r[i] >> 4]; out[2 * i + 1] = hx[r[i] & 15]; }
  out[2 * n] = 0;
  ru_secure_zero(r, sizeof r);
  return true;
}

bool ru_ct_streq(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  unsigned char d = (unsigned char)(la != lb);
  size_t n = la < lb ? la : lb;
  for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
  return d == 0;
}

/* ---------------- request guard ---------------- */
ru_guard_t ru_portal_guard(const char *ap_ip, const char *local_ip, const char *host, bool is_post) {
  if (!ap_ip || !ap_ip[0] || !local_ip || strcmp(local_ip, ap_ip) != 0) return RU_GUARD_FORBIDDEN;
  if (!host || !host[0]) return RU_GUARD_BAD_REQUEST;
  size_t n = strlen(ap_ip);
  bool ok = strncmp(host, ap_ip, n) == 0 && (host[n] == 0 || strcmp(host + n, ":80") == 0);
  if (ok) return RU_GUARD_ALLOW;
  return is_post ? RU_GUARD_BAD_REQUEST : RU_GUARD_REDIRECT;
}
