/*
 * SPDX-License-Identifier: MIT
 *
 * ds_connector.c -- placement DS connector client (placement_mode = smart).
 * See ds_connector.h and the design (XinnorLab/pNFS
 * docs/superpowers/specs/2026-09-23-placement-modes-design.md, section 7).
 *
 * Pure half: batch parsing and validation over jsmn tokens.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>

#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#define JSMN_STRICT
#include "jsmn/jsmn.h"

#include "ds_connector.h"
#include "mds_log.h"

/* -----------------------------------------------------------------------
 * Small helpers over jsmn tokens
 * ----------------------------------------------------------------------- */

struct jdoc {
    const char *js;
    const jsmntok_t *t;
    int n;
};

/*
 * Bracket depth bound, checked in one linear pass before jsmn sees the
 * text.  jsmn closes a container by walking parent links up from the LAST
 * token, so a deeply nested value costs O(depth^2): 400 000 levels (800 KiB
 * of a 4 MiB body) held the poll thread for four minutes.  The contract
 * nests six levels; 64 leaves room for any sane extension.
 */
#define DC_JSON_DEPTH_MAX 64

static bool json_depth_ok(const char *s, size_t len, int max_depth)
{
    int depth = 0;
    bool in_str = false;
    size_t i;

    for (i = 0; i < len; i++) {
        char c = s[i];

        if (in_str) {
            if (c == '\\') {
                i++;              /* skip the escaped char (\" included) */
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '[' || c == '{') {
            if (++depth > max_depth) {
                return false;
            }
        } else if (c == ']' || c == '}') {
            if (depth > 0) {
                depth--;
            }
        }
    }
    return true;
}

/*
 * Index just past the subtree rooted at i.  Iterative: jsmn emits tokens
 * in document order, so every descendant of i starts before i ends.  A
 * recursive walk would let a deeply nested value (millions of levels fit
 * in 4 MiB) overflow the poll thread's stack (review finding B-2).
 */
static int tok_skip(const struct jdoc *d, int i)
{
    int j;

    if (i < 0 || i >= d->n) {
        return d->n;
    }
    j = i + 1;
    while (j < d->n && d->t[j].start < d->t[i].end) {
        j++;
    }
    return j;
}

static bool tok_is(const struct jdoc *d, int i, jsmntype_t type)
{
    return i >= 0 && i < d->n && d->t[i].type == type;
}

static size_t tok_len(const struct jdoc *d, int i)
{
    return (size_t)(d->t[i].end - d->t[i].start);
}

static const char *tok_ptr(const struct jdoc *d, int i)
{
    return d->js + d->t[i].start;
}

/* Value index of `key` in object `obj`, or -1. */
static int tok_get(const struct jdoc *d, int obj, const char *key)
{
    int j;
    int k;
    size_t klen = strlen(key);

    if (!tok_is(d, obj, JSMN_OBJECT)) {
        return -1;
    }
    j = obj + 1;
    for (k = 0; k < d->t[obj].size; k++) {
        if (j >= d->n) {
            return -1;
        }
        if (tok_is(d, j, JSMN_STRING) && tok_len(d, j) == klen &&
            memcmp(tok_ptr(d, j), key, klen) == 0) {
            return j + 1;
        }
        j = tok_skip(d, j + 1);
    }
    return -1;
}

static bool tok_str_eq(const struct jdoc *d, int i, const char *s)
{
    size_t l = strlen(s);

    return tok_is(d, i, JSMN_STRING) && tok_len(d, i) == l &&
           memcmp(tok_ptr(d, i), s, l) == 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Append a code point as UTF-8; false when it does not fit. */
static bool utf8_put(char *buf, size_t cap, size_t *o, uint32_t cp)
{
    unsigned char tmp[4];
    size_t n;

    if (cp < 0x80) {
        tmp[0] = (unsigned char)cp; n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (unsigned char)(0xC0 | (cp >> 6));
        tmp[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (unsigned char)(0xE0 | (cp >> 12));
        tmp[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        tmp[0] = (unsigned char)(0xF0 | (cp >> 18));
        tmp[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (*o + n >= cap) {
        return false;
    }
    memcpy(buf + *o, tmp, n);
    *o += n;
    return true;
}

/* Copy a JSON string into buf, decoding the standard escapes (including
 * \uXXXX with surrogate pairs -- json.dumps emits every non-ASCII char
 * that way).  False when absent, not a string, empty, malformed or too
 * long for buf. */
static bool tok_copy(const struct jdoc *d, int i, char *buf, size_t cap)
{
    const char *s;
    size_t l;
    size_t k = 0;
    size_t o = 0;

    if (!tok_is(d, i, JSMN_STRING) || cap == 0) {
        return false;
    }
    s = tok_ptr(d, i);
    l = tok_len(d, i);
    if (l == 0) {
        return false;
    }
    while (k < l) {
        char c = s[k];

        if (c != '\\') {
            if (o + 1 >= cap) {
                return false;
            }
            buf[o++] = c;
            k++;
            continue;
        }
        if (k + 1 >= l) {
            return false;
        }
        k++;
        switch (s[k]) {
        case '"': case '\\': case '/':
            if (o + 1 >= cap) return false;
            buf[o++] = s[k]; k++; break;
        case 'n': if (o + 1 >= cap) return false; buf[o++] = '\n'; k++; break;
        case 't': if (o + 1 >= cap) return false; buf[o++] = '\t'; k++; break;
        case 'r': if (o + 1 >= cap) return false; buf[o++] = '\r'; k++; break;
        case 'b': if (o + 1 >= cap) return false; buf[o++] = '\b'; k++; break;
        case 'f': if (o + 1 >= cap) return false; buf[o++] = '\f'; k++; break;
        case 'u': {
            uint32_t cp = 0;
            int q;

            if (k + 5 > l) return false;
            for (q = 1; q <= 4; q++) {
                int h = hexval(s[k + q]);
                if (h < 0) return false;
                cp = (cp << 4) | (uint32_t)h;
            }
            k += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo = 0;

                if (k + 6 > l || s[k] != '\\' || s[k + 1] != 'u') return false;
                for (q = 2; q <= 5; q++) {
                    int h = hexval(s[k + q]);
                    if (h < 0) return false;
                    lo = (lo << 4) | (uint32_t)h;
                }
                if (lo < 0xDC00 || lo > 0xDFFF) return false;
                cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                k += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return false;
            }
            if (cp == 0 || !utf8_put(buf, cap, &o, cp)) return false;
            break;
        }
        default:
            return false;
        }
    }
    if (o == 0) {
        return false;
    }
    buf[o] = '\0';
    return true;
}

static bool tok_u64(const struct jdoc *d, int i, uint64_t *out)
{
    size_t l;
    size_t k;
    uint64_t v = 0;

    if (!tok_is(d, i, JSMN_PRIMITIVE)) {
        return false;
    }
    l = tok_len(d, i);
    if (l == 0 || l > 19) {
        return false;
    }
    for (k = 0; k < l; k++) {
        char c = tok_ptr(d, i)[k];

        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (uint64_t)(c - '0');
    }
    *out = v;
    return true;
}

static bool tok_bool(const struct jdoc *d, int i, bool *out)
{
    if (!tok_is(d, i, JSMN_PRIMITIVE)) {
        return false;
    }
    if (tok_len(d, i) == 4 && memcmp(tok_ptr(d, i), "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (tok_len(d, i) == 5 && memcmp(tok_ptr(d, i), "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool tok_is_null(const struct jdoc *d, int i)
{
    return tok_is(d, i, JSMN_PRIMITIVE) && tok_len(d, i) == 4 &&
           memcmp(tok_ptr(d, i), "null", 4) == 0;
}

/* -----------------------------------------------------------------------
 * Public helpers
 * ----------------------------------------------------------------------- */

uint64_t ds_connector_iso8601_ms(const char *s, size_t len)
{
    struct tm tm;
    uint64_t ms = 0;
    size_t i;
    time_t secs;

    /* YYYY-MM-DDTHH:MM:SS(.f+)?Z */
    if (len < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
        s[13] != ':' || s[16] != ':' || s[len - 1] != 'Z') {
        return 0;
    }
    for (i = 0; i < 19; i++) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) {
            continue;
        }
        if (!isdigit((unsigned char)s[i])) {
            return 0;
        }
    }
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0') - 1900;
    tm.tm_mon = (s[5] - '0') * 10 + (s[6] - '0') - 1;
    tm.tm_mday = (s[8] - '0') * 10 + (s[9] - '0');
    tm.tm_hour = (s[11] - '0') * 10 + (s[12] - '0');
    tm.tm_min = (s[14] - '0') * 10 + (s[15] - '0');
    tm.tm_sec = (s[17] - '0') * 10 + (s[18] - '0');
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 ||
        tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 60) {
        return 0;
    }
    if (len > 20) {
        size_t digits = 0;
        uint64_t frac = 0;

        if (s[19] != '.') {
            return 0;
        }
        for (i = 20; i + 1 < len; i++) {
            if (!isdigit((unsigned char)s[i])) {
                return 0;
            }
            if (digits < 3) {
                frac = frac * 10 + (uint64_t)(s[i] - '0');
                digits++;
            }
        }
        if (digits == 0) {
            return 0;
        }
        while (digits < 3) {
            frac *= 10;
            digits++;
        }
        ms = frac;
    }
    secs = timegm(&tm);
    if (secs < 0) {
        return 0;
    }
    return (uint64_t)secs * 1000ULL + ms;
}

bool ds_connector_endpoint_matches(const struct ds_connector_registry_ds *ds,
                                   const char *server, const char *export_path,
                                   uint32_t port)
{
    size_t el;

    if (ds == NULL || server == NULL || export_path == NULL) {
        return false;
    }
    if (strncmp(ds->host, server, MDS_DS_HOST_MAX) != 0) {
        return false;
    }
    if (port != 0 && ds->tcp_port != 0 && port != ds->tcp_port) {
        return false;
    }
    el = strlen(export_path);
    if (el == 0 || export_path[0] != '/') {
        return false;
    }
    /* "/mnt/data/" names the same share as "/mnt/data" */
    while (el > 1 && export_path[el - 1] == '/') {
        el--;
    }
    if (strlen(ds->export_path) == el && strncmp(ds->export_path, export_path, el) == 0) {
        return true;
    }
    /* the registered directory lies under the exported share */
    if (el > 1 && strncmp(ds->export_path, export_path, el) == 0 &&
        ds->export_path[el] == '/') {
        return true;
    }
    if (el == 1) {
        return ds->export_path[0] == '/';
    }
    return false;
}

void ds_connector_state_init(struct ds_connector_state *st,
                             const struct ds_connector_cfg *cfg)
{
    memset(st, 0, sizeof(*st));
    if (cfg != NULL) {
        st->cfg = *cfg;
    }
}

/* -----------------------------------------------------------------------
 * Batch application
 * ----------------------------------------------------------------------- */

static void set_detail(struct ds_connector_report *rep, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void set_detail(struct ds_connector_report *rep, const char *fmt, ...)
{
    va_list ap;

    if (rep == NULL || rep->detail[0] != '\0') {
        return;   /* keep the first detail */
    }
    va_start(ap, fmt);
    (void)vsnprintf(rep->detail, sizeof(rep->detail), fmt, ap);
    va_end(ap);
}

static const struct ds_connector_registry_ds *
registry_find(const struct ds_connector_registry *reg, uint32_t ds_id)
{
    uint32_t i;

    for (i = 0; i < reg->count; i++) {
        if (reg->ds[i].ds_id == ds_id) {
            return &reg->ds[i];
        }
    }
    return NULL;
}

/* One parsed assessment, before the binding decision. */
struct rec {
    uint32_t ds_id;
    uint64_t binding_generation;
    char     datastore_id[DC_NAME_MAX];
    char     target_id[DC_NAME_MAX];
    char     target_incarnation[DC_NAME_MAX];   /* "" when null */
    char     server[MDS_DS_HOST_MAX];
    char     export_path[MDS_DS_EXPORT_MAX];
    uint64_t port;
    char     access_scope[PM_SCOPE_MAX];
    bool     valid;
    uint64_t remaining_ttl_ms;
    char     profile_digest[PM_DIGEST_MAX];
    bool     allowed;
    uint64_t ppm;
    char     domain[PM_DOMAIN_ID_MAX];
    char     reasons[PA_REASONS_MAX][PA_REASON_LEN];
    uint32_t reason_count;
};

/* Parse one assessment object; false = shape rejection (detail set). */
static bool parse_record(const struct jdoc *d, int obj, struct rec *r,
                         struct ds_connector_report *rep)
{
    int v;
    int ep;
    int pl;
    int pr;
    int rs;
    uint64_t u;

    memset(r, 0, sizeof(*r));
    if (!tok_is(d, obj, JSMN_OBJECT)) {
        set_detail(rep, "assessment is not an object");
        return false;
    }
    if (!tok_u64(d, tok_get(d, obj, "ds_id"), &u) || u >= MDS_MAX_DS_NODES) {
        set_detail(rep, "assessment: ds_id missing or out of range");
        return false;
    }
    r->ds_id = (uint32_t)u;
    if (!tok_u64(d, tok_get(d, obj, "binding_generation"), &r->binding_generation) ||
        r->binding_generation == 0 || r->binding_generation > UINT32_MAX) {
        set_detail(rep, "ds %u: binding_generation invalid", r->ds_id);
        return false;
    }
    if (!tok_copy(d, tok_get(d, obj, "datastore_id"), r->datastore_id, sizeof(r->datastore_id)) ||
        !tok_copy(d, tok_get(d, obj, "target_id"), r->target_id, sizeof(r->target_id))) {
        set_detail(rep, "ds %u: datastore_id/target_id invalid", r->ds_id);
        return false;
    }
    v = tok_get(d, obj, "target_incarnation");
    if (v < 0 || (!tok_is_null(d, v) &&
                  !tok_copy(d, v, r->target_incarnation, sizeof(r->target_incarnation)))) {
        set_detail(rep, "ds %u: target_incarnation invalid", r->ds_id);
        return false;
    }
    ep = tok_get(d, obj, "endpoint");
    if (!tok_is(d, ep, JSMN_OBJECT) ||
        !tok_copy(d, tok_get(d, ep, "server"), r->server, sizeof(r->server)) ||
        !tok_copy(d, tok_get(d, ep, "export_path"), r->export_path, sizeof(r->export_path)) ||
        !tok_str_eq(d, tok_get(d, ep, "protocol"), "NFS") ||
        !tok_u64(d, tok_get(d, ep, "port"), &r->port) || r->port == 0 || r->port > 65535) {
        set_detail(rep, "ds %u: endpoint invalid", r->ds_id);
        return false;
    }
    v = tok_get(d, ep, "transport");
    if (!(tok_str_eq(d, v, "TCP") || tok_str_eq(d, v, "RDMA"))) {
        set_detail(rep, "ds %u: endpoint.transport invalid", r->ds_id);
        return false;
    }
    if (!tok_str_eq(d, tok_get(d, obj, "scope"), PM_CONN_SCOPE)) {
        set_detail(rep, "ds %u: scope is not %s", r->ds_id, PM_CONN_SCOPE);
        return false;
    }
    if (!tok_copy(d, tok_get(d, obj, "access_scope_id"), r->access_scope, sizeof(r->access_scope))) {
        set_detail(rep, "ds %u: access_scope_id invalid", r->ds_id);
        return false;
    }
    v = tok_get(d, obj, "quality");
    if (tok_str_eq(d, v, "VALID")) {
        r->valid = true;
    } else if (tok_str_eq(d, v, "UNKNOWN")) {
        r->valid = false;
    } else {
        set_detail(rep, "ds %u: quality invalid", r->ds_id);
        return false;
    }
    if (!tok_u64(d, tok_get(d, obj, "remaining_ttl_ms"), &r->remaining_ttl_ms) ||
        r->remaining_ttl_ms > DC_TTL_MAX_MS) {
        set_detail(rep, "ds %u: remaining_ttl_ms invalid", r->ds_id);
        return false;
    }
    pr = tok_get(d, obj, "profile");
    if (!tok_is(d, pr, JSMN_OBJECT) ||
        !tok_copy(d, tok_get(d, pr, "digest"), r->profile_digest, sizeof(r->profile_digest))) {
        set_detail(rep, "ds %u: profile.digest invalid", r->ds_id);
        return false;
    }
    pl = tok_get(d, obj, "placement");
    if (!tok_is(d, pl, JSMN_OBJECT) ||
        !tok_bool(d, tok_get(d, pl, "allowed"), &r->allowed) ||
        !tok_u64(d, tok_get(d, pl, "multiplier_ppm"), &r->ppm) || r->ppm > 1000000ULL) {
        set_detail(rep, "ds %u: placement invalid", r->ds_id);
        return false;
    }
    rs = tok_get(d, pl, "reason_codes");
    if (!tok_is(d, rs, JSMN_ARRAY)) {
        set_detail(rep, "ds %u: reason_codes missing", r->ds_id);
        return false;
    }
    {
        int j = rs + 1;
        int k;

        for (k = 0; k < d->t[rs].size; k++) {
            if (!tok_is(d, j, JSMN_STRING)) {
                set_detail(rep, "ds %u: reason_codes entry not a string", r->ds_id);
                return false;
            }
            if (r->reason_count < PA_REASONS_MAX) {
                size_t l = tok_len(d, j);

                if (l >= PA_REASON_LEN) {
                    l = PA_REASON_LEN - 1;
                }
                memcpy(r->reasons[r->reason_count], tok_ptr(d, j), l);
                r->reasons[r->reason_count][l] = '\0';
                r->reason_count++;
            }
            j = tok_skip(d, j);
        }
    }
    v = tok_get(d, obj, "resources");
    if (!tok_is(d, v, JSMN_OBJECT)) {
        set_detail(rep, "ds %u: resources missing", r->ds_id);
        return false;
    }
    v = tok_get(d, v, "capacity_domain_id");
    if (v < 0) {
        set_detail(rep, "ds %u: capacity_domain_id missing", r->ds_id);
        return false;
    }
    if (!tok_is_null(d, v) && !tok_copy(d, v, r->domain, sizeof(r->domain))) {
        set_detail(rep, "ds %u: capacity_domain_id invalid", r->ds_id);
        return false;
    }
    return true;
}

/* The pin excludes profile.digest on purpose: the digest is checked on
 * every batch (pin key + batch-wide consistency), and a connector profile
 * reload must not strand the DS in BINDING_MISMATCH until a process
 * restart (review finding B-6). */
static bool pin_matches(const struct ds_connector_pin *p, const struct rec *r,
                        const char *instance)
{
    return strcmp(p->instance, instance) == 0 &&
           p->binding_generation == (uint32_t)r->binding_generation &&
           strcmp(p->datastore_id, r->datastore_id) == 0 &&
           strcmp(p->target_id, r->target_id) == 0 &&
           strcmp(p->target_incarnation, r->target_incarnation) == 0 &&
           strcmp(p->access_scope, r->access_scope) == 0;
}

static void pin_set(struct ds_connector_pin *p, const struct rec *r, const char *instance)
{
    p->pinned = true;
    (void)snprintf(p->instance, sizeof(p->instance), "%s", instance);
    p->binding_generation = (uint32_t)r->binding_generation;
    (void)snprintf(p->datastore_id, sizeof(p->datastore_id), "%s", r->datastore_id);
    (void)snprintf(p->target_id, sizeof(p->target_id), "%s", r->target_id);
    (void)snprintf(p->target_incarnation, sizeof(p->target_incarnation), "%s", r->target_incarnation);
    (void)snprintf(p->profile_digest, sizeof(p->profile_digest), "%s", r->profile_digest);
    (void)snprintf(p->access_scope, sizeof(p->access_scope), "%s", r->access_scope);
}

/* Proposed per-instance sequence updates, committed only on DC_OK. */
struct inst_update {
    char     id[DC_NAME_MAX];
    char     epoch[DC_NAME_MAX];
    uint64_t sequence;
    bool     failed_snapshot;
    int      assessments_tok;
};

/* NOLINTNEXTLINE(readability-function-cognitive-complexity) */
enum ds_connector_drop ds_connector_apply_batch(struct ds_connector_state *st,
                                                const char *text, size_t len,
                                                const struct ds_connector_registry *reg,
                                                uint64_t now_mono_ms,
                                                struct placement_assessment_view *out,
                                                struct ds_connector_report *rep)
{
    struct ds_connector_report local_rep;
    jsmn_parser parser;
    jsmntok_t *toks = NULL;
    int ntok;
    struct jdoc d;
    int v;
    int insts;
    int n_inst;
    struct inst_update *upd = NULL;
    struct ds_connector_pin *new_pins = NULL;
    bool *seen = NULL;
    bool epoch_reset = false;
    char runtime_epoch[DC_NAME_MAX];
    char config_digest[PM_DIGEST_MAX];
    char batch_profile[PM_DIGEST_MAX];
    uint64_t generated_ms;
    enum ds_connector_drop drop = DC_SCHEMA;
    int i;

    if (rep == NULL) {
        rep = &local_rep;
    }
    memset(rep, 0, sizeof(*rep));
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (st == NULL || text == NULL || reg == NULL || out == NULL) {
        rep->drop = DC_SCHEMA;
        set_detail(rep, "null argument");
        return DC_SCHEMA;
    }
    if (len > DC_BATCH_MAX) {
        rep->drop = DC_TOO_LARGE;
        set_detail(rep, "batch is %zu bytes (limit %u)", len, DC_BATCH_MAX);
        return DC_TOO_LARGE;
    }

    /* --- tokenize ------------------------------------------------------- */
    jsmn_init(&parser);
    if (!json_depth_ok(text, len, DC_JSON_DEPTH_MAX)) {
        drop = DC_JSON;
        set_detail(rep, "nesting deeper than %d", DC_JSON_DEPTH_MAX);
        goto out_drop;
    }
    ntok = jsmn_parse(&parser, text, len, NULL, 0);
    if (ntok <= 0 || ntok > 2000000) {
        rep->drop = DC_JSON;
        set_detail(rep, "jsmn: %d", ntok);
        return DC_JSON;
    }
    toks = calloc((size_t)ntok, sizeof(*toks));
    if (toks == NULL) {
        rep->drop = DC_JSON;
        return DC_JSON;
    }
    jsmn_init(&parser);
    if (jsmn_parse(&parser, text, len, toks, (unsigned)ntok) != ntok) {
        free(toks);
        rep->drop = DC_JSON;
        set_detail(rep, "jsmn: second pass");
        return DC_JSON;
    }
    d.js = text;
    d.t = toks;
    d.n = ntok;

    /* --- envelope ------------------------------------------------------- */
    if (!tok_is(&d, 0, JSMN_OBJECT)) {
        set_detail(rep, "batch is not an object");
        goto out_drop;
    }
    v = tok_get(&d, 0, "contract_version");
    if (!tok_is(&d, v, JSMN_STRING)) {
        set_detail(rep, "contract_version missing");
        goto out_drop;
    }
    {
        uint64_t major = 0;
        size_t k;
        size_t l = tok_len(&d, v);
        const char *p = tok_ptr(&d, v);

        for (k = 0; k < l && p[k] != '.'; k++) {
            if (!isdigit((unsigned char)p[k])) {
                major = UINT64_MAX;   /* "1x" is not a version */
                break;
            }
            major = major * 10 + (uint64_t)(p[k] - '0');
        }
        if (k == 0 || k >= l || p[k] != '.' || (k > 1 && p[0] == '0') ||
            major != st->cfg.contract_major) {
            drop = DC_CONTRACT_MAJOR;
            set_detail(rep, "contract_version %.*s, expected major %u",
                       (int)l, p, (unsigned)st->cfg.contract_major);
            goto out_drop;
        }
    }
    if (!tok_copy(&d, tok_get(&d, 0, "runtime_epoch"), runtime_epoch, sizeof(runtime_epoch))) {
        set_detail(rep, "runtime_epoch missing");
        goto out_drop;
    }
    if (!tok_copy(&d, tok_get(&d, 0, "config_digest"), config_digest, sizeof(config_digest))) {
        set_detail(rep, "config_digest missing");
        goto out_drop;
    }
    if (st->cfg.expected_config_digest[0] != '\0' &&
        strcmp(st->cfg.expected_config_digest, config_digest) != 0) {
        drop = DC_CONFIG_DIGEST;
        set_detail(rep, "config_digest %s != expected %s", config_digest,
                   st->cfg.expected_config_digest);
        goto out_drop;
    }
    v = tok_get(&d, 0, "generated_at");
    if (!tok_is(&d, v, JSMN_STRING)) {
        set_detail(rep, "generated_at missing");
        goto out_drop;
    }
    generated_ms = ds_connector_iso8601_ms(tok_ptr(&d, v), tok_len(&d, v));
    if (generated_ms == 0) {
        set_detail(rep, "generated_at unparsable");
        goto out_drop;
    }
    epoch_reset = (st->runtime_epoch[0] != '\0' &&
                   strcmp(st->runtime_epoch, runtime_epoch) != 0);
    if (!epoch_reset && generated_ms < st->last_generated_at_ms) {
        drop = DC_OLD_GENERATED_AT;
        set_detail(rep, "generated_at older than the last accepted batch");
        goto out_drop;
    }
    insts = tok_get(&d, 0, "instances");
    if (!tok_is(&d, insts, JSMN_ARRAY)) {
        set_detail(rep, "instances missing");
        goto out_drop;
    }
    n_inst = toks[insts].size;
    if (n_inst > DC_INSTANCES_MAX) {
        set_detail(rep, "%d instances (limit %d)", n_inst, DC_INSTANCES_MAX);
        goto out_drop;
    }

    /* --- instances: sequence lines (validated before any state change) --- */
    upd = calloc((size_t)(n_inst > 0 ? n_inst : 1), sizeof(*upd));
    if (upd == NULL) {
        goto out_drop;
    }
    {
        int j = insts + 1;

        for (i = 0; i < n_inst; i++) {
            int io = j;
            uint64_t seq;
            int st_tok;
            int k;

            if (!tok_is(&d, io, JSMN_OBJECT)) {
                set_detail(rep, "instance %d is not an object", i);
                goto out_drop;
            }
            if (!tok_copy(&d, tok_get(&d, io, "connector_instance_id"), upd[i].id, sizeof(upd[i].id)) ||
                !tok_copy(&d, tok_get(&d, io, "epoch"), upd[i].epoch, sizeof(upd[i].epoch)) ||
                !tok_u64(&d, tok_get(&d, io, "sequence"), &seq)) {
                set_detail(rep, "instance %d: id/epoch/sequence invalid", i);
                goto out_drop;
            }
            upd[i].sequence = seq;
            for (k = 0; k < i; k++) {
                if (strcmp(upd[k].id, upd[i].id) == 0) {
                    set_detail(rep, "duplicate instance %s", upd[i].id);
                    goto out_drop;
                }
            }
            st_tok = tok_get(&d, io, "snapshot_status");
            if (tok_str_eq(&d, st_tok, "FAILED")) {
                upd[i].failed_snapshot = true;
            } else if (!(tok_str_eq(&d, st_tok, "COMPLETE") || tok_str_eq(&d, st_tok, "PARTIAL"))) {
                set_detail(rep, "instance %s: snapshot_status invalid", upd[i].id);
                goto out_drop;
            }
            upd[i].assessments_tok = tok_get(&d, io, "assessments");
            if (!tok_is(&d, upd[i].assessments_tok, JSMN_ARRAY)) {
                set_detail(rep, "instance %s: assessments missing", upd[i].id);
                goto out_drop;
            }
            if (!epoch_reset) {
                for (k = 0; k < DC_INSTANCES_MAX; k++) {
                    const struct ds_connector_instance_seq *line = &st->inst[k];

                    /* The connector re-serves its current snapshot on
                     * every GET and bumps `sequence` only on a new
                     * collection, so an EQUAL sequence is "unchanged";
                     * only a LOWER one is a replay (review finding B-1). */
                    if (line->used && strcmp(line->id, upd[i].id) == 0 &&
                        strcmp(line->epoch, upd[i].epoch) == 0 &&
                        seq < line->sequence) {
                        drop = DC_REPLAY;
                        set_detail(rep, "instance %s: sequence %llu < %llu",
                                   upd[i].id, (unsigned long long)seq,
                                   (unsigned long long)line->sequence);
                        goto out_drop;
                    }
                }
            }
            j = tok_skip(&d, io);
        }
        /* Every instance needs a sequence line; refuse rather than run
         * without replay protection (review finding B-13). */
        if (!epoch_reset) {
            int free_slots = 0;
            int need = 0;
            int k;

            for (k = 0; k < DC_INSTANCES_MAX; k++) {
                if (!st->inst[k].used) {
                    free_slots++;
                }
            }
            for (i = 0; i < n_inst; i++) {
                bool known = false;

                for (k = 0; k < DC_INSTANCES_MAX; k++) {
                    if (st->inst[k].used && strcmp(st->inst[k].id, upd[i].id) == 0) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    need++;
                }
            }
            if (need > free_slots) {
                set_detail(rep, "%d new instance ids, %d sequence lines free "
                           "(restart the connector to reset the epoch)", need, free_slots);
                goto out_drop;
            }
        }
    }

    /* --- records: duplicates first, so a duplicate never pins ---------- */
    seen = calloc(MDS_MAX_DS_NODES, sizeof(*seen));
    if (seen == NULL) {
        goto out_drop;
    }
    {
        bool *dup = calloc(MDS_MAX_DS_NODES, sizeof(*dup));

        if (dup == NULL) {
            goto out_drop;
        }
        for (i = 0; i < n_inst; i++) {
            int arr = upd[i].assessments_tok;
            int j = arr + 1;
            int k;

            for (k = 0; k < toks[arr].size; k++) {
                int obj = j;
                uint64_t u;

                j = tok_skip(&d, obj);
                if (tok_is(&d, obj, JSMN_OBJECT) &&
                    tok_u64(&d, tok_get(&d, obj, "ds_id"), &u) && u < MDS_MAX_DS_NODES) {
                    if (seen[u]) {
                        dup[u] = true;
                    }
                    seen[u] = true;
                }
            }
        }
        memcpy(seen, dup, MDS_MAX_DS_NODES * sizeof(*seen));   /* seen[] now = duplicated ids */
        free(dup);
    }

    /* --- records -------------------------------------------------------- */
    new_pins = calloc(MDS_MAX_DS_NODES, sizeof(*new_pins));
    if (new_pins == NULL) {
        goto out_drop;
    }
    if (!epoch_reset) {
        memcpy(new_pins, st->pins, MDS_MAX_DS_NODES * sizeof(*new_pins));
    }
    batch_profile[0] = '\0';
    out->count = reg->count;
    for (i = 0; i < (int)reg->count; i++) {
        out->rows[i].ds_id = reg->ds[i].ds_id;
    }
    for (i = 0; i < n_inst; i++) {
        int arr = upd[i].assessments_tok;
        int j = arr + 1;
        int k;

        for (k = 0; k < toks[arr].size; k++) {
            int obj = j;
            struct rec r;
            const struct ds_connector_registry_ds *rd;
            struct placement_assessment_row *row = NULL;
            uint32_t ri;
            bool rebound = false;

            j = tok_skip(&d, obj);
            if (!parse_record(&d, obj, &r, rep)) {
                rep->rejected_shape++;
                continue;
            }
            if (r.ds_id >= st->cfg.max_ds) {
                rep->unknown_ds++;
                continue;
            }
            rd = registry_find(reg, r.ds_id);
            if (rd == NULL) {
                rep->unknown_ds++;
                continue;
            }
            for (ri = 0; ri < reg->count; ri++) {
                if (reg->ds[ri].ds_id == r.ds_id) {
                    row = &out->rows[ri];
                    break;
                }
            }
            if (seen[r.ds_id]) {
                /* duplicate ds inside one batch: no record is trusted and
                 * nothing is pinned (review finding B-7) */
                rep->rejected_shape++;
                set_detail(rep, "ds %u appears more than once in the batch", r.ds_id);
                continue;
            }
            /* binding checks */
            if (strcmp(r.access_scope, st->cfg.access_scope) != 0) {
                rep->rejected_binding++;
                set_detail(rep, "ds %u: access_scope_id %s != %s", r.ds_id,
                           r.access_scope, st->cfg.access_scope);
                continue;
            }
            if (!ds_connector_endpoint_matches(rd, r.server, r.export_path, (uint32_t)r.port)) {
                rep->rejected_binding++;
                set_detail(rep, "ds %u: endpoint %s:%s:%llu does not match the registry %s:%s:%u",
                           r.ds_id, r.server, r.export_path, (unsigned long long)r.port,
                           rd->host, rd->export_path, (unsigned)rd->tcp_port);
                continue;
            }
            if (st->cfg.expected_profile_digest[0] != '\0' &&
                strcmp(st->cfg.expected_profile_digest, r.profile_digest) != 0) {
                rep->rejected_binding++;
                set_detail(rep, "ds %u: profile digest %s != expected", r.ds_id, r.profile_digest);
                continue;
            }
            if (batch_profile[0] == '\0') {
                (void)snprintf(batch_profile, sizeof(batch_profile), "%s", r.profile_digest);
            } else if (strcmp(batch_profile, r.profile_digest) != 0) {
                rep->rejected_binding++;
                set_detail(rep, "ds %u: profile digest differs inside the batch", r.ds_id);
                continue;
            }
            if (new_pins[r.ds_id].pinned) {
                const struct ds_connector_pin *p = &new_pins[r.ds_id];

                if (r.binding_generation > p->binding_generation) {
                    rebound = true;
                    pin_set(&new_pins[r.ds_id], &r, upd[i].id);
                } else if (!pin_matches(p, &r, upd[i].id)) {
                    rep->rejected_binding++;
                    set_detail(rep, "ds %u: BINDING_MISMATCH (gen %llu vs pinned %u, %s/%s/%s)",
                               r.ds_id, (unsigned long long)r.binding_generation,
                               (unsigned)p->binding_generation, r.datastore_id,
                               r.target_id, r.target_incarnation);
                    continue;
                }
            } else {
                pin_set(&new_pins[r.ds_id], &r, upd[i].id);
            }
            /* accepted */
            rep->accepted++;
            if (row == NULL) {
                continue;
            }
            row->present = true;
            row->received_mono_ms = now_mono_ms;
            row->expires_mono_ms = now_mono_ms + r.remaining_ttl_ms;
            row->allowed = r.allowed;
            row->multiplier_ppm = (uint32_t)r.ppm;
            memcpy(row->domain, r.domain, sizeof(row->domain));
            memcpy(row->reasons, r.reasons, sizeof(row->reasons));
            row->reason_count = r.reason_count;
            if (rebound) {
                rep->rebound++;
                row->valid = false;
                if (row->reason_count < PA_REASONS_MAX) {
                    (void)snprintf(row->reasons[row->reason_count], PA_REASON_LEN, "REBOUND");
                    row->reason_count++;
                }
            } else {
                row->valid = r.valid && !upd[i].failed_snapshot;
            }
        }
    }

    /* --- commit --------------------------------------------------------- */
    if (epoch_reset) {
        memset(st->inst, 0, sizeof(st->inst));
        MDS_LOG_INFO(LOG_COMP_MDS,
            "ds_connector: runtime_epoch changed (%s -> %s): sequence lines and "
            "binding pins reset", st->runtime_epoch, runtime_epoch);
    }
    (void)snprintf(st->runtime_epoch, sizeof(st->runtime_epoch), "%s", runtime_epoch);
    for (i = 0; i < n_inst; i++) {
        int k;
        int slot = -1;

        for (k = 0; k < DC_INSTANCES_MAX; k++) {
            if (st->inst[k].used && strcmp(st->inst[k].id, upd[i].id) == 0) {
                slot = k;
                break;
            }
        }
        if (slot < 0) {
            for (k = 0; k < DC_INSTANCES_MAX; k++) {
                if (!st->inst[k].used) {
                    slot = k;
                    break;
                }
            }
        }
        if (slot >= 0) {
            st->inst[slot].used = true;
            (void)snprintf(st->inst[slot].id, DC_NAME_MAX, "%s", upd[i].id);
            (void)snprintf(st->inst[slot].epoch, DC_NAME_MAX, "%s", upd[i].epoch);
            st->inst[slot].sequence = upd[i].sequence;
        }
    }
    memcpy(st->pins, new_pins, MDS_MAX_DS_NODES * sizeof(*new_pins));
    st->last_generated_at_ms = generated_ms;
    out->batch_valid = true;
    out->batch_received_mono_ms = now_mono_ms;
    (void)snprintf(out->config_digest, sizeof(out->config_digest), "%s", config_digest);
    (void)snprintf(out->profile_digest, sizeof(out->profile_digest), "%s", batch_profile);
    rep->drop = DC_OK;
    free(seen);
    free(new_pins);
    free(upd);
    free(toks);
    return DC_OK;

out_drop:
    free(seen);
    free(new_pins);
    free(upd);
    free(toks);
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    rep->drop = drop;
    return drop;
}

/* =======================================================================
 * I/O half: Unix-socket HTTP client, poll thread, publication
 * ======================================================================= */

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdatomic.h>

#include "ds_cache.h"
#include "mds_metrics.h"

#define DC_HEADERS_MAX (64u * 1024u)

static uint64_t dc_now_ms(void)
{
    return ds_cache_mono_ms();
}

static int wait_fd(int fd, short events, uint64_t deadline_ms)
{
    uint64_t now = dc_now_ms();
    struct pollfd pfd;
    int rc;

    if (now >= deadline_ms) {
        return 0;
    }
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    rc = poll(&pfd, 1, (int)(deadline_ms - now));
    return rc;
}

enum mds_status ds_connector_http_get(const char *socket_path, const char *path,
                                      uint32_t deadline_ms, char **body, size_t *len,
                                      int *http_status)
{
    struct sockaddr_un addr;
    char req[512];
    char *buf = NULL;
    size_t cap = 0;
    size_t used = 0;
    uint64_t deadline;
    int fd;
    int rc;
    enum mds_status st = MDS_ERR_IO;
    char *hdr_end = NULL;
    long content_length = -1;
    int status = 0;

    if (body != NULL) {
        *body = NULL;
    }
    if (len != NULL) {
        *len = 0;
    }
    if (http_status != NULL) {
        *http_status = 0;
    }
    if (socket_path == NULL || path == NULL || body == NULL || len == NULL ||
        strlen(socket_path) >= sizeof(addr.sun_path)) {
        return MDS_ERR_INVAL;
    }
    deadline = dc_now_ms() + deadline_ms;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return MDS_ERR_IO;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    (void)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno == EINPROGRESS) {
        int err = 0;
        socklen_t elen = sizeof(err);

        if (wait_fd(fd, POLLOUT, deadline) <= 0) {
            close(fd);
            return MDS_ERR_IO;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            close(fd);
            return MDS_ERR_NOTFOUND;   /* refused: nobody listens */
        }
    } else if (rc != 0) {
        close(fd);
        return MDS_ERR_NOTFOUND;       /* absent socket / refused */
    }

    rc = snprintf(req, sizeof(req),
                  "GET %s HTTP/1.1\r\nHost: connector\r\nAccept: application/json\r\n"
                  "Connection: close\r\n\r\n", path);
    if (rc < 0 || (size_t)rc >= sizeof(req)) {
        close(fd);
        return MDS_ERR_INVAL;
    }
    {
        size_t off = 0;

        while (off < (size_t)rc) {
            ssize_t n;

            if (wait_fd(fd, POLLOUT, deadline) <= 0) {
                close(fd);
                return MDS_ERR_IO;
            }
            n = send(fd, req + off, (size_t)rc - off, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    continue;
                }
                close(fd);
                return MDS_ERR_IO;
            }
            off += (size_t)n;
        }
    }

    /* Read until EOF or Content-Length satisfied, within the deadline. */
    cap = 16 * 1024;
    buf = malloc(cap);
    if (buf == NULL) {
        close(fd);
        return MDS_ERR_NOMEM;
    }
    for (;;) {
        ssize_t n;
        size_t body_have;

        if (used + 1 >= cap) {
            size_t ncap = cap * 2;
            char *nb;

            if (ncap > DC_BATCH_MAX + DC_HEADERS_MAX) {
                ncap = DC_BATCH_MAX + DC_HEADERS_MAX;
                if (used + 1 >= ncap) {
                    goto out_io;   /* oversize response */
                }
            }
            nb = realloc(buf, ncap);
            if (nb == NULL) {
                st = MDS_ERR_NOMEM;
                goto out;
            }
            buf = nb;
            cap = ncap;
        }
        rc = wait_fd(fd, POLLIN, deadline);
        if (rc <= 0) {
            goto out_io;   /* timeout */
        }
        n = read(fd, buf + used, cap - used - 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            goto out_io;
        }
        if (n == 0) {
            if (hdr_end != NULL && content_length >= 0 &&
                used - (size_t)((hdr_end + 4) - buf) < (size_t)content_length) {
                goto out_io;   /* truncated body */
            }
            break;   /* EOF */
        }
        used += (size_t)n;
        buf[used] = '\0';
        if (hdr_end == NULL) {
            hdr_end = strstr(buf, "\r\n\r\n");
            if (hdr_end != NULL) {
                const char *cl;

                if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1) {
                    goto out_io;
                }
                {
                    const char *te = strcasestr(buf, "\r\ntransfer-encoding:");

                    if (te != NULL && te < hdr_end) {
                        goto out_io;   /* chunked framing is not supported */
                    }
                }
                cl = strcasestr(buf, "\r\ncontent-length:");
                if (cl != NULL && cl < hdr_end) {
                    content_length = strtol(cl + 17, NULL, 10);
                    if (content_length < 0 || (size_t)content_length > DC_BATCH_MAX) {
                        goto out_io;
                    }
                }
            } else if (used > DC_HEADERS_MAX) {
                goto out_io;
            }
        }
        if (hdr_end != NULL && content_length >= 0) {
            body_have = used - (size_t)((hdr_end + 4) - buf);
            if (body_have >= (size_t)content_length) {
                break;
            }
        }
    }
    if (hdr_end == NULL) {
        goto out_io;
    }
    if (http_status != NULL) {
        *http_status = status;
    }
    {
        size_t body_off = (size_t)((hdr_end + 4) - buf);
        size_t body_len = used - body_off;

        if (content_length >= 0 && (size_t)content_length < body_len) {
            body_len = (size_t)content_length;
        }
        if (status == 200) {
            char *b = malloc(body_len + 1);

            if (b == NULL) {
                st = MDS_ERR_NOMEM;
                goto out;
            }
            memcpy(b, buf + body_off, body_len);
            b[body_len] = '\0';
            *body = b;
            *len = body_len;
            st = MDS_OK;
        } else if (status == 503) {
            st = MDS_ERR_DELAY;
        } else {
            st = MDS_ERR_INVAL;
        }
    }
    goto out;

out_io:
    st = MDS_ERR_IO;
out:
    free(buf);
    close(fd);
    return st;
}

/* -----------------------------------------------------------------------
 * Poll loop
 * ----------------------------------------------------------------------- */

static struct {
    bool                     configured;
    _Atomic bool             running;
    pthread_t                thread;
    int                      stop_pipe[2];
    char                     socket_path[MDS_MAX_PATH];
    uint32_t                 poll_ms;
    uint32_t                 deadline_ms;
    struct ds_cache         *cache;
    pthread_mutex_t          lock;
    struct ds_connector_state st;
    struct placement_connector_facts facts;
} g_dc = { .stop_pipe = { -1, -1 }, .lock = PTHREAD_MUTEX_INITIALIZER };

static void registry_from_cache(struct ds_connector_registry *reg)
{
    struct ds_capacity_view_row *rows;
    uint32_t n;
    uint32_t i;

    memset(reg, 0, sizeof(*reg));
    if (g_dc.cache == NULL) {
        return;
    }
    rows = calloc(MDS_MAX_DS_NODES, sizeof(*rows));
    if (rows == NULL) {
        return;
    }
    n = ds_cache_capacity_view(g_dc.cache, rows, MDS_MAX_DS_NODES);
    for (i = 0; i < n && reg->count < MDS_MAX_DS_NODES; i++) {
        struct mds_ds_info info;
        struct ds_connector_registry_ds *d = &reg->ds[reg->count];

        if (ds_cache_get(g_dc.cache, rows[i].ds_id, &info) != MDS_OK) {
            continue;
        }
        d->ds_id = info.ds_id;
        memcpy(d->host, info.host, sizeof(d->host));
        memcpy(d->export_path, info.export_path, sizeof(d->export_path));
        d->tcp_port = info.tcp_port != 0 ? info.tcp_port : info.port;
        reg->count++;
    }
    free(rows);
}

static void note_poll_error(enum ds_connector_poll_error e, enum ds_connector_drop d,
                            const char *detail)
{
    if ((unsigned)e < 8) {
        atomic_fetch_add_explicit(&g_branch_metrics.connector_poll_errors_total[e], 1,
                                  memory_order_relaxed);
    }
    if (e == DCP_DROP && (unsigned)d < 8) {
        atomic_fetch_add_explicit(&g_branch_metrics.connector_batches_dropped_total[d], 1,
                                  memory_order_relaxed);
    }
    pthread_mutex_lock(&g_dc.lock);
    g_dc.facts.last_error = e;
    g_dc.facts.last_drop = d;
    g_dc.facts.last_batch_valid = false;
    (void)snprintf(g_dc.facts.last_detail, sizeof(g_dc.facts.last_detail), "%s", detail);
    {
        /* One rule for every failure kind: reachable means an accepted
         * batch within three intervals (review finding B-4). */
        uint64_t now = dc_now_ms();

        if (g_dc.facts.last_success_mono_ms == 0 ||
            now - g_dc.facts.last_success_mono_ms > 3ULL * g_dc.poll_ms) {
            g_dc.facts.reachable = false;
        }
    }
    placement_gate_set_connector_facts(&g_dc.facts);
    atomic_store_explicit(&g_branch_metrics.connector_reachable,
                          g_dc.facts.reachable ? 1 : 0, memory_order_relaxed);
    pthread_mutex_unlock(&g_dc.lock);
}

enum mds_status ds_connector_poll_once(void)
{
    char *body = NULL;
    size_t len = 0;
    int status = 0;
    enum mds_status st;
    struct ds_connector_registry *reg;
    struct placement_assessment_view *view;
    struct ds_connector_report rep;
    enum ds_connector_drop drop;
    uint64_t now;
    uint32_t covered = 0;
    uint32_t i;

    if (!g_dc.configured) {
        return MDS_ERR_INVAL;
    }
    st = ds_connector_http_get(g_dc.socket_path, "/v1/assessments", g_dc.deadline_ms,
                               &body, &len, &status);
    if (st != MDS_OK) {
        char detail[160];

        if (st == MDS_ERR_DELAY) {
            (void)snprintf(detail, sizeof(detail), "connector not ready (503)");
            note_poll_error(DCP_UNAVAILABLE, DC_OK, detail);
        } else if (st == MDS_ERR_INVAL) {
            (void)snprintf(detail, sizeof(detail), "unexpected HTTP status %d", status);
            note_poll_error(DCP_HTTP, DC_OK, detail);
        } else if (st == MDS_ERR_NOTFOUND) {
            (void)snprintf(detail, sizeof(detail), "socket %.100s: absent or refused",
                           g_dc.socket_path);
            note_poll_error(DCP_CONNECT, DC_OK, detail);
        } else if (st == MDS_ERR_IO) {
            (void)snprintf(detail, sizeof(detail), "socket %.100s: timed out or bad response",
                           g_dc.socket_path);
            note_poll_error(DCP_TIMEOUT, DC_OK, detail);
        } else {
            (void)snprintf(detail, sizeof(detail), "transport failure (%d)", (int)st);
            note_poll_error(DCP_CONNECT, DC_OK, detail);
        }
        free(body);
        return st;
    }
    reg = calloc(1, sizeof(*reg));
    view = calloc(1, sizeof(*view));
    if (reg == NULL || view == NULL) {
        free(reg);
        free(view);
        free(body);
        return MDS_ERR_NOMEM;
    }
    registry_from_cache(reg);
    now = dc_now_ms();
    pthread_mutex_lock(&g_dc.lock);
    drop = ds_connector_apply_batch(&g_dc.st, body, len, reg, now, view, &rep);
    pthread_mutex_unlock(&g_dc.lock);
    free(body);
    if (drop != DC_OK) {
        note_poll_error(DCP_DROP, drop, rep.detail);
        free(reg);
        free(view);
        return MDS_ERR_INVAL;
    }
    placement_gate_publish_assessments(view);
    for (i = 0; i < view->count; i++) {
        if (view->rows[i].present && view->rows[i].valid) {
            covered++;
        }
    }
    atomic_fetch_add_explicit(&g_branch_metrics.connector_batches_accepted_total, 1,
                              memory_order_relaxed);
    atomic_store_explicit(&g_branch_metrics.connector_covered_ds, covered, memory_order_relaxed);
    atomic_store_explicit(&g_branch_metrics.connector_last_success_mono_ms, now,
                          memory_order_relaxed);
    atomic_store_explicit(&g_branch_metrics.connector_reachable, 1, memory_order_relaxed);
    pthread_mutex_lock(&g_dc.lock);
    g_dc.facts.reachable = true;
    g_dc.facts.last_batch_valid = true;
    g_dc.facts.last_success_mono_ms = now;
    g_dc.facts.last_error = DCP_NONE;
    g_dc.facts.last_drop = DC_OK;
    if (rep.rejected_binding != 0 || rep.rejected_shape != 0) {
        (void)snprintf(g_dc.facts.last_detail, sizeof(g_dc.facts.last_detail),
                       "accepted %u, rejected %u (%.100s)", rep.accepted,
                       rep.rejected_binding + rep.rejected_shape, rep.detail);
    } else {
        (void)snprintf(g_dc.facts.last_detail, sizeof(g_dc.facts.last_detail),
                       "accepted %u", rep.accepted);
    }
    placement_gate_set_connector_facts(&g_dc.facts);
    pthread_mutex_unlock(&g_dc.lock);
    free(reg);
    free(view);
    return MDS_OK;
}

static void *dc_thread(void *arg)
{
    sigset_t mask;

    (void)arg;
    sigfillset(&mask);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    (void)ds_connector_poll_once();
    while (atomic_load_explicit(&g_dc.running, memory_order_acquire)) {
        struct pollfd pfd = { .fd = g_dc.stop_pipe[0], .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, (int)g_dc.poll_ms);

        if (pr > 0 || !atomic_load_explicit(&g_dc.running, memory_order_acquire)) {
            break;
        }
        (void)ds_connector_poll_once();
    }
    return NULL;
}

int ds_connector_configure(const struct mds_config *cfg, struct ds_cache *cache)
{
    struct ds_connector_cfg dcfg;

    if (cfg == NULL || cfg->placement_mode != PM_SMART) {
        return -1;
    }
    if (atomic_load_explicit(&g_dc.running, memory_order_acquire)) {
        return -1;   /* stop the poller first: its fields are read unlocked */
    }
    memset(&dcfg, 0, sizeof(dcfg));
    dcfg.contract_major = cfg->ds_connector_expected_contract_major;
    dcfg.max_ds = cfg->ds_connector_max_ds;
    (void)snprintf(dcfg.access_scope, sizeof(dcfg.access_scope), "%s", cfg->ds_connector_access_scope);
    (void)snprintf(dcfg.expected_profile_digest, sizeof(dcfg.expected_profile_digest), "%s",
                   cfg->ds_connector_expected_profile_digest);
    (void)snprintf(dcfg.expected_config_digest, sizeof(dcfg.expected_config_digest), "%s",
                   cfg->ds_connector_expected_config_digest);
    pthread_mutex_lock(&g_dc.lock);
    ds_connector_state_init(&g_dc.st, &dcfg);
    (void)snprintf(g_dc.socket_path, sizeof(g_dc.socket_path), "%s", cfg->ds_connector_socket);
    g_dc.poll_ms = cfg->ds_connector_poll_ms;
    g_dc.deadline_ms = cfg->ds_connector_request_deadline_ms;
    g_dc.cache = cache;
    memset(&g_dc.facts, 0, sizeof(g_dc.facts));
    g_dc.facts.config_valid = true;
    g_dc.configured = true;
    placement_gate_set_connector_facts(&g_dc.facts);
    pthread_mutex_unlock(&g_dc.lock);
    return 0;
}

int ds_connector_start(const struct mds_config *cfg, struct ds_cache *cache)
{
    if (ds_connector_configure(cfg, cache) != 0) {
        return -1;
    }
    if (atomic_load_explicit(&g_dc.running, memory_order_acquire)) {
        return 0;
    }
    if (pipe(g_dc.stop_pipe) != 0) {
        return -1;
    }
    atomic_store_explicit(&g_dc.running, true, memory_order_release);
    if (pthread_create(&g_dc.thread, NULL, dc_thread, NULL) != 0) {
        atomic_store_explicit(&g_dc.running, false, memory_order_release);
        close(g_dc.stop_pipe[0]);
        close(g_dc.stop_pipe[1]);
        g_dc.stop_pipe[0] = -1;
        g_dc.stop_pipe[1] = -1;
        return -1;
    }
    MDS_LOG_INFO(LOG_COMP_MDS,
        "ds_connector: polling %s every %u ms (deadline %u ms, contract major %u)",
        g_dc.socket_path, (unsigned)g_dc.poll_ms, (unsigned)g_dc.deadline_ms,
        (unsigned)g_dc.st.cfg.contract_major);
    return 0;
}

void ds_connector_stop(void)
{
    if (!atomic_load_explicit(&g_dc.running, memory_order_acquire)) {
        g_dc.configured = false;
        return;
    }
    atomic_store_explicit(&g_dc.running, false, memory_order_release);
    if (g_dc.stop_pipe[1] >= 0) {
        (void)write(g_dc.stop_pipe[1], "x", 1);
    }
    (void)pthread_join(g_dc.thread, NULL);
    if (g_dc.stop_pipe[0] >= 0) {
        close(g_dc.stop_pipe[0]);
        close(g_dc.stop_pipe[1]);
        g_dc.stop_pipe[0] = -1;
        g_dc.stop_pipe[1] = -1;
    }
    g_dc.configured = false;
}

void ds_connector_facts(struct placement_connector_facts *out)
{
    if (out == NULL) {
        return;
    }
    pthread_mutex_lock(&g_dc.lock);
    *out = g_dc.facts;
    pthread_mutex_unlock(&g_dc.lock);
}
