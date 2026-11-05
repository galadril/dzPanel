/* Guition-specific extras beyond ru_hal_port.h, for the bring-up probe only. Valid after ru_port_start(). */
#pragma once
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The touch I2C bus (kept alive even if the GT911 was not found), or NULL before ru_port_start(). */
i2c_master_bus_handle_t ru_guition_touch_bus(void);
/* The GT911 handle, or NULL if touch init failed. */
esp_lcd_touch_handle_t ru_guition_touch(void);
/* 7-bit address the GT911 answered on (0x5D or 0x14), 0 if none. */
uint8_t ru_guition_touch_addr(void);

#ifdef __cplusplus
}
#endif
