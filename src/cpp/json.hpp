#pragma once
#include "nvz.hpp"
#include <memory>

namespace nvz {

// Precis så mycket JSON som v1-registret innehåller: objekt, listor,
// strängar, heltal, true/false/null. Ingen flyttalsparsning, ingen
// generisk serialisering — v2 skrivs binärt och behöver ingen.
//
// Den finns bara för bakåtkompatibiliteten. Arkiv som pythonversionen
// skrivit ska gå att packa upp med den här binären; annars vore namnbytet
// till C++ en tyst dataförlust.
struct Jvarde;
using Jptr = std::shared_ptr<Jvarde>;

struct Jvarde {
    enum class K { Null, Bool, Tal, Strang, Lista, Objekt } k = K::Null;
    bool b = false;
    i64 tal = 0;
    std::string s;
    std::vector<Jptr> lista;
    std::map<std::string, Jptr> objekt;

    const Jptr &at(const std::string &nyckel) const;
    bool har(const std::string &nyckel) const { return objekt.count(nyckel) > 0; }
    i64 som_tal() const { return k == K::Tal ? tal : 0; }
    const std::string &som_strang() const { return s; }
};

Jptr las_json(const std::string &text);

}  // namespace nvz
