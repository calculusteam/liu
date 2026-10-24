/*
 * Liu - Serial port connection
 * Cross-platform RS-232 serial terminal.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include "core/types.h"

typedef enum {
    PARITY_NONE,
    PARITY_ODD,
    PARITY_EVEN,
} SerialParity;

typedef enum {
    FLOW_NONE,
    FLOW_HARDWARE,  /* RTS/CTS */
    FLOW_SOFTWARE,  /* XON/XOFF */
} SerialFlow;

typedef struct {
    char         port[128];    /* /dev/ttyUSB0, COM3, etc. */
    u32          baud_rate;    /* 9600, 115200, etc. */
    u8           data_bits;    /* 5,6,7,8 */
    u8           stop_bits;    /* 1,2 */
    SerialParity parity;
    SerialFlow   flow_control;
} SerialConfig;

typedef struct SerialSession SerialSession;

SerialSession *serial_create(const SerialConfig *cfg);
void           serial_destroy(SerialSession *ss);
i32            serial_read(SerialSession *ss, u8 *buf, i32 buf_size);
i32            serial_write(SerialSession *ss, const u8 *data, i32 len);
bool           serial_is_alive(SerialSession *ss);
/* Pollable fd for the read path. */
i32            serial_fd(SerialSession *ss);

/* List available serial ports. Returns count, fills names array. */
i32 serial_list_ports(char (*names)[128], i32 max);

#endif
