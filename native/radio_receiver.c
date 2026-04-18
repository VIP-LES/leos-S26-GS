#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "leos/sx126x.h"

#define IPC_MSG_RX_PACKET 0x01u
#define IPC_MSG_BUTTON_EVENT 0x02u
#define IPC_MSG_TX_PACKET 0x03u
#define IPC_MSG_TX_RESULT 0x04u

#define IPC_TX_STATUS_OK 0x00u
#define IPC_TX_STATUS_RADIO_ERROR 0x02u

#define DEFAULT_SOCKET_PATH "/tmp/leos-radio.sock"
#define DEFAULT_SX1262_SPI_DEVICE "/dev/spidev0.0"
#define DEFAULT_SX1268_SPI_DEVICE "/dev/spidev0.1"
#define DEFAULT_GPIO_CHIP "/dev/gpiochip0"
#define DEFAULT_SPI_BAUD_HZ 8000000u

#define DEFAULT_SX1262_NSS 13u
#define DEFAULT_SX1262_BUSY 19u
#define DEFAULT_SX1262_DIO1 17u

#define DEFAULT_SX1268_NSS 14u
#define DEFAULT_SX1268_BUSY 27u
#define DEFAULT_SX1268_DIO1 4u
#define DEFAULT_RESET_LINE 25u

#define DEFAULT_SX1262_DIO2_RF_SWITCH_ENABLE true
#define DEFAULT_SX1262_DIO3_TCXO_ENABLE true
#define DEFAULT_SX1262_TCXO_VOLTAGE LEOS_RADIO_TCXO_2P2V
#define DEFAULT_SX1262_TCXO_DELAY_US 5000u

#define DEFAULT_SX1268_DIO2_RF_SWITCH_ENABLE true
#define DEFAULT_SX1268_DIO3_TCXO_ENABLE true
#define DEFAULT_SX1268_TCXO_VOLTAGE LEOS_RADIO_TCXO_2P2V
#define DEFAULT_SX1268_TCXO_DELAY_US 5000u
#define HEARTBEAT_INTERVAL_MS 1000u

typedef enum
{
    LOG_LEVEL_TRACE,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR,
} log_level_t;

typedef struct
{
    uint64_t dio1_edges;
    uint64_t poll_calls;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_started;
    uint64_t tx_completed_ok;
    uint64_t tx_completed_err;
    uint64_t tx_restart_rx_failures;
    uint64_t ipc_tx_results_sent;
} radio_stats_t;

typedef struct
{
    uint64_t client_connects;
    uint64_t client_disconnects;
    uint64_t ipc_messages_sent;
    uint64_t ipc_send_failures;
    uint64_t ipc_messages_received;
    uint64_t short_client_messages;
    uint64_t unknown_client_messages;
} app_stats_t;

typedef struct
{
    const char *socket_path;
    const char *sx1262_spi_device;
    const char *sx1268_spi_device;
    uint32_t spi_baud_hz;
    bool sx1262_enabled;
    bool sx1268_enabled;
    uint32_t reset_line;
    uint32_t sx1262_nss;
    uint32_t sx1262_busy;
    uint32_t sx1262_dio1;
    bool sx1262_dio2_rf_switch_enable;
    bool sx1262_dio3_tcxo_enable;
    leos_radio_tcxo_voltage_t sx1262_tcxo_voltage;
    uint32_t sx1262_tcxo_delay_us;
    uint32_t sx1268_nss;
    uint32_t sx1268_busy;
    uint32_t sx1268_dio1;
    bool sx1268_dio2_rf_switch_enable;
    bool sx1268_dio3_tcxo_enable;
    leos_radio_tcxo_voltage_t sx1268_tcxo_voltage;
    uint32_t sx1268_tcxo_delay_us;
} app_config_t;

typedef struct
{
    leos_radio_t radio;
    struct gpiod_line_request *dio1_request;
    struct gpiod_edge_event_buffer *dio1_event_buffer;
    atomic_bool irq_pending;
    bool restart_rx_after_tx;
    radio_stats_t stats;
    radio_stats_t last_heartbeat_stats;
    leos_radio_hw_config_t hw;
    leos_radio_config_t cfg;
} radio_state_t;

typedef struct
{
    app_config_t config;
    int sx1262_spi_fd;
    int sx1268_spi_fd;
    int server_fd;
    int client_fd;
    int irq_pipe_read_fd;
    int irq_pipe_write_fd;
    struct gpiod_chip *gpio_chip;
    pthread_t irq_thread;
    atomic_bool stop_requested;
    app_stats_t stats;
    app_stats_t last_heartbeat_stats;
    uint64_t last_heartbeat_ms;
    radio_state_t sx1262;
    radio_state_t sx1268;
} app_state_t;

static const char *log_level_name(log_level_t level)
{
    switch (level)
    {
    case LOG_LEVEL_TRACE:
        return "TRACE";
    case LOG_LEVEL_INFO:
        return "INFO";
    case LOG_LEVEL_WARNING:
        return "WARN";
    case LOG_LEVEL_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static void log_message(log_level_t level, const char *fmt, ...)
{
    va_list args;

    fprintf(stderr, "radio_receiver [%s]: ", log_level_name(level));
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static void log_errno_message(log_level_t level, const char *context)
{
    int err = errno;

    log_message(level, "%s: %s (errno=%d)", context, strerror(err), err);
}

static const char *radio_name(leos_radio_t radio)
{
    return (radio == LEOS_RADIO_SX1268) ? "sx1268" : "sx1262";
}

static const char *radio_status_name(leos_radio_status_t status)
{
    switch (status)
    {
    case LEOS_RADIO_OK:
        return "LEOS_RADIO_OK";
    case LEOS_RADIO_ERR_ARG:
        return "LEOS_RADIO_ERR_ARG";
    case LEOS_RADIO_ERR_STATE:
        return "LEOS_RADIO_ERR_STATE";
    case LEOS_RADIO_ERR_IO:
        return "LEOS_RADIO_ERR_IO";
    case LEOS_RADIO_ERR_TIMEOUT:
        return "LEOS_RADIO_ERR_TIMEOUT";
    case LEOS_RADIO_ERR_BUSY:
        return "LEOS_RADIO_ERR_BUSY";
    case LEOS_RADIO_ERR_DRIVER:
        return "LEOS_RADIO_ERR_DRIVER";
    default:
        return "LEOS_RADIO_ERR_UNKNOWN";
    }
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0u;
    }

    return ((uint64_t)ts.tv_sec * 1000u) + (uint64_t)(ts.tv_nsec / 1000000L);
}

static radio_state_t *radio_state_for(app_state_t *state, leos_radio_t radio)
{
    if (radio == LEOS_RADIO_SX1268)
    {
        return &state->sx1268;
    }

    return &state->sx1262;
}

static uint32_t env_u32(const char *name, uint32_t fallback)
{
    const char *value = getenv(name);
    char *endptr = NULL;
    unsigned long parsed;

    if ((value == NULL) || (*value == '\0'))
    {
        return fallback;
    }

    parsed = strtoul(value, &endptr, 10);
    if ((endptr == value) || (*endptr != '\0'))
    {
        return fallback;
    }

    return (uint32_t)parsed;
}

static const char *env_str(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return ((value == NULL) || (*value == '\0')) ? fallback : value;
}

static void load_config(app_config_t *cfg)
{
    const char *radio_enabled;

    cfg->socket_path = env_str("RADIO_SOCKET_PATH", DEFAULT_SOCKET_PATH);
    cfg->sx1262_spi_device = env_str("RADIO_SX1262_SPI_DEVICE", DEFAULT_SX1262_SPI_DEVICE);
    cfg->sx1268_spi_device = env_str("RADIO_SX1268_SPI_DEVICE", DEFAULT_SX1268_SPI_DEVICE);
    cfg->spi_baud_hz = env_u32("RADIO_SPI_BAUD_HZ", DEFAULT_SPI_BAUD_HZ);
    cfg->sx1262_enabled = true;
    cfg->sx1268_enabled = true;

    radio_enabled = env_str("RADIO_ENABLED", "both");
    if (strcmp(radio_enabled, "sx1262") == 0)
    {
        cfg->sx1268_enabled = false;
    }
    else if (strcmp(radio_enabled, "sx1268") == 0)
    {
        cfg->sx1262_enabled = false;
    }
    else if (strcmp(radio_enabled, "both") != 0)
    {
        log_message(LOG_LEVEL_WARNING, "unsupported RADIO_ENABLED=%s, defaulting to both", radio_enabled);
    }

    cfg->reset_line = env_u32("RADIO_RESET_LINE", DEFAULT_RESET_LINE);
    cfg->sx1262_nss = env_u32("RADIO_SX1262_NSS_LINE", DEFAULT_SX1262_NSS);
    cfg->sx1262_busy = env_u32("RADIO_SX1262_BUSY_LINE", DEFAULT_SX1262_BUSY);
    cfg->sx1262_dio1 = env_u32("RADIO_SX1262_DIO1_LINE", DEFAULT_SX1262_DIO1);
    cfg->sx1262_dio2_rf_switch_enable = DEFAULT_SX1262_DIO2_RF_SWITCH_ENABLE;
    cfg->sx1262_dio3_tcxo_enable = DEFAULT_SX1262_DIO3_TCXO_ENABLE;
    cfg->sx1262_tcxo_voltage = DEFAULT_SX1262_TCXO_VOLTAGE;
    cfg->sx1262_tcxo_delay_us = DEFAULT_SX1262_TCXO_DELAY_US;

    cfg->sx1268_nss = env_u32("RADIO_SX1268_NSS_LINE", DEFAULT_SX1268_NSS);
    cfg->sx1268_busy = env_u32("RADIO_SX1268_BUSY_LINE", DEFAULT_SX1268_BUSY);
    cfg->sx1268_dio1 = env_u32("RADIO_SX1268_DIO1_LINE", DEFAULT_SX1268_DIO1);
    cfg->sx1268_dio2_rf_switch_enable = DEFAULT_SX1268_DIO2_RF_SWITCH_ENABLE;
    cfg->sx1268_dio3_tcxo_enable = DEFAULT_SX1268_DIO3_TCXO_ENABLE;
    cfg->sx1268_tcxo_voltage = DEFAULT_SX1268_TCXO_VOLTAGE;
    cfg->sx1268_tcxo_delay_us = DEFAULT_SX1268_TCXO_DELAY_US;
}

static void build_radio_config(const app_state_t *state, leos_radio_t radio, leos_radio_config_t *cfg)
{
    leos_sx126x_get_default_config(radio, cfg);

    if (radio == LEOS_RADIO_SX1262)
    {
        cfg->rf_frequency_hz = 918250000u;
        cfg->tx_power_dbm = 14;
        cfg->crc_enabled = true;
        cfg->iq_inverted = false;
        cfg->bandwidth = LEOS_RADIO_BW_250_KHZ;
        cfg->coding_rate = LEOS_RADIO_CR_4_5;
        cfg->spreading_factor = LEOS_RADIO_SF_9;
        cfg->sync_word = 0x12u;
        cfg->dio2_rf_switch_enable = state->config.sx1262_dio2_rf_switch_enable;
        cfg->dio3_tcxo_enable = state->config.sx1262_dio3_tcxo_enable;
        cfg->tcxo_voltage = state->config.sx1262_tcxo_voltage;
        cfg->tcxo_delay_us = state->config.sx1262_tcxo_delay_us;
    }
    else
    {
        cfg->rf_frequency_hz = 435000000u;
        cfg->tx_power_dbm = 14;
        cfg->crc_enabled = true;
        cfg->iq_inverted = false;
        cfg->bandwidth = LEOS_RADIO_BW_500_KHZ;
        cfg->coding_rate = LEOS_RADIO_CR_4_5;
        cfg->spreading_factor = LEOS_RADIO_SF_5;
        cfg->sync_word = 0x12u;
        cfg->dio2_rf_switch_enable = state->config.sx1268_dio2_rf_switch_enable;
        cfg->dio3_tcxo_enable = state->config.sx1268_dio3_tcxo_enable;
        cfg->tcxo_voltage = state->config.sx1268_tcxo_voltage;
        cfg->tcxo_delay_us = state->config.sx1268_tcxo_delay_us;
    }
}

static void build_radio_hw(app_state_t *state, radio_state_t *radio)
{
    memset(&radio->hw, 0, sizeof(radio->hw));
    radio->hw.spi_baud_hz = state->config.spi_baud_hz;
    radio->hw.pin_sck = 10u;
    radio->hw.pin_mosi = 11u;
    radio->hw.pin_miso = 12u;
    radio->hw.pin_reset = state->config.reset_line;

    if (radio->radio == LEOS_RADIO_SX1262)
    {
        radio->hw.platform_spi = &state->sx1262_spi_fd;
        radio->hw.pin_nss = state->config.sx1262_nss;
        radio->hw.pin_busy = state->config.sx1262_busy;
        radio->hw.pin_dio1 = state->config.sx1262_dio1;
    }
    else
    {
        radio->hw.platform_spi = &state->sx1268_spi_fd;
        radio->hw.pin_nss = state->config.sx1268_nss;
        radio->hw.pin_busy = state->config.sx1268_busy;
        radio->hw.pin_dio1 = state->config.sx1268_dio1;
    }
}

static int open_spi_device(const char *path)
{
    return open(path, O_RDWR | O_CLOEXEC);
}

static const char *radio_spi_device_path(const app_state_t *state, leos_radio_t radio)
{
    if (radio == LEOS_RADIO_SX1262)
    {
        return state->config.sx1262_spi_device;
    }

    return state->config.sx1268_spi_device;
}

static int open_server_socket(const char *path)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    unlink(path);
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0)
    {
        close(fd);
        unlink(path);
        return -1;
    }

    return fd;
}

static int send_ipc_message(app_state_t *state, const uint8_t *buf, size_t len)
{
    ssize_t sent;

    if (state->client_fd < 0)
    {
        return 0;
    }

    sent = send(state->client_fd, buf, len, MSG_NOSIGNAL);
    if (sent < 0)
    {
        state->stats.ipc_send_failures++;
        close(state->client_fd);
        state->client_fd = -1;
        return -1;
    }
    state->stats.ipc_messages_sent++;
    return 0;
}

static int send_tx_result(app_state_t *state, leos_radio_t radio, uint8_t status)
{
    uint8_t message[3];

    message[0] = IPC_MSG_TX_RESULT;
    message[1] = (uint8_t)radio;
    message[2] = status;
    return send_ipc_message(state, message, sizeof(message));
}

static uint8_t ipc_tx_status_from_radio_status(leos_radio_status_t status)
{
    return (status == LEOS_RADIO_OK) ? IPC_TX_STATUS_OK : IPC_TX_STATUS_RADIO_ERROR;
}

static int emit_rx_packet(app_state_t *state, leos_radio_t radio)
{
    uint8_t message[2 + 255];
    size_t rx_len = 0u;
    leos_radio_packet_info_t info;
    leos_radio_status_t status;
    radio_state_t *radio_state = radio_state_for(state, radio);

    memset(&info, 0, sizeof(info));

    message[0] = IPC_MSG_RX_PACKET;
    message[1] = (uint8_t)radio;

    status = leos_sx126x_read(radio, &message[2], 255u, &rx_len, &info);
    if (status != LEOS_RADIO_OK)
    {
        log_message(LOG_LEVEL_ERROR, "leos_sx126x_read failed on %s", radio_name(radio));
        return -1;
    }

    radio_state->stats.rx_packets++;
    radio_state->stats.rx_bytes += (uint64_t)rx_len;
    log_message(LOG_LEVEL_TRACE, "read %zu bytes from %s", rx_len, radio_name(radio));
    return send_ipc_message(state, message, rx_len + 2u);
}

static bool radio_is_enabled(const app_state_t *state, leos_radio_t radio)
{
    if (radio == LEOS_RADIO_SX1262)
    {
        return state->config.sx1262_enabled;
    }

    return state->config.sx1268_enabled;
}

static int service_radio(app_state_t *state, radio_state_t *radio)
{
    leos_radio_status_t status;
    const bool was_tx_in_flight = leos_sx126x_tx_in_flight(radio->radio);
    const bool had_irq_pending = atomic_load(&radio->irq_pending);

    if (!radio_is_enabled(state, radio->radio))
    {
        return 0;
    }

    radio->stats.poll_calls++;
    atomic_store(&radio->irq_pending, false);

    status = leos_sx126x_poll(radio->radio);
    if (status != LEOS_RADIO_OK)
    {
        log_message(LOG_LEVEL_ERROR, "poll failed for %s with %s",
                    radio_name(radio->radio), radio_status_name(status));
        return -1;
    }

    if (had_irq_pending)
    {
        log_message(LOG_LEVEL_TRACE, "serviced deferred IRQ for %s", radio_name(radio->radio));
    }

    if (was_tx_in_flight && !leos_sx126x_tx_in_flight(radio->radio))
    {
        const leos_radio_status_t tx_status = leos_sx126x_last_tx_status(radio->radio);
        const uint8_t ipc_status = ipc_tx_status_from_radio_status(tx_status);

        if (radio->restart_rx_after_tx && (tx_status == LEOS_RADIO_OK))
        {
            status = leos_sx126x_start_rx(radio->radio);
            if (status != LEOS_RADIO_OK)
            {
                radio->stats.tx_restart_rx_failures++;
                log_message(LOG_LEVEL_ERROR, "restart RX failed for %s with %s",
                            radio_name(radio->radio), radio_status_name(status));
                return -1;
            }
        }

        if (tx_status == LEOS_RADIO_OK)
        {
            radio->stats.tx_completed_ok++;
            log_message(LOG_LEVEL_TRACE, "TX completed on %s", radio_name(radio->radio));
        }
        else
        {
            radio->stats.tx_completed_err++;
            log_message(LOG_LEVEL_WARNING, "TX completed with %s on %s",
                        radio_status_name(tx_status), radio_name(radio->radio));
        }

        radio->restart_rx_after_tx = false;
        if (send_tx_result(state, radio->radio, ipc_status) != 0)
        {
            log_message(LOG_LEVEL_ERROR, "send_tx_result failed for %s", radio_name(radio->radio));
            return -1;
        }
        radio->stats.ipc_tx_results_sent++;
    }

    if (leos_sx126x_packet_available(radio->radio))
    {
        log_message(LOG_LEVEL_TRACE, "packet available on %s", radio_name(radio->radio));
        if (emit_rx_packet(state, radio->radio) != 0)
        {
            log_message(LOG_LEVEL_ERROR, "emit_rx_packet failed on %s", radio_name(radio->radio));
            return -1;
        }
    }

    return 0;
}

static int accept_client(app_state_t *state)
{
    int client_fd = accept(state->server_fd, NULL, NULL);

    if (client_fd < 0)
    {
        return -1;
    }

    if (state->client_fd >= 0)
    {
        close(state->client_fd);
        state->stats.client_disconnects++;
    }
    state->client_fd = client_fd;
    state->stats.client_connects++;
    log_message(LOG_LEVEL_INFO, "IPC client connected on %s", state->config.socket_path);
    return 0;
}

static int handle_client_message(app_state_t *state)
{
    uint8_t buf[2 + 255];
    ssize_t len;
    leos_radio_t radio;
    leos_radio_status_t status;
    leos_radio_mode_t previous_mode;
    radio_state_t *radio_state;
    size_t payload_len;

    len = recv(state->client_fd, buf, sizeof(buf), 0);
    if (len <= 0)
    {
        close(state->client_fd);
        state->client_fd = -1;
        state->stats.client_disconnects++;
        log_message(LOG_LEVEL_WARNING, "IPC client disconnected");
        return 0;
    }

    state->stats.ipc_messages_received++;

    if ((size_t)len < 2u)
    {
        state->stats.short_client_messages++;
        log_message(LOG_LEVEL_WARNING, "ignoring short client IPC message of %zd bytes", len);
        return 0;
    }

    if (buf[0] != IPC_MSG_TX_PACKET)
    {
        state->stats.unknown_client_messages++;
        log_message(LOG_LEVEL_WARNING, "ignoring unknown client IPC message kind 0x%02x", buf[0]);
        return 0;
    }

    radio = (buf[1] == (uint8_t)LEOS_RADIO_SX1268) ? LEOS_RADIO_SX1268 : LEOS_RADIO_SX1262;
    radio_state = (radio == LEOS_RADIO_SX1268) ? &state->sx1268 : &state->sx1262;
    payload_len = (size_t)len - 2u;
    if (payload_len == 0u)
    {
        return send_tx_result(state, radio, IPC_TX_STATUS_RADIO_ERROR);
    }

    if (!radio_is_enabled(state, radio))
    {
        return send_tx_result(state, radio, IPC_TX_STATUS_RADIO_ERROR);
    }

    previous_mode = leos_sx126x_mode(radio);
    status = leos_sx126x_start_tx(radio, &buf[2], payload_len);
    if (status != LEOS_RADIO_OK)
    {
        log_message(LOG_LEVEL_WARNING, "start_tx failed for %s with %s",
                    radio_name(radio), radio_status_name(status));
        return send_tx_result(state, radio, ipc_tx_status_from_radio_status(status));
    }

    radio_state->restart_rx_after_tx = (previous_mode == LEOS_RADIO_MODE_RX);
    radio_state->stats.tx_started++;
    log_message(LOG_LEVEL_TRACE, "accepted TX request for %s (%zu bytes, restart_rx=%d)",
                radio_name(radio), payload_len, radio_state->restart_rx_after_tx ? 1 : 0);
    return 0;
}

static int app_poll_timeout_ms(const app_state_t *state)
{
    if ((state->config.sx1262_enabled && leos_sx126x_tx_in_flight(LEOS_RADIO_SX1262)) ||
        (state->config.sx1268_enabled && leos_sx126x_tx_in_flight(LEOS_RADIO_SX1268)))
    {
        return 50;
    }

    return (int)HEARTBEAT_INTERVAL_MS;
}

static void maybe_log_heartbeat(app_state_t *state)
{
    const uint64_t now = now_ms();
    radio_state_t *radios[2] = {&state->sx1262, &state->sx1268};
    const bool enabled[2] = {state->config.sx1262_enabled, state->config.sx1268_enabled};

    if ((state->last_heartbeat_ms != 0u) && ((now - state->last_heartbeat_ms) < HEARTBEAT_INTERVAL_MS))
    {
        return;
    }

    state->last_heartbeat_ms = now;
    for (size_t i = 0; i < 2u; ++i)
    {
        radio_state_t *radio = radios[i];
        const radio_stats_t delta = {
            .dio1_edges = radio->stats.dio1_edges - radio->last_heartbeat_stats.dio1_edges,
            .poll_calls = radio->stats.poll_calls - radio->last_heartbeat_stats.poll_calls,
            .rx_packets = radio->stats.rx_packets - radio->last_heartbeat_stats.rx_packets,
            .rx_bytes = radio->stats.rx_bytes - radio->last_heartbeat_stats.rx_bytes,
            .tx_started = radio->stats.tx_started - radio->last_heartbeat_stats.tx_started,
            .tx_completed_ok = radio->stats.tx_completed_ok - radio->last_heartbeat_stats.tx_completed_ok,
            .tx_completed_err = radio->stats.tx_completed_err - radio->last_heartbeat_stats.tx_completed_err,
            .tx_restart_rx_failures = radio->stats.tx_restart_rx_failures - radio->last_heartbeat_stats.tx_restart_rx_failures,
            .ipc_tx_results_sent = radio->stats.ipc_tx_results_sent - radio->last_heartbeat_stats.ipc_tx_results_sent,
        };

        if (!enabled[i])
        {
            log_message(LOG_LEVEL_INFO, "heartbeat %s disabled", radio_name(radio->radio));
        }
        else
        {
            log_message(LOG_LEVEL_INFO,
                        "heartbeat %s delta{dio1=%llu poll=%llu rx_pkts=%llu rx_bytes=%llu tx_start=%llu tx_ok=%llu tx_err=%llu tx_result=%llu} "
                        "total{dio1=%llu poll=%llu rx_pkts=%llu rx_bytes=%llu tx_start=%llu tx_ok=%llu tx_err=%llu tx_result=%llu}",
                        radio_name(radio->radio),
                        (unsigned long long)delta.dio1_edges,
                        (unsigned long long)delta.poll_calls,
                        (unsigned long long)delta.rx_packets,
                        (unsigned long long)delta.rx_bytes,
                        (unsigned long long)delta.tx_started,
                        (unsigned long long)delta.tx_completed_ok,
                        (unsigned long long)delta.tx_completed_err,
                        (unsigned long long)delta.ipc_tx_results_sent,
                        (unsigned long long)radio->stats.dio1_edges,
                        (unsigned long long)radio->stats.poll_calls,
                        (unsigned long long)radio->stats.rx_packets,
                        (unsigned long long)radio->stats.rx_bytes,
                        (unsigned long long)radio->stats.tx_started,
                        (unsigned long long)radio->stats.tx_completed_ok,
                        (unsigned long long)radio->stats.tx_completed_err,
                        (unsigned long long)radio->stats.ipc_tx_results_sent);
        }

        radio->last_heartbeat_stats = radio->stats;
    }

    {
        const app_stats_t delta = {
            .client_connects = state->stats.client_connects - state->last_heartbeat_stats.client_connects,
            .client_disconnects = state->stats.client_disconnects - state->last_heartbeat_stats.client_disconnects,
            .ipc_messages_sent = state->stats.ipc_messages_sent - state->last_heartbeat_stats.ipc_messages_sent,
            .ipc_send_failures = state->stats.ipc_send_failures - state->last_heartbeat_stats.ipc_send_failures,
            .ipc_messages_received = state->stats.ipc_messages_received - state->last_heartbeat_stats.ipc_messages_received,
            .short_client_messages = state->stats.short_client_messages - state->last_heartbeat_stats.short_client_messages,
            .unknown_client_messages = state->stats.unknown_client_messages - state->last_heartbeat_stats.unknown_client_messages,
        };

        log_message(LOG_LEVEL_INFO,
                    "heartbeat ipc delta{connect=%llu disconnect=%llu rx=%llu tx=%llu tx_fail=%llu short=%llu unknown=%llu} "
                    "total{connect=%llu disconnect=%llu rx=%llu tx=%llu tx_fail=%llu short=%llu unknown=%llu} client=%s socket=%s",
                    (unsigned long long)delta.client_connects,
                    (unsigned long long)delta.client_disconnects,
                    (unsigned long long)delta.ipc_messages_received,
                    (unsigned long long)delta.ipc_messages_sent,
                    (unsigned long long)delta.ipc_send_failures,
                    (unsigned long long)delta.short_client_messages,
                    (unsigned long long)delta.unknown_client_messages,
                    (unsigned long long)state->stats.client_connects,
                    (unsigned long long)state->stats.client_disconnects,
                    (unsigned long long)state->stats.ipc_messages_received,
                    (unsigned long long)state->stats.ipc_messages_sent,
                    (unsigned long long)state->stats.ipc_send_failures,
                    (unsigned long long)state->stats.short_client_messages,
                    (unsigned long long)state->stats.unknown_client_messages,
                    (state->client_fd >= 0) ? "connected" : "waiting",
                    state->config.socket_path);
        state->last_heartbeat_stats = state->stats;
    }
}

static int request_dio1_line(app_state_t *state, radio_state_t *radio)
{
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_cfg = NULL;
    struct gpiod_request_config *req_cfg = NULL;
    unsigned int offset = (unsigned int)radio->hw.pin_dio1;
    int rc = -1;

    settings = gpiod_line_settings_new();
    if (settings == NULL)
    {
        return -1;
    }

    if ((gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT) != 0) ||
        (gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_RISING) != 0))
    {
        goto cleanup;
    }

    line_cfg = gpiod_line_config_new();
    if (line_cfg == NULL)
    {
        goto cleanup;
    }

    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) != 0)
    {
        goto cleanup;
    }

    req_cfg = gpiod_request_config_new();
    if (req_cfg == NULL)
    {
        goto cleanup;
    }

    gpiod_request_config_set_consumer(req_cfg, "leos_radio_receiver");
    gpiod_request_config_set_event_buffer_size(req_cfg, 4);

    radio->dio1_request = gpiod_chip_request_lines(state->gpio_chip, req_cfg, line_cfg);
    if (radio->dio1_request == NULL)
    {
        goto cleanup;
    }

    radio->dio1_event_buffer = gpiod_edge_event_buffer_new(4);
    if (radio->dio1_event_buffer == NULL)
    {
        gpiod_line_request_release(radio->dio1_request);
        radio->dio1_request = NULL;
        goto cleanup;
    }

    rc = 0;

cleanup:
    if (req_cfg != NULL)
    {
        gpiod_request_config_free(req_cfg);
    }
    if (line_cfg != NULL)
    {
        gpiod_line_config_free(line_cfg);
    }
    if (settings != NULL)
    {
        gpiod_line_settings_free(settings);
    }

    return rc;
}

static void wake_main_loop(app_state_t *state)
{
    uint8_t byte = 1u;

    if (state->irq_pipe_write_fd < 0)
    {
        return;
    }

    (void)write(state->irq_pipe_write_fd, &byte, sizeof(byte));
}

static void drain_irq_pipe(app_state_t *state)
{
    uint8_t buf[32];

    if (state->irq_pipe_read_fd < 0)
    {
        return;
    }

    while (read(state->irq_pipe_read_fd, buf, sizeof(buf)) > 0)
    {
    }
}

static void sleep_for_us(long usec)
{
    struct timespec req;

    if (usec <= 0)
    {
        return;
    }

    req.tv_sec = usec / 1000000L;
    req.tv_nsec = (usec % 1000000L) * 1000L;

    while (nanosleep(&req, &req) != 0)
    {
        if (errno != EINTR)
        {
            break;
        }
    }
}

static void *irq_thread_main(void *arg)
{
    app_state_t *state = (app_state_t *)arg;
    int sx1262_fd = -1;
    int sx1268_fd = -1;

    if ((state->sx1262.dio1_request != NULL) && state->config.sx1262_enabled)
    {
        sx1262_fd = gpiod_line_request_get_fd(state->sx1262.dio1_request);
    }
    if ((state->sx1268.dio1_request != NULL) && state->config.sx1268_enabled)
    {
        sx1268_fd = gpiod_line_request_get_fd(state->sx1268.dio1_request);
    }

    for (;;)
    {
        struct pollfd fds[2];
        int sx1262_index = -1;
        int sx1268_index = -1;
        nfds_t nfds = 0u;
        int poll_rc;

        if (atomic_load(&state->stop_requested))
        {
            break;
        }

        if (sx1262_fd >= 0)
        {
            sx1262_index = (int)nfds;
            fds[nfds].fd = sx1262_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        if (sx1268_fd >= 0)
        {
            sx1268_index = (int)nfds;
            fds[nfds].fd = sx1268_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        if (nfds == 0u)
        {
            sleep_for_us(100000L);
            continue;
        }

        poll_rc = poll(fds, nfds, 100);
        if (poll_rc <= 0)
        {
            continue;
        }

        if ((sx1262_index >= 0) && ((fds[sx1262_index].revents & POLLIN) != 0))
        {
            if (gpiod_line_request_read_edge_events(state->sx1262.dio1_request,
                                                    state->sx1262.dio1_event_buffer,
                                                    4) > 0)
            {
                state->sx1262.stats.dio1_edges++;
                log_message(LOG_LEVEL_TRACE, "SX1262 DIO1 edge detected");
                leos_sx126x_handle_dio1_irq(state->sx1262.radio);
                atomic_store(&state->sx1262.irq_pending, true);
                wake_main_loop(state);
            }
        }

        if ((sx1268_index >= 0) && ((fds[sx1268_index].revents & POLLIN) != 0))
        {
            if (gpiod_line_request_read_edge_events(state->sx1268.dio1_request,
                                                    state->sx1268.dio1_event_buffer,
                                                    4) > 0)
            {
                state->sx1268.stats.dio1_edges++;
                log_message(LOG_LEVEL_TRACE, "SX1268 DIO1 edge detected");
                leos_sx126x_handle_dio1_irq(state->sx1268.radio);
                atomic_store(&state->sx1268.irq_pending, true);
                wake_main_loop(state);
            }
        }
    }

    return NULL;
}

static int init_state(app_state_t *state)
{
    int irq_pipe_fds[2];
    leos_radio_status_t status;

    if (!state->config.sx1262_enabled && !state->config.sx1268_enabled)
    {
        log_message(LOG_LEVEL_ERROR, "init_state: no radios enabled after config parsing");
        return -1;
    }

    log_message(LOG_LEVEL_INFO,
                "startup config socket=%s enabled{sx1262=%d sx1268=%d} spi{sx1262=%s sx1268=%s}",
                state->config.socket_path,
                state->config.sx1262_enabled ? 1 : 0,
                state->config.sx1268_enabled ? 1 : 0,
                state->config.sx1262_spi_device,
                state->config.sx1268_spi_device);

    if (state->config.sx1262_enabled)
    {
        state->sx1262_spi_fd = open_spi_device(state->config.sx1262_spi_device);
        if (state->sx1262_spi_fd < 0)
        {
            log_message(LOG_LEVEL_ERROR, "init_state: failed to open SPI device %s for %s",
                        state->config.sx1262_spi_device, radio_name(LEOS_RADIO_SX1262));
            log_errno_message(LOG_LEVEL_ERROR, "init_state SPI open");
            return -1;
        }
    }

    if (state->config.sx1268_enabled)
    {
        state->sx1268_spi_fd = open_spi_device(state->config.sx1268_spi_device);
        if (state->sx1268_spi_fd < 0)
        {
            log_message(LOG_LEVEL_ERROR, "init_state: failed to open SPI device %s for %s",
                        state->config.sx1268_spi_device, radio_name(LEOS_RADIO_SX1268));
            log_errno_message(LOG_LEVEL_ERROR, "init_state SPI open");
            return -1;
        }
    }

    state->server_fd = open_server_socket(state->config.socket_path);
    if (state->server_fd < 0)
    {
        log_message(LOG_LEVEL_ERROR, "init_state: failed to open server socket %s", state->config.socket_path);
        log_errno_message(LOG_LEVEL_ERROR, "init_state socket setup");
        return -1;
    }

    state->gpio_chip = gpiod_chip_open(DEFAULT_GPIO_CHIP);
    if (state->gpio_chip == NULL)
    {
        log_message(LOG_LEVEL_ERROR, "init_state: failed to open GPIO chip %s", DEFAULT_GPIO_CHIP);
        log_errno_message(LOG_LEVEL_ERROR, "init_state GPIO chip open");
        return -1;
    }

    state->sx1262.radio = LEOS_RADIO_SX1262;
    state->sx1268.radio = LEOS_RADIO_SX1268;

    build_radio_hw(state, &state->sx1262);
    build_radio_hw(state, &state->sx1268);
    build_radio_config(state, LEOS_RADIO_SX1262, &state->sx1262.cfg);
    build_radio_config(state, LEOS_RADIO_SX1268, &state->sx1268.cfg);

    if (state->config.sx1262_enabled && (request_dio1_line(state, &state->sx1262) != 0))
    {
        log_message(LOG_LEVEL_ERROR, "init_state: failed to request DIO1 line %u for %s",
                    state->sx1262.hw.pin_dio1, radio_name(state->sx1262.radio));
        log_errno_message(LOG_LEVEL_ERROR, "init_state DIO1 request");
        return -1;
    }
    if (state->config.sx1268_enabled && (request_dio1_line(state, &state->sx1268) != 0))
    {
        log_message(LOG_LEVEL_ERROR, "init_state: failed to request DIO1 line %u for %s",
                    state->sx1268.hw.pin_dio1, radio_name(state->sx1268.radio));
        log_errno_message(LOG_LEVEL_ERROR, "init_state DIO1 request");
        return -1;
    }

    if (pipe(irq_pipe_fds) != 0)
    {
        log_errno_message(LOG_LEVEL_ERROR, "init_state IRQ pipe creation");
        return -1;
    }
    state->irq_pipe_read_fd = irq_pipe_fds[0];
    state->irq_pipe_write_fd = irq_pipe_fds[1];
    (void)fcntl(state->irq_pipe_read_fd, F_SETFL, O_NONBLOCK);

    if (state->config.sx1262_enabled)
    {
        status = leos_sx126x_init(LEOS_RADIO_SX1262, &state->sx1262.hw, &state->sx1262.cfg);
        if (status != LEOS_RADIO_OK)
        {
            log_message(LOG_LEVEL_ERROR, "init_state: leos_sx126x_init(%s) failed with %s (%d)",
                        radio_name(LEOS_RADIO_SX1262), radio_status_name(status), (int)status);
            return -1;
        }
        log_message(LOG_LEVEL_INFO,
                    "init_state: leos_sx126x_init(%s) succeeded [spi=%s nss=%u busy=%u reset=%u dio1=%u]",
                    radio_name(LEOS_RADIO_SX1262),
                    radio_spi_device_path(state, LEOS_RADIO_SX1262),
                    state->sx1262.hw.pin_nss,
                    state->sx1262.hw.pin_busy,
                    state->sx1262.hw.pin_reset,
                    state->sx1262.hw.pin_dio1);
    }
    if (state->config.sx1268_enabled)
    {
        status = leos_sx126x_init(LEOS_RADIO_SX1268, &state->sx1268.hw, &state->sx1268.cfg);
        if (status != LEOS_RADIO_OK)
        {
            log_message(LOG_LEVEL_ERROR, "init_state: leos_sx126x_init(%s) failed with %s (%d)",
                        radio_name(LEOS_RADIO_SX1268), radio_status_name(status), (int)status);
            return -1;
        }
        log_message(LOG_LEVEL_INFO,
                    "init_state: leos_sx126x_init(%s) succeeded [spi=%s nss=%u busy=%u reset=%u dio1=%u]",
                    radio_name(LEOS_RADIO_SX1268),
                    radio_spi_device_path(state, LEOS_RADIO_SX1268),
                    state->sx1268.hw.pin_nss,
                    state->sx1268.hw.pin_busy,
                    state->sx1268.hw.pin_reset,
                    state->sx1268.hw.pin_dio1);
    }
    if (state->config.sx1262_enabled)
    {
        status = leos_sx126x_start_rx(LEOS_RADIO_SX1262);
        if (status != LEOS_RADIO_OK)
        {
            log_message(LOG_LEVEL_ERROR, "init_state: leos_sx126x_start_rx(%s) failed with %s (%d)",
                        radio_name(LEOS_RADIO_SX1262), radio_status_name(status), (int)status);
            return -1;
        }
        log_message(LOG_LEVEL_INFO, "init_state: leos_sx126x_start_rx(%s) succeeded",
                    radio_name(LEOS_RADIO_SX1262));
    }
    if (state->config.sx1268_enabled)
    {
        status = leos_sx126x_start_rx(LEOS_RADIO_SX1268);
        if (status != LEOS_RADIO_OK)
        {
            log_message(LOG_LEVEL_ERROR, "init_state: leos_sx126x_start_rx(%s) failed with %s (%d)",
                        radio_name(LEOS_RADIO_SX1268), radio_status_name(status), (int)status);
            return -1;
        }
        log_message(LOG_LEVEL_INFO, "init_state: leos_sx126x_start_rx(%s) succeeded",
                    radio_name(LEOS_RADIO_SX1268));
    }

    if (pthread_create(&state->irq_thread, NULL, irq_thread_main, state) != 0)
    {
        log_errno_message(LOG_LEVEL_ERROR, "init_state IRQ thread creation");
        return -1;
    }

    log_message(LOG_LEVEL_INFO, "init_state: startup completed successfully");

    return 0;
}

static void cleanup_state(app_state_t *state)
{
    if (state->irq_thread != 0)
    {
        atomic_store(&state->stop_requested, true);
        wake_main_loop(state);
        (void)pthread_join(state->irq_thread, NULL);
        state->irq_thread = 0;
    }
    if (state->sx1268.dio1_event_buffer != NULL)
    {
        gpiod_edge_event_buffer_free(state->sx1268.dio1_event_buffer);
        state->sx1268.dio1_event_buffer = NULL;
    }
    if (state->sx1268.dio1_request != NULL)
    {
        gpiod_line_request_release(state->sx1268.dio1_request);
        state->sx1268.dio1_request = NULL;
    }
    if (state->sx1262.dio1_event_buffer != NULL)
    {
        gpiod_edge_event_buffer_free(state->sx1262.dio1_event_buffer);
        state->sx1262.dio1_event_buffer = NULL;
    }
    if (state->sx1262.dio1_request != NULL)
    {
        gpiod_line_request_release(state->sx1262.dio1_request);
        state->sx1262.dio1_request = NULL;
    }
    if (state->gpio_chip != NULL)
    {
        gpiod_chip_close(state->gpio_chip);
        state->gpio_chip = NULL;
    }
    if (state->client_fd >= 0)
    {
        close(state->client_fd);
        state->client_fd = -1;
    }
    if (state->server_fd >= 0)
    {
        close(state->server_fd);
        unlink(state->config.socket_path);
        state->server_fd = -1;
    }
    if (state->irq_pipe_read_fd >= 0)
    {
        close(state->irq_pipe_read_fd);
        state->irq_pipe_read_fd = -1;
    }
    if (state->irq_pipe_write_fd >= 0)
    {
        close(state->irq_pipe_write_fd);
        state->irq_pipe_write_fd = -1;
    }
    if (state->sx1262_spi_fd >= 0)
    {
        close(state->sx1262_spi_fd);
        state->sx1262_spi_fd = -1;
    }
    if (state->sx1268_spi_fd >= 0)
    {
        close(state->sx1268_spi_fd);
        state->sx1268_spi_fd = -1;
    }
}

int main(void)
{
    app_state_t state;

    memset(&state, 0, sizeof(state));
    state.sx1262_spi_fd = -1;
    state.sx1268_spi_fd = -1;
    state.server_fd = -1;
    state.client_fd = -1;
    state.irq_pipe_read_fd = -1;
    state.irq_pipe_write_fd = -1;
    load_config(&state.config);

    if (init_state(&state) != 0)
    {
        log_message(LOG_LEVEL_ERROR, "initialization failed");
        cleanup_state(&state);
        return 1;
    }

    for (;;)
    {
        struct pollfd fds[3];
        nfds_t nfds = 0u;
        int poll_rc;

        fds[nfds].fd = state.server_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        if (state.client_fd >= 0)
        {
            fds[nfds].fd = state.client_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        fds[nfds].fd = state.irq_pipe_read_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        poll_rc = poll(fds, nfds, app_poll_timeout_ms(&state));
        if (poll_rc < 0)
        {
            log_errno_message(LOG_LEVEL_ERROR, "main loop poll");
            break;
        }

        for (nfds_t i = 0; i < nfds; ++i)
        {
            if ((fds[i].revents & POLLIN) == 0)
            {
                continue;
            }

            if (fds[i].fd == state.server_fd)
            {
                (void)accept_client(&state);
                continue;
            }

            if ((state.client_fd >= 0) && (fds[i].fd == state.client_fd))
            {
                (void)handle_client_message(&state);
                continue;
            }

            if (fds[i].fd == state.irq_pipe_read_fd)
            {
                drain_irq_pipe(&state);
            }
        }

        (void)service_radio(&state, &state.sx1262);
        (void)service_radio(&state, &state.sx1268);
        maybe_log_heartbeat(&state);
    }

    cleanup_state(&state);
    return 1;
}
