// A JSON Schema validator for the subset of JSON Schema 2020-12 the tool schemas use: type,
// properties, required, additionalProperties, items, prefixItems, minItems, maxItems, enum,
// const, minimum, maximum, exclusiveMinimum, exclusiveMaximum, minLength, maxLength, oneOf,
// anyOf, allOf and local $ref ("#/$defs/..."). Every problem carries a JSON pointer and a plain
// fix, so an agent can correct its call (plan 4.4: "validation errors that tell an agent exactly
// what to fix"). Keywords outside the subset are ignored; a test checks that no tool schema uses one.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace zmcp {

using J = nlohmann::json;

struct Problem {
    std::string path;    // JSON pointer into the value, "" for the value itself
    std::string problem; // what is wrong
    std::string fix;     // what to do
};

// Validates `value` against `schema` (whose $defs resolve local references). Stops after `limit` problems.
std::vector<Problem> validate(const J& schema, const J& value, int limit = 20);

// The keywords the validator understands (and the annotation keywords it accepts and ignores).
bool knownKeyword(const std::string& keyword);
// Keywords in `schema` (recursively) that are neither understood nor annotations.
std::vector<std::string> unknownKeywords(const J& schema);

J problemsToJson(const std::vector<Problem>& problems);

// Edit distance, for "did you mean" suggestions.
int editDistance(const std::string& a, const std::string& b);
// The closest of `candidates` to `word` when it is close enough, otherwise "".
std::string closest(const std::string& word, const std::vector<std::string>& candidates);

} // namespace zmcp
