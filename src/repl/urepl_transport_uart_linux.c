/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_transport_uart_linux.c — POSIX termios UART transport
 *
 * For the Linux host connecting to a real UART-on-USB device (e.g.
 * /dev/ttyUSB0, /dev/ttyACM0) — the inverse of the pty harness, where
 * the local process plays the embedded role.  Single-client (one open
 * file descriptor per server registration); the test pattern is to
 * call urepl_uart_linux_state_open("/dev/ttyUSB0", 115200) and pass
 * the returned state to urbi_repl_register_transport.
 *
 * Real-hardware exercise comes in v0.9.2 when the Pico USB CDC port
 * lands; for v0.9.1 this TU compiles and is callable but the canonical
 * end-to-end CI signal is the pty harness in urepl_transport_pty.c.
 *
 * Only compiled when URBI_ENABLE_REPL=1 AND on Linux.  Non-Linux
 * compilers see an empty TU; the symbol UREPL_UART_LINUX_TRANSPORT is
 * absent and the embedder can't link against it. */
#include "urbi/repl.h"

#if defined(__linux__) && defined(URBI_ENABLE_REPL)

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/* CRTSCTS (hardware flow-control flag) is POSIX XSI but gated behind
 * _GNU_SOURCE on Linux.  -std=c99 with -Wpedantic doesn't define it.
 * Provide a safe fallback: if the kernel header doesn't expose it via
 * the current feature-test set, use 0 (clear nothing extra). */
#ifndef CRTSCTS
#  define CRTSCTS 0
#endif

typedef struct UUartLinuxState {
    int  fd;
    bool accepted;
} UUartLinuxState;

/* Map an integer baud rate to its termios speed_t constant.  Returns
 * B0 (= 0) if the baud isn't one of the supported values; the caller
 * treats that as "open failed" and surfaces NULL. */
static speed_t
baud_to_termios(int baud)
{
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B0;
    }
}

UUartLinuxState *
urepl_uart_linux_state_open(const char *dev_path, int baud)
{
    if (dev_path == NULL) {
        return NULL;
    }
    speed_t s = baud_to_termios(baud);
    if (s == B0) {
        return NULL;
    }
    int fd = open(dev_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return NULL;
    }
    /* Configure 8N1, raw mode, hardware-flow-control off. */
    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) {
        (void)close(fd);
        return NULL;
    }
    cfmakeraw(&tio);
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, s);
    cfsetospeed(&tio, s);
    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        (void)close(fd);
        return NULL;
    }
    UUartLinuxState *st = (UUartLinuxState *)calloc(1, sizeof(*st));
    if (st == NULL) {
        (void)close(fd);
        return NULL;
    }
    st->fd = fd;
    return st;
}

void
urepl_uart_linux_state_close(UUartLinuxState *st)
{
    if (st == NULL) {
        return;
    }
    if (st->fd >= 0) {
        (void)close(st->fd);
        st->fd = -1;
    }
    free(st);
}

static int
uart_linux_accept(void *listener_state, int *out_client_fd)
{
    UUartLinuxState *st = (UUartLinuxState *)listener_state;
    if (st == NULL || out_client_fd == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    if (st->accepted) {
        return -1;  /* single-client */
    }
    st->accepted = true;
    *out_client_fd = st->fd;
    return 0;
}

static int
uart_linux_read(int client_fd, void *buf, size_t n)
{
    ssize_t r = read(client_fd, buf, n);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return -1;
        }
        return -errno;
    }
    return (int)r;
}

static int
uart_linux_write(int client_fd, const void *buf, size_t n)
{
    ssize_t w = write(client_fd, buf, n);
    if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return -1;
        }
        return -errno;
    }
    return (int)w;
}

static void
uart_linux_close(int client_fd)
{
    (void)client_fd;  /* lifecycle on UUartLinuxState */
}

static int
uart_linux_pollable_fd(int client_fd)
{
    return client_fd;
}

const UTransport UREPL_UART_LINUX_TRANSPORT = {
    .name           = "uart-linux",
    .accept_fn      = uart_linux_accept,
    .read_fn        = uart_linux_read,
    .write_fn       = uart_linux_write,
    .close_fn       = uart_linux_close,
    .pollable_fd_fn = uart_linux_pollable_fd
};

#endif /* __linux__ && URBI_ENABLE_REPL */
