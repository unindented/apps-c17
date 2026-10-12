#ifndef APPS_OS_OS_POSIX_H
#define APPS_OS_OS_POSIX_H

struct Os;

// `os_posix.c` is the POSIX implementation of every operation `os/os.h` declares. This header adds
// only the way to get one.

/**
 * @brief Returns the POSIX host.
 *
 * It carries no state, so every call returns the same host and nothing has to release it.
 *
 * @return The host, never `NULL`.
 */
struct Os* os_posix(void) __attribute__((returns_nonnull));

#endif
