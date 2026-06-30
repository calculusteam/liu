/*
 * Liu - Serial port implementation
 */
#include "ssh/serial.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <dirent.h>
#include <sys/ioctl.h>

struct SerialSession {
    int  fd;
    bool alive;
    SerialConfig config;
};

static speed_t baud_to_speed(u32 baud) {
    switch (baud) {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:     return B9600;
    }
}

SerialSession *serial_create(const SerialConfig *cfg) {
    SerialSession *ss = calloc(1, sizeof(SerialSession));
    if (!ss) return NULL;
    ss->config = *cfg;

    ss->fd = open(cfg->port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (ss->fd < 0) {
        free(ss);
        return NULL;
    }

    struct termios tio;
    /* Bail out if the fd isn't a real tty — otherwise tio stays uninitialized. */
    if (tcgetattr(ss->fd, &tio) != 0) {
        close(ss->fd);
        free(ss);
        return NULL;
    }

    /* Raw mode */
    cfmakeraw(&tio);

    /* Baud rate */
    speed_t speed = baud_to_speed(cfg->baud_rate);
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);

    /* Data bits */
    tio.c_cflag &= ~CSIZE;
    switch (cfg->data_bits) {
        case 5: tio.c_cflag |= CS5; break;
        case 6: tio.c_cflag |= CS6; break;
        case 7: tio.c_cflag |= CS7; break;
        default: tio.c_cflag |= CS8; break;
    }

    /* Stop bits */
    if (cfg->stop_bits == 2)
        tio.c_cflag |= CSTOPB;
    else
        tio.c_cflag &= ~CSTOPB;

    /* Parity */
    switch (cfg->parity) {
    case PARITY_NONE:
        tio.c_cflag &= ~PARENB;
        break;
    case PARITY_ODD:
        tio.c_cflag |= PARENB | PARODD;
        break;
    case PARITY_EVEN:
        tio.c_cflag |= PARENB;
        tio.c_cflag &= ~PARODD;
        break;
    }

    /* Flow control */
    switch (cfg->flow_control) {
    case FLOW_NONE:
        tio.c_cflag &= ~CRTSCTS;
        tio.c_iflag &= ~(IXON | IXOFF | IXANY);
        break;
    case FLOW_HARDWARE:
        tio.c_cflag |= CRTSCTS;
        break;
    case FLOW_SOFTWARE:
        tio.c_iflag |= IXON | IXOFF;
        break;
    }

    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    tcsetattr(ss->fd, TCSANOW, &tio);
    tcflush(ss->fd, TCIOFLUSH);

    ss->alive = true;
    return ss;
}

void serial_destroy(SerialSession *ss) {
    if (!ss) return;
    if (ss->fd >= 0) close(ss->fd);
    free(ss);
}

i32 serial_read(SerialSession *ss, u8 *buf, i32 buf_size) {
    ssize_t n = read(ss->fd, buf, (size_t)buf_size);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        ss->alive = false;
        return -1;
    }
    return (i32)n;
}

i32 serial_write(SerialSession *ss, const u8 *data, i32 len) {
    ssize_t n = write(ss->fd, data, (size_t)len);
    return n > 0 ? (i32)n : -1;
}

bool serial_is_alive(SerialSession *ss) {
    return ss && ss->alive;
}

i32 serial_list_ports(char (*names)[128], i32 max) {
    i32 count = 0;

#ifdef PLATFORM_MACOS
    DIR *dir = opendir("/dev");
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < max) {
        if (strncmp(entry->d_name, "tty.", 4) == 0 ||
            strncmp(entry->d_name, "cu.", 3) == 0) {
            snprintf(names[count], 128, "/dev/%s", entry->d_name);
            count++;
        }
    }
    closedir(dir);
#elif defined(PLATFORM_LINUX)
    DIR *dir = opendir("/dev");
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < max) {
        if (strncmp(entry->d_name, "ttyUSB", 6) == 0 ||
            strncmp(entry->d_name, "ttyACM", 6) == 0 ||
            strncmp(entry->d_name, "ttyS", 4) == 0) {
            snprintf(names[count], 128, "/dev/%s", entry->d_name);
            count++;
        }
    }
    closedir(dir);
#endif
    return count;
}

/* Pollable fd accessor. */
i32 serial_fd(SerialSession *ss) {
    return ss ? ss->fd : -1;
}
