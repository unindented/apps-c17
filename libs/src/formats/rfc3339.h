#ifndef APPS_FORMATS_RFC3339_H
#define APPS_FORMATS_RFC3339_H

#include "core/str.h"

/**
 * The Unix epoch as an RFC 3339 date-time in UTC. A format that requires a date, such as Atom's
 * `<updated>`, gets it for a value that has none. Every date an app writes spells a zero offset
 * `Z`, as this does.
 */
#define RFC3339_EPOCH S("1970-01-01T00:00:00Z")

#endif
