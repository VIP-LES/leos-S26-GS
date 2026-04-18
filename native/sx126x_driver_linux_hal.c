#include "sx126x_driver_linux_hal.h"

#include <errno.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) ( sizeof( x ) / sizeof( ( x )[0] ) )
#endif

static sx126x_hal_status_t sx126x_linux_sleep_us( uint32_t us )
{
    struct timespec ts;

    ts.tv_sec  = us / 1000000U;
    ts.tv_nsec = (long) ( us % 1000000U ) * 1000L;

    return ( nanosleep( &ts, NULL ) == 0 ) ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

static uint64_t sx126x_linux_monotonic_ms( void )
{
    struct timespec ts;

    if( clock_gettime( CLOCK_MONOTONIC, &ts ) != 0 )
    {
        return 0;
    }

    return ( (uint64_t) ts.tv_sec * 1000ULL ) + ( (uint64_t) ts.tv_nsec / 1000000ULL );
}

sx126x_hal_status_t sx126x_linux_init( sx126x_linux_context_t* ctx )
{
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;

    if( ( ctx == NULL ) || ( ctx->spi_fd < 0 ) || ( ctx->busy_req == NULL ) || ( ctx->reset_req == NULL ) )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    if( ctx->spi_speed_hz == 0 )
    {
        ctx->spi_speed_hz = 8000000U;
    }
    if( ctx->busy_timeout_ms == 0 )
    {
        ctx->busy_timeout_ms = 1000U;
    }
    if( ctx->reset_pulse_ms == 0 )
    {
        ctx->reset_pulse_ms = 10U;
    }
    if( ctx->reset_wait_ms == 0 )
    {
        ctx->reset_wait_ms = 10U;
    }

    if( ioctl( ctx->spi_fd, SPI_IOC_WR_MODE, &mode ) < 0 )
    {
        return SX126X_HAL_STATUS_ERROR;
    }
    if( ioctl( ctx->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits ) < 0 )
    {
        return SX126X_HAL_STATUS_ERROR;
    }
    if( ioctl( ctx->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &ctx->spi_speed_hz ) < 0 )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    return SX126X_HAL_STATUS_OK;
}

void sx126x_linux_deinit( sx126x_linux_context_t* ctx )
{
    (void) ctx;
}

sx126x_hal_status_t sx126x_linux_wait_while_busy( const sx126x_linux_context_t* ctx )
{
    const uint64_t start_ms = sx126x_linux_monotonic_ms( );

    if( ( ctx == NULL ) || ( ctx->busy_req == NULL ) )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    while( 1 )
    {
        const int value = gpiod_line_request_get_value( ctx->busy_req, ctx->busy_offset );
        if( value < 0 )
        {
            return SX126X_HAL_STATUS_ERROR;
        }
        if( value == 0 )
        {
            return SX126X_HAL_STATUS_OK;
        }
        if( ( sx126x_linux_monotonic_ms( ) - start_ms ) > ctx->busy_timeout_ms )
        {
            return SX126X_HAL_STATUS_ERROR;
        }
        if( sx126x_linux_sleep_us( 100 ) != SX126X_HAL_STATUS_OK )
        {
            return SX126X_HAL_STATUS_ERROR;
        }
    }
}

static sx126x_hal_status_t sx126x_linux_spi_transfer( const sx126x_linux_context_t* ctx, const uint8_t* tx_data,
                                                      uint8_t* rx_data, uint16_t len )
{
    struct spi_ioc_transfer tr;

    if( ( ctx == NULL ) || ( tx_data == NULL ) || ( len == 0 ) )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    memset( &tr, 0, sizeof( tr ) );
    tr.tx_buf        = (unsigned long) tx_data;
    tr.rx_buf        = (unsigned long) rx_data;
    tr.len           = len;
    tr.speed_hz      = ctx->spi_speed_hz;
    tr.bits_per_word = 8;
    tr.delay_usecs   = 0;
    tr.cs_change     = 0;

    if( ioctl( ctx->spi_fd, SPI_IOC_MESSAGE( 1 ), &tr ) < 1 )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_write( const void* context, const uint8_t* command, const uint16_t command_length,
                                      const uint8_t* data, const uint16_t data_length )
{
    const sx126x_linux_context_t* ctx = (const sx126x_linux_context_t*) context;
    uint8_t buffer[512];
    const size_t total_len = (size_t) command_length + (size_t) data_length;

    if( ( ctx == NULL ) || ( command == NULL ) || ( command_length == 0 ) || ( total_len > sizeof( buffer ) ) )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    if( sx126x_linux_wait_while_busy( ctx ) != SX126X_HAL_STATUS_OK )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    memcpy( buffer, command, command_length );
    if( ( data != NULL ) && ( data_length > 0 ) )
    {
        memcpy( buffer + command_length, data, data_length );
    }

    if( sx126x_linux_spi_transfer( ctx, buffer, NULL, (uint16_t) total_len ) != SX126X_HAL_STATUS_OK )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    return sx126x_linux_wait_while_busy( ctx );
}

sx126x_hal_status_t sx126x_hal_read( const void* context, const uint8_t* command, const uint16_t command_length,
                                     uint8_t* data, const uint16_t data_length )
{
    const sx126x_linux_context_t* ctx = (const sx126x_linux_context_t*) context;
    uint8_t tx_buffer[512];
    uint8_t rx_buffer[512];
    const size_t total_len = (size_t) command_length + (size_t) data_length;

    if( ( ctx == NULL ) || ( command == NULL ) || ( command_length == 0 ) || ( data == NULL ) || ( data_length == 0 ) ||
        ( total_len > sizeof( tx_buffer ) ) )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    if( sx126x_linux_wait_while_busy( ctx ) != SX126X_HAL_STATUS_OK )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    memcpy( tx_buffer, command, command_length );
    memset( tx_buffer + command_length, SX126X_NOP, data_length );
    memset( rx_buffer, 0, total_len );

    if( sx126x_linux_spi_transfer( ctx, tx_buffer, rx_buffer, (uint16_t) total_len ) != SX126X_HAL_STATUS_OK )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    memcpy( data, rx_buffer + command_length, data_length );

    return sx126x_linux_wait_while_busy( ctx );
}

sx126x_hal_status_t sx126x_hal_reset( const void* context )
{
    const sx126x_linux_context_t* ctx = (const sx126x_linux_context_t*) context;

    if( ( ctx == NULL ) || ( ctx->reset_req == NULL ) )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    if( gpiod_line_request_set_value( ctx->reset_req, ctx->reset_offset, 0 ) < 0 )
    {
        return SX126X_HAL_STATUS_ERROR;
    }
    if( sx126x_linux_sleep_us( ctx->reset_pulse_ms * 1000U ) != SX126X_HAL_STATUS_OK )
    {
        return SX126X_HAL_STATUS_ERROR;
    }
    if( gpiod_line_request_set_value( ctx->reset_req, ctx->reset_offset, 1 ) < 0 )
    {
        return SX126X_HAL_STATUS_ERROR;
    }
    if( sx126x_linux_sleep_us( ctx->reset_wait_ms * 1000U ) != SX126X_HAL_STATUS_OK )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    return sx126x_linux_wait_while_busy( ctx );
}

sx126x_hal_status_t sx126x_hal_wakeup( const void* context )
{
    const sx126x_linux_context_t* ctx = (const sx126x_linux_context_t*) context;
    const uint8_t command[]           = { 0xC0, 0x00 };

    if( ( ctx == NULL ) || ( ctx->spi_fd < 0 ) )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    if( sx126x_linux_spi_transfer( ctx, command, NULL, (uint16_t) ARRAY_SIZE( command ) ) != SX126X_HAL_STATUS_OK )
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    return sx126x_linux_wait_while_busy( ctx );
}
