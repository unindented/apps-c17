# CMake reads these options automatically with `--graphviz`.

set(GRAPHVIZ_GRAPH_NAME "surge target dependencies")
set(GRAPHVIZ_GRAPH_HEADER
    "graph [ rankdir = \"LR\", bgcolor = \"transparent\" ];\nnode [ fontsize = \"11\" ];\nedge [ fontsize = \"9\" ];"
)

set(GRAPHVIZ_EXECUTABLES TRUE)
set(GRAPHVIZ_STATIC_LIBS TRUE)
set(GRAPHVIZ_SHARED_LIBS FALSE)
set(GRAPHVIZ_MODULE_LIBS FALSE)
set(GRAPHVIZ_INTERFACE_LIBS TRUE)
set(GRAPHVIZ_OBJECT_LIBS TRUE)
set(GRAPHVIZ_UNKNOWN_LIBS FALSE)
set(GRAPHVIZ_EXTERNAL_LIBS TRUE)
set(GRAPHVIZ_CUSTOM_TARGETS FALSE)

# A link flag, such as the `-L<sdk>/usr/lib -liconv` that libgit2 links, names a path on the build
# machine, so the graph leaves it out.
set(GRAPHVIZ_IGNORE_TARGETS
    "apps_build_tests"
    "surge_unit_.*"
    "os_memory"
    "testing"
    "vendor_acutest"
    "^-L.*"
)

set(GRAPHVIZ_GENERATE_PER_TARGET FALSE)
set(GRAPHVIZ_GENERATE_DEPENDERS FALSE)
