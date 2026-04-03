#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "leos/sx126x.h"

#define IPC_MSG_RX_PACKET    0x01u
#define IPC_MSG_BUTTON_EVENT 0x02u
#define IPC_MSG_TX_PACKET    0x03u
#define IPC_MSG_TX_RESULT    0x04u

#define IPC_TX_STATUS_OK           0x00u
#define IPC_TX_STATUS_UNSUPPORTED  0x01u
#define IPC_TX_STATUS_RADIO_ERROR  0x02u

#define DEFAULT_SOCKET_PATH   "/tmp/leos-radio.sock"
#define DEFAULT_SPI_DEVICE    "/dev/spidev0.0"
#define DEFAULT_GPIO_CHIP     "/dev/gpiochip0"
#define DEFAULT_SPI_BAUD_HZ   8000000u

#define DEFAULT_SX1262_NSS    13u
#define DEFAULT_SX1262_BUSY   19u
#define DEFAULT_SX1262_RESET  15u
#define DEFAULT_SX1262_DIO1   17u

#define DEFAULT_SX1268_NSS    14u
#define DEFAULT_SX1268_BUSY   18u
#define DEFAULT_SX1268_RESET  15u
#define DEFAULT_SX1268_DIO1   16u

typedef struct
{
    const char *socket_path;
    const char *spi_device;
    const char *gpio_chip_path;
    uint32_t spi_baud_hz;
    uint32_t sx1262_nss;
    uint32_t sx1262_busy;
    uint32_t sx1262_reset;
    uint32_t sx1262_dio1;
    uint32_t sx1268_nss;
    uint32_t sx1268_busy;
    uint32_t sx1268_reset;
    uint32_t sx1268_dio1;
} app_config_t;

typedef struct
{
    leos_radio_t radio;
    struct gpiod_line *dio1_line;
    bool irq_pending;
    leos_radio_hw_config_t hw;
    leos_radio_config_t cfg;
} radio_state_t;

typedef struct
{
    app_config_t config;
    int spi_fd;
    int server_fd;
    int client_fd;
    struct gpiod_chip *gpio_chip;
    radio_state_t sx1262;
    radio_state_t sx1268;
} app_state_t;

static void log_error(const char *message)
{
    fprintf(stderr, "radio_receiver: %s\n", message);
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
    cfg->socket_path = env_str("RADIO_SOCKET_PATH", DEFAULT_SOCKET_PATH);
    cfg->spi_device = env_str("RADIO_SPI_DEVICE", DEFAULT_SPI_DEVICE);
    cfg->gpio_chip_path = env_str("RADIO_GPIO_CHIP", DEFAULT_GPIO_CHIP);
    cfg->spi_baud_hz = env_u32("RADIO_SPI_BAUD_HZ", DEFAULT_SPI_BAUD_HZ);

    cfg->sx1262_nss = env_u32("RADIO_SX1262_NSS_LINE", DEFAULT_SX1262_NSS);
    cfg->sx1262_busy = env_u32("RADIO_SX1262_BUSY_LINE", DEFAULT_SX1262_BUSY);
    cfg->sx1262_reset = env_u32("RADIO_SX1262_RESET_LINE", DEFAULT_SX1262_RESET);
    cfg->sx1262_dio1 = env_u32("RADIO_SX1262_DIO1_LINE", DEFAULT_SX1262_DIO1);

    cfg->sx1268_nss = env_u32("RADIO_SX1268_NSS_LINE", DEFAULT_SX1268_NSS);
    cfg->sx1268_busy = env_u32("RADIO_SX1268_BUSY_LINE", DEFAULT_SX1268_BUSY);
    cfg->sx1268_reset = env_u32("RADIO_SX1268_RESET_LINE", DEFAULT_SX1268_RESET);
    cfg->sx1268_dio1 = env_u32("RADIO_SX1268_DIO1_LINE", DEFAULT_SX1268_DIO1);
}

static void build_radio_config(leos_radio_t radio, leos_radio_config_t *cfg)
{
    leos_sx126x_get_default_config(radio, cfg);

    if (radio == LEOS_RADIO_SX1262)
    {
        cfg->rf_frequency_hz = 915000000u;
        cfg->tx_power_dbm = 14;
        cfg->crc_enabled = true;
        cfg->iq_inverted = false;
        cfg->bandwidth = LEOS_RADIO_BW_125_KHZ;
        cfg->coding_rate = LEOS_RADIO_CR_4_5;
        cfg->spreading_factor = LEOS_RADIO_SF_9;
        cfg->sync_word = 0x12u;
    }
    else
    {
        cfg->rf_frequency_hz = 435000000u;
        cfg->tx_power_dbm = 14;
        cfg->crc_enabled = true;
        cfg->iq_inverted = false;
        cfg->bandwidth = LEOS_RADIO_BW_500_KHZ;
        cfg->coding_rate = LEOS_RADIO_CR_4_5;
        cfg->spreading_factor = LEOS_RADIO_SF_7;
        cfg->sync_word = 0x12u;
    }
}

static void build_radio_hw(app_state_t *state, radio_state_t *radio)
{
    memset(&radio->hw, 0, sizeof(radio->hw));
    radio->hw.platform_spi = &state->spi_fd;
    radio->hw.spi_baud_hz = state->config.spi_baud_hz;
    radio->hw.pin_sck = 10u;
    radio->hw.pin_mosi = 11u;
    radio->hw.pin_miso = 12u;

    if (radio->radio == LEOS_RADIO_SX1262)
    {
        radio->hw.pin_nss = state->config.sx1262_nss;
        radio->hw.pin_busy = state->config.sx1262_busy;
        radio->hw.pin_reset = state->config.sx1262_reset;
        radio->hw.pin_dio1 = state->config.sx1262_dio1;
    }
    else
    {
        radio->hw.pin_nss = state->config.sx1268_nss;
        radio->hw.pin_busy = state->config.sx1268_busy;
        radio->hw.pin_reset = state->config.sx1268_reset;
        radio->hw.pin_dio1 = state->config.sx1268_dio1;
    }
}

static int open_spi_device(const char *path)
{
    return open(path, O_RDWR | O_CLOEXEC);
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
        close(state->client_fd);
        state->client_fd = -1;
        return -1;
    }

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

static int emit_rx_packet(app_state_t *state, leos_radio_t radio)
{
    uint8_t message[2 + 255];
    size_t rx_len = 0u;
    leos_radio_packet_info_t info;
    leos_radio_status_t status;

    memset(&info, 0, sizeof(info));

    message[0] = IPC_MSG_RX_PACKET;
    message[1] = (uint8_t)radio;

    status = leos_sx126x_read(radio, &message[2], 255u, &rx_len, &info);
    if (status != LEOS_RADIO_OK)
    {
        return -1;
    }

    return send_ipc_message(state, message, rx_len + 2u);
}

static int service_radio_irq(app_state_t *state, radio_state_t *radio)
{
    leos_radio_status_t status;

    radio->irq_pending = false;
    leos_sx126x_handle_dio1_irq(radio->radio);

    status = leos_sx126x_process_irq(radio->radio);
    if (status != LEOS_RADIO_OK)
    {
        return -1;
    }

    if (leos_sx126x_packet_available(radio->radio))
    {
        if (emit_rx_packet(state, radio->radio) != 0)
        {
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
    }
    state->client_fd = client_fd;
    return 0;
}

static int handle_client_message(app_state_t *state)
{
    uint8_t buf[2 + 255];
    ssize_t len;
    leos_radio_t radio;

    len = recv(state->client_fd, buf, sizeof(buf), 0);
    if (len <= 0)
    {
        close(state->client_fd);
        state->client_fd = -1;
        return 0;
    }

    if ((size_t)len < 2u)
    {
        return 0;
    }

    if (buf[0] != IPC_MSG_TX_PACKET)
    {
        return 0;
    }

    radio = (buf[1] == (uint8_t)LEOS_RADIO_SX1268) ? LEOS_RADIO_SX1268 : LEOS_RADIO_SX1262;

    /*
     * TX IPC is reserved for future use. RX is the only active workflow in
     * this implementation, so return a minimal unsupported status for now.
     */
    return send_tx_result(state, radio, IPC_TX_STATUS_UNSUPPORTED);
}

static int request_dio1_line(app_state_t *state, radio_state_t *radio)
{
    radio->dio1_line = gpiod_chip_get_line(state->gpio_chip, (unsigned int)radio->hw.pin_dio1);
    if (radio->dio1_line == NULL)
    {
        return -1;
    }

    return gpiod_line_request_rising_edge_events(radio->dio1_line, "leos_radio_receiver");
}

static int init_state(app_state_t *state)
{
    state->spi_fd = open_spi_device(state->config.spi_device);
    if (state->spi_fd < 0)
    {
        return -1;
    }

    state->server_fd = open_server_socket(state->config.socket_path);
    if (state->server_fd < 0)
    {
        return -1;
    }

    state->gpio_chip = gpiod_chip_open(state->config.gpio_chip_path);
    if (state->gpio_chip == NULL)
    {
        return -1;
    }

    state->sx1262.radio = LEOS_RADIO_SX1262;
    state->sx1268.radio = LEOS_RADIO_SX1268;

    build_radio_hw(state, &state->sx1262);
    build_radio_hw(state, &state->sx1268);
    build_radio_config(LEOS_RADIO_SX1262, &state->sx1262.cfg);
    build_radio_config(LEOS_RADIO_SX1268, &state->sx1268.cfg);

    if (request_dio1_line(state, &state->sx1262) != 0)
    {
        return -1;
    }
    if (request_dio1_line(state, &state->sx1268) != 0)
    {
        return -1;
    }

    if (leos_sx126x_init(LEOS_RADIO_SX1262, &state->sx1262.hw, &state->sx1262.cfg) != LEOS_RADIO_OK)
    {
        return -1;
    }
    if (leos_sx126x_init(LEOS_RADIO_SX1268, &state->sx1268.hw, &state->sx1268.cfg) != LEOS_RADIO_OK)
    {
        return -1;
    }
    if (leos_sx126x_start_rx(LEOS_RADIO_SX1262) != LEOS_RADIO_OK)
    {
        return -1;
    }
    if (leos_sx126x_start_rx(LEOS_RADIO_SX1268) != LEOS_RADIO_OK)
    {
        return -1;
    }

    return 0;
}

static void cleanup_state(app_state_t *state)
{
    if (state->sx1268.dio1_line != NULL)
    {
        gpiod_line_release(state->sx1268.dio1_line);
        state->sx1268.dio1_line = NULL;
    }
    if (state->sx1262.dio1_line != NULL)
    {
        gpiod_line_release(state->sx1262.dio1_line);
        state->sx1262.dio1_line = NULL;
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
    if (state->spi_fd >= 0)
    {
        close(state->spi_fd);
        state->spi_fd = -1;
    }
}

int main(void)
{
    app_state_t state;

    memset(&state, 0, sizeof(state));
    state.spi_fd = -1;
    state.server_fd = -1;
    state.client_fd = -1;
    load_config(&state.config);

    if (init_state(&state) != 0)
    {
        log_error("initialization failed");
        cleanup_state(&state);
        return 1;
    }

    for (;;)
    {
        struct pollfd fds[4];
        nfds_t nfds = 0u;
        int sx1262_fd = gpiod_line_event_get_fd(state.sx1262.dio1_line);
        int sx1268_fd = gpiod_line_event_get_fd(state.sx1268.dio1_line);
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

        fds[nfds].fd = sx1262_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        fds[nfds].fd = sx1268_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        poll_rc = poll(fds, nfds, -1);
        if (poll_rc < 0)
        {
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

            if (fds[i].fd == sx1262_fd)
            {
                struct gpiod_line_event event;
                if (gpiod_line_event_read(state.sx1262.dio1_line, &event) == 0)
                {
                    state.sx1262.irq_pending = true;
                }
                continue;
            }

            if (fds[i].fd == sx1268_fd)
            {
                struct gpiod_line_event event;
                if (gpiod_line_event_read(state.sx1268.dio1_line, &event) == 0)
                {
                    state.sx1268.irq_pending = true;
                }
            }
        }

        if (state.sx1262.irq_pending)
        {
            (void)service_radio_irq(&state, &state.sx1262);
        }
        if (state.sx1268.irq_pending)
        {
            (void)service_radio_irq(&state, &state.sx1268);
        }
    }

    cleanup_state(&state);
    return 1;
}
