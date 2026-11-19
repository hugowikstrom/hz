#pragma once
#include "nvz.hpp"
#include <memory>

namespace nvz {

// Exactly as much JSON as the v1 register contains: objects, lists, strings,
// integers, true/false/null. No float parsing, no generic serialization —
// v2 is written in binary and needs none.
//
// It exists only for backward compatibility. Archives written by the Python
// implementation must unpack with this binary; otherwise the move to C++
// would have been a silent data loss.
struct Jvalue;
using Jptr = std::shared_ptr<Jvalue>;

struct Jvalue {
    enum class K { Null, Bool, Num, Str, List, Obj } k = K::Null;
    bool b = false;
    i64 num = 0;
    std::string s;
    std::vector<Jptr> list;
    std::map<std::string, Jptr> obj;

    const Jptr &at(const std::string &key) const;
    bool has(const std::string &key) const { return obj.count(key) > 0; }
    i64 as_num() const { return k == K::Num ? num : 0; }
    const std::string &as_str() const { return s; }
};

Jptr parse_json(const std::string &text);

}  // namespace nvz
