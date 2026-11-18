#include "Schema.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace zmcp {
namespace {

std::string typeOf(const J& v) {
    if (v.is_null()) return "null";
    if (v.is_boolean()) return "boolean";
    if (v.is_number_integer() || v.is_number_unsigned()) return "integer";
    if (v.is_number_float()) return std::floor(v.get<double>()) == v.get<double>() && std::isfinite(v.get<double>()) ? "integer" : "number";
    if (v.is_string()) return "string";
    if (v.is_array()) return "array";
    return "object";
}

bool isType(const J& v, const std::string& t) {
    const std::string actual = typeOf(v);
    if (t == "number")
        return actual == "number" || actual == "integer";
    return actual == t;
}

std::string escapeToken(const std::string& key) {
    std::string out;
    for (const char c : key) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out.push_back(c);
    }
    return out;
}

std::string describeValue(const J& v) {
    std::string s = v.dump();
    if (s.size() > 60)
        s = s.substr(0, 57) + "...";
    return typeOf(v) + " " + s;
}

std::string listOf(const J& values, std::size_t max = 12) {
    std::string out;
    std::size_t i = 0;
    for (const auto& v : values) {
        if (i == max) {
            out += ", ...";
            break;
        }
        out += (i++ ? ", " : "") + v.dump();
    }
    return out;
}

std::string hint(const J& schema) {
    if (schema.is_object()) {
        if (auto d = schema.find("description"); d != schema.end() && d->is_string())
            return " (" + d->get<std::string>() + ")";
    }
    return {};
}

struct Validator {
    const J& root;
    std::vector<Problem>& out;
    int limit;
    int depth = 0;

    void add(std::string path, std::string problem, std::string fix) {
        if (static_cast<int>(out.size()) < limit)
            out.push_back({std::move(path), std::move(problem), std::move(fix)});
    }
    bool full() const { return static_cast<int>(out.size()) >= limit; }

    const J* resolve(const J& schema) const {
        if (!schema.is_object())
            return &schema;
        auto ref = schema.find("$ref");
        if (ref == schema.end() || !ref->is_string())
            return &schema;
        const std::string r = ref->get<std::string>();
        if (r.rfind("#", 0) != 0)
            return nullptr;
        try {
            return &root.at(J::json_pointer(r.substr(1)));
        } catch (...) {
            return nullptr;
        }
    }

    // True when `value` matches `schema` without recording problems.
    bool matches(const J& schema, const J& value) {
        std::vector<Problem> scratch;
        Validator inner{root, scratch, 1, depth + 1};
        inner.check(schema, value, "");
        return scratch.empty();
    }

    void check(const J& schemaIn, const J& value, const std::string& path) {
        if (full() || depth > 64)
            return;
        const J* sp = resolve(schemaIn);
        if (sp == nullptr) {
            add(path, "the schema refers to something that does not exist", "report this to the server's author");
            return;
        }
        const J& s = *sp;
        if (s.is_boolean()) {
            if (!s.get<bool>())
                add(path, "no value is allowed here", "remove it");
            return;
        }
        if (!s.is_object())
            return;
        ++depth;
        if (auto t = s.find("type"); t != s.end()) {
            bool ok = false;
            std::string expected;
            if (t->is_string()) {
                ok = isType(value, t->get<std::string>());
                expected = t->get<std::string>();
            } else if (t->is_array()) {
                for (const auto& each : *t) {
                    ok = ok || (each.is_string() && isType(value, each.get<std::string>()));
                    expected += (expected.empty() ? "" : " or ") + each.get<std::string>();
                }
            }
            if (!ok) {
                add(path, "expected " + expected + ", got " + describeValue(value), "give " + (expected == "integer" ? std::string("a whole number") : "a value of type " + expected) + hint(s));
                --depth;
                return;
            }
        }
        if (auto c = s.find("const"); c != s.end() && value != *c)
            add(path, "must be " + c->dump() + ", got " + describeValue(value), "use " + c->dump());
        if (auto e = s.find("enum"); e != s.end() && e->is_array()) {
            if (std::find(e->begin(), e->end(), value) == e->end()) {
                std::string fix = "use one of: " + listOf(*e);
                if (value.is_string()) {
                    std::vector<std::string> names;
                    for (const auto& v : *e)
                        if (v.is_string())
                            names.push_back(v.get<std::string>());
                    if (auto near = closest(value.get<std::string>(), names); !near.empty())
                        fix = "did you mean \"" + near + "\"? " + fix;
                }
                add(path, describeValue(value) + " is not allowed here", fix);
            }
        }
        if (value.is_number()) {
            const double v = value.get<double>();
            if (auto b = s.find("minimum"); b != s.end() && b->is_number() && v < b->get<double>())
                add(path, value.dump() + " is below the minimum " + b->dump(), "use a value of at least " + b->dump() + hint(s));
            if (auto b = s.find("maximum"); b != s.end() && b->is_number() && v > b->get<double>())
                add(path, value.dump() + " is above the maximum " + b->dump(), "use a value of at most " + b->dump() + hint(s));
            if (auto b = s.find("exclusiveMinimum"); b != s.end() && b->is_number() && !(v > b->get<double>()))
                add(path, value.dump() + " must be above " + b->dump(), "use a value above " + b->dump() + hint(s));
            if (auto b = s.find("exclusiveMaximum"); b != s.end() && b->is_number() && !(v < b->get<double>()))
                add(path, value.dump() + " must be below " + b->dump(), "use a value below " + b->dump() + hint(s));
            if (!std::isfinite(v))
                add(path, "the number is not finite", "use a finite number");
        }
        if (value.is_string()) {
            const auto len = static_cast<double>(value.get<std::string>().size());
            if (auto b = s.find("minLength"); b != s.end() && b->is_number() && len < b->get<double>())
                add(path, "the text is too short", "use at least " + b->dump() + " characters" + hint(s));
            if (auto b = s.find("maxLength"); b != s.end() && b->is_number() && len > b->get<double>())
                add(path, "the text is too long (" + std::to_string(static_cast<long long>(len)) + " characters)", "use at most " + b->dump() + " characters");
        }
        if (value.is_array()) {
            const auto n = static_cast<double>(value.size());
            if (auto b = s.find("minItems"); b != s.end() && b->is_number() && n < b->get<double>())
                add(path, "the list has " + std::to_string(value.size()) + " entries", "give at least " + b->dump() + hint(s));
            if (auto b = s.find("maxItems"); b != s.end() && b->is_number() && n > b->get<double>())
                add(path, "the list has " + std::to_string(value.size()) + " entries", "give at most " + b->dump());
            std::size_t start = 0;
            if (auto p = s.find("prefixItems"); p != s.end() && p->is_array()) {
                for (std::size_t i = 0; i < p->size() && i < value.size(); ++i)
                    check((*p)[i], value[i], path + "/" + std::to_string(i));
                start = p->size();
            }
            if (auto items = s.find("items"); items != s.end())
                for (std::size_t i = start; i < value.size() && !full(); ++i)
                    check(*items, value[i], path + "/" + std::to_string(i));
        }
        if (value.is_object()) {
            const J* props = nullptr;
            if (auto p = s.find("properties"); p != s.end() && p->is_object())
                props = &*p;
            if (auto r = s.find("required"); r != s.end() && r->is_array())
                for (const auto& key : *r)
                    if (key.is_string() && !value.contains(key.get<std::string>())) {
                        const J* ps = props && props->contains(key.get<std::string>()) ? &(*props)[key.get<std::string>()] : nullptr;
                        add(path + "/" + escapeToken(key.get<std::string>()), "the field \"" + key.get<std::string>() + "\" is missing",
                            "add \"" + key.get<std::string>() + "\"" + (ps ? hint(*ps) : std::string()));
                    }
            const auto additional = s.find("additionalProperties");
            std::vector<std::string> known;
            if (props)
                for (auto it = props->begin(); it != props->end(); ++it)
                    known.push_back(it.key());
            for (auto it = value.begin(); it != value.end() && !full(); ++it) {
                const std::string childPath = path + "/" + escapeToken(it.key());
                if (props && props->contains(it.key())) {
                    check((*props)[it.key()], it.value(), childPath);
                } else if (additional != s.end()) {
                    if (additional->is_boolean() && !additional->get<bool>()) {
                        std::string fix = "remove it";
                        if (auto near = closest(it.key(), known); !near.empty())
                            fix = "did you mean \"" + near + "\"? Otherwise remove it";
                        if (!known.empty() && known.size() <= 20) {
                            std::string all;
                            for (const auto& k : known)
                                all += (all.empty() ? "" : ", ") + k;
                            fix += " (fields here: " + all + ")";
                        }
                        add(childPath, "unknown field \"" + it.key() + "\"", fix);
                    } else if (additional->is_object()) {
                        check(*additional, it.value(), childPath);
                    }
                }
            }
        }
        if (auto all = s.find("allOf"); all != s.end() && all->is_array())
            for (const auto& sub : *all)
                check(sub, value, path);
        if (auto any = s.find("anyOf"); any != s.end() && any->is_array()) {
            bool ok = false;
            for (const auto& sub : *any)
                ok = ok || matches(sub, value);
            if (!ok)
                explainAlternatives(*any, value, path, "anyOf");
        }
        if (auto one = s.find("oneOf"); one != s.end() && one->is_array()) {
            int count = 0;
            for (const auto& sub : *one)
                count += matches(sub, value) ? 1 : 0;
            if (count != 1) {
                if (count == 0)
                    explainAlternatives(*one, value, path, "oneOf");
                else
                    add(path, "the value fits more than one of the allowed forms", "make it fit exactly one" + hint(s));
            }
        }
        --depth;
    }

    // No alternative matched: report the problems of the alternative that came closest.
    void explainAlternatives(const J& alternatives, const J& value, const std::string& path, const char*) {
        std::vector<Problem> best;
        bool first = true;
        for (const auto& sub : alternatives) {
            std::vector<Problem> scratch;
            Validator inner{root, scratch, limit, depth + 1};
            inner.check(sub, value, path);
            // Prefer an alternative whose type fits (its problems are deeper than the value itself).
            const auto score = [&](const std::vector<Problem>& ps) {
                int s = static_cast<int>(ps.size()) * 10;
                for (const auto& p : ps)
                    if (p.path == path && p.problem.rfind("expected ", 0) == 0)
                        s += 1000;
                return s;
            };
            if (first || score(scratch) < score(best)) {
                best = scratch;
                first = false;
            }
        }
        std::string forms;
        for (const auto& sub : alternatives) {
            const J* r = resolve(sub);
            if (r && r->is_object() && r->contains("description"))
                forms += (forms.empty() ? "" : "; or ") + (*r)["description"].get<std::string>();
        }
        if (best.empty())
            add(path, "the value fits none of the allowed forms", forms.empty() ? "check the tool's input schema" : "give " + forms);
        for (auto& p : best)
            if (!full())
                out.push_back(p);
    }
};

} // namespace

std::vector<Problem> validate(const J& schema, const J& value, int limit) {
    std::vector<Problem> out;
    Validator v{schema, out, limit};
    v.check(schema, value, "");
    return out;
}

bool knownKeyword(const std::string& k) {
    static const std::set<std::string> known = {
        "type", "properties", "required", "additionalProperties", "items", "prefixItems", "minItems", "maxItems", "enum",
        "const", "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "minLength", "maxLength", "oneOf", "anyOf",
        "allOf", "$ref", "$defs", "$schema",
        // annotations
        "title", "description", "default", "examples", "deprecated", "readOnly", "writeOnly", "$comment"};
    return known.count(k) > 0;
}

namespace {
void collectUnknown(const J& s, std::vector<std::string>& out, const std::string& path) {
    if (!s.is_object())
        return;
    for (auto it = s.begin(); it != s.end(); ++it) {
        if (!knownKeyword(it.key()))
            out.push_back(path + "/" + it.key());
        if (it.key() == "properties" || it.key() == "$defs") {
            if (it->is_object())
                for (auto p = it->begin(); p != it->end(); ++p)
                    collectUnknown(p.value(), out, path + "/" + it.key() + "/" + p.key());
        } else if (it.key() == "items" || it.key() == "additionalProperties") {
            collectUnknown(it.value(), out, path + "/" + it.key());
        } else if ((it.key() == "oneOf" || it.key() == "anyOf" || it.key() == "allOf" || it.key() == "prefixItems") && it->is_array()) {
            for (std::size_t i = 0; i < it->size(); ++i)
                collectUnknown((*it)[i], out, path + "/" + it.key() + "/" + std::to_string(i));
        }
    }
}
} // namespace

std::vector<std::string> unknownKeywords(const J& schema) {
    std::vector<std::string> out;
    collectUnknown(schema, out, "");
    return out;
}

J problemsToJson(const std::vector<Problem>& problems) {
    J list = J::array();
    for (const auto& p : problems)
        list.push_back({{"path", p.path}, {"problem", p.problem}, {"fix", p.fix}});
    return list;
}

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
        prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const int sub = prev[j - 1] + (std::tolower(static_cast<unsigned char>(a[i - 1])) == std::tolower(static_cast<unsigned char>(b[j - 1])) ? 0 : 1);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string closest(const std::string& word, const std::vector<std::string>& candidates) {
    std::string best;
    int bestD = 1 << 30;
    for (const auto& c : candidates) {
        const int d = editDistance(word, c);
        if (d < bestD) {
            bestD = d;
            best = c;
        }
    }
    const int allowed = std::max(2, static_cast<int>(word.size()) / 3);
    return bestD <= allowed && bestD > 0 ? best : std::string();
}

} // namespace zmcp
