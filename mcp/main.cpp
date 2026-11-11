// zederia-mcp: the agent bridge. Milestone 1 only establishes the target; the protocol layer,
// tools, resources and the live link arrive in milestone 6.
#include "zederia/Version.h"
#include "zederia/params/Registry.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") != 0) {
        std::fprintf(stderr, "zederia-mcp %s: this build only answers --version; the agent bridge is not built yet.\n",
                     zed::kVersion);
        return 2;
    }
    std::printf("zederia-mcp %s (%d parameters)\n", zed::kVersion, zed::Registry::get().size());
    return 0;
}
