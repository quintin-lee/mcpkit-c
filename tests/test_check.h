/* Shared NDEBUG-independent test check for all mcpkit-c tests.
 *
 * Plain assert() is compiled out under -DNDEBUG (the default Release ctest
 * configuration), which silently drops the call AND its side effects. CHECK
 * always evaluates its argument and aborts with a diagnostic on failure.
 */
#ifndef MCPKIT_TEST_CHECK_H
#define MCPKIT_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) {                                                                 \
            fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #x, __FILE__, __LINE__);  \
            abort();                                                                \
        }                                                                           \
    } while (0)

#endif
