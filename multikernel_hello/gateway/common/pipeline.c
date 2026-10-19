/*
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "gateway_common.h"
#include "gateway_engine.h"

#include <string.h>

static uint32_t make_gate_response(uint8_t *buffer, uint8_t status,
                                   const char *body)
{
    GateResponse *response = (GateResponse *)(void *)buffer;
    response->conn_id = 0u;
    response->status = status;
    response->body_len = body == NULL ? 0u : (uint32_t)strlen(body);
    if (response->body_len != 0u) {
        memcpy(response->body, body, response->body_len);
    }
    return GATE_RESPONSE_HEADER_SIZE + response->body_len;
}

static uint32_t make_app_request(uint8_t *buffer,
                                 const SecurityParamsWire *wire)
{
    AppRequest *request = (AppRequest *)(void *)buffer;
    request->conn_id = 0u;
    request->path_id = wire->path_id;
    request->method = wire->method;
    request->role = wire->role;
    request->scope = wire->scope;
    request->subject_id_len = wire->subject_id_len;
    memcpy(request->subject_id, wire->subject_id, SUBJECT_ID_MAX);
    request->body_len = wire->body_len;
    if (wire->body_len != 0u) {
        memcpy(request->body, wire->body, wire->body_len);
    }
    return APP_REQUEST_HEADER_SIZE + wire->body_len;
}

int pipeline_process_frame(const uint8_t *frame, uint32_t frame_len,
                           PipelineOutput *output)
{
    const SecurityParamsWire *wire = NULL;
    DecisionInput input = {0};

    if (output == NULL) {
        return -1;
    }
    output->kind = OUTPUT_NONE;
    output->len = 0u;

    if (!validate_security_params(frame, frame_len, &wire, &input)) {
        output->kind = OUTPUT_GATE_RESPONSE;
        output->len = make_gate_response(
            output->bytes, GATE_STATUS_ERROR,
            "{\"status\":\"error\",\"message\":\"invalid gate frame\"}");
        return 0;
    }

    if (input.path_id == PATH_POLICY && input.method == METHOD_PUT) {
        const UpdateResult update = engine_update(
            &input, wire->body, wire->body_len);
        output->kind = OUTPUT_GATE_RESPONSE;
        if (update == UPDATE_APPLIED) {
            output->len = make_gate_response(
                output->bytes, GATE_STATUS_OK,
                "{\"status\":\"ok\",\"message\":\"policy updated\"}");
        } else if (update == UPDATE_REJECTED) {
            output->len = make_gate_response(
                output->bytes, GATE_STATUS_DENIED,
                "{\"status\":\"denied\",\"message\":\"access denied\"}");
        } else {
            output->len = make_gate_response(
                output->bytes, GATE_STATUS_ERROR,
                "{\"status\":\"error\",\"message\":\"engine failure\"}");
        }
        return 0;
    }

    if (input.rate_count >= MAX_RATE) {
        output->kind = OUTPUT_GATE_RESPONSE;
        output->len = make_gate_response(
            output->bytes, GATE_STATUS_RATE_LIMITED,
            "{\"status\":\"rate_limited\",\"message\":\"too many requests\"}");
        return 0;
    }

    const DecisionResult decision = engine_decide(&input);
    if (decision == DECISION_ALLOW) {
        output->kind = OUTPUT_APP_REQUEST;
        output->len = make_app_request(output->bytes, wire);
    } else {
        output->kind = OUTPUT_GATE_RESPONSE;
        output->len = make_gate_response(
            output->bytes,
            decision == DECISION_INTERNAL_ERROR
                ? GATE_STATUS_ERROR : GATE_STATUS_DENIED,
            decision == DECISION_INTERNAL_ERROR
                ? "{\"status\":\"error\",\"message\":\"engine failure\"}"
                : "{\"status\":\"denied\",\"message\":\"access denied\"}");
    }
    return 0;
}
