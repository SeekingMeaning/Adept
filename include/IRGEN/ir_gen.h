
#ifndef IR_GEN_H
#define IR_GEN_H

/*
    ================================ ir_gen.h =================================
    Module for generating intermediate-representation from an abstract
    syntax tree
    ---------------------------------------------------------------------------
*/

#include "IR/ir.h"
#include "AST/ast.h"
#include "UTIL/ground.h"
#include "DRVR/compiler.h"
#include "IRGEN/ir_builder.h"

// ---------------- ir_gen_type_mappings ----------------
int ir_gen(compiler_t *compiler, object_t *object);

// ---------------- ir_gen_functions_body ----------------
// Generates IR function skeletons for AST functions.
int ir_gen_functions(compiler_t *compiler, object_t *object);

// ---------------- ir_gen_functions_body ----------------
// Generates IR function bodies for AST functions.
// Assumes IR function skeletons were already generated.
int ir_gen_functions_body(compiler_t *compiler, object_t *object);

// ---------------- ir_gen_globals ----------------
// Generates IR globals from AST globals
int ir_gen_globals(compiler_t *compiler, object_t *object);

// ---------------- ir_gen_globals_init ----------------
// Generates IR instructions for initializing global variables
int ir_gen_globals_init(ir_builder_t *builder);

// ---------------- ir_func_mapping_cmp ----------------
// Compares two 'ir_func_mapping_t' structures.
// Used for qsort()
int ir_func_mapping_cmp(const void *a, const void *b);

// ---------------- ir_method_cmp ----------------
// Compares two 'ir_method_t' structures.
// Used for qsort()
int ir_method_cmp(const void *a, const void *b);

#endif // IR_GEN_H
