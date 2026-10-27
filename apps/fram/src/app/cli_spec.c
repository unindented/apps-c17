#include "app/cli_spec.h"

#include "app/cmd_build.h"
#include "app/cmd_config.h"
#include "cli/cli.h"
#include "core/str.h"
#include "domain/fram_version.h"
#include "domain/site_config.h"

void cli_spec_init(struct CliSpec* spec) {
  *spec = (struct CliSpec){
      .description = S("A static photo and video gallery generator."),
      .noun = S("gallery"),
      .config_path_default = SITE_CONFIG_PATH_DEFAULT,
      .generator = fram_generator_string(),
      .build = cmd_build_run,
      .config_print = cmd_config_print,
  };
}
