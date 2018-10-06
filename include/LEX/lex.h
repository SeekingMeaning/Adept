
#ifndef LEX_H
#define LEX_H

/*
    ================================== lex.h ==================================
    Module for performing lexical analysis
    ---------------------------------------------------------------------------
*/

#include "LEX/token.h"
#include "UTIL/ground.h"
#include "DRVR/compiler.h"

// ---------------- lex_state_t ----------------
// Container for lexical analysis state
typedef struct {
    unsigned int state;
    char *buildup;
    length_t buildup_length;
    length_t buildup_capacity;
    bool is_hexadecimal;
} lex_state_t;

// ==============================================================
// -------------- Possible lexical analysis states --------------
// ==============================================================
#define LEX_STATE_IDLE         0x00000000
#define LEX_STATE_WORD         0x00000001
#define LEX_STATE_STRING       0x00000002
#define LEX_STATE_CSTRING      0x00000003
#define LEX_STATE_EQUALS       0x00000004
#define LEX_STATE_NOT          0x00000005
#define LEX_STATE_NUMBER       0x00000006
#define LEX_STATE_CONSTANT     0x00000007
#define LEX_STATE_ADD          0x00000008
#define LEX_STATE_SUBTRACT     0x00000009
#define LEX_STATE_MULTIPLY     0x0000000A
#define LEX_STATE_DIVIDE       0x0000000B
#define LEX_STATE_MODULUS      0x0000000C
#define LEX_STATE_LINECOMMENT  0x0000000D
#define LEX_STATE_LONGCOMMENT  0x0000000E
#define LEX_STATE_ENDCOMMENT   0x0000000F
#define LEX_STATE_LESS         0x00000010
#define LEX_STATE_GREATER      0x00000011
#define LEX_STATE_UBERAND      0x00000012
#define LEX_STATE_UBEROR       0x00000013
#define LEX_STATE_COLON        0x00000014

// ---------------- lex ----------------
// Entry point for lexical analysis
errorcode_t lex(compiler_t *compiler, object_t *object);

// ---------------- lex_state_init ----------------
// Initializes lexical analysis state
void lex_state_init(lex_state_t *lex_state);

// ---------------- lex_state_free ----------------
// Frees data within lexical analysis state
void lex_state_free(lex_state_t *lex_state);

// ---------------- lex_get_location ----------------
// Retrieves line and column of an index in a buffer
void lex_get_location(const char *buffer, length_t i, int *line, int *column);

#endif // LEX_H
