
#include "LEX/lex.h"
#include "UTIL/util.h"
#include "UTIL/color.h"
#include "UTIL/ground.h"
#include "UTIL/search.h"
#include "UTIL/filename.h"
#include "UTIL/builtin_type.h"
#include "BRIDGE/any.h"
#include "BRIDGE/rtti.h"
#include "IRGEN/ir_gen.h"
#include "IRGEN/ir_gen_expr.h"
#include "IRGEN/ir_gen_find.h"
#include "IRGEN/ir_gen_stmt.h"
#include "IRGEN/ir_gen_type.h"

errorcode_t ir_gen(compiler_t *compiler, object_t *object){
    ir_module_t *module = &object->ir_module;
    ast_t *ast = &object->ast;

    ir_job_list_t job_list;
    memset(&job_list, 0, sizeof(ir_job_list_t));

    ir_implementation_setup();
    ir_module_init(module, ast->funcs_length, ast->globals_length, ast->funcs_length + ast->func_aliases_length + 32);
    object->compilation_stage = COMPILATION_STAGE_IR_MODULE;

    if(ir_gen_type_mappings(compiler, object)
    || ir_gen_globals(compiler, object)
    || ir_gen_functions(compiler, object, &job_list)
    || ir_gen_functions_body(compiler, object, &job_list)
    || ir_gen_special_globals(compiler, object)
    || ir_gen_fill_in_rtti(object)){
        free(job_list.jobs);
        return FAILURE;
    }

    return SUCCESS;
}

errorcode_t ir_gen_functions(compiler_t *compiler, object_t *object, ir_job_list_t *job_list){
    // NOTE: Only ir_gens function skeletons

    ast_t *ast = &object->ast;
    ir_module_t *module = &object->ir_module;
    ast_func_t **ast_funcs = &ast->funcs;
    ast_func_alias_t **ast_func_aliases = &ast->func_aliases;

    // Setup IR variadic array type if it exists
    if(ast->common.ast_variadic_array){
        // Resolve ast_variadic_array type
        if(ir_gen_resolve_type(compiler, object, ast->common.ast_variadic_array, &module->common.ir_variadic_array)) return FAILURE;
    }

    // Generate function skeletons
    for(length_t ast_func_id = 0; ast_func_id != ast->funcs_length; ast_func_id++){
        ast_func_t *ast_func = &(*ast_funcs)[ast_func_id];
        if(ast_func->traits & AST_FUNC_POLYMORPHIC) continue;
        if(ir_gen_func_head(compiler, object, ast_func, ast_func_id, false, NULL)) return FAILURE;
    }

    // Sort various mappings
    qsort(module->func_mappings, module->func_mappings_length, sizeof(ir_func_mapping_t), ir_func_mapping_cmp);
    qsort(module->methods, module->methods_length, sizeof(ir_method_t), ir_method_cmp);
    qsort(module->generic_base_methods, module->generic_base_methods_length, sizeof(ir_generic_base_method_t), ir_generic_base_method_cmp);

    if(ir_gen_job_list(object, job_list)) return FAILURE;

    // Generate function aliases
    trait_t req_traits_mask = AST_FUNC_VARARG | AST_FUNC_VARIADIC;
    for(length_t ast_func_alias_id = 0; ast_func_alias_id != ast->func_aliases_length; ast_func_alias_id++){
        ast_func_alias_t *falias = &(*ast_func_aliases)[ast_func_alias_id];

        funcpair_t pair;
        errorcode_t error;
        bool is_unique = true;

        if(falias->match_first_of_name){
            error = ir_gen_find_func_named(compiler, object, falias->to, &is_unique, &pair);
        } else {
            error = ir_gen_find_func(compiler, object, job_list, falias->to, falias->arg_types, falias->arity, req_traits_mask, falias->required_traits, &pair);
        }

        if(error){
            compiler_panicf(compiler, falias->source, "Failed to find proper destination function '%s'", falias->to);
            return FAILURE;
        }

        if(!is_unique && compiler_warnf(compiler, falias->source, "Multiple functions named '%s', using the first of them", falias->to)){
            return FAILURE;
        }
        
        ir_module_insert_func_mapping(module, falias->from, pair.ir_func_id, pair.ast_func_id, true);
    }

    errorcode_t error;

    // Find __variadic_array__ (if it exists)
    error = ir_gen_find_special_func(compiler, object, "__variadic_array__", &module->common.variadic_ir_func_id);
    if(error == ALT_FAILURE) return FAILURE;
    
    return SUCCESS;
}

errorcode_t ir_gen_func_head(compiler_t *compiler, object_t *object, ast_func_t *ast_func, length_t ast_func_id,
        bool preserve_sortedness, ir_func_mapping_t *optional_out_new_mapping){
    ir_module_t *module = &object->ir_module;

    expand((void**) &module->funcs, sizeof(ir_func_t), module->funcs_length, &module->funcs_capacity, 1, object->ast.funcs_length);
    ir_func_t *module_func = &module->funcs[module->funcs_length];
    length_t ir_func_id = module->funcs_length;

    module_func->name = ast_func->name;
    module_func->maybe_filename = NULL;
    module_func->maybe_definition_string = NULL;
    module_func->maybe_line_number = 0;
    module_func->maybe_column_number = 0;
    module_func->traits = TRAIT_NONE;
    module_func->return_type = NULL;
    module_func->argument_types = malloc(sizeof(ir_type_t*) * (ast_func->traits & AST_FUNC_VARIADIC ? ast_func->arity + 1 : ast_func->arity));
    module_func->arity = 0;
    module_func->basicblocks = NULL; // Will be set after 'basicblocks' contains all of the basicblocks
    module_func->basicblocks_length = 0; // Will be set after 'basicblocks' contains all of the basicblocks
    module_func->scope = NULL;
    module_func->variable_count = 0;
    module_func->export_as = ast_func->export_as;

    if(ast_func->traits & AST_FUNC_VARIADIC){
        module_func->argument_types[ast_func->arity] = module->common.ir_variadic_array;
    }

    if(compiler->checks & COMPILER_NULL_CHECKS){
        module_func->maybe_definition_string = ir_gen_ast_definition_string(&module->pool, ast_func);        
        module_func->maybe_filename = compiler->objects[ast_func->source.object_index]->filename;

        int line, column;
        lex_get_location(compiler->objects[ast_func->source.object_index]->buffer, ast_func->source.index, &line, &column);
        module_func->maybe_line_number = line;
        module_func->maybe_column_number = column;
    }

    #if AST_FUNC_FOREIGN     == IR_FUNC_FOREIGN      && \
        AST_FUNC_VARARG      == IR_FUNC_VARARG       && \
        AST_FUNC_MAIN        == IR_FUNC_MAIN         && \
        AST_FUNC_STDCALL     == IR_FUNC_STDCALL      && \
        AST_FUNC_POLYMORPHIC == IR_FUNC_POLYMORPHIC
    module_func->traits = ast_func->traits & 0x1F;
    #else
    if(ast_func->traits & AST_FUNC_FOREIGN)     module_func->traits |= IR_FUNC_FOREIGN;
    if(ast_func->traits & AST_FUNC_VARARG)      module_func->traits |= IR_FUNC_VARARG;
    if(ast_func->traits & AST_FUNC_MAIN)        module_func->traits |= IR_FUNC_MAIN;
    if(ast_func->traits & AST_FUNC_STDCALL)     module_func->traits |= IR_FUNC_STDCALL;
    if(ast_func->traits & AST_FUNC_POLYMORPHIC) module_func->traits |= IR_FUNC_POLYMORPHIC;
    #endif

    module->funcs_length++;

    ir_func_mapping_t *new_mapping = ir_module_insert_func_mapping(module, ast_func->name, ir_func_id, ast_func_id, preserve_sortedness);
    if(optional_out_new_mapping) *optional_out_new_mapping = *new_mapping;

    if(!(ast_func->traits & AST_FUNC_FOREIGN)){
        if(ast_func->arity > 0 && strcmp(ast_func->arg_names[0], "this") == 0){
            // This is a struct method, attach a reference to the struct
            ast_type_t *this_type = &ast_func->arg_types[0];

            // Do basic checking to make sure the type is in the format: *Structure
            if(this_type->elements_length != 2 || this_type->elements[0]->id != AST_ELEM_POINTER){
                compiler_panic(compiler, this_type->source, "Type of 'this' must be a pointer to a struct");
                return FAILURE; 
            }

            switch(this_type->elements[1]->id){
            case AST_ELEM_BASE: {
                    // Check that the base isn't a primitive
                    char *base = ((ast_elem_base_t*) this_type->elements[1])->base;
                    if(typename_is_entended_builtin_type(base)){
                        compiler_panicf(compiler, this_type->source, "Type of 'this' must be a pointer to a struct (%s is a primitive)", base);
                        return FAILURE;
                    }

                    // Find the target structure
                    ast_struct_t *target = object_struct_find(NULL, object, &compiler->tmp, ((ast_elem_base_t*) this_type->elements[1])->base, NULL);
                    if(target == NULL){
                        compiler_panicf(compiler, this_type->source, "Undeclared struct '%s'", ((ast_elem_base_t*) this_type->elements[1])->base, NULL);
                        return FAILURE;
                    }

                    const weak_cstr_t method_name = module_func->name;
                    weak_cstr_t struct_name = ((ast_elem_base_t*) this_type->elements[1])->base;
                    ir_module_insert_method(module, struct_name, method_name, ir_func_id, ast_func_id, preserve_sortedness);
                }
                break;
            case AST_ELEM_GENERIC_BASE: {
                    ast_elem_generic_base_t *generic_base = (ast_elem_generic_base_t*) this_type->elements[1];
                    ast_polymorphic_struct_t *template = object_polymorphic_struct_find(NULL, object, &compiler->tmp, generic_base->name, NULL);
                    
                    if(template == NULL){
                        compiler_panicf(compiler, this_type->source, "Undeclared polymorphic struct '%s'", generic_base->name);
                        return FAILURE;
                    }

                    if(template->generics_length != generic_base->generics_length){
                        compiler_panic(compiler, this_type->source, "INTERNAL ERROR: ir_gen_func_head got method with incorrect number of type parameters for generic struct type for 'this'");
                        return FAILURE;
                    }

                    ir_module_insert_generic_method(module,
                        generic_base->name,
                        generic_base->generics, // NOTE: Memory for function argument types should persist, so this is ok
                        generic_base->generics_length,
                        module_func->name,
                        ir_func_id,
                        ast_func_id,
                    preserve_sortedness);
                }
                break;
            default:
                compiler_panic(compiler, this_type->source, "Type of 'this' must be a pointer to a struct");
                return FAILURE;
            }
        }
    } else {
        // If 'foreign' function, we don't ever process the statements, which is where we generate the IR argument types here instead

        while(module_func->arity != ast_func->arity){
            if(ir_gen_resolve_type(compiler, object, &ast_func->arg_types[module_func->arity], &module_func->argument_types[module_func->arity])) return FAILURE;
            module_func->arity++;
        }
    }

    if(ast_func->traits & AST_FUNC_MAIN){
        module->common.has_main = true;
        module->common.ast_main_id = ast_func_id;
        module->common.ir_main_id = ir_func_id;
    }

    if(ast_func->traits & AST_FUNC_MAIN && ast_type_is_void(&ast_func->return_type)){
        // If it's the main function and returns void, return int under the hood
        module_func->return_type = ir_pool_alloc(&module->pool, sizeof(ir_type_t));
        module_func->return_type->kind = TYPE_KIND_S32;
        // neglect 'module_func->return_type->extra'
    } else {
        if(ir_gen_resolve_type(compiler, object, &ast_func->return_type, &module_func->return_type)) return FAILURE;
    }

    return SUCCESS;
}

errorcode_t ir_gen_functions_body(compiler_t *compiler, object_t *object, ir_job_list_t *job_list){
    // NOTE: Only ir_gens function body; assumes skeleton already exists

    ast_func_t **ast_funcs = &object->ast.funcs;
    
    ir_builder_init(object->ir_module.init_builder, compiler, object, object->ir_module.common.ast_main_id, object->ir_module.common.ir_main_id, job_list, true);
    ir_builder_init(object->ir_module.deinit_builder, compiler, object, object->ir_module.common.ast_main_id, object->ir_module.common.ir_main_id, job_list, true);

    while(job_list->length != 0){
        ir_func_mapping_t *job = &job_list->jobs[--job_list->length];
        if((*ast_funcs)[job->ast_func_id].traits & AST_FUNC_FOREIGN) continue;

        if(ir_gen_func_statements(compiler, object, job->ast_func_id, job->ir_func_id, job_list)) return FAILURE;
    }
    
    return SUCCESS;
}

errorcode_t ir_gen_job_list(object_t *object, ir_job_list_t *job_list){
    length_t *mappings_length = &object->ir_module.func_mappings_length;

    job_list->jobs = malloc(sizeof(ir_func_mapping_t) * (*mappings_length + 4));
    job_list->length = *mappings_length;
    job_list->capacity = *mappings_length + 4;

    memcpy(job_list->jobs, object->ir_module.func_mappings, sizeof(ir_func_mapping_t) * *mappings_length);
    return SUCCESS;
}

errorcode_t ir_gen_globals(compiler_t *compiler, object_t *object){
    ast_t *ast = &object->ast;
    ir_module_t *module = &object->ir_module;

    for(length_t g = 0; g != ast->globals_length; g++){
        ir_global_t *global = &module->globals[g];

        global->name = ast->globals[g].name;
        global->traits = TRAIT_NONE;

        if(ast->globals[g].traits & AST_GLOBAL_EXTERNAL) global->traits |= IR_GLOBAL_EXTERNAL;
        if(ast->globals[g].traits & AST_GLOBAL_THREAD_LOCAL) global->traits |= IR_GLOBAL_THREAD_LOCAL;
        
        global->trusted_static_initializer = NULL;

        if(ir_gen_resolve_type(compiler, object, &ast->globals[g].type, &global->type)){
            return FAILURE;
        }

        module->globals_length++;
    }

    return SUCCESS;
}

errorcode_t ir_gen_globals_init(ir_builder_t *builder){
    // Generates instructions for initializing global variables
    ast_global_t *globals = builder->object->ast.globals;
    length_t globals_length = builder->object->ast.globals_length;

    for(length_t g = 0; g != globals_length; g++){
        ast_global_t *ast_global = &globals[g];
        if(ast_global->initial == NULL) continue;

        ir_value_t *value;
        ast_type_t value_ast_type;

        if(ir_gen_expr(builder, ast_global->initial, &value, false, &value_ast_type)) return FAILURE;

        if(!ast_types_conform(builder, &value, &value_ast_type, &ast_global->type, CONFORM_MODE_ASSIGNING)){
            char *a_type_str = ast_type_str(&value_ast_type);
            char *b_type_str = ast_type_str(&ast_global->type);
            compiler_panicf(builder->compiler, ast_global->initial->source, "Incompatible types '%s' and '%s'", a_type_str, b_type_str);
            free(a_type_str);
            free(b_type_str);
            ast_type_free(&value_ast_type);
            return FAILURE;
        }

        ast_type_free(&value_ast_type);

        ir_type_t *ptr_to_type = ir_type_pointer_to(builder->pool, builder->object->ir_module.globals[g].type);
        ir_value_t *destination = build_gvarptr(builder, ptr_to_type, g);
        build_store(builder, value, destination, ast_global->source);
    }
    return SUCCESS;
}

errorcode_t ir_gen_special_globals(compiler_t *compiler, object_t *object){
    ast_global_t *globals = object->ast.globals;
    length_t globals_length = object->ast.globals_length;

    for(length_t g = 0; g != globals_length; g++){
        ast_global_t *ast_global = &globals[g];
        if(!(ast_global->traits & AST_GLOBAL_SPECIAL)) continue;
        if(ir_gen_special_global(compiler, object, ast_global, g)) return FAILURE;
    }

    return SUCCESS;
}

errorcode_t ir_gen_special_global(compiler_t *compiler, object_t *object, ast_global_t *ast_global, length_t global_variable_id){
    // NOTE: Assumes (ast_global->traits & AST_GLOBAL_SPECIAL)

    ir_module_t *ir_module = &object->ir_module;
    ir_pool_t *pool = &ir_module->pool;
    type_table_t *type_table = object->ast.type_table;

    ir_type_t *ptr_to_type = ir_pool_alloc(pool, sizeof(ir_type_t));
    ptr_to_type->kind = TYPE_KIND_POINTER;
    ptr_to_type->extra = NULL;

    if(ir_gen_resolve_type(compiler, object, &ast_global->type, (ir_type_t**) &ptr_to_type->extra)){
        internalerrorprintf("Failed to get IR type for special global variable\n");
        return FAILURE;
    }

    // NOTE: DANGEROUS: 'global_variable_id' is assumed to be the same between AST and IR global variable lists
    ir_global_t *ir_global = &ir_module->globals[global_variable_id];

    // TODO: CLEANUP: Cleanup this messy code
    if(ast_global->traits & AST_GLOBAL___TYPES__){
        ir_type_t *any_type_type, *any_struct_type_type, *any_union_type_type, *any_ptr_type_type,
            *any_funcptr_type_type, *any_fixed_array_type_type, *any_type_ptr_type = NULL, *ubyte_ptr_type;
        
        if(compiler->traits & COMPILER_NO_TYPEINFO){
            if(!ir_type_map_find(&ir_module->type_map, "AnyType", &any_type_type)){
                internalerrorprintf("Failed to get 'AnyType' which should've been injected\n");
                redprintf("    (when creating null pointer to initialize __types__ because type info was disabled)\n")
                return FAILURE;
            }

            any_type_ptr_type = ir_type_pointer_to(pool, any_type_type);
            ir_global->trusted_static_initializer = build_null_pointer_of_type(pool, ir_type_pointer_to(pool, any_type_ptr_type));
            return SUCCESS;
        }

        if(!ir_type_map_find(&ir_module->type_map, "AnyType", &any_type_type)
        || !ir_type_map_find(&ir_module->type_map, "AnyStructType", &any_struct_type_type)
        || !ir_type_map_find(&ir_module->type_map, "AnyUnionType", &any_union_type_type)
        || !ir_type_map_find(&ir_module->type_map, "AnyPtrType", &any_ptr_type_type)
        || !ir_type_map_find(&ir_module->type_map, "AnyFuncPtrType", &any_funcptr_type_type)
        || !ir_type_map_find(&ir_module->type_map, "AnyFixedArrayType", &any_fixed_array_type_type)
        || !ir_type_map_find(&ir_module->type_map, "ubyte", &ubyte_ptr_type)){
            internalerrorprintf("Failed to find types used by the runtime type table that should've been injected\n");
            return FAILURE;
        }

        ir_type_t *usize_type = ir_pool_alloc(pool, sizeof(ir_type_t));
        usize_type->kind = TYPE_KIND_U64;

        any_type_ptr_type = ir_type_pointer_to(pool, any_type_type);
        ubyte_ptr_type = ir_type_pointer_to(pool, ubyte_ptr_type);

        ir_value_array_literal_t *array_literal = ir_pool_alloc(pool, sizeof(ir_value_array_literal_t));
        array_literal->values = NULL; // Will be set to array_values
        array_literal->length = type_table->length;

        ir_value_t *array_value = ir_pool_alloc(pool, sizeof(ir_value_t));
        array_value->value_type = VALUE_TYPE_ARRAY_LITERAL;
        array_value->type = ir_type_pointer_to(pool, any_type_ptr_type);
        array_value->extra = array_literal;
        
        ir_value_t **array_values = ir_pool_alloc(pool, sizeof(ir_value_t*) * type_table->length);

        for(length_t i = 0; i != type_table->length; i++){
            if(ir_gen_resolve_type(compiler, object, &type_table->entries[i].ast_type, &type_table->entries[i].ir_type)){
                return FAILURE;
            }

            switch(type_table->entries[i].ir_type->kind){
            case TYPE_KIND_POINTER:
                array_values[i] = build_anon_global(ir_module, any_ptr_type_type, true);
                break;
            case TYPE_KIND_STRUCTURE:
                array_values[i] = build_anon_global(ir_module, any_struct_type_type, true);
                break;
            case TYPE_KIND_UNION:
                array_values[i] = build_anon_global(ir_module, any_union_type_type, true);
                break;
            case TYPE_KIND_FUNCPTR:
                array_values[i] = build_anon_global(ir_module, any_funcptr_type_type, true);
                break;
            case TYPE_KIND_FIXED_ARRAY:
                array_values[i] = build_anon_global(ir_module, any_fixed_array_type_type, true);
                break;
            default:
                array_values[i] = build_anon_global(ir_module, any_type_type, true);
            }
        }

        // TODO: CLEANUP: Cleanup this messy code
        for(length_t i = 0; i != type_table->length; i++){
            ir_type_t *initializer_type;
            ir_value_t **initializer_members;
            length_t initializer_members_length;
            unsigned int any_type_kind_id = ANY_TYPE_KIND_VOID;
            unsigned int type_type_kind = type_table->entries[i].ir_type->kind;

            switch(type_type_kind){
            case TYPE_KIND_POINTER: {
                    /* struct AnyPtrType(kind AnyTypeKind, name *ubyte, is_alias bool, size usize, subtype *AnyType) */

                    initializer_members = ir_pool_alloc(pool, sizeof(ir_value_t*) * 5);
                    initializer_members_length = 5;

                    maybe_index_t subtype_index = -1;

                    // HACK: We really shouldn't be doing this
                    if(type_table->entries[i].ast_type.elements_length > 1){
                        ast_type_t dereferenced = ast_type_clone(&type_table->entries[i].ast_type);
                        
                        ast_type_dereference(&dereferenced);

                        char *dereferenced_name = ast_type_str(&dereferenced);
                        subtype_index = type_table_find(type_table, dereferenced_name);
                        ast_type_free(&dereferenced);
                        free(dereferenced_name);
                    }

                    if(subtype_index == -1){
                        ir_value_t *null_pointer = build_null_pointer_of_type(pool, any_type_ptr_type);
                        initializer_members[4] = null_pointer; // subtype
                    } else {
                        initializer_members[4] = build_const_bitcast(pool, array_values[subtype_index], any_type_ptr_type); // subtype
                    }

                    initializer_type = any_ptr_type_type;
                }
                break;
            case TYPE_KIND_STRUCTURE: case TYPE_KIND_UNION: {
                    /* struct AnyStructType (kind AnyTypeKind, name *ubyte, is_alias bool, size usize, members **AnyType, length usize, offsets *usize, member_names **ubyte, is_packed bool) */
                    /* struct AnyUnionType (kind AnyTypeKind, name *ubyte, is_alias bool, size usize, members **AnyType, length usize, member_names **ubyte) */

                    ir_type_extra_composite_t *composite = (ir_type_extra_composite_t*) type_table->entries[i].ir_type->extra;
                    initializer_members_length = type_type_kind == TYPE_KIND_STRUCTURE ? 9 : 7;
                    initializer_members = ir_pool_alloc(pool, sizeof(ir_value_t*) * initializer_members_length);

                    ir_value_t **composite_members = ir_pool_alloc(pool, sizeof(ir_value_t*) * composite->subtypes_length);
                    ir_value_t **composite_offsets = type_type_kind == TYPE_KIND_STRUCTURE ? ir_pool_alloc(pool, sizeof(ir_value_t*) * composite->subtypes_length) : NULL;
                    ir_value_t **composite_member_names = ir_pool_alloc(pool, sizeof(ir_value_t*) * composite->subtypes_length);

                    ast_elem_t *elem = type_table->entries[i].ast_type.elements[0];

                    if(elem->id == AST_ELEM_GENERIC_BASE){
                        ast_elem_generic_base_t *generic_base = (ast_elem_generic_base_t*) elem;
                        
                        // Find polymorphic struct
                        ast_polymorphic_struct_t *template = object_polymorphic_struct_find(NULL, object, &compiler->tmp, generic_base->name, NULL);
                        
                        if(template == NULL){
                            internalerrorprintf("Failed to find polymorphic struct '%s' that should exist when generating runtime type table!\n", generic_base->name);
                            return FAILURE;
                        }

                        // Substitute generic type parameters
                        ast_poly_catalog_t catalog;
                        ast_poly_catalog_init(&catalog);

                        if(template->generics_length != generic_base->generics_length){
                            internalerrorprintf("Polymorphic struct '%s' type parameter length mismatch when generating runtime type table!\n", generic_base->name);
                            ast_poly_catalog_free(&catalog);
                            return FAILURE;
                        }

                        for(length_t i = 0; i != template->generics_length; i++){
                            ast_poly_catalog_add_type(&catalog, template->generics[i], &generic_base->generics[i]);
                        }

                        ast_type_t container_ast_type;
                        if(resolve_type_polymorphics(compiler, type_table, &catalog, &type_table->entries[i].ast_type, &container_ast_type)){
                            ast_poly_catalog_free(&catalog);
                            return FAILURE;
                        }

                        // Generate meta data
                        for(length_t s = 0; s != composite->subtypes_length; s++){
                            ast_type_t member_ast_type;

                            // DANGEROUS: WARNING: Accessing 'template->field_types' based on index going to 'composite->subtypes_length'
                            if(resolve_type_polymorphics(compiler, type_table, &catalog, &template->field_types[s], &member_ast_type)){
                                ast_poly_catalog_free(&catalog);
                                return FAILURE;
                            }
                            
                            char *member_type_name = ast_type_str(&member_ast_type);
                            maybe_index_t subtype_index = type_table_find(type_table, member_type_name);
                            free(member_type_name);

                            if(subtype_index == -1){
                                composite_members[s] = build_null_pointer_of_type(pool, any_type_ptr_type); // members[s]
                            } else {
                                composite_members[s] = build_const_bitcast(pool, array_values[subtype_index], any_type_ptr_type); // members[s]
                            }

                            ir_type_t *struct_ir_type;
                            if(ir_gen_resolve_type(compiler, object, &container_ast_type, &struct_ir_type)){
                                internalerrorprintf("ir_gen_resolve_type for RTTI composite offset computation failed!\n");
                                ast_type_free(&container_ast_type);
                                ast_type_free(&member_ast_type);
                                return FAILURE;
                            }

                            if(composite_offsets) composite_offsets[s] = build_offsetof_ex(pool, usize_type, struct_ir_type, s);
                            composite_member_names[s] = build_literal_cstr_ex(pool, &ir_module->type_map, template->field_names[s]);
                            ast_type_free(&member_ast_type);
                        }

                        ast_type_free(&container_ast_type);
                        ast_poly_catalog_free(&catalog);
                    } else if(elem->id == AST_ELEM_BASE){
                        const char *struct_name = ((ast_elem_base_t*) elem)->base;
                        ast_struct_t *structure = object_struct_find(NULL, object, &compiler->tmp, struct_name, NULL);

                        if(structure == NULL){
                            internalerrorprintf("Failed to find struct '%s' that should exist when generating runtime type table!\n", struct_name);
                            return FAILURE;
                        }

                        if(structure->field_count != composite->subtypes_length){
                            internalerrorprintf("Mismatching member count of IR as AST types for struct '%s' when generating runtime type table!\n", struct_name);
                            return FAILURE;
                        }

                        for(length_t s = 0; s != composite->subtypes_length; s++){
                            char *member_type_name = ast_type_str(&structure->field_types[s]);
                            maybe_index_t subtype_index = type_table_find(type_table, member_type_name);
                            free(member_type_name);

                            if(subtype_index == -1){
                                composite_members[s] = build_null_pointer_of_type(pool, any_type_ptr_type); // members[s]
                            } else {
                                composite_members[s] = build_const_bitcast(pool, array_values[subtype_index], any_type_ptr_type); // members[s]
                            }

                            // SPEED: TODO: Make 'struct_ast_type' not dynamically allocated
                            ast_type_t struct_ast_type;
                            ast_type_make_base(&struct_ast_type, strclone(struct_name));

                            ir_type_t *struct_ir_type;
                            if(ir_gen_resolve_type(compiler, object, &struct_ast_type, &struct_ir_type)){
                                internalerrorprintf("ir_gen_resolve_type for RTTI composite offset computation failed!\n");
                                ast_type_free(&struct_ast_type);
                                return FAILURE;
                            }

                            ast_type_free(&struct_ast_type);

                            if(composite_offsets) composite_offsets[s] = build_offsetof_ex(pool, usize_type, struct_ir_type, s);
                            composite_member_names[s] = build_literal_cstr_ex(pool, &ir_module->type_map, structure->field_names[s]);
                        }
                    } else {
                        internalerrorprintf("Unknown AST type element for TYPE_KIND_STRUCTURE when generating runtime type information!\n");
                        return FAILURE;
                    }

                    ir_value_t *members_array = build_static_array(pool, any_type_ptr_type, composite_members, composite->subtypes_length);
                    ir_value_t *member_names_array = build_static_array(pool, ubyte_ptr_type, composite_member_names, composite->subtypes_length);
                    
                    if(type_type_kind == TYPE_KIND_STRUCTURE){
                        ir_value_t *offsets_array = build_static_array(pool, usize_type, composite_offsets, composite->subtypes_length);

                        initializer_members[4] = members_array;
                        initializer_members[5] = build_literal_usize(pool, composite->subtypes_length); // length
                        initializer_members[6] = offsets_array;
                        initializer_members[7] = member_names_array;
                        initializer_members[8] = build_bool(pool, composite->traits & TYPE_KIND_COMPOSITE_PACKED); // is_packed
                        initializer_type = any_struct_type_type;
                    } else {
                        initializer_members[4] = members_array;
                        initializer_members[5] = build_literal_usize(pool, composite->subtypes_length); // length
                        initializer_members[6] = member_names_array;
                        initializer_type = any_union_type_type;
                    }
                }
                break;
            case TYPE_KIND_FIXED_ARRAY: {
                    /* struct AnyFixedArrayType (kind AnyTypeKind, name *ubyte, is_alias bool, size usize, subtype *AnyType, length usize) */

                    initializer_members = ir_pool_alloc(pool, sizeof(ir_value_t*) * 6);
                    initializer_members_length = 6;

                    // DANGEROUS: NOTE: Assumes elements after AST_ELEM_FIXED_ARRAY
                    ast_elem_fixed_array_t *elem = (ast_elem_fixed_array_t*) type_table->entries[i].ast_type.elements[0];

                    maybe_index_t subtype_index = -1;

                    // HACK: We really shouldn't be doing this
                    if(type_table->entries[i].ast_type.elements_length > 1){
                        ast_type_t unwrapped = ast_type_clone(&type_table->entries[i].ast_type);

                        ast_type_unwrap_fixed_array(&unwrapped);

                        char *unwrapped_name = ast_type_str(&unwrapped);
                        subtype_index = type_table_find(type_table, unwrapped_name);
                        ast_type_free(&unwrapped);
                        free(unwrapped_name);
                    }

                    if(subtype_index == -1){
                        ir_value_t *null_pointer = build_null_pointer_of_type(pool, any_type_ptr_type);
                        initializer_members[4] = null_pointer; // subtype
                    } else {
                        initializer_members[4] = build_const_bitcast(pool, array_values[subtype_index], any_type_ptr_type); // subtype
                    }

                    initializer_members[5] = build_literal_usize(pool, elem->length);
                    initializer_type = any_fixed_array_type_type;
                }
                break;
            case TYPE_KIND_FUNCPTR: {
                    /* struct AnyFuncPtrType (kind AnyTypeKind, name *ubyte, is_alias bool, size usize, args **AnyType, length usize, return_type *AnyType, is_vararg bool, is_stdcall bool) */

                    initializer_members = ir_pool_alloc(pool, sizeof(ir_value_t*) * 9);
                    initializer_members_length = 9;

                    // DANGEROUS: NOTE: Assumes element is AST_ELEM_FUNC
                    ast_elem_func_t *elem = (ast_elem_func_t*) type_table->entries[i].ast_type.elements[0];

                    maybe_index_t subtype_index = -1;

                    // TODO: CLEANUP: Clean up this messy code
                    char *type_string;
                    ir_value_t **args = ir_pool_alloc(pool, sizeof(ir_value_t*) * elem->arity);

                    for(length_t i = 0; i != elem->arity; i++){
                        type_string = ast_type_str(&elem->arg_types[i]);
                        subtype_index = type_table_find(type_table, type_string);
                        free(type_string);

                        if(subtype_index == -1){
                            args[i] = build_null_pointer_of_type(pool, any_type_ptr_type);
                        } else {
                            args[i] = build_const_bitcast(pool, array_values[subtype_index], any_type_ptr_type);
                        }
                    }

                    type_string = ast_type_str(elem->return_type);
                    subtype_index = type_table_find(type_table, type_string);
                    free(type_string);
                    
                    ir_value_t *return_type;
                    if(subtype_index == -1){
                        return_type = build_null_pointer_of_type(pool, any_type_ptr_type);
                    } else {
                        return_type = build_const_bitcast(pool, array_values[subtype_index], any_type_ptr_type);
                    }

                    initializer_members[4] = build_static_array(pool, any_type_ptr_type, args, elem->arity);
                    initializer_members[5] = build_literal_usize(pool, elem->arity);
                    initializer_members[6] = return_type;
                    initializer_members[7] = build_bool(pool, elem->traits & AST_FUNC_VARARG);
                    initializer_members[8] = build_bool(pool, elem->traits & AST_FUNC_STDCALL);
                    initializer_type = any_funcptr_type_type;
                }
                break;
            default:
                initializer_members = ir_pool_alloc(pool, sizeof(ir_value_t*) * 4);
                initializer_members_length = 4;
                initializer_type = any_type_type;
            }

            switch(type_type_kind){
            case TYPE_KIND_NONE:        any_type_kind_id = ANY_TYPE_KIND_VOID; break;
            case TYPE_KIND_POINTER:     any_type_kind_id = ANY_TYPE_KIND_PTR; break;
            case TYPE_KIND_S8:          any_type_kind_id = ANY_TYPE_KIND_BYTE; break;
            case TYPE_KIND_S16:         any_type_kind_id = ANY_TYPE_KIND_SHORT; break;
            case TYPE_KIND_S32:         any_type_kind_id = ANY_TYPE_KIND_INT; break;
            case TYPE_KIND_S64:         any_type_kind_id = ANY_TYPE_KIND_LONG; break;
            case TYPE_KIND_U8:          any_type_kind_id = ANY_TYPE_KIND_UBYTE; break;
            case TYPE_KIND_U16:         any_type_kind_id = ANY_TYPE_KIND_USHORT; break;
            case TYPE_KIND_U32:         any_type_kind_id = ANY_TYPE_KIND_UINT; break;
            case TYPE_KIND_U64:         any_type_kind_id = ANY_TYPE_KIND_ULONG; break;
            case TYPE_KIND_FLOAT:       any_type_kind_id = ANY_TYPE_KIND_FLOAT; break;
            case TYPE_KIND_DOUBLE:      any_type_kind_id = ANY_TYPE_KIND_DOUBLE; break;
            case TYPE_KIND_BOOLEAN:     any_type_kind_id = ANY_TYPE_KIND_BOOL; break;
            case TYPE_KIND_STRUCTURE:   any_type_kind_id = ANY_TYPE_KIND_STRUCT; break;
            case TYPE_KIND_UNION:       any_type_kind_id = ANY_TYPE_KIND_UNION; break;
            case TYPE_KIND_FUNCPTR:     any_type_kind_id = ANY_TYPE_KIND_FUNC_PTR; break;
            case TYPE_KIND_FIXED_ARRAY: any_type_kind_id = ANY_TYPE_KIND_FIXED_ARRAY; break;

            // Unsupported Type Kinds
            case TYPE_KIND_HALF:        any_type_kind_id = ANY_TYPE_KIND_USHORT; break;
            // case TYPE_KIND_UNION: ignored
            }

            initializer_members[0] = build_literal_usize(pool, any_type_kind_id); // kind
            initializer_members[1] = build_literal_cstr_ex(pool, &ir_module->type_map, type_table->entries[i].name); // name
            initializer_members[2] = build_bool(pool, type_table->entries[i].is_alias); // is_alias
            initializer_members[3] = build_const_sizeof(pool, usize_type, type_table->entries[i].ir_type); // size

            ir_value_t *initializer = build_static_struct(ir_module, initializer_type, initializer_members, initializer_members_length, false);
            build_anon_global_initializer(ir_module, array_values[i], initializer);

            if(initializer_type != any_type_ptr_type){
                array_values[i] = build_const_bitcast(pool, array_values[i], any_type_ptr_type);
            }
        }

        array_literal->values = array_values;
        ir_global->trusted_static_initializer = array_value;
        return SUCCESS;
    }

    if(ast_global->traits & AST_GLOBAL___TYPES_LENGTH__){
        if(compiler->traits & COMPILER_NO_TYPEINFO){
            ir_global->trusted_static_initializer = build_literal_usize(pool, 0);
            return SUCCESS;
        }

        ir_value_t *value = build_literal_usize(pool, type_table->length);
        ir_global->trusted_static_initializer = value;
        return SUCCESS;
    }

    if(ast_global->traits & AST_GLOBAL___TYPE_KINDS__){
        ir_type_t *ubyte_ptr_type, *ubyte_ptr_ptr_type;

        if(!ir_type_map_find(&ir_module->type_map, "ubyte", &ubyte_ptr_type)){
            internalerrorprintf("Failed to find types used by the runtime type table that should've been injected\n");
            return FAILURE;
        }

        // Construct IR Types we need
        ubyte_ptr_type = ir_type_pointer_to(pool, ubyte_ptr_type);
        ubyte_ptr_ptr_type = ir_type_pointer_to(pool, ubyte_ptr_type);

        if(compiler->traits & COMPILER_NO_TYPEINFO){
            ir_global->trusted_static_initializer = build_null_pointer_of_type(pool, ubyte_ptr_ptr_type);
            return SUCCESS;
        }

        ir_value_array_literal_t *kinds_array_literal = ir_pool_alloc(pool, sizeof(ir_value_array_literal_t));
        kinds_array_literal->values = NULL; // Will be set to array_values
        kinds_array_literal->length = MAX_ANY_TYPE_KIND + 1;

        ir_value_t *kinds_array_value = ir_pool_alloc(pool, sizeof(ir_value_t));
        kinds_array_value->value_type = VALUE_TYPE_ARRAY_LITERAL;
        kinds_array_value->type = ir_type_pointer_to(pool, ubyte_ptr_type);
        kinds_array_value->extra = kinds_array_literal;
        
        ir_value_t **array_values = ir_pool_alloc(pool, sizeof(ir_value_t*) * (MAX_ANY_TYPE_KIND + 1));

        for(length_t i = 0; i != MAX_ANY_TYPE_KIND + 1; i++){
            array_values[i] = build_literal_cstr_ex(pool, &ir_module->type_map, (weak_cstr_t) any_type_kind_names[i]);
        }

        kinds_array_literal->values = array_values;
        ir_global->trusted_static_initializer = kinds_array_value;
        return SUCCESS;
    }

    if(ast_global->traits & AST_GLOBAL___TYPE_KINDS_LENGTH__){
        if(compiler->traits & COMPILER_NO_TYPEINFO){
            ir_global->trusted_static_initializer = build_literal_usize(pool, 0);
            return SUCCESS;
        }

        ir_value_t *value = build_literal_usize(pool, MAX_ANY_TYPE_KIND + 1);
        ir_global->trusted_static_initializer = value;
        return SUCCESS;
    }

    // Should never reach
    internalerrorprintf("Encountered unknown special global variable '%s'\n", ast_global->name);
    return FAILURE;
}

errorcode_t ir_gen_fill_in_rtti(object_t *object){
    ir_module_t *ir_module = &object->ir_module;

    type_table_t *type_table = object->ast.type_table;
    if(type_table == NULL) return FAILURE;

    rtti_relocation_t *relocations = ir_module->rtti_relocations;
    length_t relocations_length = ir_module->rtti_relocations_length;

    for(length_t i = 0; i != relocations_length; i++){
        if(rtti_resolve(type_table, &relocations[i])) return FAILURE;
    }

    return SUCCESS;
}

weak_cstr_t ir_gen_ast_definition_string(ir_pool_t *pool, ast_func_t *ast_func){
    length_t length = 0;
    length_t capacity = 96;
    strong_cstr_t string = malloc(capacity);

    // TODO: CLEANUP: Clean up this messy code
    // TODO: CLEANUP: Factor out a lot of this garbage

    {
        // Append function name
        length_t name_length = strlen(ast_func->name);
        expand((void**) &string, sizeof(char), length, &capacity, name_length, capacity);
        memcpy(&string[length], ast_func->name, name_length);
        length += name_length;
    }

    // Append '('
    expand((void**) &string, sizeof(char), length, &capacity, 1, capacity);
    string[length++] = '(';

    // Append argument list
    for(length_t i = 0; i != ast_func->arity; i++){
        char *type_str = ast_type_str(&ast_func->arg_types[i]);
        length_t type_str_len = strlen(type_str);
        expand((void**) &string, sizeof(char), length, &capacity, type_str_len, capacity);
        memcpy(&string[length], type_str, type_str_len);
        length += type_str_len;
        free(type_str);

        if(i + 1 != ast_func->arity){
            expand((void**) &string, sizeof(char), length, &capacity, 2, capacity);
            string[length++] = ',';
            string[length++] = ' ';
        }
    }

    // Append '( '
    expand((void**) &string, sizeof(char), length, &capacity, 2, capacity);
    string[length++] = ')';
    string[length++] = ' ';

    {
        // Append return type
        char *type_str = ast_type_str(&ast_func->return_type);
        length_t type_str_len = strlen(type_str);
        expand((void**) &string, sizeof(char), length, &capacity, type_str_len, capacity);
        memcpy(&string[length], type_str, type_str_len);
        length += type_str_len;
        free(type_str);
    }

    // Terminate string
    expand((void**) &string, sizeof(char), length, &capacity, 1, capacity);
    string[length++] = '\0';

    // Trade heap allocated string for IR memory pooled string
    char *destination = ir_pool_alloc(pool, length);
    memcpy(destination, string, length);
    free(string);
    return destination;
}

errorcode_t ir_gen_do_builtin_warn_bad_printf_format(ir_builder_t *builder, funcpair_t pair, ast_type_t *ast_types, ir_value_t **ir_values, source_t source, length_t variadic_length){
    // Find index of 'format' argument
    maybe_index_t format_index = -1;

    for(length_t name_index = 0; name_index != pair.ast_func->arity; name_index++){
        if(strcmp(pair.ast_func->arg_names[name_index], "format") == 0){
            format_index = name_index;
            break;
        }
    }

    if(format_index < 0){
        compiler_panicf(builder->compiler, pair.ast_func->source, "Function marked as __builtin_warn_bad_printf must have an argument named 'format'!\n");
        return FAILURE;
    }

    if(!ast_type_is_base_of(&pair.ast_func->arg_types[format_index], "String")){
        compiler_panicf(builder->compiler, pair.ast_func->source, "Function marked as __builtin_warn_bad_printf must have 'format' be a String!\n");
        return FAILURE;
    }

    // Undo __pass__ call
    if(ir_values[format_index]->value_type != VALUE_TYPE_RESULT) return SUCCESS;
    ir_value_result_t *pass_result = (ir_value_result_t*) ir_values[format_index]->extra;
    
    // Undo result value to get call instruction
    ir_instr_call_t *call_instr = (ir_instr_call_t*) builder->basicblocks[pass_result->block_id].instructions[pass_result->instruction_id];
    ir_value_t *string_literal = call_instr->values[0];

    // Don't check if not string literal
    if(string_literal->value_type != VALUE_TYPE_STRUCT_LITERAL) return SUCCESS;

    // DANGEROUS:
    // Assuming string literal is created correctly since the AST type is 'String' and IR value type kind is VALUE_TYPE_STRUCT_LITERAL
    ir_value_struct_literal_t *extra = (ir_value_struct_literal_t*) string_literal->extra;
    
    ir_value_t *cstr_of_len_literal = extra->values[0];
    if(cstr_of_len_literal->value_type != VALUE_TYPE_CSTR_OF_LEN) return SUCCESS;
    
    ir_value_cstr_of_len_t *string = (ir_value_cstr_of_len_t*) cstr_of_len_literal->extra;
    length_t substitutions_gotten = 0;

    char *p = string->array;
    char *end = &string->array[string->length];

    while(p != end){
        if(*p++ != '%') continue;

        if(p == end || *p == '%') break;

        // size_modifier == 1 for 'hh'
        // size_modifier == 2 for 'h'
        // size_modifier == 4 for none
        // size_modifier == 8 for 'l' or 'll'
        int size_modifier = 4;

        while(p != end){
            if(*p == 'l'){
                size_modifier *= 2;
                if(size_modifier > 8) size_modifier = 8;
                p++;
                continue;
            }

            if(*p == 'h'){
                size_modifier /= 2;
                if(size_modifier == 0) size_modifier = 1;
                p++;
                continue;
            }
            break;
        }
        if(p == end) break;

        if(substitutions_gotten >= variadic_length){
            strong_cstr_t escaped = string_to_escaped_string(string->array, string->length, '"');
            compiler_panicf(builder->compiler, source, "Not enough arguments specified for format %s", escaped);
            free(escaped);
            return FAILURE;
        }

        ast_type_t *given_type = &ast_types[pair.ast_func->arity + substitutions_gotten++];
        weak_cstr_t target = NULL;

        switch(*p++){
        case 'S':
            if(!ast_type_is_base_of(given_type, "String")){
                bad_printf_format(builder->compiler, source, given_type, "String", substitutions_gotten, size_modifier, *(p - 1), false);
                return FAILURE;
            }
            break;
        case 'b': case 'B': case 'y': case 'Y':
            if(!ast_type_is_base_of(given_type, "bool")){
                bad_printf_format(builder->compiler, source, given_type, "bool", substitutions_gotten, size_modifier, *(p - 1), false);
                return FAILURE;
            }
            break;
        case 's':
            if(!ast_type_is_base_ptr_of(given_type, "ubyte")){
                bad_printf_format(builder->compiler, source, given_type, "*ubyte", substitutions_gotten, size_modifier, *(p - 1), false);
                return FAILURE;
            }
            break;
        case 'c':
            if(!ast_type_is_base_of(given_type, "ubyte")){
                bad_printf_format(builder->compiler, source, given_type, "ubyte", substitutions_gotten, size_modifier, *(p - 1), false);
                return FAILURE;
            }
            break;
        case 'z':
            if(*p++ != 'u'){
                compiler_panicf(builder->compiler, source, "Expected 'u' after '%%z' in format string");
                return FAILURE;
            }

            if(!ast_type_is_base_of(given_type, "usize")){
                bad_printf_format(builder->compiler, source, given_type, "usize", substitutions_gotten, size_modifier, *(p - 2), false);
                return FAILURE;
            }
            break;
        case 'd': case 'i':
            switch(size_modifier){
            case 1:  target = "byte";  break;
            case 2:  target = "short"; break;
            case 8:  target = "long";  break;
            default: target = "int";
            }

            if(ast_type_is_base_of(given_type, target)){
                // Always allowed
            } else if(
                ast_type_is_base_of(given_type, "bool") ||
                ast_type_is_base_of(given_type, "byte") || 
                ast_type_is_base_of(given_type, "ubyte") || 
                ast_type_is_base_of(given_type, "short") || 
                ast_type_is_base_of(given_type, "ushort") ||
                ast_type_is_base_of(given_type, "int") ||
                ast_type_is_base_of(given_type, "uint") || 
                ast_type_is_base_of(given_type, "long") || 
                ast_type_is_base_of(given_type, "ulong") || 
                ast_type_is_base_of(given_type, "usize") || 
                ast_type_is_base_of(given_type, "successful")
            ){
                // Allowed, but discouraged (Error if RTTI is disabled)
                if(builder->compiler->traits & COMPILER_NO_TYPEINFO){
                    bad_printf_format(builder->compiler, source, given_type, target, substitutions_gotten, size_modifier, *(p - 1), true);
                    return FAILURE;
                }
            } else {
                // Never allowed
                bad_printf_format(builder->compiler, source, given_type, target, substitutions_gotten, size_modifier, *(p - 1), false);
                return FAILURE;
            }
            break;
        case 'u':
            switch(size_modifier){
            case 1:  target = "ubyte";  break;
            case 2:  target = "ushort"; break;
            case 8:  target = "ulong";  break;
            default: target = "uint";
            }

            if(ast_type_is_base_of(given_type, target) || (size_modifier == 8 && ast_type_is_base_of(given_type, "usize"))){
                // Always allowed
            } else if(
                ast_type_is_base_of(given_type, "bool") ||
                ast_type_is_base_of(given_type, "byte") || 
                ast_type_is_base_of(given_type, "ubyte") || 
                ast_type_is_base_of(given_type, "short") || 
                ast_type_is_base_of(given_type, "ushort") || 
                ast_type_is_base_of(given_type, "int") || 
                ast_type_is_base_of(given_type, "uint") || 
                ast_type_is_base_of(given_type, "long") || 
                ast_type_is_base_of(given_type, "ulong") || 
                ast_type_is_base_of(given_type, "usize") || 
                ast_type_is_base_of(given_type, "successful")
            ){
                // Allowed, but discouraged (Error if RTTI is disabled)
                if(builder->compiler->traits & COMPILER_NO_TYPEINFO){
                    bad_printf_format(builder->compiler, source, given_type, target, substitutions_gotten, size_modifier, *(p - 1), true);
                    return FAILURE;
                }
            } else {
                // Never allowed
                bad_printf_format(builder->compiler, source, given_type, target, substitutions_gotten, size_modifier, *(p - 1), false);
                return FAILURE;
            }
            break;
        case 'f':
            switch(size_modifier){
            case 2:  target = "float";  break;
            default: target = "double";
            }
            if(ast_type_is_base_of(given_type, target)){
                // Always allowed
            } else if(
                ast_type_is_base_of(given_type, "double") ||
                ast_type_is_base_of(given_type, "float") ||
                ast_type_is_base_of(given_type, "bool") ||
                ast_type_is_base_of(given_type, "byte") || 
                ast_type_is_base_of(given_type, "ubyte") || 
                ast_type_is_base_of(given_type, "short") || 
                ast_type_is_base_of(given_type, "ushort") || 
                ast_type_is_base_of(given_type, "int") || 
                ast_type_is_base_of(given_type, "uint") || 
                ast_type_is_base_of(given_type, "long") || 
                ast_type_is_base_of(given_type, "ulong") || 
                ast_type_is_base_of(given_type, "usize") || 
                ast_type_is_base_of(given_type, "successful")
            ){
                // Allowed, but discouraged (Error if RTTI is disabled)
                if(builder->compiler->traits & COMPILER_NO_TYPEINFO){
                    bad_printf_format(builder->compiler, source, given_type, target, substitutions_gotten, size_modifier, *(p - 1), true);
                    return FAILURE;
                }
            } else {
                // Never allowed
                bad_printf_format(builder->compiler, source, given_type, target, substitutions_gotten, size_modifier, *(p - 1), false);
                return FAILURE;
            }
            break;
        case 'p':
            if(!ast_type_is_pointer(given_type) && !ast_type_is_base_of(given_type, "ptr")){
                // Never allowed
                bad_printf_format(builder->compiler, source, given_type, "ptr' or '*T", substitutions_gotten, size_modifier, *(p - 1), false);
                return FAILURE;
            }
            break;
        default:
            if(compiler_warnf(builder->compiler, source, "Unrecognized format specifier '%%%c'", *(p - 1)))
                return FAILURE;
        }
    }

    if(substitutions_gotten < variadic_length){
        strong_cstr_t escaped = string_to_escaped_string(string->array, string->length, '"');
        compiler_panicf(builder->compiler, source, "Too many arguments specified for format %s", escaped);
        free(escaped);
        return FAILURE;
    }

    return SUCCESS;
}

void bad_printf_format(compiler_t *compiler, source_t source, ast_type_t *given_type, weak_cstr_t expected, int variadic_argument_number, int size_modifier, char specifier, bool is_semimatch){
    weak_cstr_t modifiers = "";
    weak_cstr_t additional_part = "";
    
    switch(size_modifier){
    case 1: modifiers = "hh"; break;
    case 2: modifiers = "h";  break;
    case 8: modifiers = "l";  break;
    }

    // Hack to get '%z' to always print as '%zu'
    if(specifier == 'z') additional_part = "u";

    strong_cstr_t incorrect_type = ast_type_str(given_type);
    compiler_panicf(compiler, source, "Got value of incorrect type for format specifier '%%%s%c%s'", modifiers, specifier, additional_part);
    printf("\n");

    printf("    Expected value of type '%s', got value of type '%s'\n", expected, incorrect_type);
    printf("    For %d%s variadic argument\n", variadic_argument_number, get_numeric_ending(variadic_argument_number));
    
    if(is_semimatch){
        printf("    Support for this type mismatch requires runtime type information\n");
    }

    free(incorrect_type);
}

weak_cstr_t get_numeric_ending(length_t integer){
    if(integer > 9 && integer < 11) return "th";

    switch(integer % 10){
    case 1: return "st";
    case 2: return "nd";
    case 3: return "rd";
    }

    return "th";
}
