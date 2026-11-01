/* Dev tool: parse a Domoticz response file and print one line per device (no names, for safe sharing).
 * usage: dz_dump file.json */
#include <stdio.h>
#include <stdlib.h>
#include "dz_parse.h"
static bool pr(const dz_device_t *d, void *u) {
  (void)u;
  printf("%-10s type=%-24s sub=%-22s stv=%-3d prot=%d fav=%d ctl=%d\n", dz_cap_name(d->cap), d->type, d->subtype,
         d->switch_type, d->is_protected, d->favorite, d->controllable);
  return true;
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  FILE *f = fopen(argv[1], "rb");
  if (!f) return 2;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1);
  if (fread(b, 1, (size_t)n, f) != (size_t)n) return 2;
  dz_result_t r;
  dz_err_t e = dz_parse_response(b, (size_t)n, pr, NULL, &r);
  free(b);
  printf("err=%d devices=%d rejected=%d status=%s\n", e, r.devices, r.rejected, r.status);
  return e;
}
