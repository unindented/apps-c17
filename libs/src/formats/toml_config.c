#include <limits.h>

#include "core/arena.h"
#include "core/str.h"
#include "formats/toml.h"
#include "os/os.h"

// This file holds only the read of a config, so a target that links `formats` and no host, such as
// the tests of the other modules, does not pull in `os_read`.

// `toml_parse_document` takes only a document whose length fits an `int`.
_Static_assert(TOML_CONFIG_LEN_MAX <= INT_MAX,
               "an accepted config must fit the int length tomlc17 takes");

int toml_load_config(struct Str config_path,
                     struct Os* os,
                     struct Arena scratch,
                     toml_result_t* parsed_out,
                     struct Log* log) {
  struct Str config_data = {0};
  if (os_read(os, config_path, TOML_CONFIG_LEN_MAX, OS_READ_TEXT, &scratch, &config_data,
              S("failed to read config"), log) != 0) {
    return -1;
  }
  // tomlc17 copies the input, so the bytes can stay in `scratch`.
  return toml_parse_document(config_data, config_path, parsed_out, S("failed to parse config"),
                             log);
}
