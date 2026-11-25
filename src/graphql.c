/* graphql.c - Pinterest persisted-query GraphQL transport for the close-up
 *             features (related pins, comments, reactions).  v6.
 *
 * provenance for each entry.  Pinterest rotates hashes with web deploys; when
 * that happens GraphQL answers {"errors":[{"message":"PersistedQueryNotFound"}]}
 * and only this table needs updating (env overrides exist for experiments).
 */
#define _POSIX_C_SOURCE 200809L

#include "graphql.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc.h"

/* ------------------------------------------------------------------ table */

typedef struct {
    const char *hash;    /* 64-hex persisted query hash ("" = unknown)      */
    const char *op_name; /* X-Pinterest-GraphQL-Name / operation name        */
    const char *env;     /* optional override env var (hash rotation tests) */
    const char *source;  /* provenance / verification note                  */
} GqlOpDef;

static const GqlOpDef g_ops[PC_GQL_OP_COUNT] = {
    /* Verified live 2026-10-31: replay with cookies returns
     * data.v3GetPinQueryv2 {__typename, data:<pin entity>}. */
    { "5259cb3cb95959fbfe0ad596663c4a02e467a77f6ffbf3844df4628f60a96a1a",
      "CloseupPageQuery", "PC_GQL_HASH_CLOSEUP",
      "live close-up page capture (Lead/api-re)" },
    /* Optional fallback for related pins if REST RelatedModulesResource
     * breaks (api-re task-15, captured live 2026-10-31).  Response path:
     * data.v3RelatedPinsForPinSeoQuery.data.connection.edges[].node. */
    { "22d4b5f01f07c1be71875fc3555a98a630cb701ea7090b818d4dc456325515e6",
      "GatedCloseupRelatedPinsFeedQuery", "PC_GQL_HASH_RELATED",
      "live capture (api-re task-15) - fallback only" },
    /* Unused: comments are REST (UnifiedCommentsResource). */
    { "", "CommentsQuery", "PC_GQL_HASH_COMMENTS", "unused (REST path)" },
    /* Unused: reactions/comments are REST (ApiResource/create,
     * AggregatedCommentResource/create).  Kept for completeness. */
    { "", "LikePinMutation", "PC_GQL_HASH_LIKE", "unused (REST path)" },
    { "", "AddCommentMutation", "PC_GQL_HASH_ADD_COMMENT", "unused (REST path)" },
};

const char *pc_gql_hash(PcGqlOp op)
{
    const char *env;

    if (op < 0 || op >= PC_GQL_OP_COUNT)
        return "";
    env = g_ops[op].env ? getenv(g_ops[op].env) : NULL;
    if (env && *env)
        return env;
    return g_ops[op].hash;
}

const char *pc_gql_op_name(PcGqlOp op)
{
    if (op < 0 || op >= PC_GQL_OP_COUNT)
        return "";
    return g_ops[op].op_name;
}

/* --------------------------------------------------------------- helpers */

static bool gql_set_err(char **err, const char *fmt, ...)
{
    char    buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (err)
        *err = strdup(buf);
    return false;
}

static void body_snippet(const char *body, size_t len, char *dst, size_t dstsz)
{
    size_t i, o = 0;

    if (!dst || dstsz == 0)
        return;
    for (i = 0; body && i < len && o + 1 < dstsz; i++) {
        unsigned char ch = (unsigned char)body[i];

        if (ch == '\n' || ch == '\r' || ch == '\t')
            ch = ' ';
        if (ch >= 32)
            dst[o++] = (char)ch;
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------- debug shape dump */

static void dump_node(cJSON *node, int depth)
{
    int i, n;

    if (!node || depth > 4)
        return;
    switch (node->type) {
    case cJSON_Object:
        n = cJSON_GetArraySize(node);
        fprintf(stderr, "%*s{object %d keys}", depth * 2, "", n);
        if (depth < 3) {
            fputc('\n', stderr);
            for (i = 0; i < n && i < 24; i++) {
                cJSON *child = cJSON_GetArrayItem(node, i);

                fprintf(stderr, "%*s%s: ", depth * 2 + 2, "",
                        child->string ? child->string : "?");
                dump_node(child, depth + 1);
            }
            if (n > 24)
                fprintf(stderr, "%*s...\n", depth * 2 + 2, "");
        } else {
            fputc('\n', stderr);
        }
        break;
    case cJSON_Array:
        n = cJSON_GetArraySize(node);
        fprintf(stderr, "[array %d]\n", n);
        if (n > 0 && depth < 3)
            dump_node(cJSON_GetArrayItem(node, 0), depth + 1);
        break;
    case cJSON_String: {
        const char *s = node->valuestring ? node->valuestring : "";
        size_t      len = strlen(s);

        fprintf(stderr, "\"%.60s%s\" (%zu chars)\n", s, len > 60 ? "..." : "",
                len);
        break;
    }
    case cJSON_Number:
        fprintf(stderr, "%g\n", node->valuedouble);
        break;
    case cJSON_True:
        fprintf(stderr, "true\n");
        break;
    case cJSON_False:
        fprintf(stderr, "false\n");
        break;
    default:
        fprintf(stderr, "null\n");
        break;
    }
}

void pc_gql_debug_dump(const char *label, const char *json, size_t len)
{
    cJSON *root;

    if (!getenv("PC_GRAPHQL_DEBUG"))
        return;
    if (!json) {
        fprintf(stderr, "graphql[%s]: <null>\n", label ? label : "?");
        return;
    }
    root = cJSON_ParseWithLength(json, len);
    fprintf(stderr, "graphql[%s]: %zu bytes%s\n", label ? label : "?", len,
            root ? "" : " (unparsable)");
    if (!root)
        return;
    dump_node(root, 1);
    cJSON_Delete(root);
}

/* --------------------------------------------------------------- request */

bool pc_gql_post(PcHttp *http, PcGqlOp op, const char *source_path,
                 const char *variables_json, PcGqlMode mode,
                 PcHttpResp *resp, char **err)
{
    const char *hash;
    const char *op_name;
    const char *host;
    cJSON      *root = NULL;
    cJSON      *vars = NULL;
    char       *body = NULL;
    char        url[512];
    char        csrf[300];
    char        h_csrf[340];
    char        h_src[700];
    char        h_ref[900];
    char        h_org[340];
    char        h_name[340];
    char        snippet[200];
    const char *hdrs[10];
    int         i = 0;
    bool        ok;

    if (err)
        *err = NULL;
    if (resp)
        memset(resp, 0, sizeof *resp);
    if (!http || !variables_json || !resp)
        return gql_set_err(err, "graphql: invalid arguments");

    hash = pc_gql_hash(op);
    op_name = pc_gql_op_name(op);
    if (!hash || !*hash)
        return gql_set_err(err,
                           "graphql: %s queryHash not discovered yet (pending "
                           "RE) - refusing to send", op_name);
    if (!pc_http_cookie(http, "csrftoken", csrf, sizeof csrf))
        return gql_set_err(err,
                           "graphql: no csrftoken cookie (call pc_bootstrap "
                           "first)");

    vars = cJSON_Parse(variables_json);
    if (!cJSON_IsObject(vars)) {
        cJSON_Delete(vars);
        return gql_set_err(err, "graphql: invalid variables JSON");
    }
    root = cJSON_CreateObject();
    if (!root || !cJSON_AddStringToObject(root, "queryHash", hash)) {
        cJSON_Delete(vars);
        cJSON_Delete(root);
        return gql_set_err(err, "graphql: out of memory");
    }
    cJSON_AddItemToObject(root, "variables", vars);
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body)
        return gql_set_err(err, "graphql: out of memory");

    if (!source_path || !*source_path)
        source_path = "/";
    host = pc_http_host(http);
    snprintf(url, sizeof url, "%s/_/graphql/", host);
    snprintf(h_csrf, sizeof h_csrf, "X-CSRFToken: %s", csrf);
    snprintf(h_src, sizeof h_src, "X-Pinterest-Source-Url: %s", source_path);
    snprintf(h_ref, sizeof h_ref, "Referer: %s%s", host, source_path);
    snprintf(h_org, sizeof h_org, "Origin: %s", host);
    snprintf(h_name, sizeof h_name, "X-Pinterest-GraphQL-Name: %s", op_name);
    hdrs[i++] = "X-Requested-With: XMLHttpRequest";
    hdrs[i++] = h_csrf;
    hdrs[i++] = "X-Pinterest-AppState: active";
    hdrs[i++] = h_name;
    hdrs[i++] = h_src;
    hdrs[i++] = h_org;
    hdrs[i++] = h_ref;
    hdrs[i++] = "Accept: application/json";
    hdrs[i]   = NULL;

    if (getenv("PC_GRAPHQL_DEBUG"))
        fprintf(stderr,
                "graphql request: op=%s hash=%.12s... src=%s vars=%zu bytes\n",
                op_name, hash, source_path, strlen(variables_json));

    if (mode == PC_GQL_WRITE)
        ok = pc_http_post_json_once(http, url, body, hdrs, resp);
    else
        ok = pc_http_post_json(http, url, body, hdrs, resp);

    if (!ok)
        gql_set_err(err, "graphql: %s", pc_http_error(http));
    else if (getenv("PC_GRAPHQL_DEBUG"))
        pc_gql_debug_dump("response", resp->body, resp->body_len);
    else if (resp->status != 200) {
        body_snippet(resp->body, resp->body_len, snippet, sizeof snippet);
        gql_set_err(err, "graphql: HTTP %ld (%s)", resp->status, snippet);
    }

    free(body);
    return ok;
}

cJSON *pc_gql_parse(PcHttpResp *resp, char **err)
{
    cJSON      *root;
    cJSON      *errors;
    cJSON      *data;
    const char *msg = NULL;
    char        snippet[200];

    if (err)
        *err = NULL;
    if (!resp || !resp->body)
        return gql_set_err(err, "graphql: empty response"), NULL;
    if (resp->status != 200) {
        body_snippet(resp->body, resp->body_len, snippet, sizeof snippet);
        return gql_set_err(err, "graphql: HTTP %ld (%s)", resp->status,
                           snippet),
               NULL;
    }
    root = cJSON_ParseWithLength(resp->body, resp->body_len);
    if (!root)
        return gql_set_err(err, "graphql: invalid JSON response"), NULL;
    errors = cJSON_GetObjectItemCaseSensitive(root, "errors");
    if (cJSON_IsArray(errors) && cJSON_GetArraySize(errors) > 0) {
        cJSON *first = cJSON_GetArrayItem(errors, 0);
        cJSON *m = cJSON_GetObjectItemCaseSensitive(first, "message");

        if (cJSON_IsString(m))
            msg = m->valuestring;
        gql_set_err(err, "graphql error: %s", msg ? msg : "unknown");
        cJSON_Delete(root);
        return NULL;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        gql_set_err(err, "graphql: response has no data object");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}
