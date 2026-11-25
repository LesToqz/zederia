// Limits on JSON from other programs (agent clients, the live-link pipe). A document nested
// thousands of levels deep is legal JSON and fits easily within the request size limit, but code
// that walks the parsed value recursively (copies, schema checks, output) could run out of stack
// and end the process. Such text is refused before it is parsed.
#pragma once

#include <algorithm>
#include <string_view>

namespace zed {

// Zederia's documents nest about eight levels deep; requests may nest far more than that.
inline constexpr int kMaxJsonDepth = 64;

// The deepest nesting of arrays and objects in JSON text, found without parsing it (the contents
// of strings are skipped). Unbalanced text gives the depth it reached.
inline int jsonNestingDepth(std::string_view text) noexcept {
    int depth = 0, deepest = 0;
    bool inString = false, escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            deepest = std::max(deepest, ++depth);
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return deepest;
}

inline bool jsonNestingAllowed(std::string_view text) noexcept { return jsonNestingDepth(text) <= kMaxJsonDepth; }

} // namespace zed
