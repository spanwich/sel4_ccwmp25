/* SPDX-License-Identifier: BSD-2-Clause */
#include "stage.h"

#include "gateway_abi.h"

__attribute__((section(".demo_config"))) DemoConfig demo_config;

static const char *reason_phrase(uint16_t status)
{
    switch (status) {
    case 200u: return "OK";
    case 400u: return "Bad Request";
    case 401u: return "Unauthorized";
    case 403u: return "Forbidden";
    case 404u: return "Not Found";
    case 413u: return "Content Too Large";
    case 429u: return "Too Many Requests";
    case 503u: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static size_t append(uint8_t *output, size_t offset, const char *text)
{
    const size_t length = strlen(text);
    memcpy(output + offset, text, length);
    return offset + length;
}

static size_t append_u32(uint8_t *output, size_t offset, uint32_t value)
{
    char digits[10];
    size_t index = sizeof(digits);
    do {
        digits[--index] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    const size_t length = sizeof(digits) - index;
    memcpy(output + offset, digits + index, length);
    return offset + length;
}

static void resource_response(DemoMessage *message)
{
    if (message->kind != DEMO_MESSAGE_SECURITY_PARAMS ||
        message->length < SECURITY_PARAMS_HEADER_SIZE) {
        return;
    }
    const SecurityParamsWire *wire =
        (const SecurityParamsWire *)(const void *)message->payload;
    if (wire->path_id == PATH_STATUS) {
        demo_set_body_response(
            message, 200u,
            wire->role == ROLE_ADMIN
                ? "{\"status\":\"ok\",\"role\":\"ADMIN\"}"
                : "{\"status\":\"ok\",\"role\":\"OPERATOR\"}");
    } else if (wire->path_id == PATH_LOGOUT) {
        demo_set_body_response(message, 200u,
            "{\"status\":\"ok\",\"message\":\"logged out\"}");
    } else if (wire->path_id == PATH_DEMO) {
        demo_set_body_response(message, 200u,
            "{\"status\":\"ok\",\"resource\":\"demo\"}");
    } else if (wire->path_id == PATH_REPORTS) {
        demo_set_body_response(message, 200u,
            "{\"status\":\"ok\",\"resource\":\"reports\"}");
    } else {
        demo_set_body_response(message, 200u, "{\"status\":\"ok\"}");
    }
}

static void format_http(DemoMessage *message)
{
    uint8_t response[DEMO_PAYLOAD_BYTES];
    if (message->kind != DEMO_MESSAGE_BODY_RESPONSE ||
        message->length > DEMO_PAYLOAD_BYTES) {
        return;
    }
    const uint32_t body_length = message->length;
    const uint16_t status = message->http_status;
    size_t offset = append(response, 0u, "HTTP/1.1 ");
    offset = append_u32(response, offset, status);
    offset = append(response, offset, " ");
    offset = append(response, offset, reason_phrase(status));
    offset = append(response, offset,
        "\r\nContent-Type: application/json\r\nContent-Length: ");
    offset = append_u32(response, offset, body_length);
    offset = append(response, offset, "\r\nConnection: close\r\n\r\n");
    if (offset + body_length > sizeof(response)) {
        demo_set_body_response(message, 500u, "{\"status\":\"error\"}");
        format_http(message);
        return;
    }
    memcpy(response + offset, message->payload, body_length);
    offset += body_length;
    memcpy(message->payload, response, offset);
    message->kind = DEMO_MESSAGE_HTTP_RESPONSE;
    message->length = (uint32_t)offset;
}

static void serve(DemoMessage *message)
{
    resource_response(message);
    format_http(message);
}

void init(void)
{
    microkit_dbg_puts("HTTP_DEMO_COMPONENT,name=protected_app\n");
}

void notified(microkit_channel channel)
{
    (void)channel;
    demo_stage_service(serve);
}
