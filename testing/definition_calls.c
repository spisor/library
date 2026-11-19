#include "definition_calls.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define PARAMETER_LIMIT 4
#define PARAMETER_BYTES 2048
#define NO_PARAMETER ((size_t)-1)

typedef union parameter_buffer {
    long double long_double_alignment;
    void *pointer_alignment;
    uint64_t integer_alignment;
    unsigned char bytes[PARAMETER_BYTES];
} parameter_buffer;

/** What one call changes in its empty parameters: one `size` short, or one answer's count nonzero. */
typedef struct call_change {
    size_t short_parameter;
    size_t counted_parameter;
    size_t count_offset;
} call_change;

#ifdef _WIN32
typedef HMODULE library_handle;

static library_handle load_library(const char *path) {
    return LoadLibraryA(path);
}

static const void *definition_of(library_handle library, const char *symbol) {
    FARPROC getter = GetProcAddress(library, symbol);
    return getter == NULL ? NULL : ((const void *(*)(void))(void (*)(void))getter)();
}

static void unload_library(library_handle library) {
    FreeLibrary(library);
}
#else
typedef void *library_handle;

static library_handle load_library(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static const void *definition_of(library_handle library, const char *symbol) {
    void *getter = dlsym(library, symbol);
    const void *(*call)(void) = NULL;
    if (getter == NULL) {
        return NULL;
    }
    memcpy(&call, &getter, sizeof call);
    return call();
}

static void unload_library(library_handle library) {
    dlclose(library);
}
#endif

static int fits(const spisor_definition_operation *operation) {
    if (operation->parameter_count > PARAMETER_LIMIT) {
        return 0;
    }
    for (size_t index = 0; index < operation->parameter_count; index++) {
        if (operation->parameters[index].size > PARAMETER_BYTES) {
            return 0;
        }
    }
    return 1;
}

static void empty_parameters(const spisor_definition_operation *operation, const call_change *change,
                             parameter_buffer *buffers) {
    const size_t three = 3;
    for (size_t index = 0; index < operation->parameter_count; index++) {
        const spisor_definition_parameter *parameter = &operation->parameters[index];
        uint32_t size = (uint32_t)(index == change->short_parameter ? parameter->shape - 1 : parameter->size);
        memset(buffers[index].bytes, 0, sizeof buffers[index].bytes);
        memcpy(buffers[index].bytes, &size, sizeof size);
        if (index == change->counted_parameter) {
            memcpy(buffers[index].bytes + change->count_offset, &three, sizeof three);
        }
    }
}

static int answers_unchanged(const spisor_definition_operation *operation, const parameter_buffer *before,
                             const parameter_buffer *after) {
    for (size_t index = 0; index < operation->parameter_count; index++) {
        const spisor_definition_parameter *parameter = &operation->parameters[index];
        if (parameter->is_answer && memcmp(before[index].bytes, after[index].bytes, parameter->size) != 0) {
            return 0;
        }
    }
    return 1;
}

/** Calls `operation` with `change` made to its empty parameters and requires MALFORMED naming `refused`, untouched. */
static int refuses_untouched(const spisor_definition_operation *operation, const void *definition,
                             const call_change *change, const char *refused, const char *label) {
    parameter_buffer buffers[PARAMETER_LIMIT];
    parameter_buffer before[PARAMETER_LIMIT];
    void *pointers[PARAMETER_LIMIT] = {NULL, NULL, NULL, NULL};
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    int passed;
    empty_parameters(operation, change, buffers);
    for (size_t index = 0; index < operation->parameter_count; index++) {
        pointers[index] = buffers[index].bytes;
    }
    memcpy(before, buffers, sizeof buffers);
    status = operation->call(definition, pointers, &refusal);
    passed = status == SPISOR_MALFORMED && refusal.detail != NULL && strstr(refusal.detail, refused) != NULL &&
             answers_unchanged(operation, before, buffers);
    if (!passed) {
        fprintf(stderr, "%s %s %s: expected MALFORMED naming \"%s\" with every answer untouched; got %d, \"%s\"\n",
                operation->symbol, operation->name, label, refused, (int)status,
                refusal.detail == NULL ? "(none)" : refusal.detail);
    }
    operation->release_refusal(definition, &refusal);
    return passed ? 0 : 1;
}

static int short_shapes_of(const spisor_definition_operation *operation, const void *definition) {
    int failures = 0;
    for (size_t index = 0; index < operation->parameter_count; index++) {
        call_change change = {index, NO_PARAMETER, 0};
        failures += refuses_untouched(operation, definition, &change, "short of its first shape",
                                      operation->parameters[index].type);
    }
    return failures;
}

static int counted_answers_of(const spisor_definition_operation *operation, const void *definition) {
    int failures = 0;
    for (size_t index = 0; index < operation->parameter_count; index++) {
        const spisor_definition_parameter *parameter = &operation->parameters[index];
        for (size_t count = 0; parameter->is_answer && count < parameter->count_field_count; count++) {
            call_change change = {NO_PARAMETER, index, parameter->count_offsets[count]};
            failures += refuses_untouched(operation, definition, &change, "not empty", parameter->count_names[count]);
        }
    }
    return failures;
}

typedef int (*operation_check)(const spisor_definition_operation *operation, const void *definition);

static int check_every_operation(const char *library_path, operation_check check) {
    int failures = 0;
    library_handle library = load_library(library_path);
    if (library == NULL) {
        fprintf(stderr, "could not load %s\n", library_path);
        return 1;
    }
    for (size_t index = 0; index < spisor_definition_operation_count; index++) {
        const spisor_definition_operation *operation = &spisor_definition_operations[index];
        const void *definition = definition_of(library, operation->symbol);
        if (definition == NULL || !fits(operation)) {
            fprintf(stderr, "%s %s: not exported, or a parameter larger than this check holds\n", operation->symbol,
                    operation->name);
            failures++;
            continue;
        }
        failures += check(operation, definition);
    }
    unload_library(library);
    return failures;
}

int spisor_check_short_shapes(const char *library_path) {
    return check_every_operation(library_path, short_shapes_of);
}

int spisor_check_counted_answers(const char *library_path) {
    size_t count_fields = 0;
    for (size_t index = 0; index < spisor_definition_operation_count; index++) {
        const spisor_definition_operation *operation = &spisor_definition_operations[index];
        for (size_t parameter = 0; parameter < operation->parameter_count; parameter++) {
            count_fields += operation->parameters[parameter].count_field_count;
        }
    }
    if (count_fields == 0) {
        fprintf(stderr, "no answer counts an array, so nothing was checked\n");
        return 1;
    }
    return check_every_operation(library_path, counted_answers_of);
}
