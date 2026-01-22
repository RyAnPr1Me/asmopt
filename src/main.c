#include "asmopt.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define asmopt_isatty _isatty
#define asmopt_fileno _fileno
#else
#include <unistd.h>
#define asmopt_isatty isatty
#define asmopt_fileno fileno
#endif

typedef struct {
    const char* input_path;
    const char* output_path;
    const char* format;
    const char* report_path;
    const char* cfg_path;
    const char* march;
    const char* mtune;
    int opt_level;
    bool has_opt_level;
    bool no_optimize;
    bool preserve_all;
    bool stats;
    bool dump_ir;
    bool dump_cfg;
    int verbose;
    bool quiet;
    bool amd_optimize;
} asmopt_cli_options;

static void asmopt_set_bool_option(asmopt_context* ctx, const char* key, bool value) {
    asmopt_set_option(ctx, key, value ? "1" : "0");
}

static void asmopt_print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [options] input.s -o output.s\n"
            "Options:\n"
            "  -i, --input <file>       Input assembly file\n"
            "  -o, --output <file>      Output assembly file\n"
            "  -f, --format <format>    Syntax format (intel, att)\n"
            "  -O0..-O4                 Optimization level\n"
            "  --enable <opt>           Enable optimization\n"
            "  --disable <opt>          Disable optimization\n"
            "  --no-optimize            Parse and regenerate without optimization\n"
            "  --preserve-all           Preserve comments and formatting\n"
            "  --report <file>          Write optimization report\n"
            "  --stats                  Print optimization statistics\n"
            "  --cfg <file>             Write CFG dot output\n"
            "  --dump-ir                Dump IR to stderr\n"
            "  --dump-cfg               Dump CFG to stderr\n"
            "  -v, --verbose            Verbose output\n"
            "  -q, --quiet              Suppress non-error output\n"
            "  -m, --march <arch>        Target architecture\n"
            "  --mtune <cpu>            Target CPU\n"
            "  --amd-optimize           Enable AMD optimizations\n"
            "  --no-amd-optimize        Disable AMD optimizations\n",
            prog);
}

static bool asmopt_parse_args(int argc, char** argv, asmopt_cli_options* options, asmopt_context* ctx) {
    options->opt_level = 2;
    options->has_opt_level = false;
    options->amd_optimize = true;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "-i") == 0 || strcmp(arg, "--input") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            options->input_path = argv[++i];
        } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            options->output_path = argv[++i];
        } else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--format") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            options->format = argv[++i];
            asmopt_set_format(ctx, options->format);
        } else if (strcmp(arg, "-O0") == 0 || strcmp(arg, "-O1") == 0 || strcmp(arg, "-O2") == 0 ||
                   strcmp(arg, "-O3") == 0 || strcmp(arg, "-O4") == 0) {
            options->opt_level = arg[2] - '0';
            options->has_opt_level = true;
            asmopt_set_optimization_level(ctx, options->opt_level);
        } else if (strcmp(arg, "--enable") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            asmopt_enable_optimization(ctx, argv[++i]);
        } else if (strcmp(arg, "--disable") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            asmopt_disable_optimization(ctx, argv[++i]);
        } else if (strcmp(arg, "--no-optimize") == 0) {
            options->no_optimize = true;
            asmopt_set_no_optimize(ctx, 1);
            asmopt_set_bool_option(ctx, "no_optimize", true);
        } else if (strcmp(arg, "--preserve-all") == 0) {
            options->preserve_all = true;
            asmopt_set_preserve_all(ctx, 1);
            asmopt_set_bool_option(ctx, "preserve_all", true);
        } else if (strcmp(arg, "--report") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            options->report_path = argv[++i];
        } else if (strcmp(arg, "--stats") == 0) {
            options->stats = true;
            asmopt_set_bool_option(ctx, "stats", true);
        } else if (strcmp(arg, "--cfg") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            options->cfg_path = argv[++i];
        } else if (strcmp(arg, "--dump-ir") == 0) {
            options->dump_ir = true;
            asmopt_set_bool_option(ctx, "dump_ir", true);
        } else if (strcmp(arg, "--dump-cfg") == 0) {
            options->dump_cfg = true;
            asmopt_set_bool_option(ctx, "dump_cfg", true);
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            options->verbose += 1;
            asmopt_set_option(ctx, "verbose", "1");
        } else if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
            options->quiet = true;
            asmopt_set_option(ctx, "quiet", "1");
        } else if (strcmp(arg, "-m") == 0 || strcmp(arg, "--march") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            options->march = argv[++i];
            asmopt_set_option(ctx, "march", options->march);
            asmopt_set_option(ctx, "architecture", options->march);
        } else if (strcmp(arg, "--mtune") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            options->mtune = argv[++i];
            asmopt_set_target_cpu(ctx, options->mtune);
            asmopt_set_option(ctx, "mtune", options->mtune);
        } else if (strcmp(arg, "--amd-optimize") == 0) {
            options->amd_optimize = true;
            asmopt_set_amd_optimizations(ctx, 1);
            asmopt_set_bool_option(ctx, "amd_optimize", true);
        } else if (strcmp(arg, "--no-amd-optimize") == 0) {
            options->amd_optimize = false;
            asmopt_set_amd_optimizations(ctx, 0);
            asmopt_set_bool_option(ctx, "amd_optimize", false);
        } else if (arg[0] == '-') {
            asmopt_set_option(ctx, arg, "");
        } else if (!options->input_path) {
            options->input_path = arg;
        } else {
            asmopt_set_option(ctx, "extra", arg);
        }
    }
    return true;
}

static char* asmopt_read_stdin(void) {
    size_t cap = 4096;
    size_t len = 0;
    char* buffer = malloc(cap);
    if (!buffer) {
        return NULL;
    }
    int ch = 0;
    while ((ch = getchar()) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char* next = realloc(buffer, cap);
            if (!next) {
                free(buffer);
                return NULL;
            }
            buffer = next;
        }
        buffer[len++] = (char)ch;
    }
    buffer[len] = '\0';
    return buffer;
}

static bool asmopt_write_file(const char* path, const char* data) {
    if (!path || strcmp(path, "-") == 0) {
        fputs(data, stdout);
        return true;
    }
    FILE* handle = fopen(path, "w");
    if (!handle) {
        return false;
    }
    fputs(data, handle);
    fclose(handle);
    return true;
}

static bool asmopt_write_report(const char* path, const char* data) {
    if (!path || strcmp(path, "-") == 0) {
        fputs(data, stderr);
        return true;
    }
    FILE* handle = fopen(path, "w");
    if (!handle) {
        return false;
    }
    fputs(data, handle);
    fclose(handle);
    return true;
}

int main(int argc, char** argv) {
    asmopt_cli_options options = {0};
    asmopt_context* ctx = asmopt_create("x86-64");
    if (!ctx) {
        fprintf(stderr, "Failed to initialize asmopt\n");
        return 1;
    }
    if (!asmopt_parse_args(argc, argv, &options, ctx)) {
        asmopt_print_usage(argv[0]);
        asmopt_destroy(ctx);
        return 1;
    }
    if (!options.input_path && !options.quiet) {
        asmopt_set_option(ctx, "stdin", "1");
    }
    if (!options.input_path && asmopt_isatty(asmopt_fileno(stdin))) {
        asmopt_print_usage(argv[0]);
        asmopt_destroy(ctx);
        return 1;
    }

    int parse_result = 0;
    if (!options.input_path || strcmp(options.input_path, "-") == 0) {
        char* input = asmopt_read_stdin();
        if (!input) {
            fprintf(stderr, "Failed to read stdin\n");
            asmopt_destroy(ctx);
            return 1;
        }
        parse_result = asmopt_parse_string(ctx, input);
        free(input);
    } else {
        parse_result = asmopt_parse_file(ctx, options.input_path);
    }
    if (parse_result != 0) {
        fprintf(stderr, "Failed to read input\n");
        asmopt_destroy(ctx);
        return 1;
    }

    if (asmopt_optimize(ctx) != 0) {
        fprintf(stderr, "Optimization failed\n");
        asmopt_destroy(ctx);
        return 1;
    }

    if (options.dump_ir) {
        char* ir = asmopt_dump_ir_text(ctx);
        if (ir) {
            fputs(ir, stderr);
            free(ir);
        }
    }
    if (options.dump_cfg) {
        char* cfg = asmopt_dump_cfg_text(ctx);
        if (cfg) {
            fputs(cfg, stderr);
            free(cfg);
        }
    }
    if (options.cfg_path) {
        char* dot = asmopt_dump_cfg_dot(ctx);
        if (!dot || !asmopt_write_file(options.cfg_path, dot)) {
            fprintf(stderr, "Failed to write CFG\n");
            free(dot);
            asmopt_destroy(ctx);
            return 1;
        }
        free(dot);
    }
    if (options.report_path) {
        char* report = asmopt_generate_report(ctx);
        if (!report || !asmopt_write_report(options.report_path, report)) {
            fprintf(stderr, "Failed to write report\n");
            free(report);
            asmopt_destroy(ctx);
            return 1;
        }
        free(report);
    }
    if (options.stats) {
        size_t original = 0;
        size_t optimized = 0;
        size_t replacements = 0;
        size_t removals = 0;
        asmopt_get_stats(ctx, &original, &optimized, &replacements, &removals);
        fprintf(stderr,
                "Statistics:\n"
                "  original_lines: %zu\n"
                "  optimized_lines: %zu\n"
                "  replacements: %zu\n"
                "  removals: %zu\n",
                original, optimized, replacements, removals);
    }

    char* output = asmopt_generate_assembly(ctx);
    if (!output) {
        fprintf(stderr, "Failed to generate output\n");
        asmopt_destroy(ctx);
        return 1;
    }
    if (!asmopt_write_file(options.output_path, output)) {
        fprintf(stderr, "Failed to write output\n");
        free(output);
        asmopt_destroy(ctx);
        return 1;
    }
    free(output);
    asmopt_destroy(ctx);
    return 0;
}
