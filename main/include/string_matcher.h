#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Launch line-by-line pattern matching task on Core 0
bool string_matcher_init(void);

// Stop string matcher
void string_matcher_stop(void);

#ifdef __cplusplus
}
#endif
