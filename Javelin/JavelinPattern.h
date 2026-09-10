//============================================================================

#pragma once
#include <stdbool.h>
#include <stddef.h>

//============================================================================

#ifdef __cplusplus
extern "C" {
#endif

//============================================================================

typedef void* jp_pattern_t;
typedef void* jp_bytecode_t;

//============================================================================

// Options to pass into compile
#define JP_OPTION_IGNORE_CASE               1       // "i" option
#define JP_OPTION_MULTILINE                 2       // "m" option. This allows ^ and $ to match \n lines mid-string
#define JP_OPTION_DOTALL                    4       // "s" option. This allows . to match newlines
#define JP_OPTION_UNICODE_CASE              8       // "u" option. When IGNORE_CASE is used, this allows unicode case folding
#define JP_OPTION_UNGREEDY                  0x10    // "U" option
#define JP_OPTION_UCP                       0x40    // Unicode properties for \d, \w, \s, \b and their complements.
                                                    // Independent of UTF8 and UNICODE_CASE; without UTF8, classifies bytes as Latin-1.

#define JP_OPTION_UTF8                      0x100   // Decode UTF-8; otherwise match individual bytes.

#define JP_OPTION_GLOB_SYNTAX               0x400   // With this flag, ? is treated as [^/], * as [^/]* and ** as .*
                                                    // You will probably combine this with JP_OPTION_ANCHORED

#define JP_OPTION_ANCHORED                  0x800

#define JP_OPTION_NO_OPTIMIZE               0x1000

#define JP_OPTION_PREFER_NFA                0x2000
#define JP_OPTION_PREFER_BACK_TRACKING      0x4000
#define JP_OPTION_PREFER_SCAN_AND_CAPTURE   0x6000
#define JP_OPTION_PREFER_NO_SCAN            0x8000	// Use this when you're primarily using jp_full_match to extract sub-matches from strings that are expected to match

//============================================================================

// Result codes from jp_pattern_compile & jp_bytecode_compile
#define JP_RESULT_OK                                     0
#define JP_RESULT_INTERNAL_ERROR                         1

#define JP_RESULT_EXPECTED_CLOSE_GROUP                   2
#define JP_RESULT_INVALID_BACK_REFERENCE                 3
#define JP_RESULT_INVALID_OPTIONS                        4
#define JP_RESULT_LOOK_BEHIND_NOT_CONSTANT_BYTE_LENGTH   5
#define JP_RESULT_MALFORMED_CONDITIONAL                  6
#define JP_RESULT_MAXIMUM_REPETITION_COUNT_EXCEEDED      7
#define JP_RESULT_MINIMUM_COUNT_EXCEEDS_MAXIMUM_COUNT    8
#define JP_RESULT_TOO_MANY_BYTECODE_INSTRUCTIONS         9
#define JP_RESULT_TOO_MANY_CAPTURES                      10
#define JP_RESULT_TOO_MANY_PROGRESS_CHECK_INSTRUCTIONS   11
#define JP_RESULT_UNABLE_TO_PARSE_GROUP_TYPE             12
#define JP_RESULT_UNABLE_TO_PARSE_REPETITION             13
#define JP_RESULT_UNABLE_TO_RESOLVE_RECURSE_TARGET       14
#define JP_RESULT_UNEXPECTED_CONTROL_CHARACTER           15
#define JP_RESULT_UNEXPECTED_END_OF_PATTERN              16
#define JP_RESULT_UNEXPECTED_GROUP_OPTIONS               17
#define JP_RESULT_UNEXPECTED_HEX_CHARACTER               18
#define JP_RESULT_UNEXPECTED_LOOK_BEHIND_TYPE            19
#define JP_RESULT_UNEXPECTED_TOKEN                       20
#define JP_RESULT_UNKNOWN_ESCAPE                         21
#define JP_RESULT_UNKNOWN_POSIX_CHARACTER_CLASS          22
#define JP_RESULT_MALFORMED_UNICODE_PROPERTY              23
#define JP_RESULT_UNKNOWN_UNICODE_PROPERTY                24

//============================================================================

// Functions to create a precompiled pattern. precompiled patterns can be used with jp_create
int         jp_bytecode_compile(jp_bytecode_t* out_result, const char* pattern, int options);
const void* jp_bytecode_get_data(jp_bytecode_t bytecode);
size_t      jp_bytecode_get_length(jp_bytecode_t bytecode);
void        jp_bytecode_free(jp_bytecode_t bytecode);

// Functions to create a pattern object.
int  jp_pattern_compile(jp_pattern_t* out_result, const char* pattern, int options);
int  jp_pattern_create(jp_pattern_t* out_result, const void* byte_code, size_t byte_code_length, bool make_copy_of_byte_code);
void jp_pattern_free(jp_pattern_t pattern);

// Functions using pattern objects
int    jp_get_number_of_captures(jp_pattern_t pattern);
bool   jp_has_full_match(jp_pattern_t pattern, const void* data, size_t data_length);
bool   jp_has_partial_match(jp_pattern_t pattern, const void* data, size_t data_length, size_t data_offset);
bool   jp_full_match(jp_pattern_t pattern, const void* data, size_t data_length, const void** captures);
bool   jp_partial_match(jp_pattern_t pattern, const void* data, size_t data_length, const void** captures, size_t data_offset);
// Visit non-overlapping matches in leftmost-first order, starting at data_offset.
// from/to are byte offsets into the original data, with to exclusive, even in
// UTF-8 mode. After an empty match, nonempty alternatives at the same position
// are tried before searching at later character boundaries. An empty match at
// the end of the input completes the scan.
// on_match must be non-null. Return 0 to continue, or any other value to stop;
// jp_scan returns that exact value, or 0 when scanning completes. An offset
// beyond data_length completes without calling on_match. data may be null when
// data_length is zero. Keep the pattern and input alive and the input unchanged
// until scanning returns. Callbacks may start independent, nested scans.
int jp_scan(jp_pattern_t pattern, const void* data, size_t data_length, void* user,
            int (*on_match)(size_t from, size_t to, void* user), size_t data_offset);
// Like jp_scan, but report all capture groups for each match. capture_count
// includes group zero, and captures has 2 * capture_count pointers: begin/end
// pairs into the original input, with end exclusive. Unmatched groups have
// two null pointers; participating empty groups have equal, non-null pointers.
// The array is read-only and valid only during the callback. For null, empty
// input, empty captures point to a temporary non-null location valid during
// the callback. The scanner clears unmatched groups between matches.
int jp_scan_captures(jp_pattern_t pattern, const void* data, size_t data_length, void* user,
                     int (*on_match)(const void* const* captures, size_t capture_count, void* user), size_t data_offset);

// Functions to control behavior of matching.
void jp_dfa_memory_manager_mode_set_unlimited();
void jp_dfa_memory_manager_mode_set_global_limit(size_t number_of_bytes_limit);
void jp_dfa_memory_manager_mode_set_per_pattern_limit(size_t number_of_bytes_limit);

void jp_stack_growth_handler_disable();
void jp_stack_growth_handler_set(void* (*allocate_function)(size_t), void (*free_function)(void*));

//============================================================================

#ifdef __cplusplus
}
#endif

//============================================================================
