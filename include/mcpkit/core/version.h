/**
 * @file version.h
 * Exposes the mcpkit version string.
 *
 * The value is read from the `VERSION` file at CMake configure time
 * and baked into `mcpkit_version_generated.h` in the build tree, so
 * all translation units agree without hardcoding.
 *
 * @ingroup mcpkit-core
 */

#ifndef MCPKIT_CORE_VERSION_H
#define MCPKIT_CORE_VERSION_H


/**
 * @brief Returns the mcpkit version string (e.g. `"0.1.0"`).
 *
 * The pointer is static and valid for the lifetime of the process; the
 * caller must not free it.
 *
 * @return Borrowed string, valid for the lifetime of the process.
 */
const char *mcpkit_version_string(void);

#endif
