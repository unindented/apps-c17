#ifndef FRAM_TEST_ALBUM_SCAN_H
#define FRAM_TEST_ALBUM_SCAN_H

#include "core/str.h"
#include "domain/site_config.h"

// This is header-only and `static inline` so that the in-memory and the video tests of the probe
// phase, two executables, share one configuration without a separate test library to link.

/**
 * @brief Initializes the minimal fixed configuration the scan tests share.
 *
 * Every string is a borrowed literal, so the caller releases nothing beyond what `site_config_init`
 * itself requires.
 *
 * @param site_config Configuration to initialize. Must not be `NULL`.
 */
static inline void test_album_scan_init_config(struct SiteConfig* site_config)
    __attribute__((nonnull(1)));

static inline void test_album_scan_init_config(struct SiteConfig* site_config) {
  site_config_init(site_config);
  site_config->title = S("Gallery");
  site_config->description = S("Description");
  site_config->author = S("Author");
  site_config->media_dir = S("/photos");
  site_config->output_dir = S("/public");
}

#endif
