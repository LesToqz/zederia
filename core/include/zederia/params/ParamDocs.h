// The registry as documents: docs/PARAMETERS.md and the machine-readable schema that tools and
// agents read. Both are generated, never written by hand.
#pragma once

#include <string>

namespace zed {

std::string registryMarkdown();
std::string registryJson();

} // namespace zed
