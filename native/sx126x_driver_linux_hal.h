#ifndef SX126X_DRIVER_LINUX_HAL_H
#define SX126X_DRIVER_LINUX_HAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include <gpiod.h>

#include "sx126x_hal.h"

typedef struct sx126x_linux_context_s
{
    int spi_fd;
    uint32_t spi_speed_hz;

    struct gpiod_line_request* busy_req;
    unsigned int busy_offset;

    struct gpiod_line_request* reset_req;
    unsigned int reset_offset;

    uint32_t busy_timeout_ms;
    uint32_t reset_pulse_ms;
    uint32_t reset_wait_ms;
} sx126x_linux_context_t;

sx126x_hal_status_t sx126x_linux_wait_while_busy( const sx126x_linux_context_t* ctx );
sx126x_hal_status_t sx126x_linux_init( sx126x_linux_context_t* ctx );
void sx126x_linux_deinit( sx126x_linux_context_t* ctx );

#ifdef __cplusplus
}
#endif

#endif
