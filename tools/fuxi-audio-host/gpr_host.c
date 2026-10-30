// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the GPR packet codec and the audio framework readiness query (xnu/fuxi-audio/fuxi_gpr.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../xnu/fuxi-audio/fuxi_gpr.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

/* What the DSP would send back to our query. */
static uint32_t reply(uint8_t *out, const uint8_t *query, uint32_t opcode, const uint32_t *words, unsigned n)
{
    FuxiGprPacket q, p;
    CHECK(fuxi_gpr_decode(query, FUXI_GPR_HEADER_BYTES, &q) == FuxiGprOk);
    memset(&p, 0, sizeof p);
    p.dst_domain = q.src_domain; p.src_domain = q.dst_domain; p.src_port = q.dst_port; p.dst_port = q.src_port; p.token = q.token;
    p.opcode = opcode;
    return fuxi_gpr_encode(out, FUXI_GPR_MAX_PACKET, &p, words, 4 * n);
}

int main(void)
{
    uint8_t buffer[FUXI_GPR_MAX_PACKET], query[FUXI_GPR_HEADER_BYTES], answer[FUXI_GPR_MAX_PACKET];
    FuxiGprPacket p, d;

    /* Exact header bytes. */
    memset(&p, 0, sizeof p);
    p.dst_domain = FUXI_GPR_DOMAIN_ADSP; p.src_domain = FUXI_GPR_DOMAIN_APPS; p.src_port = 1; p.dst_port = 1; p.token = 0x1234; p.opcode = FUXI_GPR_OP_APM_GET_SPF_STATE;
    const uint8_t expected[24] = {0x60, 0x18, 0, 0, 0x02, 0x03, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0x34, 0x12, 0, 0, 0x21, 0x10, 0x00, 0x01};
    CHECK(fuxi_gpr_encode(buffer, sizeof buffer, &p, NULL, 0) == 24 && !memcmp(buffer, expected, 24));
    /* With a payload; round trip. */
    const uint32_t words[3] = {7, 8, 9};
    CHECK(fuxi_gpr_encode(buffer, sizeof buffer, &p, words, 12) == 36 && le32(buffer) == (0x60u | 36u << 8));
    CHECK(fuxi_gpr_decode(buffer, 36, &d) == FuxiGprOk && d.size == 36 && d.header_bytes == 24 && d.payload_bytes == 12 && le32(d.payload + 8) == 9);
    CHECK(d.dst_domain == 2 && d.src_domain == 3 && d.src_port == 1 && d.dst_port == 1 && d.token == 0x1234 && d.opcode == FUXI_GPR_OP_APM_GET_SPF_STATE);
    /* A received buffer can be longer than the packet (intent size). */
    CHECK(fuxi_gpr_decode(buffer, 512, &d) == FuxiGprOk && d.payload_bytes == 12);
    /* Limits and malformed packets. */
    CHECK(fuxi_gpr_encode(buffer, 35, &p, words, 12) == 0 && fuxi_gpr_encode(buffer, sizeof buffer, &p, NULL, 4) == 0);
    CHECK(fuxi_gpr_encode(buffer, sizeof buffer, &p, buffer, FUXI_GPR_MAX_PACKET - 23) == 0);
    fuxi_gpr_encode(buffer, sizeof buffer, &p, words, 12);
    CHECK(fuxi_gpr_decode(buffer, 23, &d) == FuxiGprShort && fuxi_gpr_decode(buffer, 35, &d) == FuxiGprBadSize);
    buffer[0] = 0x61; CHECK(fuxi_gpr_decode(buffer, 36, &d) == FuxiGprBadVersion);
    buffer[0] = 0x50; CHECK(fuxi_gpr_decode(buffer, 36, &d) == FuxiGprBadHeader);
    buffer[0] = 0x70; CHECK(fuxi_gpr_decode(buffer, 36, &d) == FuxiGprOk && d.header_bytes == 28 && d.payload_bytes == 8 && le32(d.payload) == 8);
    buffer[0] = 0x60; buffer[1] = 0x10; CHECK(fuxi_gpr_decode(buffer, 36, &d) == FuxiGprBadSize);  /* size 16 < header */
    /* Basic result. */
    uint32_t op = 0, status = 0;
    const uint32_t basic[2] = {FUXI_GPR_OP_APM_GET_SPF_STATE, 0x0b};
    memset(&p, 0, sizeof p); p.opcode = FUXI_GPR_OP_BASIC_RESULT;
    fuxi_gpr_encode(buffer, sizeof buffer, &p, basic, 8);
    CHECK(fuxi_gpr_decode(buffer, 32, &d) == FuxiGprOk && fuxi_gpr_basic_result(&d, &op, &status) == 0 && op == FUXI_GPR_OP_APM_GET_SPF_STATE && status == 0x0b);
    fuxi_gpr_encode(buffer, sizeof buffer, &p, basic, 4);
    CHECK(fuxi_gpr_decode(buffer, 28, &d) == FuxiGprOk && fuxi_gpr_basic_result(&d, &op, &status) == -1);

    /* The readiness query: not ready twice, then ready. */
    FuxiGprSpfProbe s;
    fuxi_gpr_spf_init(&s, FUXI_GPR_PORT_APM, 5);
    CHECK(fuxi_gpr_spf_next(&s, query) == 24 && le32(query + 20) == FUXI_GPR_OP_APM_GET_SPF_STATE && le32(query + 16) == 1 && s.state == FuxiSpfWaiting);
    CHECK(fuxi_gpr_spf_next(&s, query) == 0);   /* one in flight */
    uint32_t zero = 0, one = 1;
    uint32_t n = reply(answer, query, FUXI_GPR_OP_APM_SPF_STATE, &zero, 1);
    CHECK(fuxi_gpr_spf_receive(&s, answer, n) == FuxiSpfNotReady && s.state == FuxiSpfIdle && s.last_status == 0);
    CHECK(fuxi_gpr_spf_receive(&s, answer, n) == FuxiSpfIgnored);   /* repeated answer: nothing in flight */
    CHECK(fuxi_gpr_spf_next(&s, query) == 24 && le32(query + 16) == 2);
    /* A late answer to the first query (old token), another port's packet and a broken one are ignored. */
    uint8_t stale[FUXI_GPR_MAX_PACKET];
    memcpy(stale, answer, n);
    CHECK(fuxi_gpr_spf_receive(&s, stale, n) == FuxiSpfIgnored);
    n = reply(answer, query, FUXI_GPR_OP_APM_SPF_STATE, &one, 1);
    answer[12] = 9;
    CHECK(fuxi_gpr_spf_receive(&s, answer, n) == FuxiSpfIgnored);
    answer[12] = 1;
    CHECK(fuxi_gpr_spf_receive(&s, answer, 10) == FuxiSpfIgnored && s.ignored == 4);
    /* A timeout lets the owner ask again. */
    CHECK(fuxi_gpr_spf_timeout(&s) == FuxiSpfIgnored && s.timeouts == 1 && s.state == FuxiSpfIdle);
    CHECK(fuxi_gpr_spf_next(&s, query) == 24 && le32(query + 16) == 3);
    /* The APM refuses the query: an error, and asking again is allowed. */
    n = reply(answer, query, FUXI_GPR_OP_BASIC_RESULT, basic, 2);
    CHECK(fuxi_gpr_spf_receive(&s, answer, n) == FuxiSpfError && s.errors == 1 && s.last_status == 0x0b && s.state == FuxiSpfIdle);
    CHECK(fuxi_gpr_spf_next(&s, query) == 24);
    n = reply(answer, query, FUXI_GPR_OP_APM_SPF_STATE, &one, 1);
    CHECK(fuxi_gpr_spf_receive(&s, answer, n) == FuxiSpfIsReady && s.state == FuxiSpfReady && s.answers == 2);
    CHECK(fuxi_gpr_spf_next(&s, query) == 0 && fuxi_gpr_spf_timeout(&s) == FuxiSpfIgnored);

    /* Out of tries: the last "not ready" or timeout gives up. */
    fuxi_gpr_spf_init(&s, 2, 2);
    CHECK(fuxi_gpr_spf_next(&s, query) == 24 && le32(query + 8) == 2);
    CHECK(fuxi_gpr_spf_timeout(&s) == FuxiSpfIgnored && fuxi_gpr_spf_next(&s, query) == 24);
    CHECK(fuxi_gpr_spf_timeout(&s) == FuxiSpfDone && s.state == FuxiSpfGaveUp && fuxi_gpr_spf_next(&s, query) == 0);
    fuxi_gpr_spf_init(&s, 1, 1);
    fuxi_gpr_spf_next(&s, query);
    n = reply(answer, query, FUXI_GPR_OP_APM_SPF_STATE, &zero, 1);
    CHECK(fuxi_gpr_spf_receive(&s, answer, n) == FuxiSpfDone && s.state == FuxiSpfGaveUp);
    printf("PASS GPR: %u checks\n", checks);
    return 0;
}
