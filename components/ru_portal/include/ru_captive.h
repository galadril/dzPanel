/* ru_captive: pure helpers for the setup portal's SoftAP: DNS answers, captive-portal probe paths, the Wi-Fi QR payload,
 * the random per-session password. No IDF, no allocation. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fills buf with n random bytes (the glue uses esp_fill_random; tests use a fixed generator). false = failure. */
typedef bool (*ru_rand_fn)(void *user, uint8_t *buf, size_t n);

/* ---- DNS: answer every A query with the AP address ----
 * q/qlen is one UDP datagram. Accepts a standard query (QR=0, opcode 0) with exactly one question, no compression in the
 * question, labels <= 63, name <= 255, class IN; at most one additional record (EDNS) which is dropped. A and ANY... only
 * QTYPE A (1) gets an answer record (TTL 30 s, pointer to the question name); every other type (AAAA, HTTPS, ...) gets a
 * NOERROR reply with no answer so clients do not wait. Anything malformed, any response packet, any other opcode is
 * dropped. Returns the reply length (>= 12), or 0 = send nothing. Never writes beyond cap; replies are <= 512 bytes. */
size_t ru_dns_answer(const uint8_t *q, size_t qlen, const uint8_t ip[4], uint8_t *out, size_t cap);

/* ---- probe paths ---- */
typedef enum {
  RU_PROBE_ROOT = 0,  /* "/" */
  RU_PROBE_ANDROID,   /* /generate_204, /gen_204 */
  RU_PROBE_APPLE,     /* /hotspot-detect.html, /library/test/success.html */
  RU_PROBE_WINDOWS,   /* /ncsi.txt, /connecttest.txt, /redirect */
  RU_PROBE_OTHER      /* anything else */
} ru_probe_t;
ru_probe_t ru_captive_classify(const char *path);
/* "http://<ap_ip>/" into out. false when it does not fit or ap_ip is not a plain dotted quad. */
bool ru_portal_url(const char *ap_ip, char *out, size_t cap);

/* ---- QR payloads ---- */
/* "WIFI:T:WPA;S:<ssid>;P:<password>;;" with \ ; , : " escaped by a backslash. A value made only of hex digits is wrapped
 * in double quotes (the format's rule, so scanners do not read it as hex). Empty password: "WIFI:T:nopass;S:<ssid>;;".
 * false when the ssid is empty or over 32 bytes, the password is over 63 bytes, a control character is present, or the
 * result does not fit. out is zeroed on failure. The result contains the password: never log it. */
bool ru_qr_wifi_payload(const char *ssid, const char *password, char *out, size_t cap);

/* ---- random per-session credentials ---- */
#define RU_PW_ALPHABET "23456789ABCDEFGHJKLMNPQRSTUVWXYZ" /* 32 symbols, no 0 O 1 I L */
#define RU_PW_MIN 12
#define RU_PW_MAX 32
/* len chars (RU_PW_MIN..RU_PW_MAX) from the alphabet, uniformly (32 symbols so 5 bits per byte, no modulo bias).
 * out needs len+1 bytes. false on a bad length or a random failure (out zeroed). */
bool ru_gen_password(ru_rand_fn rnd, void *user, unsigned len, char *out, size_t cap);
/* "RoundOS-XXXX" (4 alphabet chars) for the AP name. cap >= 13. */
bool ru_gen_ap_ssid(ru_rand_fn rnd, void *user, char *out, size_t cap);
/* Lowercase hex of n random bytes (CSRF token). out needs 2n+1. */
bool ru_gen_hex(ru_rand_fn rnd, void *user, unsigned n, char *out, size_t cap);

/* Constant-time equality of two C strings (length leaks, content does not). */
bool ru_ct_streq(const char *a, const char *b);
