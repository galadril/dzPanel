/* Minimal test harness (placeholder until Unity is vendored). */
#pragma once
#include <stdio.h>
#include <math.h>
static int mt_fail, mt_run;
#define CHECK(c) do { mt_run++; if (!(c)) { mt_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))
#define MT_DONE() do { printf("%d checks, %d failed\n", mt_run, mt_fail); return mt_fail ? 1 : 0; } while (0)
