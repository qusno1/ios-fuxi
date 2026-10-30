/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/ShengGpr.c (08cde20edc32, BSD-2-Clause-Patent) with fuxi_ names. */
#include "fuxi_gpr.h"

static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

uint32_t fuxi_gpr_encode(uint8_t *out, uint32_t capacity, const FuxiGprPacket *p, const void *payload, uint32_t payload_bytes)
{
    if (payload_bytes > FUXI_GPR_MAX_PACKET - FUXI_GPR_HEADER_BYTES || (payload_bytes && !payload)) return 0;
    const uint32_t size = FUXI_GPR_HEADER_BYTES + payload_bytes;
    if (size > capacity) return 0;
    /* word 0: version 3:0, header size in words 7:4, packet size in bytes 31:8; word 1: destination domain 7:0, source domain 15:8 */
    put32(out, (FUXI_GPR_VERSION & 0xfu) | (FUXI_GPR_HEADER_BYTES / 4) << 4 | size << 8);
    put32(out + 4, (p->dst_domain & 0xffu) | (p->src_domain & 0xffu) << 8);
    put32(out + 8, p->src_port);
    put32(out + 12, p->dst_port);
    put32(out + 16, p->token);
    put32(out + 20, p->opcode);
    for (uint32_t i = 0; i < payload_bytes; ++i) out[FUXI_GPR_HEADER_BYTES + i] = ((const uint8_t *)payload)[i];
    return size;
}

int fuxi_gpr_decode(const uint8_t *in, uint32_t length, FuxiGprPacket *p)
{
    if (length < FUXI_GPR_HEADER_BYTES) return FuxiGprShort;
    const uint32_t w0 = le32(in), w1 = le32(in + 4);
    p->version = w0 & 0xfu;
    p->header_bytes = ((w0 >> 4) & 0xfu) * 4;
    p->size = w0 >> 8;
    if (p->version != FUXI_GPR_VERSION) return FuxiGprBadVersion;
    if (p->header_bytes < FUXI_GPR_HEADER_BYTES) return FuxiGprBadHeader;
    if (p->size < p->header_bytes || p->size > length) return FuxiGprBadSize;
    p->dst_domain = w1 & 0xffu;
    p->src_domain = (w1 >> 8) & 0xffu;
    p->src_port = le32(in + 8);
    p->dst_port = le32(in + 12);
    p->token = le32(in + 16);
    p->opcode = le32(in + 20);
    p->payload = in + p->header_bytes;
    p->payload_bytes = p->size - p->header_bytes;
    return FuxiGprOk;
}

int fuxi_gpr_basic_result(const FuxiGprPacket *p, uint32_t *opcode, uint32_t *status)
{
    if (p->opcode != FUXI_GPR_OP_BASIC_RESULT || p->payload_bytes < 8) return -1;
    *opcode = le32(p->payload);
    *status = le32(p->payload + 4);
    return 0;
}

void fuxi_gpr_spf_init(FuxiGprSpfProbe *s, uint32_t port, uint32_t max_queries)
{
    for (unsigned i = 0; i < sizeof *s; ++i) ((uint8_t *)s)[i] = 0;
    s->port = port;
    s->max_queries = max_queries;
    s->state = FuxiSpfIdle;
}

uint32_t fuxi_gpr_spf_next(FuxiGprSpfProbe *s, uint8_t out[FUXI_GPR_HEADER_BYTES])
{
    if (s->state != FuxiSpfIdle || s->queries >= s->max_queries) return 0;
    const FuxiGprPacket p = {.dst_domain = FUXI_GPR_DOMAIN_ADSP, .src_domain = FUXI_GPR_DOMAIN_APPS, .src_port = s->port, .dst_port = FUXI_GPR_PORT_APM,
                         .token = ++s->token, .opcode = FUXI_GPR_OP_APM_GET_SPF_STATE};
    s->queries++;
    s->state = FuxiSpfWaiting;
    return fuxi_gpr_encode(out, FUXI_GPR_HEADER_BYTES, &p, 0, 0);
}

static int retry_or_stop(FuxiGprSpfProbe *s)
{
    if (s->queries >= s->max_queries) {s->state = FuxiSpfGaveUp; return FuxiSpfDone;}
    s->state = FuxiSpfIdle;
    return FuxiSpfNotReady;
}

int fuxi_gpr_spf_receive(FuxiGprSpfProbe *s, const uint8_t *data, uint32_t length)
{
    FuxiGprPacket p;
    if (fuxi_gpr_decode(data, length, &p) != FuxiGprOk || p.dst_port != s->port || p.src_domain != FUXI_GPR_DOMAIN_ADSP) {s->ignored++; return FuxiSpfIgnored;}
    s->last_opcode = p.opcode;
    if (s->state != FuxiSpfWaiting || p.token != s->token) {s->ignored++; return FuxiSpfIgnored;}
    uint32_t opcode = 0, status = 0;
    if (p.opcode == FUXI_GPR_OP_APM_SPF_STATE && p.payload_bytes >= 4) {
        s->answers++;
        s->last_status = le32(p.payload);
        if (s->last_status == 1) {s->state = FuxiSpfReady; return FuxiSpfIsReady;}
        const int r = retry_or_stop(s);
        return r;
    }
    if (!fuxi_gpr_basic_result(&p, &opcode, &status) && opcode == FUXI_GPR_OP_APM_GET_SPF_STATE) {
        s->errors++;
        s->last_status = status;
        retry_or_stop(s);
        return FuxiSpfError;
    }
    s->ignored++;
    return FuxiSpfIgnored;
}

int fuxi_gpr_spf_timeout(FuxiGprSpfProbe *s)
{
    if (s->state != FuxiSpfWaiting) return FuxiSpfIgnored;
    s->timeouts++;
    return retry_or_stop(s) == FuxiSpfDone ? FuxiSpfDone : FuxiSpfIgnored;
}
