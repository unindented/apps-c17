#include "app/cmd_config.h"

#include "core/arena.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/site_config.h"

int cmd_config_print(struct Str config_path,
                     struct Os* os,
                     struct Arena* perm,
                     struct Arena scratch,
                     struct Log* log) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  if (site_config_load(&site_config, config_path, os, perm, scratch, log) != 0 || log->errors > 0) {
    return -1;
  }
  return site_config_print(&site_config, os, scratch);
}
