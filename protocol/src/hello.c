/*
 * DroidScreen Protocol - Wireless hello serialization
 */

#include "droidscreen/hello.h"

#include <string.h>

size_t ds_hello_serialize(uint8_t *buf, const ds_hello_t *hello)
{
    memcpy(buf, hello->desktop_id, DS_HELLO_ID_SIZE);
    memcpy(buf + DS_HELLO_ID_SIZE, hello->token, DS_HELLO_TOKEN_SIZE);
    memset(buf + DS_HELLO_ID_SIZE + DS_HELLO_TOKEN_SIZE, 0, DS_HELLO_NAME_SIZE);
    size_t name_len = strnlen(hello->name, DS_HELLO_NAME_SIZE - 1);
    memcpy(buf + DS_HELLO_ID_SIZE + DS_HELLO_TOKEN_SIZE, hello->name, name_len);
    return DS_HELLO_SIZE;
}

size_t ds_hello_deserialize(const uint8_t *buf, ds_hello_t *hello)
{
    memcpy(hello->desktop_id, buf, DS_HELLO_ID_SIZE);
    memcpy(hello->token, buf + DS_HELLO_ID_SIZE, DS_HELLO_TOKEN_SIZE);
    memcpy(hello->name, buf + DS_HELLO_ID_SIZE + DS_HELLO_TOKEN_SIZE, DS_HELLO_NAME_SIZE);
    hello->name[DS_HELLO_NAME_SIZE - 1] = '\0';
    return DS_HELLO_SIZE;
}

size_t ds_hello_resp_serialize(uint8_t *buf, const ds_hello_resp_t *resp)
{
    buf[0] = resp->status;
    memcpy(buf + 1, resp->token, DS_HELLO_TOKEN_SIZE);
    return DS_HELLO_RESP_SIZE;
}

size_t ds_hello_resp_deserialize(const uint8_t *buf, ds_hello_resp_t *resp)
{
    resp->status = buf[0];
    memcpy(resp->token, buf + 1, DS_HELLO_TOKEN_SIZE);
    return DS_HELLO_RESP_SIZE;
}
