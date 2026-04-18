#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "sx126x.h"
#include "sx126x_driver_linux_hal.h"

#define IPC_MSG_RX_PACKET 0x01u
#define IPC_MSG_BUTTON_EVENT 0x02u
#define IPC_MSG_TX_PACKET 0x03u
#define IPC_MSG_TX_RESULT 0x04u

#define IPC_TX_STATUS_OK 0x00u
#define IPC_TX_STATUS_RADIO_ERROR 0x02u

#define RADIO_ID_SX1262 0u
#define RADIO_ID_SX1268 1u

#define DEFAULT_SOCKET_PATH "/tmp/leos-radio.sock"
#define DEFAULT_GPIO_CHIP "/dev/gpiochip0"
#define DEFAULT_RESET_LINE 25u
#define DEFAULT_SPI_BAUD_HZ 8000000u
#define DEFAULT_RX_POLL_MS 5

#define DEFAULT_SX1262_SPI_DEVICE "/dev/spidev0.0"
#define DEFAULT_SX1268_SPI_DEVICE "/dev/spidev0.1"
#define DEFAULT_SX1262_BUSY_LINE 19u
#define DEFAULT_SX1268_BUSY_LINE 27u

#define DEFAULT_SX1262_RF_FREQUENCY_HZ 918250000u
#define DEFAULT_SX1262_TX_POWER_DBM 14
#define DEFAULT_SX1262_TX_TIMEOUT_MS 1000u

#define DEFAULT_SX1268_RF_FREQUENCY_HZ 435000000u
#define DEFAULT_SX1268_TX_POWER_DBM 14
#define DEFAULT_SX1268_TX_TIMEOUT_MS 1000u

#define DEFAULT_BUSY_TIMEOUT_MS 1000u
#define DEFAULT_RESET_PULSE_MS 10u
#define DEFAULT_RESET_WAIT_MS 10u
#define DEFAULT_TCXO_DELAY_RTC_STEPS 640u
#define DEFAULT_OCP_SETTING 0x38u

typedef enum
{
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_TRACE,
} log_level_t;

typedef struct
{
    const char* socket_path;
    const char* gpio_chip_path;
    uint32_t spi_baud_hz;
    unsigned int reset_line;
    bool sx1262_enabled;
    bool sx1268_enabled;
} app_config_t;

typedef struct
{
    uint8_t radio_id;
    const char* name;
    bool enabled;
    const char* spi_device;
    unsigned int busy_line;
    uint32_t rf_frequency_hz;
    int8_t tx_power_dbm;
    uint32_t tx_timeout_ms;
    sx126x_mod_params_lora_t mod_params;
    sx126x_pkt_params_lora_t rx_pkt_params;
    int spi_fd;
    struct gpiod_line_request* busy_req;
    sx126x_linux_context_t hal;
    bool tx_in_flight;
    bool rx_active;
} radio_instance_t;

typedef struct
{
    app_config_t config;
    int server_fd;
    int client_fd;
    struct gpiod_chip* gpio_chip;
    struct gpiod_line_request* reset_req;
    radio_instance_t sx1262;
    radio_instance_t sx1268;
} app_state_t;

static const char* log_level_name( log_level_t level )
{
    switch( level )
    {
    case LOG_LEVEL_INFO:
        return "INFO";
    case LOG_LEVEL_WARN:
        return "WARN";
    case LOG_LEVEL_ERROR:
        return "ERROR";
    case LOG_LEVEL_TRACE:
        return "TRACE";
    default:
        return "UNKNOWN";
    }
}

static void log_message( log_level_t level, const char* fmt, ... )
{
    va_list args;

    fprintf( stderr, "new_radio_receiver [%s]: ", log_level_name( level ) );
    va_start( args, fmt );
    vfprintf( stderr, fmt, args );
    va_end( args );
    fputc( '\n', stderr );
}

static void log_errno_message( const char* context )
{
    log_message( LOG_LEVEL_ERROR, "%s: %s (errno=%d)", context, strerror( errno ), errno );
}

static const char* env_str( const char* name, const char* fallback )
{
    const char* value = getenv( name );
    return ( ( value == NULL ) || ( *value == '\0' ) ) ? fallback : value;
}

static uint32_t env_u32( const char* name, uint32_t fallback )
{
    const char* value = getenv( name );
    char* endptr      = NULL;
    unsigned long out;

    if( ( value == NULL ) || ( *value == '\0' ) )
    {
        return fallback;
    }

    out = strtoul( value, &endptr, 10 );
    if( ( endptr == value ) || ( *endptr != '\0' ) )
    {
        return fallback;
    }

    return (uint32_t) out;
}

static void load_config( app_state_t* state )
{
    const char* radio_enabled = env_str( "RADIO_ENABLED", "both" );

    state->config.socket_path    = env_str( "RADIO_SOCKET_PATH", DEFAULT_SOCKET_PATH );
    state->config.gpio_chip_path = env_str( "RADIO_GPIO_CHIP_PATH", DEFAULT_GPIO_CHIP );
    state->config.spi_baud_hz    = env_u32( "RADIO_SPI_BAUD_HZ", DEFAULT_SPI_BAUD_HZ );
    state->config.reset_line     = env_u32( "RADIO_RESET_LINE", DEFAULT_RESET_LINE );
    state->config.sx1262_enabled = true;
    state->config.sx1268_enabled = true;

    if( strcmp( radio_enabled, "sx1262" ) == 0 )
    {
        state->config.sx1268_enabled = false;
    }
    else if( strcmp( radio_enabled, "sx1268" ) == 0 )
    {
        state->config.sx1262_enabled = false;
    }
    else if( strcmp( radio_enabled, "both" ) != 0 )
    {
        log_message( LOG_LEVEL_WARN, "unsupported RADIO_ENABLED=%s, defaulting to both", radio_enabled );
    }

    state->sx1262.radio_id         = RADIO_ID_SX1262;
    state->sx1262.name             = "sx1262";
    state->sx1262.enabled          = state->config.sx1262_enabled;
    state->sx1262.spi_device       = env_str( "RADIO_SX1262_SPI_DEVICE", DEFAULT_SX1262_SPI_DEVICE );
    state->sx1262.busy_line        = env_u32( "RADIO_SX1262_BUSY_LINE", DEFAULT_SX1262_BUSY_LINE );
    state->sx1262.rf_frequency_hz  = env_u32( "RADIO_SX1262_RF_FREQUENCY_HZ", DEFAULT_SX1262_RF_FREQUENCY_HZ );
    state->sx1262.tx_power_dbm     = (int8_t) env_u32( "RADIO_SX1262_TX_POWER_DBM", DEFAULT_SX1262_TX_POWER_DBM );
    state->sx1262.tx_timeout_ms    = env_u32( "RADIO_SX1262_TX_TIMEOUT_MS", DEFAULT_SX1262_TX_TIMEOUT_MS );
    state->sx1262.mod_params.sf    = SX126X_LORA_SF9;
    state->sx1262.mod_params.bw    = SX126X_LORA_BW_250;
    state->sx1262.mod_params.cr    = SX126X_LORA_CR_4_5;
    state->sx1262.mod_params.ldro  = 0;
    state->sx1262.rx_pkt_params.preamble_len_in_symb = 12;
    state->sx1262.rx_pkt_params.header_type = SX126X_LORA_PKT_EXPLICIT;
    state->sx1262.rx_pkt_params.pld_len_in_bytes = 255;
    state->sx1262.rx_pkt_params.crc_is_on = true;
    state->sx1262.rx_pkt_params.invert_iq_is_on = false;
    state->sx1262.spi_fd = -1;

    state->sx1268.radio_id         = RADIO_ID_SX1268;
    state->sx1268.name             = "sx1268";
    state->sx1268.enabled          = state->config.sx1268_enabled;
    state->sx1268.spi_device       = env_str( "RADIO_SX1268_SPI_DEVICE", DEFAULT_SX1268_SPI_DEVICE );
    state->sx1268.busy_line        = env_u32( "RADIO_SX1268_BUSY_LINE", DEFAULT_SX1268_BUSY_LINE );
    state->sx1268.rf_frequency_hz  = env_u32( "RADIO_SX1268_RF_FREQUENCY_HZ", DEFAULT_SX1268_RF_FREQUENCY_HZ );
    state->sx1268.tx_power_dbm     = (int8_t) env_u32( "RADIO_SX1268_TX_POWER_DBM", DEFAULT_SX1268_TX_POWER_DBM );
    state->sx1268.tx_timeout_ms    = env_u32( "RADIO_SX1268_TX_TIMEOUT_MS", DEFAULT_SX1268_TX_TIMEOUT_MS );
    state->sx1268.mod_params.sf    = SX126X_LORA_SF7;
    state->sx1268.mod_params.bw    = SX126X_LORA_BW_500;
    state->sx1268.mod_params.cr    = SX126X_LORA_CR_4_5;
    state->sx1268.mod_params.ldro  = 0;
    state->sx1268.rx_pkt_params.preamble_len_in_symb = 12;
    state->sx1268.rx_pkt_params.header_type = SX126X_LORA_PKT_EXPLICIT;
    state->sx1268.rx_pkt_params.pld_len_in_bytes = 255;
    state->sx1268.rx_pkt_params.crc_is_on = true;
    state->sx1268.rx_pkt_params.invert_iq_is_on = false;
    state->sx1268.spi_fd = -1;
}

static int open_server_socket( const char* path )
{
    int fd;
    struct sockaddr_un addr;

    fd = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0 );
    if( fd < 0 )
    {
        return -1;
    }

    memset( &addr, 0, sizeof( addr ) );
    addr.sun_family = AF_UNIX;
    strncpy( addr.sun_path, path, sizeof( addr.sun_path ) - 1 );

    unlink( path );
    if( bind( fd, (const struct sockaddr*) &addr, sizeof( addr ) ) < 0 )
    {
        close( fd );
        return -1;
    }
    if( listen( fd, 1 ) < 0 )
    {
        close( fd );
        unlink( path );
        return -1;
    }

    return fd;
}

static int send_ipc_message( app_state_t* state, const uint8_t* buf, size_t len )
{
    ssize_t sent;

    if( state->client_fd < 0 )
    {
        return 0;
    }

    sent = send( state->client_fd, buf, len, MSG_NOSIGNAL );
    if( sent < 0 )
    {
        close( state->client_fd );
        state->client_fd = -1;
        return -1;
    }

    return 0;
}

static int send_tx_result( app_state_t* state, uint8_t radio_id, uint8_t status )
{
    const uint8_t message[] = { IPC_MSG_TX_RESULT, radio_id, status };
    return send_ipc_message( state, message, sizeof( message ) );
}

static int request_input_line( struct gpiod_chip* chip, unsigned int offset, const char* consumer,
                               struct gpiod_line_request** out_req )
{
    struct gpiod_line_settings* settings = NULL;
    struct gpiod_line_config* config     = NULL;
    struct gpiod_request_config* req_cfg = NULL;
    int rc                               = -1;

    settings = gpiod_line_settings_new( );
    config   = gpiod_line_config_new( );
    req_cfg  = gpiod_request_config_new( );
    if( ( settings == NULL ) || ( config == NULL ) || ( req_cfg == NULL ) )
    {
        goto cleanup;
    }

    gpiod_request_config_set_consumer( req_cfg, consumer );
    if( gpiod_line_settings_set_direction( settings, GPIOD_LINE_DIRECTION_INPUT ) != 0 )
    {
        goto cleanup;
    }
    if( gpiod_line_config_add_line_settings( config, &offset, 1, settings ) != 0 )
    {
        goto cleanup;
    }

    *out_req = gpiod_chip_request_lines( chip, req_cfg, config );
    rc       = ( *out_req != NULL ) ? 0 : -1;

cleanup:
    gpiod_line_settings_free( settings );
    gpiod_line_config_free( config );
    gpiod_request_config_free( req_cfg );
    return rc;
}

static int request_output_line( struct gpiod_chip* chip, unsigned int offset, const char* consumer,
                                struct gpiod_line_request** out_req )
{
    struct gpiod_line_settings* settings = NULL;
    struct gpiod_line_config* config     = NULL;
    struct gpiod_request_config* req_cfg = NULL;
    int rc                               = -1;

    settings = gpiod_line_settings_new( );
    config   = gpiod_line_config_new( );
    req_cfg  = gpiod_request_config_new( );
    if( ( settings == NULL ) || ( config == NULL ) || ( req_cfg == NULL ) )
    {
        goto cleanup;
    }

    gpiod_request_config_set_consumer( req_cfg, consumer );
    if( gpiod_line_settings_set_direction( settings, GPIOD_LINE_DIRECTION_OUTPUT ) != 0 )
    {
        goto cleanup;
    }
    if( gpiod_line_settings_set_output_value( settings, GPIOD_LINE_VALUE_ACTIVE ) != 0 )
    {
        goto cleanup;
    }
    if( gpiod_line_config_add_line_settings( config, &offset, 1, settings ) != 0 )
    {
        goto cleanup;
    }

    *out_req = gpiod_chip_request_lines( chip, req_cfg, config );
    rc       = ( *out_req != NULL ) ? 0 : -1;

cleanup:
    gpiod_line_settings_free( settings );
    gpiod_line_config_free( config );
    gpiod_request_config_free( req_cfg );
    return rc;
}

static int radio_open( app_state_t* state, radio_instance_t* radio )
{
    if( !radio->enabled )
    {
        return 0;
    }

    radio->spi_fd = open( radio->spi_device, O_RDWR | O_CLOEXEC );
    if( radio->spi_fd < 0 )
    {
        log_errno_message( "open spi device failed" );
        return -1;
    }

    if( request_input_line( state->gpio_chip, radio->busy_line, radio->name, &radio->busy_req ) != 0 )
    {
        log_errno_message( "request busy line failed" );
        return -1;
    }

    radio->hal.spi_fd           = radio->spi_fd;
    radio->hal.spi_speed_hz     = state->config.spi_baud_hz;
    radio->hal.busy_req         = radio->busy_req;
    radio->hal.busy_offset      = radio->busy_line;
    radio->hal.reset_req        = state->reset_req;
    radio->hal.reset_offset     = state->config.reset_line;
    radio->hal.busy_timeout_ms  = DEFAULT_BUSY_TIMEOUT_MS;
    radio->hal.reset_pulse_ms   = DEFAULT_RESET_PULSE_MS;
    radio->hal.reset_wait_ms    = DEFAULT_RESET_WAIT_MS;
    radio->tx_in_flight         = false;
    radio->rx_active            = false;

    if( sx126x_linux_init( &radio->hal ) != SX126X_HAL_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s sx126x_linux_init failed", radio->name );
        return -1;
    }

    return 0;
}

static int radio_configure( radio_instance_t* radio )
{
    const sx126x_pa_cfg_params_t pa_cfg = {
        .pa_duty_cycle = 0x04,
        .hp_max        = 0x07,
        .device_sel    = 0x00,
        .pa_lut        = 0x01,
    };

    if( !radio->enabled )
    {
        return 0;
    }

    if( sx126x_hal_reset( &radio->hal ) != SX126X_HAL_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s reset failed", radio->name );
        return -1;
    }
    if( sx126x_hal_wakeup( &radio->hal ) != SX126X_HAL_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s wakeup failed", radio->name );
        return -1;
    }
    if( sx126x_set_standby( &radio->hal, SX126X_STANDBY_CFG_RC ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s standby RC failed", radio->name );
        return -1;
    }
    if( sx126x_set_pkt_type( &radio->hal, SX126X_PKT_TYPE_LORA ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set packet type failed", radio->name );
        return -1;
    }
    if( sx126x_set_dio3_as_tcxo_ctrl( &radio->hal, SX126X_TCXO_CTRL_2_2V, DEFAULT_TCXO_DELAY_RTC_STEPS ) !=
        SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set TCXO failed", radio->name );
        return -1;
    }
    if( sx126x_clear_device_errors( &radio->hal ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_WARN, "%s clear device errors failed", radio->name );
    }
    if( sx126x_set_standby( &radio->hal, SX126X_STANDBY_CFG_XOSC ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s standby XOSC failed", radio->name );
        return -1;
    }
    if( sx126x_stop_timer_on_preamble( &radio->hal, false ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s stop timer on preamble failed", radio->name );
        return -1;
    }
    if( sx126x_set_reg_mode( &radio->hal, SX126X_REG_MODE_DCDC ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set regulator mode failed", radio->name );
        return -1;
    }
    if( sx126x_set_pa_cfg( &radio->hal, &pa_cfg ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set pa cfg failed", radio->name );
        return -1;
    }
    if( sx126x_set_rx_tx_fallback_mode( &radio->hal, SX126X_FALLBACK_STDBY_XOSC ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set fallback mode failed", radio->name );
        return -1;
    }
    if( sx126x_set_dio2_as_rf_sw_ctrl( &radio->hal, true ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s enable DIO2 RF switch failed", radio->name );
        return -1;
    }
    if( sx126x_set_tx_params( &radio->hal, radio->tx_power_dbm, SX126X_RAMP_10_US ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set tx params failed", radio->name );
        return -1;
    }
    if( sx126x_set_lora_mod_params( &radio->hal, &radio->mod_params ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set modulation params failed", radio->name );
        return -1;
    }
    if( sx126x_set_rf_freq( &radio->hal, radio->rf_frequency_hz ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set RF frequency failed", radio->name );
        return -1;
    }
    if( sx126x_set_buffer_base_address( &radio->hal, 0x00, 0x00 ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set buffer base address failed", radio->name );
        return -1;
    }
    if( sx126x_set_lora_pkt_params( &radio->hal, &radio->rx_pkt_params ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set rx packet params failed", radio->name );
        return -1;
    }
    if( sx126x_set_lora_symb_nb_timeout( &radio->hal, 0 ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set symbol timeout failed", radio->name );
        return -1;
    }
    if( sx126x_set_lora_sync_word( &radio->hal, 0x12 ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set sync word failed", radio->name );
        return -1;
    }
    if( sx126x_cfg_tx_clamp( &radio->hal ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_WARN, "%s configure tx clamp failed", radio->name );
    }
    if( sx126x_set_ocp_value( &radio->hal, DEFAULT_OCP_SETTING ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_WARN, "%s set OCP failed", radio->name );
    }
    if( sx126x_set_dio_irq_params(
            &radio->hal,
            SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | SX126X_IRQ_CRC_ERROR | SX126X_IRQ_HEADER_ERROR |
                SX126X_IRQ_TIMEOUT | SX126X_IRQ_SYNC_WORD_VALID | SX126X_IRQ_HEADER_VALID,
            SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | SX126X_IRQ_CRC_ERROR | SX126X_IRQ_HEADER_ERROR |
                SX126X_IRQ_TIMEOUT | SX126X_IRQ_SYNC_WORD_VALID | SX126X_IRQ_HEADER_VALID,
            SX126X_IRQ_NONE, SX126X_IRQ_NONE ) != SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_ERROR, "%s set IRQ params failed", radio->name );
        return -1;
    }

    return 0;
}

static int radio_start_rx( radio_instance_t* radio )
{
    if( !radio->enabled )
    {
        return 0;
    }

    if( sx126x_clear_irq_status( &radio->hal, SX126X_IRQ_ALL ) != SX126X_STATUS_OK )
    {
        return -1;
    }
    if( sx126x_set_lora_pkt_params( &radio->hal, &radio->rx_pkt_params ) != SX126X_STATUS_OK )
    {
        return -1;
    }
    if( sx126x_set_rx_with_timeout_in_rtc_step( &radio->hal, SX126X_RX_CONTINUOUS ) != SX126X_STATUS_OK )
    {
        return -1;
    }

    radio->rx_active     = true;
    radio->tx_in_flight  = false;
    return 0;
}

static int radio_start_tx( radio_instance_t* radio, const uint8_t* payload, uint8_t len )
{
    sx126x_pkt_params_lora_t tx_params;

    if( !radio->enabled || radio->tx_in_flight || ( payload == NULL ) || ( len == 0u ) )
    {
        return -1;
    }

    tx_params = radio->rx_pkt_params;
    tx_params.pld_len_in_bytes = len;

    if( sx126x_clear_irq_status( &radio->hal, SX126X_IRQ_ALL ) != SX126X_STATUS_OK )
    {
        return -1;
    }
    if( sx126x_set_standby( &radio->hal, SX126X_STANDBY_CFG_RC ) != SX126X_STATUS_OK )
    {
        return -1;
    }
    if( sx126x_set_lora_pkt_params( &radio->hal, &tx_params ) != SX126X_STATUS_OK )
    {
        return -1;
    }
    if( sx126x_write_buffer( &radio->hal, 0x00, payload, len ) != SX126X_STATUS_OK )
    {
        return -1;
    }
    if( sx126x_set_tx( &radio->hal, radio->tx_timeout_ms ) != SX126X_STATUS_OK )
    {
        return -1;
    }

    radio->tx_in_flight = true;
    radio->rx_active    = false;
    return 0;
}

static int emit_rx_packet( app_state_t* state, radio_instance_t* radio )
{
    sx126x_rx_buffer_status_t rx_status;
    sx126x_pkt_status_lora_t pkt_status;
    uint8_t message[2 + 255];

    if( sx126x_get_rx_buffer_status( &radio->hal, &rx_status ) != SX126X_STATUS_OK )
    {
        return -1;
    }
    if( rx_status.pld_len_in_bytes > 255u )
    {
        return -1;
    }
    if( sx126x_read_buffer( &radio->hal, rx_status.buffer_start_pointer, &message[2], rx_status.pld_len_in_bytes ) !=
        SX126X_STATUS_OK )
    {
        return -1;
    }

    if( sx126x_get_lora_pkt_status( &radio->hal, &pkt_status ) == SX126X_STATUS_OK )
    {
        log_message( LOG_LEVEL_TRACE, "%s rx len=%u rssi=%d snr=%d", radio->name, rx_status.pld_len_in_bytes,
                     pkt_status.rssi_pkt_in_dbm, pkt_status.snr_pkt_in_db );
    }

    message[0] = IPC_MSG_RX_PACKET;
    message[1] = radio->radio_id;
    return send_ipc_message( state, message, (size_t) rx_status.pld_len_in_bytes + 2u );
}

static int radio_service_irqs( app_state_t* state, radio_instance_t* radio )
{
    sx126x_irq_mask_t irq_status = SX126X_IRQ_NONE;
    const bool tx_was_in_flight  = radio->tx_in_flight;

    if( !radio->enabled )
    {
        return 0;
    }

    if( sx126x_get_irq_status( &radio->hal, &irq_status ) != SX126X_STATUS_OK )
    {
        return -1;
    }
    if( irq_status == SX126X_IRQ_NONE )
    {
        return 0;
    }
    if( sx126x_clear_irq_status( &radio->hal, irq_status ) != SX126X_STATUS_OK )
    {
        return -1;
    }

    if( irq_status & SX126X_IRQ_SYNC_WORD_VALID )
    {
        log_message( LOG_LEVEL_TRACE, "%s sync word valid", radio->name );
    }
    if( irq_status & SX126X_IRQ_HEADER_VALID )
    {
        log_message( LOG_LEVEL_TRACE, "%s header valid", radio->name );
    }
    if( irq_status & SX126X_IRQ_HEADER_ERROR )
    {
        log_message( LOG_LEVEL_WARN, "%s header error", radio->name );
    }
    if( irq_status & SX126X_IRQ_CRC_ERROR )
    {
        log_message( LOG_LEVEL_WARN, "%s crc error", radio->name );
    }

    if( ( irq_status & SX126X_IRQ_RX_DONE ) != 0u )
    {
        const bool has_header_error = ( irq_status & SX126X_IRQ_HEADER_ERROR ) != 0u;
        const bool has_crc_error    = ( irq_status & SX126X_IRQ_CRC_ERROR ) != 0u;

        if( !has_header_error && !has_crc_error )
        {
            if( emit_rx_packet( state, radio ) != 0 )
            {
                log_message( LOG_LEVEL_WARN, "%s failed to emit rx packet", radio->name );
            }
        }
    }

    if( ( irq_status & SX126X_IRQ_TX_DONE ) != 0u )
    {
        radio->tx_in_flight = false;
        if( send_tx_result( state, radio->radio_id, IPC_TX_STATUS_OK ) != 0 )
        {
            log_message( LOG_LEVEL_WARN, "%s failed to send tx result", radio->name );
        }
        if( radio_start_rx( radio ) != 0 )
        {
            return -1;
        }
        log_message( LOG_LEVEL_TRACE, "%s tx complete, rx restored", radio->name );
    }

    if( ( irq_status & SX126X_IRQ_TIMEOUT ) != 0u )
    {
        log_message( LOG_LEVEL_WARN, "%s timeout", radio->name );
        if( tx_was_in_flight )
        {
            radio->tx_in_flight = false;
            if( send_tx_result( state, radio->radio_id, IPC_TX_STATUS_RADIO_ERROR ) != 0 )
            {
                log_message( LOG_LEVEL_WARN, "%s failed to send tx timeout", radio->name );
            }
            if( radio_start_rx( radio ) != 0 )
            {
                return -1;
            }
        }
    }

    return 0;
}

static int accept_client( app_state_t* state )
{
    int client_fd = accept( state->server_fd, NULL, NULL );

    if( client_fd < 0 )
    {
        return -1;
    }

    if( state->client_fd >= 0 )
    {
        close( state->client_fd );
    }
    state->client_fd = client_fd;
    log_message( LOG_LEVEL_INFO, "IPC client connected on %s", state->config.socket_path );
    return 0;
}

static int handle_client_message( app_state_t* state )
{
    uint8_t buf[2 + 255];
    ssize_t len;
    radio_instance_t* tx_radio;
    size_t payload_len;

    len = recv( state->client_fd, buf, sizeof( buf ), 0 );
    if( len <= 0 )
    {
        close( state->client_fd );
        state->client_fd = -1;
        return 0;
    }
    if( (size_t) len < 2u )
    {
        return 0;
    }
    if( buf[0] != IPC_MSG_TX_PACKET )
    {
        return 0;
    }

    tx_radio   = &state->sx1262;
    payload_len = (size_t) len - 2u;

    if( buf[1] != RADIO_ID_SX1262 )
    {
        log_message( LOG_LEVEL_WARN, "rejecting tx request for radio_id=%u; tx is sx1262-only", buf[1] );
        return send_tx_result( state, buf[1], IPC_TX_STATUS_RADIO_ERROR );
    }
    if( !tx_radio->enabled || ( payload_len == 0u ) )
    {
        return send_tx_result( state, RADIO_ID_SX1262, IPC_TX_STATUS_RADIO_ERROR );
    }
    if( radio_start_tx( tx_radio, &buf[2], (uint8_t) payload_len ) != 0 )
    {
        log_message( LOG_LEVEL_WARN, "%s failed to start tx len=%zu", tx_radio->name, payload_len );
        return send_tx_result( state, RADIO_ID_SX1262, IPC_TX_STATUS_RADIO_ERROR );
    }

    log_message( LOG_LEVEL_TRACE, "%s accepted tx request len=%zu", tx_radio->name, payload_len );
    return 0;
}

static void radio_close( radio_instance_t* radio )
{
    if( radio->busy_req != NULL )
    {
        gpiod_line_request_release( radio->busy_req );
        radio->busy_req = NULL;
    }
    if( radio->spi_fd >= 0 )
    {
        close( radio->spi_fd );
        radio->spi_fd = -1;
    }
}

static void cleanup( app_state_t* state )
{
    if( state->client_fd >= 0 )
    {
        close( state->client_fd );
        state->client_fd = -1;
    }
    if( state->server_fd >= 0 )
    {
        close( state->server_fd );
        state->server_fd = -1;
    }
    if( state->config.socket_path != NULL )
    {
        unlink( state->config.socket_path );
    }
    radio_close( &state->sx1262 );
    radio_close( &state->sx1268 );
    if( state->reset_req != NULL )
    {
        gpiod_line_request_release( state->reset_req );
        state->reset_req = NULL;
    }
    if( state->gpio_chip != NULL )
    {
        gpiod_chip_close( state->gpio_chip );
        state->gpio_chip = NULL;
    }
}

int main( void )
{
    app_state_t state;

    memset( &state, 0, sizeof( state ) );
    state.server_fd = -1;
    state.client_fd = -1;

    load_config( &state );

    state.gpio_chip = gpiod_chip_open( state.config.gpio_chip_path );
    if( state.gpio_chip == NULL )
    {
        log_errno_message( "open gpio chip failed" );
        cleanup( &state );
        return 1;
    }
    if( request_output_line( state.gpio_chip, state.config.reset_line, "new_radio_receiver_reset", &state.reset_req ) != 0 )
    {
        log_errno_message( "request reset line failed" );
        cleanup( &state );
        return 1;
    }

    if( radio_open( &state, &state.sx1262 ) != 0 || radio_open( &state, &state.sx1268 ) != 0 )
    {
        cleanup( &state );
        return 1;
    }
    if( radio_configure( &state.sx1262 ) != 0 || radio_configure( &state.sx1268 ) != 0 )
    {
        cleanup( &state );
        return 1;
    }
    if( radio_start_rx( &state.sx1262 ) != 0 || radio_start_rx( &state.sx1268 ) != 0 )
    {
        log_message( LOG_LEVEL_ERROR, "failed to start continuous rx" );
        cleanup( &state );
        return 1;
    }

    state.server_fd = open_server_socket( state.config.socket_path );
    if( state.server_fd < 0 )
    {
        log_errno_message( "open server socket failed" );
        cleanup( &state );
        return 1;
    }

    log_message( LOG_LEVEL_INFO, "listening socket=%s sx1262=%s sx1268=%s",
                 state.config.socket_path,
                 state.sx1262.enabled ? "enabled" : "disabled",
                 state.sx1268.enabled ? "enabled" : "disabled" );
    log_message( LOG_LEVEL_INFO, "sx1262 rx freq=%u sf=9 bw=250 tx_only=1", state.sx1262.rf_frequency_hz );
    log_message( LOG_LEVEL_INFO, "sx1268 rx freq=%u sf=7 bw=500 tx_only=0", state.sx1268.rf_frequency_hz );

    while( 1 )
    {
        struct pollfd fds[2];
        nfds_t nfds = 0u;

        fds[nfds].fd     = state.server_fd;
        fds[nfds].events = POLLIN;
        nfds++;

        if( state.client_fd >= 0 )
        {
            fds[nfds].fd     = state.client_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if( poll( fds, nfds, DEFAULT_RX_POLL_MS ) < 0 )
        {
            if( errno == EINTR )
            {
                continue;
            }
            log_errno_message( "poll failed" );
            break;
        }

        if( fds[0].revents & POLLIN )
        {
            if( accept_client( &state ) != 0 )
            {
                log_errno_message( "accept client failed" );
            }
        }
        if( ( state.client_fd >= 0 ) && ( nfds > 1u ) && ( fds[1].revents & POLLIN ) )
        {
            if( handle_client_message( &state ) != 0 )
            {
                log_message( LOG_LEVEL_WARN, "client message handling failed" );
            }
        }

        if( radio_service_irqs( &state, &state.sx1262 ) != 0 )
        {
            log_message( LOG_LEVEL_ERROR, "%s irq service failed", state.sx1262.name );
        }
        if( radio_service_irqs( &state, &state.sx1268 ) != 0 )
        {
            log_message( LOG_LEVEL_ERROR, "%s irq service failed", state.sx1268.name );
        }
    }

    cleanup( &state );
    return 1;
}
