#pragma once
#include "sdkconfig.h"
/* Starts the periodic heap/stack log line and the failed-allocation hook when CONFIG_RU_HEAP_LOG=y; otherwise a no-op. */
void ru_heap_log_start(void);
