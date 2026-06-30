/*
 * Liu - Telnet protocol (RFC 854/855/1073/1091)
 * Full option negotiation, NAWS, terminal type.
 */
#ifndef TELNET_H
#define TELNET_H

#include "core/types.h"

/* Telnet commands */
#define TEL_IAC   255  /* Interpret As Command */
#define TEL_DONT  254
#define TEL_DO    253
#define TEL_WONT  252
#define TEL_WILL  251
#define TEL_SB    250  /* Subnegotiation Begin */
#define TEL_GA    249  /* Go Ahead */
#define TEL_EL    248  /* Erase Line */
#define TEL_EC    247  /* Erase Character */
#define TEL_AYT   246  /* Are You There */
#define TEL_AO    245  /* Abort Output */
#define TEL_IP    244  /* Interrupt Process */
#define TEL_BRK   243  /* Break */
#define TEL_SE    240  /* Subnegotiation End */
#define TEL_NOP   241

/* Telnet options */
#define TELOPT_ECHO          1
#define TELOPT_SGA           3   /* Suppress Go Ahead */
#define TELOPT_TTYPE        24   /* Terminal Type */
#define TELOPT_NAWS         31   /* Window Size */
#define TELOPT_TSPEED       32   /* Terminal Speed */
#define TELOPT_LINEMODE     34
#define TELOPT_NEW_ENVIRON  39

typedef struct TelnetSession TelnetSession;

/* Create telnet session — connects TCP to host:port */
TelnetSession *telnet_create(const char *host, i32 port, i32 cols, i32 rows);

/* Destroy and close */
void telnet_destroy(TelnetSession *ts);

/* Non-blocking read from server. Returns bytes of user data placed in buf. */
i32 telnet_read(TelnetSession *ts, u8 *buf, i32 buf_size);

/* Write raw user data to server (escapes IAC bytes). */
i32 telnet_write(TelnetSession *ts, const u8 *data, i32 len);

/* Notify server of window size change */
void telnet_resize(TelnetSession *ts, i32 cols, i32 rows);

/* Is connected? */
bool telnet_is_alive(TelnetSession *ts);

/* Get underlying socket fd (for poll) */
i32 telnet_fd(TelnetSession *ts);

#endif /* TELNET_H */
