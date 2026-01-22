#ifndef ASMOPT_H
#define ASMOPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct asmopt_context asmopt_context;

asmopt_context* asmopt_create(const char* architecture);
void asmopt_set_option(asmopt_context* ctx, const char* option, const char* value);
void asmopt_set_optimization_level(asmopt_context* ctx, int level);
void asmopt_set_target_cpu(asmopt_context* ctx, const char* cpu);
int asmopt_parse_file(asmopt_context* ctx, const char* filename);
int asmopt_parse_string(asmopt_context* ctx, const char* assembly);
int asmopt_optimize(asmopt_context* ctx);
char* asmopt_generate_assembly(asmopt_context* ctx);
char* asmopt_generate_report(asmopt_context* ctx);
void asmopt_get_stats(asmopt_context* ctx, size_t* original, size_t* optimized, size_t* replacements, size_t* removals);
void asmopt_destroy(asmopt_context* ctx);
void asmopt_enable_optimization(asmopt_context* ctx, const char* name);
void asmopt_disable_optimization(asmopt_context* ctx, const char* name);
void asmopt_set_format(asmopt_context* ctx, const char* format);
void asmopt_set_no_optimize(asmopt_context* ctx, int enabled);
void asmopt_set_preserve_all(asmopt_context* ctx, int enabled);
void asmopt_set_amd_optimizations(asmopt_context* ctx, int enabled);
char* asmopt_dump_ir_text(asmopt_context* ctx);
char* asmopt_dump_cfg_text(asmopt_context* ctx);
char* asmopt_dump_cfg_dot(asmopt_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
