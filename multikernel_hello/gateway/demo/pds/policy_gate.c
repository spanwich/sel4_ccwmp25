/* SPDX-License-Identifier: BSD-2-Clause */
#include "stage.h"

#include "gateway_common.h"
#include "gateway_engine.h"
#include "gateway_timing.h"
#include "dashboard_html.h"
#define write_u32_le fstar_write_u32_le
#include "HTTP_Extract_Complete.h"
#undef write_u32_le
#include "HTTP_Extract_Types.h"
#include "../../verified/rate_counter/RateCounter.h"

__attribute__((section(".demo_config"))) DemoConfig demo_config;
__attribute__((section(".auth_config"))) AuthConfig auth_config;

static DemoMessage in_flight;
static bool awaiting_auth;
static uint8_t admin_count;
static uint8_t operator_count;
#if D1_FAULT_KIND == 2 || D1_FAULT_KIND == 5 || D1_FAULT_KIND == 6
static uint32_t private_reply_count;
#endif

static const char DASHBOARD_HTTP_HEADER[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Length: " DASHBOARD_HTML_LENGTH_TEXT "\r\n"
    "Cache-Control: no-store\r\n"
    "Connection: close\r\n\r\n";

_Static_assert(sizeof(DASHBOARD_HTTP_HEADER) - 1u + DASHBOARD_HTML_LENGTH <=
               DEMO_PAYLOAD_BYTES,
               "dashboard response exceeds the bounded demo payload");

static AuthQueue *auth_request_queue(void)
{
    return (AuthQueue *)(uintptr_t)auth_config.request_vaddr;
}

static AuthQueue *auth_response_queue(void)
{
    return (AuthQueue *)(uintptr_t)auth_config.response_vaddr;
}

static bool is_dashboard_request(const uint8_t *wire, uint32_t wire_length)
{
    return wire_length >= SECURITY_PARAMS_HEADER_SIZE &&
        wire[0] == 0u && wire[1] == 0u && wire[2] == 0u && wire[3] == 0u &&
        wire[5] == METHOD_GET;
}

static void set_dashboard_response(DemoMessage *message)
{
    const size_t header_length = sizeof(DASHBOARD_HTTP_HEADER) - 1u;
    memcpy(message->payload, DASHBOARD_HTTP_HEADER, header_length);
    memcpy(message->payload + header_length, dashboard_html,
           DASHBOARD_HTML_LENGTH);
    message->kind = DEMO_MESSAGE_HTTP_RESPONSE;
    message->http_status = 200u;
    message->length = (uint32_t)(header_length + DASHBOARD_HTML_LENGTH);
}

static void extract(DemoMessage *message)
{
    uint8_t wire[WEB_FRAME_MTU];
    uint32_t wire_length = 0u;
    if (message->kind != DEMO_MESSAGE_RAW_HTTP ||
        message->length > DEMO_PAYLOAD_BYTES) {
        demo_set_body_response(message, 400u,
            "{\"status\":\"error\",\"message\":\"malformed request\"}");
        return;
    }
    const uint8_t result = extract_security_params(
        message->payload, message->length, wire, &wire_length);
    if (result != 0u || wire_length > DEMO_PAYLOAD_BYTES) {
        demo_set_body_response(
            message,
            result == extract_body_too_large ? 413u :
                result == extract_path_unknown ? 404u : 400u,
            result == extract_body_too_large
                ? "{\"status\":\"error\",\"message\":\"body too large\"}"
                : result == extract_path_unknown
                    ? "{\"status\":\"not_found\",\"message\":\"unknown endpoint\"}"
                    : "{\"status\":\"error\",\"message\":\"malformed request\"}");
        return;
    }
    if (is_dashboard_request(wire, wire_length)) {
        set_dashboard_response(message);
        return;
    }
    message->kind = DEMO_MESSAGE_SECURITY_PARAMS;
    message->length = wire_length;
    message->http_status = 0u;
    memcpy(message->payload, wire, wire_length);
}

typedef struct {
    uint8_t *bytes;
    size_t length;
    bool valid;
} JsonWriter;

static void json_append(JsonWriter *writer, const char *text)
{
    const size_t length = strlen(text);
    if (!writer->valid || writer->length + length > DEMO_PAYLOAD_BYTES) {
        writer->valid = false;
        return;
    }
    memcpy(writer->bytes + writer->length, text, length);
    writer->length += length;
}

static void json_append_u32(JsonWriter *writer, uint32_t value)
{
    char digits[10];
    size_t index = sizeof(digits);
    do {
        digits[--index] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    if (!writer->valid ||
        writer->length + sizeof(digits) - index > DEMO_PAYLOAD_BYTES) {
        writer->valid = false;
        return;
    }
    memcpy(writer->bytes + writer->length, digits + index,
           sizeof(digits) - index);
    writer->length += sizeof(digits) - index;
}

static void json_append_path_id(JsonWriter *writer, uint32_t value)
{
    static const char HEX[] = "0123456789abcdef";
    json_append(writer, "\"0x");
    for (int shift = 28; shift >= 0 && writer->valid; shift -= 4) {
        if (writer->length >= DEMO_PAYLOAD_BYTES) {
            writer->valid = false;
            return;
        }
        writer->bytes[writer->length++] =
            (uint8_t)HEX[(value >> (uint32_t)shift) & 0x0fu];
    }
    json_append(writer, "\"");
}

static const char *path_name(uint32_t path_id)
{
    switch (path_id) {
    case PATH_LOGIN: return "\"/api/login\"";
    case PATH_LOGOUT: return "\"/api/logout\"";
    case PATH_STATUS: return "\"/api/status\"";
    case PATH_POLICY: return "\"/api/policy\"";
    case PATH_DEMO: return "\"/api/demo\"";
    case PATH_REPORTS: return "\"/api/reports\"";
    default: return "null";
    }
}

static void set_policy_read_response(DemoMessage *message)
{
    CompactPolicy policy;
    if (engine_get_policy(&policy) != 0) {
        demo_set_body_response(message, 500u,
            "{\"status\":\"error\",\"message\":\"policy read failed\"}");
        return;
    }

    JsonWriter writer = {
        .bytes = message->payload,
        .valid = true,
    };
    json_append(&writer, "{\"declared_rules\":");
    json_append_u32(&writer, policy.num_rules);
    json_append(&writer, ",\"slots\":[");
    for (size_t slot = 0u; slot < MAX_RULES; ++slot) {
        const PolicyRule *rule = &policy.rules[slot];
        if (slot != 0u) {
            json_append(&writer, ",");
        }
        json_append(&writer, "{\"slot\":");
        json_append_u32(&writer, (uint32_t)slot);
        json_append(&writer, ",\"path\":");
        json_append(&writer, path_name(rule->path_id));
        json_append(&writer, ",\"path_id\":");
        json_append_path_id(&writer, rule->path_id);
        json_append(&writer, ",\"method_mask\":");
        json_append_u32(&writer, rule->method);
        json_append(&writer, ",\"min_role\":");
        json_append_u32(&writer, rule->min_role);
        json_append(&writer, ",\"required_scope\":");
        json_append_u32(&writer, rule->required_scope);
        json_append(&writer, "}");
    }
    json_append(&writer, "]}");
    if (!writer.valid) {
        demo_set_body_response(message, 500u,
            "{\"status\":\"error\",\"message\":\"policy output too large\"}");
        return;
    }
    message->kind = DEMO_MESSAGE_BODY_RESPONSE;
    message->http_status = 200u;
    message->length = (uint32_t)writer.length;
}

static void apply_policy(DemoMessage *message)
{
    PipelineOutput output;
    if (message->kind != DEMO_MESSAGE_SECURITY_PARAMS) {
        return;
    }
    if (pipeline_process_frame(message->payload, message->length, &output) != 0) {
        demo_set_body_response(message, 500u,
            "{\"status\":\"error\",\"message\":\"policy engine failure\"}");
        return;
    }
    if (output.kind == OUTPUT_APP_REQUEST) {
        const SecurityParamsWire *wire =
            (const SecurityParamsWire *)(const void *)message->payload;
        if (wire->path_id == PATH_POLICY && wire->method == METHOD_GET) {
            set_policy_read_response(message);
        }
        return;
    }
    if (output.kind != OUTPUT_GATE_RESPONSE ||
        output.len < GATE_RESPONSE_HEADER_SIZE) {
        demo_set_body_response(message, 500u,
            "{\"status\":\"error\",\"message\":\"invalid policy output\"}");
        return;
    }
    const GateResponse *response = (const GateResponse *)(const void *)output.bytes;
    uint16_t status = 500u;
    if (response->status == GATE_STATUS_OK) {
        status = 200u;
    } else if (response->status == GATE_STATUS_DENIED) {
        status = 403u;
    } else if (response->status == GATE_STATUS_RATE_LIMITED) {
        status = 429u;
    } else if (response->status == GATE_STATUS_NO_AUTH) {
        status = 401u;
    } else if (response->status == GATE_STATUS_NOT_FOUND) {
        status = 404u;
    }
    if (response->body_len > DEMO_PAYLOAD_BYTES) {
        demo_set_body_response(message, 500u,
            "{\"status\":\"error\",\"message\":\"oversize policy output\"}");
        return;
    }
    message->kind = DEMO_MESSAGE_BODY_RESPONSE;
    message->http_status = status;
    message->length = response->body_len;
    memcpy(message->payload, response->body, response->body_len);
}

static void set_login_response(DemoMessage *message, const AuthMessage *answer)
{
    static const char PREFIX[] = "{\"status\":\"ok\",\"token\":\"";
    static const char SUFFIX[] = "\"}";
    if (!answer->valid) {
        demo_set_body_response(message, 401u,
            "{\"status\":\"unauthorized\",\"message\":\"bad credentials\"}");
        return;
    }
    if (answer->length == 0u || answer->length > AUTH_TOKEN_MAX ||
        sizeof(PREFIX) - 1u + answer->length + sizeof(SUFFIX) - 1u >
            DEMO_PAYLOAD_BYTES) {
        demo_set_body_response(message, 500u,
            "{\"status\":\"error\",\"message\":\"invalid auth answer\"}");
        return;
    }
    for (uint32_t i = 0u; i < answer->length; ++i) {
        const uint8_t c = answer->payload[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            demo_set_body_response(message, 500u,
                "{\"status\":\"error\",\"message\":\"invalid auth answer\"}");
            return;
        }
    }
    uint32_t length = (uint32_t)(sizeof(PREFIX) - 1u);
    memcpy(message->payload, PREFIX, length);
    memcpy(message->payload + length, answer->payload, answer->length);
    length += answer->length;
    memcpy(message->payload + length, SUFFIX, sizeof(SUFFIX) - 1u);
    length += (uint32_t)(sizeof(SUFFIX) - 1u);
    message->kind = DEMO_MESSAGE_BODY_RESPONSE;
    message->http_status = 200u;
    message->length = length;
}

static void process_auth_answer(void)
{
    AuthMessage answer;
    if (!awaiting_auth || !auth_queue_receive(auth_response_queue(), &answer)) {
        return;
    }
    awaiting_auth = false;
    SecurityParamsWire *wire = (SecurityParamsWire *)(void *)in_flight.payload;
    if (wire->path_id == PATH_LOGIN && wire->method == METHOD_POST) {
        if (answer.kind != AUTH_LOGIN_RESULT) {
            demo_set_body_response(&in_flight, 500u,
                "{\"status\":\"error\",\"message\":\"invalid auth answer\"}");
        } else {
            set_login_response(&in_flight, &answer);
        }
    } else if (answer.kind != AUTH_IDENTIFY_RESULT) {
        demo_set_body_response(&in_flight, 500u,
            "{\"status\":\"error\",\"message\":\"invalid auth answer\"}");
    } else if (!answer.valid) {
        demo_set_body_response(&in_flight, 403u,
            "{\"status\":\"denied\",\"message\":\"invalid token\"}");
    } else {
        wire->role = answer.role;
        wire->scope = answer.scope;
        wire->subject_id_len = 0u;
        memset(wire->subject_id, 0, sizeof(wire->subject_id));
        uint8_t *counter = wire->role == ROLE_ADMIN
            ? &admin_count : &operator_count;
        RateCounter_tick_and_publish(counter, &wire->rate_count);
        apply_policy(&in_flight);
    }
    demo_stage_forward(&in_flight);
    microkit_notify(auth_config.channel);
}

static void process_input(void)
{
    if (awaiting_auth || !demo_stage_flush()) {
        return;
    }
    DemoMessage message;
    if (!demo_queue_receive(demo_input_queue(&demo_config), &message)) {
        return;
    }
    microkit_notify(demo_config.input_channel);
    extract(&message);
    if (message.kind != DEMO_MESSAGE_SECURITY_PARAMS) {
        demo_stage_forward(&message);
        return;
    }
    SecurityParamsWire *wire = (SecurityParamsWire *)(void *)message.payload;
    if (message.length < SECURITY_PARAMS_HEADER_SIZE) {
        demo_set_body_response(&message, 400u,
            "{\"status\":\"error\",\"message\":\"malformed request\"}");
        demo_stage_forward(&message);
        return;
    }
    if (wire->token_len == 0u &&
        !(wire->path_id == PATH_LOGIN && wire->method == METHOD_POST)) {
        demo_set_body_response(&message, 401u,
            "{\"status\":\"unauthorized\",\"message\":\"bearer token required\"}");
        demo_stage_forward(&message);
        return;
    }
    AuthMessage request = {0};
    if (wire->path_id == PATH_LOGIN && wire->method == METHOD_POST) {
        request.kind = AUTH_LOGIN;
        request.length = wire->body_len;
        if (request.length > AUTH_PAYLOAD_BYTES) {
            demo_set_body_response(&message, 413u,
                "{\"status\":\"error\",\"message\":\"body too large\"}");
            demo_stage_forward(&message);
            return;
        }
        memcpy(request.payload, wire->body, request.length);
    } else {
        request.kind = AUTH_IDENTIFY;
        request.length = wire->token_len;
        if (request.length > AUTH_TOKEN_MAX) {
            demo_set_body_response(&message, 403u,
                "{\"status\":\"denied\",\"message\":\"invalid token\"}");
            demo_stage_forward(&message);
            return;
        }
        memcpy(request.payload, wire->token, request.length);
    }
    if (!auth_queue_send(auth_request_queue(), &request)) {
        demo_set_body_response(&message, 500u,
            "{\"status\":\"error\",\"message\":\"auth queue busy\"}");
        demo_stage_forward(&message);
        return;
    }
    in_flight = message;
    awaiting_auth = true;
    microkit_notify(auth_config.channel);
}

void init(void)
{
    if (engine_init() != 0) {
        microkit_dbg_puts("HTTP_DEMO_FAIL,reason=engine_init\n");
    } else {
        if (gateway_timing_run() != 0) {
            microkit_dbg_puts("HTTP_GATE_TIMING_FAIL\n");
        }
#if DEMO_ENGINE_CEDAR
        microkit_dbg_puts("HTTP_DEMO_COMPONENT,name=policy_gate,engine=cedar\n");
#else
        microkit_dbg_puts("HTTP_DEMO_COMPONENT,name=policy_gate,engine=everparse\n");
#endif
    }
}

void notified(microkit_channel channel)
{
#if D1_FAULT_KIND == 3
    microkit_dbg_puts("D1_INJECTION_BEGIN,origin=GATE,target=self_fault\n");
    __builtin_trap();
#endif
#if D1_FAULT_KIND == 2 || D1_FAULT_KIND == 5 || D1_FAULT_KIND == 6
    microkit_dbg_puts("D1_TARGET_COUNTER,pd=GATE,channel=");
    microkit_dbg_put32(channel);
    microkit_dbg_puts(",private_replies_before=");
    microkit_dbg_put32(private_reply_count);
    if (channel == auth_config.channel) {
        ++private_reply_count;
    }
    microkit_dbg_puts(",after=");
    microkit_dbg_put32(private_reply_count);
    microkit_dbg_putc('\n');
#endif
    if (channel == auth_config.channel) {
        process_auth_answer();
    }
    process_input();
}
