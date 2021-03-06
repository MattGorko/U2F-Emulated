#include <err.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "frame.h"
#include "raw_message.h"
#include "authenticate.h"
#include "register.h"

#include "../u2f-hid/commands.h"
#include "../u2f-hid/packet.h"
#include "../u2f-hid/message.h"

#include "../crypto.h"
#include "../utils/xalloc.h"
#include "authenticate.h"


/**
** \brief Add user precense to the authentification response
**
** \param response The response
** \param presence The presence
*/
static void authenticate_response_user_pre(struct message *response,
    uint8_t presence)
{
    /* Add  to response */
    message_add_data(response, &presence, sizeof(presence));

    /* Log */
    dump_bytes("User precense", &presence, sizeof(presence));
}

/**
** \brief Add counter to the authentification response
**
** \param response The response
** \param counter The counter
*/
static void authenticate_response_counter(struct message *response,
    uint32_t counter)
{
    /* Counter */
    uint8_t counter_buffer[sizeof(uint32_t)];

    /* Fill it */
    counter_buffer[0] = counter & 0xFF;
    counter_buffer[1] = (counter >> 8) & 0xFF;
    counter_buffer[2] = (counter >> 16) & 0xFF;
    counter_buffer[3] = (counter >> 24) & 0xFF;

    /* Add to response */
    message_add_data(response, counter_buffer, sizeof(uint32_t));
    dump_bytes("counter", counter_buffer, sizeof(uint32_t));
}

/**
** \brief Add the signature to the authentification response
**
** \param response The response
** \param key The key used to sign
** \param params The authentification params
** \param presence The user presence
** \param counter The counter
*/
static void authenticate_response_signature(struct message *response,
    EC_KEY *key,
    const struct authentification_params *params,
    uint8_t presence,
    uint32_t counter
)
{
    /* Signature */
    size_t buffer_to_sign_size =
        U2F_APP_PARAM_SIZE
        + sizeof(presence)
        + sizeof(counter)
        + U2F_CHA_PARAM_SIZE;

    /* Buffer to sign */
    uint8_t *buffer_to_sign = xmalloc(buffer_to_sign_size);

    /* Fill */
    size_t index = 0;
    /* App Param */
    memcpy(buffer_to_sign + index,
        &params->application_param,
        U2F_APP_PARAM_SIZE);
    index += U2F_APP_PARAM_SIZE;

    /* User precense */
    buffer_to_sign[index] = presence;
    index += sizeof(presence);

    /* Counter */
    uint8_t counter_buffer[sizeof(uint32_t)];
    /* Fill it */
    counter_buffer[0] = counter & 0xFF;
    counter_buffer[1] = (counter >> 8) & 0xFF;
    counter_buffer[2] = (counter >> 16) & 0xFF;
    counter_buffer[3] = (counter >> 24) & 0xFF;
    /* Add it */
    memcpy(buffer_to_sign + index,
        counter_buffer,
        sizeof(uint32_t));
    index += sizeof(uint32_t);

    /* Challenge Param */
    memcpy(buffer_to_sign + index,
        &params->challenge_param,
        U2F_CHA_PARAM_SIZE);
    index += U2F_CHA_PARAM_SIZE;

    /* Digest */
    uint8_t *digest = NULL;
    size_t digest_len =
        crypto_hash(buffer_to_sign, buffer_to_sign_size, &digest);

    /* Sign */
    uint8_t *signature_buffer = NULL;
    size_t signature_len =
        crypto_ec_sign_with_key(key,
                                digest,
                                digest_len,
                                &signature_buffer);

    /* Add it */
    message_add_data(response, signature_buffer, signature_len);

    /* Log */
    dump_bytes("Signature", signature_buffer, signature_len);

    /* Free */
    free(buffer_to_sign);
    free(digest);
    free(signature_buffer);
}

/**
** \brief Add status code to the authentification response
**
** \param response The response
** \param status The status code
*/
static void authenticate_response_sw(struct message *response,
    uint32_t status)
{
    /* SW */
    uint8_t sw[2] = { status >> 8, status & 0xFF };

    /* Add to response */
    message_add_data(response, sw, 2);

    /* Log */
    dump_bytes("SW", sw, 2);
}

/**
** \brief Get the ciphered key handle from the request
**
** \param request The request
** \param params The authentification response
** \param size The ref size of the ciphered key handle
** \return The ciphered key handle
*/
static uint8_t *authenticate_get_key_handle_cipher(
    const struct message *request,
    const struct authentification_params *params,
    uint8_t *size)
{
    /* Offset */
    size_t offset = U2F_APDU_HEADER_SIZE
        + U2F_APP_PARAM_SIZE
        + U2F_CHA_PARAM_SIZE
        + sizeof(params->key_handle_size);

    /* Size */
    *size = params->key_handle_size;

    /* Allocate */
    uint8_t *key_handle_cipher = xmalloc(params->key_handle_size);

    /* Get key handle cipher */
    message_read(request,
                key_handle_cipher,
                offset,
                params->key_handle_size);

    /* Log */
    dump_bytes("key_handle_cipher", key_handle_cipher,
        params->key_handle_size);

    return key_handle_cipher;
}

/**
** \brief Decrypt ciphered key handle
**
** \param key_handle_cipher The ciphered key handle
** \param key_handle_cipher_size The ciphered key handle size
** \param size The ref size of the plain key handle
** \return The plain key handle
*/
static uint8_t *authenticate_decrypt_key_handle_cipher(
    const uint8_t *key_handle_cipher,
    size_t key_handle_cipher_size,
    size_t *size)
{
    /* Cipher Key handle */
    uint8_t *key_handle = NULL;
    size_t key_handle_size = crypto_aes_decrypt(
            key_handle_cipher,
            key_handle_cipher_size,
            &key_handle);

    /* Size */
    *size = key_handle_size;

    /* Log */
    dump_bytes("Key handle size", (uint8_t *)size, sizeof(size));
    dump_bytes("Key handle", key_handle, key_handle_size);

    return key_handle;
}

/**
** \brief Get the pubkey from the key handle
**
** \param key_handle The plain key handle
** \param key_handle_size The plain key handle size
** \return The pubkey
*/
static EC_KEY *authenticate_get_pubkey_from_key_handle(
    const uint8_t *key_handle, size_t key_handle_size)
{

    /* Privkey */
    size_t privkey_size = key_handle_size - U2F_APP_PARAM_SIZE;
    EC_KEY *key = crypto_ec_bytes_to_key(key_handle, privkey_size);

    /* Log */
    dump_bytes("Privkey", key_handle, privkey_size);

    return key;
}

/**
** \brief Handle check authentification request
**
** \param request The check authentification request message
** \return The response
*/
static struct message *raw_authenticate_check(
    const struct message *request)
{
    /* Log */
    fprintf(stderr, "           Check\n");

    /* Request */
    struct authentification_params params;
    message_read(request, (uint8_t *)&params,
                U2F_APDU_HEADER_SIZE,
                sizeof(struct authentification_params));

    /* Response */
    struct message *response =
        message_new_blank(request->init_packet->cid, CMD_MSG);

    /* Key handle ciphered */
    uint8_t key_handle_cipher_size = 0;
    uint8_t *key_handle_cipher =
        authenticate_get_key_handle_cipher(request,
            &params, &key_handle_cipher_size);

    /* Key handle decrypt */
    size_t key_handle_size = 0;
    uint8_t *key_handle = authenticate_decrypt_key_handle_cipher(
        key_handle_cipher,
        key_handle_cipher_size,
        &key_handle_size
    );

    /* Privkey */
    size_t privkey_size = key_handle_size - U2F_APP_PARAM_SIZE;
    dump_bytes("App Param Key:", key_handle + privkey_size,
        U2F_APP_PARAM_SIZE);
    dump_bytes("App Param Client:", params.application_param,
        U2F_APP_PARAM_SIZE);

    if (memcmp(key_handle + privkey_size,
        params.application_param, U2F_APP_PARAM_SIZE) != 0)
    {
        /* Log */
        warnx("Mismatch in App Param");

        /* SW */
        authenticate_response_sw(response, SW_WRONG_DATA);
    }
    else
        authenticate_response_sw(response,
            SW_CONDITIONS_NOT_SATISFIED);

    /* Dump request */
    size_t request_buffer_size =
        packet_init_get_bcnt(request->init_packet);
    uint8_t *request_buffer = xmalloc(request_buffer_size);
    message_read(request, request_buffer, 0, request_buffer_size);
    dump_bytes("Message", request_buffer, request_buffer_size);

    /* Dump response */
    size_t response_buffer_size =
        packet_init_get_bcnt(response->init_packet);
    uint8_t *response_buffer = xmalloc(response_buffer_size);
    message_read(response, response_buffer, 0, response_buffer_size);
    dump_bytes("Message", response_buffer, response_buffer_size);

    /* Free */
    free(key_handle_cipher);
    free(key_handle);
    free(request_buffer);
    free(response_buffer);

    return response;
}

/**
** \brief Handle enforce authentification request
**
** \param request The enforce authentification request message
** \return The response
*/
static struct message *raw_authenticate_enforce(
    const struct message *request)
{
    /* Log */
    fprintf(stderr, "           Enforce\n");

    /* Request */
    struct authentification_params params;
    message_read(request, (uint8_t *)&params,
                U2F_APDU_HEADER_SIZE,
                sizeof(struct authentification_params));

    /* Response */
    struct message *response =
        message_new_blank(request->init_packet->cid, CMD_MSG);

    /* Key handle ciphered */
    uint8_t key_handle_cipher_size = 0;
    uint8_t *key_handle_cipher =
        authenticate_get_key_handle_cipher(request,
            &params, &key_handle_cipher_size);

    /* Key handle decrypt */
    size_t key_handle_size = 0;
    uint8_t *key_handle = authenticate_decrypt_key_handle_cipher(
        key_handle_cipher,
        params.key_handle_size,
        &key_handle_size
    );

    /* Privkey */
    EC_KEY *key  = authenticate_get_pubkey_from_key_handle(
        key_handle, key_handle_size);

    /* User precense */
    authenticate_response_user_pre(response, true);

    /* Counter */
    authenticate_response_counter(response, 1);

    /* Signature */
    authenticate_response_signature(response,
        key,
        &params,
        1,
        1);

    /* SW */
    authenticate_response_sw(response, SW_NO_ERROR);

    /* Dump request */
    size_t request_buffer_size =
        packet_init_get_bcnt(request->init_packet);
    uint8_t *request_buffer = xmalloc(request_buffer_size);
    message_read(request, request_buffer, 0, request_buffer_size);
    dump_bytes("Message", request_buffer, request_buffer_size);

    /* Dump response */
    size_t response_buffer_size =
        packet_init_get_bcnt(response->init_packet);
    uint8_t *response_buffer = xmalloc(response_buffer_size);
    message_read(response, response_buffer, 0, response_buffer_size);
    dump_bytes("Message", response_buffer, response_buffer_size);

    /* Free */
    free(key_handle_cipher);
    free(key_handle);
    EC_KEY_free(key);
    free(request_buffer);
    free(response_buffer);

    return response;
}

/**
** \brief Handle no enforce authentification request
**
** \param request The no enforce authentification request message
** \return The response
*/
static struct message *raw_authenticate_no_enforce(
    const struct message *request)
{
    /* Log */
    fprintf(stderr, "           No enforce\n");

    (void)request;
    return NULL;
}

struct message *raw_authenticate_handler(
    const struct message *request)
{
    /* Log */
    fprintf(stderr, "       Authenticate\n");

    /* Get frame header */
    struct frame_header *header = (struct frame_header *)
                                      request->init_packet->data;

    /* handle request based on type */
    switch (header->p1)
    {
    case U2F_AUTH_CHECK:
        return raw_authenticate_check(request);
    case U2F_AUTH_ENFORCE:
        return raw_authenticate_enforce(request);
    case U2F_AUTH_NO_ENFORCE:
        return raw_authenticate_no_enforce(request);
    default:
        warnx("Unknow authentification type: %d", header->p1);
        return NULL;
    }
    /* Should not be ewecuted */

    return NULL;
}
