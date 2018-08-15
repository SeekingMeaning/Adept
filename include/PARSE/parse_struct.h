
#ifndef PARSE_STRUCT_H
#define PARSE_STRUCT_H

#include "UTIL/ground.h"
#include "PARSE/parse_ctx.h"

// ------------------ parse_struct ------------------
// Parses a struct
int parse_struct(parse_ctx_t *ctx);

// ------------------ parse_struct_head ------------------
// Parses the head of a struct
int parse_struct_head(parse_ctx_t *ctx, char **out_name, bool *out_is_packed);

// ------------------ parse_struct_body ------------------
// Parses the body of a struct
int parse_struct_body(parse_ctx_t *ctx, char ***names, ast_type_t **types, length_t *length);

// ------------------ parse_struct_grow_fields ------------------
// Grows struct fields so that another field can be appended
void parse_struct_grow_fields(char ***names, ast_type_t **types, length_t length, length_t *capacity, length_t backfill);

// ------------------ parse_struct_free_fields ------------------
// Frees struct fields (used for when errors occur)
void parse_struct_free_fields(char **names, ast_type_t *types, length_t length, length_t backfill);

#endif // PARSE_STRUCT_H
