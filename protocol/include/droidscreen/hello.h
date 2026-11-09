/*
 * DroidScreen Protocol - Wireless hello (desktop identification + pairing)
 *
 * Connections that arrive over the network (not through the adb loopback
 * tunnel) must start with DS_MSG_HELLO before the handshake. The tablet uses
 * it to decide whether the desktop is trusted:
 *
 *   - known desktop_id + matching token  -> accepted immediately
 *   - otherwise                          -> the user is asked on the tablet;
 *                                           on approval a fresh token is issued
 *
 * The desktop stores the returned token and presents it on later connections.
 */

#ifndef DROIDSCREEN_HELLO_H
#define DROIDSCREEN_HELLO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS_HELLO_ID_SIZE      16
#define DS_HELLO_TOKEN_SIZE   16
#define DS_HELLO_NAME_SIZE    64   /* UTF-8, NUL-padded */

#define DS_HELLO_SIZE       (DS_HELLO_ID_SIZE + DS_HELLO_TOKEN_SIZE + DS_HELLO_NAME_SIZE)
#define DS_HELLO_RESP_SIZE  (1 + DS_HELLO_TOKEN_SIZE)

/* How long the tablet waits for the user to approve a new desktop. The
 * desktop must wait at least this long for DS_MSG_HELLO_RESP. */
#define DS_HELLO_APPROVAL_TIMEOUT_MS  60000

typedef enum {
    DS_HELLO_ACCEPTED = 0,
    DS_HELLO_REJECTED = 1
} ds_hello_status_t;

typedef struct {
    uint8_t desktop_id[DS_HELLO_ID_SIZE];
    uint8_t token[DS_HELLO_TOKEN_SIZE];      /* all zero when not paired yet */
    char    name[DS_HELLO_NAME_SIZE];        /* always NUL-terminated after deserialize */
} ds_hello_t;

typedef struct {
    uint8_t status;                          /* ds_hello_status_t */
    uint8_t token[DS_HELLO_TOKEN_SIZE];      /* valid when status == ACCEPTED */
} ds_hello_resp_t;

size_t ds_hello_serialize(uint8_t *buf, const ds_hello_t *hello);
size_t ds_hello_deserialize(const uint8_t *buf, ds_hello_t *hello);
size_t ds_hello_resp_serialize(uint8_t *buf, const ds_hello_resp_t *resp);
size_t ds_hello_resp_deserialize(const uint8_t *buf, ds_hello_resp_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_HELLO_H */
