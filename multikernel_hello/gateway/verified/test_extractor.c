/*
 * Test harness for F*-verified extract_security_params
 *
 * Tests the complete HTTP extraction pipeline against the
 * hand-written extract.c behavior.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "HTTP_Extract_Complete.h"
#include "HTTP_Extract_Types.h"

static int pass = 0, total = 0;

/* Constants matching extract.h enum */
#define EXTRACT_OK              0
#define EXTRACT_INCOMPLETE      1
#define EXTRACT_MALFORMED       2
#define EXTRACT_BODY_TOO_LARGE  3
#define EXTRACT_PATH_TRAVERSAL  4
#define EXTRACT_METHOD_UNKNOWN  5
#define EXTRACT_PATH_UNKNOWN    6

/* Constants matching security_params_wire.h */
#define SECPARAMS_HEADER_SIZE   175
#define MAX_INLINE_BODY         1361

/* Sentinel path id constants */
#define PATH_LOGIN    0x11111111u
#define PATH_LOGOUT   0x22222222u
#define PATH_STATUS   0x33333333u
#define PATH_POLICY   0x44444444u
#define PATH_DEMO     0x55555555u
#define PATH_REPORTS  0x66666666u

/* Read little-endian uint32 from buffer at offset */
static uint32_t read_u32_le(const uint8_t *buf, uint32_t off)
{
    return (uint32_t)buf[off]
         | ((uint32_t)buf[off + 1] << 8)
         | ((uint32_t)buf[off + 2] << 16)
         | ((uint32_t)buf[off + 3] << 24);
}

static const char *result_name(uint8_t code)
{
    switch (code) {
        case EXTRACT_OK:             return "OK";
        case EXTRACT_INCOMPLETE:     return "INCOMPLETE";
        case EXTRACT_MALFORMED:      return "MALFORMED";
        case EXTRACT_BODY_TOO_LARGE: return "BODY_TOO_LARGE";
        case EXTRACT_PATH_TRAVERSAL: return "PATH_TRAVERSAL";
        case EXTRACT_METHOD_UNKNOWN: return "METHOD_UNKNOWN";
        case EXTRACT_PATH_UNKNOWN:   return "PATH_UNKNOWN";
        default:                     return "UNKNOWN";
    }
}

/* Use sizeof()-1 to auto-calculate string literal length */
#define SLEN(s) (sizeof(s) - 1)

/* Test that extraction produces expected result code */
#define TEST_RESULT(name, input, exp_code) do { \
    total++; \
    uint8_t out[1536] = {0}; \
    uint32_t olen = 0; \
    uint8_t r = extract_security_params((uint8_t *)(input), SLEN(input), out, &olen); \
    if (r == (exp_code)) { \
        printf("  PASS: %s -> %s\n", (name), result_name(r)); \
        pass++; \
    } else { \
        printf("  FAIL: %s (expected %s, got %s)\n", \
               (name), result_name(exp_code), result_name(r)); \
    } \
} while(0)

/* Test extraction produces OK with correct wire format fields */
#define TEST_WIRE(name, input, exp_hash, exp_method, exp_body_len) do { \
    total++; \
    uint8_t out[1536] = {0}; \
    uint32_t olen = 0; \
    uint8_t r = extract_security_params((uint8_t *)(input), SLEN(input), out, &olen); \
    uint32_t got_hash = read_u32_le(out, 1); \
    uint8_t got_method = out[5]; \
    uint32_t got_body_len = read_u32_le(out, 171); \
    uint8_t got_rate_count = out[0]; \
    if (r == EXTRACT_OK && \
        got_hash == (exp_hash) && \
        got_method == (exp_method) && \
        got_body_len == (exp_body_len) && \
        got_rate_count == 0 && \
        olen == SECPARAMS_HEADER_SIZE + (exp_body_len)) { \
        printf("  PASS: %s (hash=0x%08x method=%u body=%u olen=%u)\n", \
               (name), got_hash, got_method, got_body_len, olen); \
        pass++; \
    } else { \
        printf("  FAIL: %s (r=%s hash=0x%08x method=%u body=%u olen=%u)\n", \
               (name), result_name(r), got_hash, got_method, got_body_len, olen); \
    } \
} while(0)

int main(void)
{
    printf("=== extract_security_params test suite ===\n\n");

    /* --- Successful extraction (known endpoints) --- */
    printf("--- Known endpoints ---\n");

    TEST_WIRE("GET /api/status",
              "GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n",
              PATH_STATUS, 1, 0);

    TEST_WIRE("GET /api/policy",
              "GET /api/policy HTTP/1.1\r\nHost: x\r\n\r\n",
              PATH_POLICY, 1, 0);

    TEST_WIRE("POST /api/login with body",
              "POST /api/login HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world",
              PATH_LOGIN, 2, 11);

    TEST_WIRE("POST /api/logout",
              "POST /api/logout HTTP/1.1\r\n\r\n",
              PATH_LOGOUT, 2, 0);

    TEST_WIRE("PUT /api/policy with body",
              "PUT /api/policy HTTP/1.1\r\nContent-Length: 5\r\n\r\nABCDE",
              PATH_POLICY, 0x04, 5);

    TEST_WIRE("GET /api/demo",
              "GET /api/demo HTTP/1.1\r\nHost: x\r\n\r\n",
              PATH_DEMO, 1, 0);

    TEST_WIRE("GET /api/reports",
              "GET /api/reports HTTP/1.1\r\nHost: x\r\n\r\n",
              PATH_REPORTS, 1, 0);

    /* --- Dashboard (root path) --- */
    printf("\n--- Dashboard ---\n");

    TEST_WIRE("GET / (dashboard)",
              "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
              0, 1, 0);

    /* --- Error codes --- */
    printf("\n--- Error codes ---\n");

    TEST_RESULT("no headers (incomplete)",
                "GET /api/status HTTP/1.1",
                EXTRACT_INCOMPLETE);

    TEST_WIRE("DELETE /api/status",
              "DELETE /api/status HTTP/1.1\r\n\r\n",
              PATH_STATUS, 0x08, 0);

    TEST_WIRE("HEAD /api/status",
              "HEAD /api/status HTTP/1.1\r\n\r\n",
              PATH_STATUS, 0x20, 0);

    TEST_RESULT("path traversal",
                "GET /../etc/passwd HTTP/1.1\r\n\r\n",
                EXTRACT_PATH_TRAVERSAL);

    TEST_RESULT("path traversal in api",
                "GET /api/../secret HTTP/1.1\r\n\r\n",
                EXTRACT_PATH_TRAVERSAL);

    TEST_RESULT("unknown endpoint",
                "GET /api/unknown HTTP/1.1\r\n\r\n",
                EXTRACT_PATH_UNKNOWN);

    TEST_RESULT("unknown short path",
                "GET /x HTTP/1.1\r\n\r\n",
                EXTRACT_PATH_UNKNOWN);

    {
        total++;
        uint8_t request[] =
            "GET /api/\x00hidden HTTP/1.1\r\n\r\n";
        uint8_t out[1536] = {0};
        uint32_t olen = 0;
        uint8_t result = extract_security_params(
            request, (uint32_t)sizeof(request) - 1u, out, &olen);
        if (result == EXTRACT_MALFORMED) {
            printf("  PASS: null byte in path -> MALFORMED\n");
            pass++;
        } else {
            printf("  FAIL: null byte in path (expected MALFORMED, got %s)\n",
                   result_name(result));
        }
    }

    TEST_RESULT("body too large",
                "POST /api/login HTTP/1.1\r\nContent-Length: 9999\r\n\r\n",
                EXTRACT_BODY_TOO_LARGE);

    TEST_RESULT("body incomplete",
                "POST /api/login HTTP/1.1\r\nContent-Length: 100\r\n\r\nshort",
                EXTRACT_INCOMPLETE);

    /* --- Wire format validation --- */
    printf("\n--- Wire format details ---\n");

    {
        total++;
        const char *req = "POST /api/login HTTP/1.1\r\nContent-Length: 5\r\n\r\nABCDE";
        uint8_t out[1536] = {0};
        uint32_t olen = 0;
        uint8_t r = extract_security_params((uint8_t *)req, strlen(req), out, &olen);

        /* Verify body bytes are copied correctly */
        int body_ok = (out[175] == 'A' && out[176] == 'B' && out[177] == 'C' &&
                       out[178] == 'D' && out[179] == 'E');
        /* Verify zeroed region (bytes 6-170) */
        int zero_ok = 1;
        for (int i = 6; i <= 170; i++) {
            if (out[i] != 0) { zero_ok = 0; break; }
        }
        if (r == EXTRACT_OK && body_ok && zero_ok && olen == 180) {
            printf("  PASS: body bytes + zeroed region\n");
            pass++;
        } else {
            printf("  FAIL: body bytes + zeroed region (r=%s body_ok=%d zero_ok=%d olen=%u)\n",
                   result_name(r), body_ok, zero_ok, olen);
        }
    }

    /* --- Edge cases --- */
    printf("\n--- Edge cases ---\n");

    TEST_RESULT("empty input",
                "",
                EXTRACT_INCOMPLETE);

    TEST_RESULT("just CRLF",
                "\r\n\r\n",
                EXTRACT_METHOD_UNKNOWN);

    /* Content-Length: 0 with no body */
    TEST_WIRE("CL 0 no body",
              "GET /api/status HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
              PATH_STATUS, 1, 0);

    /* Query string stripped */
    TEST_WIRE("path with query",
              "GET /api/login?user=admin HTTP/1.1\r\n\r\n",
              PATH_LOGIN, 1, 0);

    TEST_WIRE("status with encoded query",
              "GET /api/status?x=hello%20world%26more HTTP/1.1\r\n\r\n",
              PATH_STATUS, 1, 0);

    /* --- Bearer token extraction (Phase 3) --- */
    printf("\n--- Bearer token extraction ---\n");

#define TOKEN_LEN_OFF  42
#define TOKEN_OFF      43

    /* TV7: GET with bearer token */
    {
        total++;
        const char *req = "GET /api/status HTTP/1.1\r\nAuthorization: Bearer abc123token\r\n\r\n";
        uint8_t out[1536] = {0};
        uint32_t olen = 0;
        uint8_t r = extract_security_params((uint8_t *)req, strlen(req), out, &olen);
        uint8_t got_tlen = out[TOKEN_LEN_OFF];
        int tok_ok = (got_tlen == 11) &&
                     (memcmp(&out[TOKEN_OFF], "abc123token", 11) == 0);
        if (r == EXTRACT_OK && tok_ok && olen == SECPARAMS_HEADER_SIZE) {
            printf("  PASS: bearer token present (tlen=%u)\n", got_tlen);
            pass++;
        } else {
            printf("  FAIL: bearer token present (r=%s tlen=%u olen=%u)\n",
                   result_name(r), got_tlen, olen);
        }
    }

    /* TV8: GET without authorization header — token_len must be 0 */
    {
        total++;
        const char *req = "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n";
        uint8_t out[1536] = {0};
        uint32_t olen = 0;
        uint8_t r = extract_security_params((uint8_t *)req, strlen(req), out, &olen);
        uint8_t got_tlen = out[TOKEN_LEN_OFF];
        /* Token region should be zeroed */
        int zero_ok = 1;
        for (int j = TOKEN_OFF; j < TOKEN_OFF + 128; j++) {
            if (out[j] != 0) { zero_ok = 0; break; }
        }
        if (r == EXTRACT_OK && got_tlen == 0 && zero_ok) {
            printf("  PASS: no bearer token (tlen=0, region zeroed)\n");
            pass++;
        } else {
            printf("  FAIL: no bearer token (r=%s tlen=%u zero_ok=%d)\n",
                   result_name(r), got_tlen, zero_ok);
        }
    }

    /* TV9: Token too long (>128 bytes) — truncated to 128 */
    {
        total++;
        /* Build request with 200-char token */
        char req[512];
        int pos = sprintf(req, "GET /api/status HTTP/1.1\r\nAuthorization: Bearer ");
        for (int j = 0; j < 200; j++) req[pos++] = 'A';
        pos += sprintf(req + pos, "\r\n\r\n");
        uint8_t out[1536] = {0};
        uint32_t olen = 0;
        uint8_t r = extract_security_params((uint8_t *)req, (uint32_t)pos, out, &olen);
        uint8_t got_tlen = out[TOKEN_LEN_OFF];
        /* First 128 bytes should all be 'A' */
        int tok_ok = 1;
        for (int j = 0; j < 128; j++) {
            if (out[TOKEN_OFF + j] != 'A') { tok_ok = 0; break; }
        }
        if (r == EXTRACT_OK && got_tlen == 128 && tok_ok) {
            printf("  PASS: token truncated to 128 (tlen=%u)\n", got_tlen);
            pass++;
        } else {
            printf("  FAIL: token truncated (r=%s tlen=%u tok_ok=%d)\n",
                   result_name(r), got_tlen, tok_ok);
        }
    }

    /* TV10: POST with bearer token AND body */
    {
        total++;
        const char *req = "POST /api/logout HTTP/1.1\r\nAuthorization: Bearer mytoken\r\nContent-Length: 5\r\n\r\nABCDE";
        uint8_t out[1536] = {0};
        uint32_t olen = 0;
        uint8_t r = extract_security_params((uint8_t *)req, strlen(req), out, &olen);
        uint8_t got_tlen = out[TOKEN_LEN_OFF];
        uint32_t got_body_len = read_u32_le(out, 171);
        int tok_ok = (got_tlen == 7) &&
                     (memcmp(&out[TOKEN_OFF], "mytoken", 7) == 0);
        int body_ok = (got_body_len == 5) &&
                      (memcmp(&out[175], "ABCDE", 5) == 0);
        if (r == EXTRACT_OK && tok_ok && body_ok && olen == SECPARAMS_HEADER_SIZE + 5) {
            printf("  PASS: bearer + body (tlen=%u body=%u)\n", got_tlen, got_body_len);
            pass++;
        } else {
            printf("  FAIL: bearer + body (r=%s tlen=%u body=%u olen=%u)\n",
                   result_name(r), got_tlen, got_body_len, olen);
        }
    }

    printf("\n=== Results: %d/%d passed ===\n", pass, total);
    return (pass == total) ? 0 : 1;
}
