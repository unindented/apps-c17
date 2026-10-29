#include "app/cli_spec.h"

#include "app/cmd_build.h"
#include "app/cmd_config.h"
#include "cli/cli.h"
#include "core/str.h"
#include "domain/site_config.h"
#include "domain/surge_version.h"
#include "markup/highlight.h"

void cli_spec_init(struct CliSpec* spec) {
  *spec = (struct CliSpec){
      .description = S("A static site generator for git repositories."),
      .noun = S("site"),
      .config_path_default = SITE_CONFIG_PATH_DEFAULT,
      .generator = surge_generator_string(),
      .grammars = highlight_grammar_list(),
      .build = cmd_build_run,
      .config_print = cmd_config_print,
  };
}
