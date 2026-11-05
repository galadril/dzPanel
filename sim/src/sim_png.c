/* Minimal PNG writer: 8-bit RGB, filter 0, deflate "stored" blocks. No zlib. */
#include "sim_core.h"
#include <stdio.h>
#include <stdlib.h>

static uint32_t crc_table[256];

static void crc_init(void) {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static bool chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
  uint8_t hdr[8];
  put32(hdr, len);
  for (int i = 0; i < 4; i++) hdr[4 + i] = (uint8_t)type[i];
  uint32_t crc = crc_update(0xFFFFFFFFu, hdr + 4, 4);
  if (len) crc = crc_update(crc, data, len);
  crc ^= 0xFFFFFFFFu;
  uint8_t tail[4];
  put32(tail, crc);
  if (fwrite(hdr, 1, 8, f) != 8) return false;
  if (len && fwrite(data, 1, len, f) != len) return false;
  return fwrite(tail, 1, 4, f) == 4;
}

bool sim_save_png(const sim_t *s, const char *path, bool masked) {
  crc_init();
  int w = s->size, h = s->size;
  size_t rowb = (size_t)w * 3 + 1, raw_n = rowb * (size_t)h;
  uint8_t *raw = malloc(raw_n);
  if (!raw) return false;
  for (int y = 0; y < h; y++) {
    uint8_t *r = raw + (size_t)y * rowb;
    *r++ = 0;
    for (int x = 0; x < w; x++) {
      uint32_t px = s->fb[y * w + x];
      if (masked && !s->square && sim_outside_circle(w, x, y)) px = 0;
      *r++ = (uint8_t)(px >> 16); *r++ = (uint8_t)(px >> 8); *r++ = (uint8_t)px;
    }
  }
  /* zlib stream: header, stored blocks, adler32 */
  size_t nblk = (raw_n + 65534) / 65535;
  size_t z_n = 2 + raw_n + nblk * 5 + 4;
  uint8_t *z = malloc(z_n);
  if (!z) { free(raw); return false; }
  size_t o = 0;
  z[o++] = 0x78; z[o++] = 0x01;
  uint32_t a = 1, b = 0;
  for (size_t off = 0; off < raw_n; off += 65535) {
    size_t n = raw_n - off > 65535 ? 65535 : raw_n - off;
    z[o++] = (off + n >= raw_n) ? 1 : 0;
    z[o++] = (uint8_t)(n & 0xFF); z[o++] = (uint8_t)(n >> 8);
    z[o++] = (uint8_t)(~n & 0xFF); z[o++] = (uint8_t)((~n >> 8) & 0xFF);
    for (size_t i = 0; i < n; i++) { a = (a + raw[off + i]) % 65521u; b = (b + a) % 65521u; z[o++] = raw[off + i]; }
  }
  put32(z + o, (b << 16) | a);
  o += 4;
  FILE *f = fopen(path, "wb");
  bool ok = f != NULL;
  if (ok) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)w); put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) && chunk(f, "IDAT", z, (uint32_t)o) && chunk(f, "IEND", NULL, 0);
    if (fclose(f) != 0) ok = false;
  }
  free(z);
  free(raw);
  return ok;
}
