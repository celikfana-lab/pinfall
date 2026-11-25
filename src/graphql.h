/* graphql.h - PinFall Pinterest persisted-query GraphQL transport (v6).
 *
 * Pinterest's close-up features (related pins, comments, reactions) are served
 * by POST /_/graphql/ with a *persisted query hash* + variables, not by the
 * classic /resource/<Name>/get/ RPCs.  This module owns that transport and the
 * hash constants (single source of truth).
 *
 *
 * Hash sources: live nl.pinterest.com web app persisted-query table / captured
 * requests.  Pinterest rotates these with web deploys; on rotation GraphQL
 * answers {"errors":[{"message":"PersistedQueryNotFound"}]} — update the table
 * in graphql.c (or override the env var for experiments, e.g.
 * PC_GQL_HASH_RELATED=<64 hex>) without touching anything else.
 */
#ifndef PINFALL_GRAPHQL_H
#define PINFALL_GRAPHQL_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "http.h"

typedef enum {
    PC_GQL_OP_CLOSEUP = 0,     /* CloseupPageQuery (pin entity; verified)   */
    PC_GQL_OP_RELATED,         /* related pins ("More like this")           */
    PC_GQL_OP_COMMENTS,        /* comments list                             */
    PC_GQL_OP_LIKE,            /* like / unlike mutation (UNVERIFIED)       */
    PC_GQL_OP_ADD_COMMENT,     /* add comment mutation (UNVERIFIED)         */
    PC_GQL_OP_COUNT
} PcGqlOp;

typedef enum {
    PC_GQL_READ  = 0,          /* normal retry policy                       */
    PC_GQL_WRITE = 1           /* single-shot: mutations are not idempotent */
} PcGqlMode;

/* Persisted hash / operation name for an op ("" hash = not discovered yet). */
const char *pc_gql_hash(PcGqlOp op);
const char *pc_gql_op_name(PcGqlOp op);

/* POST /_/graphql/ with {"queryHash":<hash>,"variables":<variables_json>}.
 * source_path is the X-Pinterest-Source-Url / Referer path (e.g.
 * "/pin/123/").  Fills *resp; returns false only on transport failure (a
 * non-200 HTTP status is still returned in resp->status).  Caller frees *resp
 * and *err.  PC_GRAPHQL_DEBUG=1 dumps request shape (no cookie values). */
bool pc_gql_post(PcHttp *http, PcGqlOp op, const char *source_path,
                 const char *variables_json, PcGqlMode mode,
                 PcHttpResp *resp, char **err);

/* Validate a GraphQL response: HTTP 200, valid JSON, no "errors" array.
 * Returns the parsed root (caller cJSON_Delete) or NULL + *err containing the
 * HTTP status and/or first GraphQL error message. */
cJSON *pc_gql_parse(PcHttpResp *resp, char **err);

/* PC_GRAPHQL_DEBUG=1: print a shape summary of a JSON document to stderr
 * (keys, types, lengths; strings truncated).  Never prints cookies. */
void pc_gql_debug_dump(const char *label, const char *json, size_t len);

#endif /* PINFALL_GRAPHQL_H */
