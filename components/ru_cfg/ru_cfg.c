#include "ru_cfg.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

void ru_secure_zero(void *p, size_t n) {
  volatile unsigned char *v = (volatile unsigned char *)p;
  while (n--) *v++ = 0;
}

const char *ru_cfg_err_name(ru_cfg_err_t e) {
  switch (e) {
    case RU_CFG_OK: return "ok";
    case RU_CFG_BAD_JSON: return "bad_json";
    case RU_CFG_BAD_VERSION: return "bad_version";
    case RU_CFG_BAD_URL: return "bad_url";
    case RU_CFG_TOO_LONG: return "too_long";
    case RU_CFG_NOT_SET: return "not_set";
    case RU_CFG_STORE: return "store";
    case RU_CFG_BAD_WIFI: return "bad_wifi";
  }
  return "?";
}

/* ---------------- URL ---------------- */

static bool has_scheme(const char *s) {
  if (!isalpha((unsigned char)s[0])) return false;
  size_t i = 1;
  while (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.') i++;
  return s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/';
}

static bool path_char_ok(unsigned char c) {
  if (isalnum(c)) return true;
  return c && strchr("-._~/%:@!$&'()*+,;=", c) != NULL;
}

ru_cfg_err_t ru_cfg_normalise_url(const char *in, char *out, size_t cap, bool *https) {
  if (!in || !out || cap < 2) return RU_CFG_BAD_URL;
  out[0] = 0;
  while (*in == ' ' || (*in >= '\t' && *in <= '\r')) in++;
  size_t n = strlen(in);
  while (n && (in[n - 1] == ' ' || (in[n - 1] >= '\t' && in[n - 1] <= '\r'))) n--;
  if (n == 0) return RU_CFG_BAD_URL;
  if (n > 400) return RU_CFG_TOO_LONG;
  char s[420];
  size_t o = 0;
  if (!has_scheme(in)) { memcpy(s, "http://", 7); o = 7; }
  memcpy(s + o, in, n);
  s[o + n] = 0;
  for (size_t i = 0; s[i]; i++)
    if ((unsigned char)s[i] <= 0x20 || (unsigned char)s[i] >= 0x7f || s[i] == '\\') return RU_CFG_BAD_URL;
  bool tls;
  const char *rest;
  if (!strncasecmp(s, "http://", 7)) { tls = false; rest = s + 7; }
  else if (!strncasecmp(s, "https://", 8)) { tls = true; rest = s + 8; }
  else return RU_CFG_BAD_URL;
  if (https) *https = tls;
  size_t alen = strcspn(rest, "/?#");
  const char *path = rest + alen;
  if (strchr(path, '?') || strchr(path, '#')) return RU_CFG_BAD_URL;
  if (alen && memchr(rest, '@', alen)) return RU_CFG_BAD_URL; /* user info */
  if (memchr(rest, '?', alen) || memchr(rest, '#', alen)) return RU_CFG_BAD_URL;
  /* host[:port] */
  char host[130];
  size_t hl;
  const char *port = NULL;
  size_t pl = 0;
  if (alen && rest[0] == '[') {
    const char *close = memchr(rest, ']', alen);
    if (!close) return RU_CFG_BAD_URL;
    hl = (size_t)(close - rest) + 1;
    if (hl < 4 || hl >= sizeof host) return RU_CFG_BAD_URL;
    for (size_t i = 1; i + 1 < hl; i++)
      if (!isxdigit((unsigned char)rest[i]) && rest[i] != ':' && rest[i] != '.') return RU_CFG_BAD_URL;
    if (close + 1 < rest + alen) {
      if (close[1] != ':') return RU_CFG_BAD_URL;
      port = close + 2;
      pl = (size_t)(rest + alen - port);
    }
  } else {
    const char *colon = memchr(rest, ':', alen);
    hl = colon ? (size_t)(colon - rest) : alen;
    if (hl >= sizeof host) return RU_CFG_TOO_LONG;
    if (hl == 0) return RU_CFG_BAD_URL;
    for (size_t i = 0; i < hl; i++) {
      unsigned char c = (unsigned char)rest[i];
      if (!isalnum(c) && c != '-' && c != '.' && c != '_') return RU_CFG_BAD_URL;
    }
    if (colon) { port = colon + 1; pl = (size_t)(rest + alen - port); }
  }
  for (size_t i = 0; i < hl; i++) host[i] = (char)tolower((unsigned char)rest[i]);
  host[hl] = 0;
  char portbuf[8] = "";
  if (port && pl) {
    if (pl > 5) return RU_CFG_BAD_URL;
    unsigned v = 0;
    for (size_t i = 0; i < pl; i++) {
      if (!isdigit((unsigned char)port[i])) return RU_CFG_BAD_URL;
      v = v * 10 + (unsigned)(port[i] - '0');
    }
    if (v == 0 || v > 65535) return RU_CFG_BAD_URL;
    if (!((!tls && v == 80) || (tls && v == 443))) { memcpy(portbuf, port, pl); portbuf[pl] = 0; }
  }
  /* path: validate, drop trailing slashes, refuse dot segments */
  size_t plen = strlen(path);
  while (plen && path[plen - 1] == '/') plen--;
  for (size_t i = 0; i < plen; i++)
    if (!path_char_ok((unsigned char)path[i])) return RU_CFG_BAD_URL;
  for (size_t i = 0; i < plen;) {
    size_t j = i;
    if (path[j] == '/') j++;
    size_t k = j;
    while (k < plen && path[k] != '/') k++;
    size_t sl = k - j;
    if ((sl == 1 && path[j] == '.') || (sl == 2 && path[j] == '.' && path[j + 1] == '.')) return RU_CFG_BAD_URL;
    i = k > i ? k : i + 1;
  }
  size_t need = (tls ? 8 : 7) + hl + (portbuf[0] ? 1 + strlen(portbuf) : 0) + plen + 1;
  if (need > cap || need > RU_CFG_URL_MAX) { out[0] = 0; return RU_CFG_TOO_LONG; }
  char *w = out;
  w += sprintf(w, "%s://%s", tls ? "https" : "http", host);
  if (portbuf[0]) w += sprintf(w, ":%s", portbuf);
  memcpy(w, path, plen);
  w[plen] = 0;
  return RU_CFG_OK;
}

/* ---------------- token ---------------- */

ru_cfg_err_t ru_cfg_check_token(const char *t, bool *jwt_like) {
  if (jwt_like) *jwt_like = false;
  if (!t || !*t) return RU_CFG_BAD_JSON;
  size_t n = strlen(t);
  if (n >= RU_CFG_TOKEN_MAX) return RU_CFG_TOO_LONG;
  int dots = 0;
  bool b64 = true;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)t[i];
    if (c <= 0x20 || c >= 0x7f) return RU_CFG_BAD_JSON;
    if (c == '.') dots++;
    else if (!isalnum(c) && c != '_' && c != '-') b64 = false;
  }
  if (jwt_like) *jwt_like = dots == 2 && b64 && t[0] != '.';
  return RU_CFG_OK;
}

ru_cfg_err_t ru_cfg_validate(const ru_cfg_t *c) {
  char norm[RU_CFG_URL_MAX];
  bool https = false;
  if (!c) return RU_CFG_BAD_JSON;
  ru_cfg_err_t e = ru_cfg_normalise_url(c->url, norm, sizeof norm, &https);
  if (e != RU_CFG_OK) return e;
  if (strcmp(norm, c->url)) return RU_CFG_BAD_URL; /* stored form must already be normal */
  if (!https && !c->insecure_lan) return RU_CFG_BAD_URL;
  return ru_cfg_check_token(c->token, NULL);
}

/* ---------------- JSON (bounded, allocation free) ---------------- */

typedef struct {
  const char *p, *end;
  int depth;
  bool overflow; /* a captured string did not fit */
  bool nul;      /* an escape decoded to a NUL byte */
} jp_t;

static void ws(jp_t *j) {
  while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++;
}

static int hex4(const char *p, const char *end, unsigned *out) {
  if (end - p < 4) return 0;
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return 0;
  }
  *out = v;
  return 1;
}

/* Parses a string at j->p (on the opening quote). dst may be NULL (skip). Escapes decode to UTF-8; any resulting
 * byte < 0x20 other than via escape is invalid JSON. Sets overflow instead of truncating silently. */
static bool jstr(jp_t *j, char *dst, size_t cap) {
  if (j->p >= j->end || *j->p != '"') return false;
  j->p++;
  size_t n = 0;
  if (dst && cap) dst[0] = 0;
#define EMIT(ch) do { if (dst) { if (n + 1 < cap) dst[n] = (char)(ch); else j->overflow = true; } n++; } while (0)
  while (j->p < j->end) {
    unsigned char c = (unsigned char)*j->p++;
    if (c == '"') {
      if (dst && cap) dst[n < cap ? n : cap - 1] = 0;
      return true;
    }
    if (c < 0x20) return false;
    if (c != '\\') { EMIT(c); continue; }
    if (j->p >= j->end) return false;
    char e = *j->p++;
    switch (e) {
      case '"': EMIT('"'); break;
      case '\\': EMIT('\\'); break;
      case '/': EMIT('/'); break;
      case 'b': EMIT('\b'); break;
      case 'f': EMIT('\f'); break;
      case 'n': EMIT('\n'); break;
      case 'r': EMIT('\r'); break;
      case 't': EMIT('\t'); break;
      case 'u': {
        unsigned u;
        if (!hex4(j->p, j->end, &u)) return false;
        j->p += 4;
        if (u >= 0xD800 && u <= 0xDBFF) { /* surrogate pair */
          unsigned lo;
          if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u' || !hex4(j->p + 2, j->end, &lo) || lo < 0xDC00 || lo > 0xDFFF)
            return false;
          j->p += 6;
          u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        } else if (u >= 0xDC00 && u <= 0xDFFF) return false;
        if (u == 0) j->nul = true;
        if (u < 0x80) EMIT(u);
        else if (u < 0x800) { EMIT(0xC0 | (u >> 6)); EMIT(0x80 | (u & 0x3F)); }
        else if (u < 0x10000) { EMIT(0xE0 | (u >> 12)); EMIT(0x80 | ((u >> 6) & 0x3F)); EMIT(0x80 | (u & 0x3F)); }
        else { EMIT(0xF0 | (u >> 18)); EMIT(0x80 | ((u >> 12) & 0x3F)); EMIT(0x80 | ((u >> 6) & 0x3F)); EMIT(0x80 | (u & 0x3F)); }
        break;
      }
      default: return false;
    }
  }
#undef EMIT
  return false; /* unterminated */
}

static bool jlit(jp_t *j, const char *lit) {
  size_t n = strlen(lit);
  if ((size_t)(j->end - j->p) < n || memcmp(j->p, lit, n)) return false;
  j->p += n;
  return true;
}

static bool jnum(jp_t *j, double *out) {
  const char *s = j->p;
  if (j->p < j->end && *j->p == '-') j->p++;
  if (j->p >= j->end || !isdigit((unsigned char)*j->p)) return false;
  if (*j->p == '0') j->p++;
  else while (j->p < j->end && isdigit((unsigned char)*j->p)) j->p++;
  bool frac = false;
  if (j->p < j->end && *j->p == '.') {
    frac = true;
    j->p++;
    if (j->p >= j->end || !isdigit((unsigned char)*j->p)) return false;
    while (j->p < j->end && isdigit((unsigned char)*j->p)) j->p++;
  }
  if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
    frac = true;
    j->p++;
    if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
    if (j->p >= j->end || !isdigit((unsigned char)*j->p)) return false;
    while (j->p < j->end && isdigit((unsigned char)*j->p)) j->p++;
  }
  /* only small integers matter here; anything else reports as -1 */
  double v = -1;
  if (!frac && j->p - s <= 6) {
    v = 0;
    const char *q = s;
    bool neg = *q == '-';
    if (neg) q++;
    for (; q < j->p; q++) v = v * 10 + (*q - '0');
    if (neg) v = -v;
  }
  if (out) *out = v;
  return true;
}

static bool jskip(jp_t *j);
static bool jskip_container(jp_t *j, char close) {
  if (++j->depth > RU_CFG_JSON_DEPTH) return false;
  j->p++;
  ws(j);
  if (j->p < j->end && *j->p == close) { j->p++; j->depth--; return true; }
  for (;;) {
    ws(j);
    if (close == '}') {
      if (!jstr(j, NULL, 0)) return false;
      ws(j);
      if (j->p >= j->end || *j->p++ != ':') return false;
    }
    if (!jskip(j)) return false;
    ws(j);
    if (j->p >= j->end) return false;
    if (*j->p == ',') { j->p++; continue; }
    if (*j->p == close) { j->p++; j->depth--; return true; }
    return false;
  }
}
static bool jskip(jp_t *j) {
  ws(j);
  if (j->p >= j->end) return false;
  switch (*j->p) {
    case '{': return jskip_container(j, '}');
    case '[': return jskip_container(j, ']');
    case '"': return jstr(j, NULL, 0);
    case 't': return jlit(j, "true");
    case 'f': return jlit(j, "false");
    case 'n': return jlit(j, "null");
    default: return jnum(j, NULL);
  }
}

typedef struct {
  char url[RU_CFG_URL_MAX + 64];
  char token[RU_CFG_TOKEN_MAX + 8];
  char ssid[96], pass[160]; /* longer than any valid value so "too long" is a validation result, not truncation */
  bool have_url, have_token, insecure, force, have_v, have_dz;
  bool have_wifi, have_ssid, have_pass, wifi_overflow;
  double v;
} fields_t;

static bool parse_wifi(jp_t *j, fields_t *f) {
  ws(j);
  if (j->p >= j->end || *j->p != '{') return false;
  if (++j->depth > RU_CFG_JSON_DEPTH) return false;
  j->p++;
  f->have_wifi = true;
  ws(j);
  if (j->p < j->end && *j->p == '}') { j->p++; j->depth--; return true; }
  for (;;) {
    char key[24];
    ws(j);
    bool prev = j->overflow;
    j->overflow = false;
    if (!jstr(j, key, sizeof key)) return false;
    bool key_overflow = j->overflow;
    j->overflow = prev;
    ws(j);
    if (j->p >= j->end || *j->p++ != ':') return false;
    ws(j);
    if (!key_overflow && (!strcmp(key, "ssid") || !strcmp(key, "password"))) {
      bool is_ssid = key[0] == 's';
      char *dst = is_ssid ? f->ssid : f->pass;
      size_t cap = is_ssid ? sizeof f->ssid : sizeof f->pass;
      prev = j->overflow;
      j->overflow = false;
      j->nul = false;
      if (!jstr(j, dst, cap)) return false;
      if (j->overflow || j->nul) f->wifi_overflow = true; /* too long, or an embedded NUL: invalid either way */
      j->overflow = prev;
      if (is_ssid) f->have_ssid = true; else f->have_pass = true;
    } else if (!jskip(j)) return false;
    ws(j);
    if (j->p >= j->end) return false;
    if (*j->p == ',') { j->p++; continue; }
    if (*j->p == '}') { j->p++; j->depth--; return true; }
    return false;
  }
}

static bool parse_dz(jp_t *j, fields_t *f) {
  ws(j);
  if (j->p >= j->end || *j->p != '{') return false;
  if (++j->depth > RU_CFG_JSON_DEPTH) return false;
  j->p++;
  f->have_dz = true;
  ws(j);
  if (j->p < j->end && *j->p == '}') { j->p++; j->depth--; return true; }
  for (;;) {
    char key[24];
    ws(j);
    bool prev = j->overflow;
    j->overflow = false;
    if (!jstr(j, key, sizeof key)) return false;
    bool key_overflow = j->overflow; /* a long key is just an unknown key */
    j->overflow = prev;
    ws(j);
    if (j->p >= j->end || *j->p++ != ':') return false;
    ws(j);
    if (!key_overflow && !strcmp(key, "url")) {
      j->nul = false;
      /* an embedded NUL would silently cut the string short: a different address than the one sent. Refuse. */
      if (!jstr(j, f->url, sizeof f->url) || j->nul) return false;
      f->have_url = true;
    } else if (!key_overflow && !strcmp(key, "token")) {
      j->nul = false;
      /* same for the token: a truncated secret must never be stored as if it were the real one */
      if (!jstr(j, f->token, sizeof f->token) || j->nul) return false;
      f->have_token = true;
    } else if (!key_overflow && !strcmp(key, "insecure_lan")) {
      if (jlit(j, "true")) f->insecure = true;
      else if (jlit(j, "false")) f->insecure = false;
      else return false;
    } else if (!jskip(j)) return false;
    ws(j);
    if (j->p >= j->end) return false;
    if (*j->p == ',') { j->p++; continue; }
    if (*j->p == '}') { j->p++; j->depth--; return true; }
    return false;
  }
}

ru_cfg_err_t ru_cfg_parse_line(const char *json, size_t len, ru_cfg_line_t *out) {
  fields_t f;
  ru_cfg_err_t r = RU_CFG_BAD_JSON;
  if (out) memset(out, 0, sizeof *out);
  if (!json || !out) return RU_CFG_BAD_JSON;
  memset(&f, 0, sizeof f);
  jp_t j = {json, json + len, 0, false, false};
  ws(&j);
  if (j.p >= j.end || *j.p != '{') goto done;
  j.depth = 1;
  j.p++;
  ws(&j);
  if (j.p < j.end && *j.p == '}') { j.p++; goto trailing; }
  for (;;) {
    char key[24];
    ws(&j);
    bool prev = j.overflow;
    j.overflow = false;
    if (!jstr(&j, key, sizeof key)) goto done;
    bool key_overflow = j.overflow;
    j.overflow = prev;
    ws(&j);
    if (j.p >= j.end || *j.p++ != ':') goto done;
    ws(&j);
    if (!key_overflow && !strcmp(key, "v")) {
      double v;
      if (!jnum(&j, &v)) { /* wrong type */ ws(&j); goto done; }
      f.have_v = true;
      f.v = v;
    } else if (!key_overflow && !strcmp(key, "domoticz")) {
      if (!parse_dz(&j, &f)) goto done;
    } else if (!key_overflow && !strcmp(key, "wifi")) {
      if (!parse_wifi(&j, &f)) goto done;
    } else if (!key_overflow && !strcmp(key, "force")) {
      if (jlit(&j, "true")) f.force = true;
      else if (jlit(&j, "false")) f.force = false;
      else goto done;
    } else if (!jskip(&j)) goto done;
    ws(&j);
    if (j.p >= j.end) goto done;
    if (*j.p == ',') { j.p++; continue; }
    if (*j.p == '}') { j.p++; break; }
    goto done;
  }
trailing:
  ws(&j);
  if (j.p != j.end) goto done;
  if (!f.have_v) goto done;
  if (f.v != 1) { r = RU_CFG_BAD_VERSION; goto done; }
  if (!f.have_dz && !f.have_wifi) goto done;
  if (f.have_wifi) {
    if (f.wifi_overflow || !f.have_ssid || !f.have_pass || !ru_wifi_ssid_check(f.ssid) || !ru_wifi_pass_check(f.pass)) {
      r = RU_CFG_BAD_WIFI;
      goto done;
    }
    memcpy(out->wifi.ssid, f.ssid, strlen(f.ssid) + 1);
    memcpy(out->wifi.pass, f.pass, strlen(f.pass) + 1);
    out->have_wifi = true;
  }
  if (f.have_dz) {
    if (!f.have_url || !f.have_token) goto done;
    if (j.overflow) { r = RU_CFG_TOO_LONG; goto done; }
    bool https;
    ru_cfg_err_t e = ru_cfg_normalise_url(f.url, out->dz.url, sizeof out->dz.url, &https);
    if (e != RU_CFG_OK) { r = e; goto done; }
    if (!https && !f.insecure) { r = RU_CFG_BAD_URL; goto done; }
    e = ru_cfg_check_token(f.token, NULL);
    if (e != RU_CFG_OK) { r = e; goto done; }
    memcpy(out->dz.token, f.token, strlen(f.token) + 1);
    out->dz.insecure_lan = f.insecure;
    out->have_dz = true;
  }
  out->force = f.force;
  r = RU_CFG_OK;
done:
  ru_secure_zero(&f, sizeof f);
  if (r != RU_CFG_OK) ru_secure_zero(out, sizeof *out);
  return r;
}

void ru_cfg_line_wipe(ru_cfg_line_t *l) {
  if (l) ru_secure_zero(l, sizeof *l);
}

ru_cfg_err_t ru_cfg_parse_json(const char *json, size_t len, ru_cfg_t *out, bool *force) {
  ru_cfg_line_t l;
  if (out) memset(out, 0, sizeof *out);
  if (force) *force = false;
  if (!out) return RU_CFG_BAD_JSON;
  ru_cfg_err_t r = ru_cfg_parse_line(json, len, &l);
  if (r == RU_CFG_OK && !l.have_dz) r = RU_CFG_BAD_JSON;
  if (r == RU_CFG_OK) {
    *out = l.dz;
    if (force) *force = l.force;
  }
  ru_cfg_line_wipe(&l);
  return r;
}

/* ---------------- storage ---------------- */

#define K_URL "dz_url"
#define K_TOKEN "dz_token"
#define K_INSEC "dz_insecure"
#define K_VER "dz_ver"

ru_cfg_err_t ru_cfg_clear(ru_kv_t *kv) {
  const char *keys[] = {K_VER, K_TOKEN, K_URL, K_INSEC};
  ru_cfg_err_t r = RU_CFG_OK;
  for (size_t i = 0; i < 4; i++) {
    ru_kv_err_t e = ru_kv_erase_key(kv, RU_CFG_NS_SECRETS, keys[i]);
    if (e != RU_KV_OK && e != RU_KV_NOT_FOUND) r = RU_CFG_STORE;
  }
  return r;
}

ru_cfg_err_t ru_cfg_save(ru_kv_t *kv, const ru_cfg_t *c) {
  ru_cfg_err_t v = ru_cfg_validate(c);
  if (v != RU_CFG_OK) return v;
  (void)ru_kv_erase_key(kv, RU_CFG_NS_SECRETS, K_VER); /* invalidate first: a crash mid-write leaves "not set" */
  if (ru_kv_set_str(kv, RU_CFG_NS_SECRETS, K_URL, c->url) != RU_KV_OK ||
      ru_kv_set_str(kv, RU_CFG_NS_SECRETS, K_INSEC, c->insecure_lan ? "1" : "0") != RU_KV_OK ||
      ru_kv_set_str(kv, RU_CFG_NS_SECRETS, K_TOKEN, c->token) != RU_KV_OK ||
      ru_kv_set_str(kv, RU_CFG_NS_SECRETS, K_VER, "1") != RU_KV_OK) {
    (void)ru_cfg_clear(kv);
    return RU_CFG_STORE;
  }
  return RU_CFG_OK;
}

bool ru_cfg_is_set(ru_kv_t *kv) {
  char v[4];
  return ru_kv_get_str(kv, RU_CFG_NS_SECRETS, K_VER, v, sizeof v) == RU_KV_OK && v[0] == '1';
}

ru_cfg_err_t ru_cfg_load(ru_kv_t *kv, ru_cfg_t *out) {
  char v[4], ins[4];
  memset(out, 0, sizeof *out);
  if (!ru_cfg_is_set(kv)) return RU_CFG_NOT_SET;
  if (ru_kv_get_str(kv, RU_CFG_NS_SECRETS, K_URL, out->url, sizeof out->url) != RU_KV_OK ||
      ru_kv_get_str(kv, RU_CFG_NS_SECRETS, K_TOKEN, out->token, sizeof out->token) != RU_KV_OK ||
      ru_kv_get_str(kv, RU_CFG_NS_SECRETS, K_INSEC, ins, sizeof ins) != RU_KV_OK) {
    ru_cfg_wipe(out);
    return RU_CFG_NOT_SET;
  }
  (void)v;
  out->insecure_lan = ins[0] == '1';
  if (ru_cfg_validate(out) != RU_CFG_OK) { ru_cfg_wipe(out); return RU_CFG_NOT_SET; }
  return RU_CFG_OK;
}

void ru_cfg_token_masked(const ru_cfg_t *c, char *out, size_t cap) {
  if (!out || !cap) return;
  const char *s = (c && c->token[0]) ? "********" : "(none)"; /* fixed text: reveals neither content nor length */
  size_t n = strlen(s);
  if (n >= cap) n = cap - 1;
  memcpy(out, s, n);
  out[n] = 0;
}

void ru_cfg_wipe(ru_cfg_t *c) {
  if (c) ru_secure_zero(c, sizeof *c);
}

/* ---------------- Wi-Fi credentials ---------------- */

/* strict UTF-8 (no overlongs, no surrogates, <= U+10FFFF) that also rejects C0, DEL and C1 control characters */
bool ru_wifi_ssid_check(const char *ssid) {
  if (!ssid) return false;
  size_t n = strlen(ssid);
  if (n < 1 || n > 32) return false;
  const unsigned char *s = (const unsigned char *)ssid;
  for (size_t i = 0; i < n;) {
    unsigned c = s[i];
    if (c < 0x20 || c == 0x7F) return false;
    if (c < 0x80) { i++; continue; }
    size_t k;
    unsigned cp;
    if (c >= 0xC2 && c <= 0xDF) { k = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { k = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { k = 3; cp = c & 0x07; }
    else return false;
    if (i + k >= n) return false; /* truncated sequence */
    for (size_t m = 1; m <= k; m++) {
      if ((s[i + m] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (s[i + m] & 0x3F);
    }
    if ((k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    if (cp >= 0x80 && cp <= 0x9F) return false;
    i += k + 1;
  }
  return true;
}

bool ru_wifi_pass_check(const char *pass) {
  if (!pass) return false;
  size_t n = strlen(pass);
  if (n == 0) return true;
  if (n == 64) {
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)pass[i])) return false;
    return true;
  }
  if (n < 8 || n > 63) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)pass[i];
    if (c < 0x20 || c > 0x7E) return false;
  }
  return true;
}

void ru_wifi_pass_masked(const ru_wifi_creds_t *c, char *out, size_t cap) {
  if (!out || !cap) return;
  const char *s = (c && c->pass[0]) ? "********" : "(none)";
  size_t n = strlen(s);
  if (n >= cap) n = cap - 1;
  memcpy(out, s, n);
  out[n] = 0;
}

bool ru_wifi_creds_valid(const char *ssid, const char *pass) {
  if (!ssid || !pass) return false;
  size_t sl = strlen(ssid), pl = strlen(pass);
  if (sl < 1 || sl > 32) return false;
  if (pl != 0 && (pl < 8 || pl > 64)) return false;
  return true;
}

ru_cfg_err_t ru_wifi_creds_save(ru_kv_t *kv, const char *ssid, const char *pass) {
  if (!ru_wifi_creds_valid(ssid, pass)) return RU_CFG_BAD_JSON;
  if (ru_kv_set_str(kv, RU_CFG_NS_SECRETS, "wifi_ssid", ssid) != RU_KV_OK ||
      ru_kv_set_str(kv, RU_CFG_NS_SECRETS, "wifi_pass", pass) != RU_KV_OK) {
    (void)ru_wifi_creds_clear(kv);
    return RU_CFG_STORE;
  }
  return RU_CFG_OK;
}
ru_cfg_err_t ru_wifi_creds_load(ru_kv_t *kv, ru_wifi_creds_t *out) {
  memset(out, 0, sizeof *out);
  if (ru_kv_get_str(kv, RU_CFG_NS_SECRETS, "wifi_ssid", out->ssid, sizeof out->ssid) != RU_KV_OK ||
      ru_kv_get_str(kv, RU_CFG_NS_SECRETS, "wifi_pass", out->pass, sizeof out->pass) != RU_KV_OK ||
      !ru_wifi_creds_valid(out->ssid, out->pass)) {
    ru_wifi_creds_wipe(out);
    return RU_CFG_NOT_SET;
  }
  return RU_CFG_OK;
}
ru_cfg_err_t ru_wifi_creds_clear(ru_kv_t *kv) {
  ru_kv_err_t a = ru_kv_erase_key(kv, RU_CFG_NS_SECRETS, "wifi_ssid");
  ru_kv_err_t b = ru_kv_erase_key(kv, RU_CFG_NS_SECRETS, "wifi_pass");
  return ((a == RU_KV_OK || a == RU_KV_NOT_FOUND) && (b == RU_KV_OK || b == RU_KV_NOT_FOUND)) ? RU_CFG_OK : RU_CFG_STORE;
}
void ru_wifi_creds_wipe(ru_wifi_creds_t *c) {
  if (c) ru_secure_zero(c, sizeof *c);
}
