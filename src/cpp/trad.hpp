#pragma once
#include "nvz.hpp"

namespace nvz {

// Klassen avgör tre saker: vilken omvandling som provas, vilket block posten
// hamnar i, och vilken kodek blocket får. Ändelsen är en gissning, magiska
// bytes är ett besked.
std::string klassificera(const std::string &sokvag, const u8 *huvud, size_t n);

// Går igenom trädet en gång och plockar fram poster i stabil ordning.
// Returnerar antalet överhoppade specialfiler i *overhoppat*.
std::vector<Post> las_trad(const std::string &rot, size_t *overhoppat);

// Återskapar en post på disk: katalog, länk eller fil med rätt rättigheter
// och tid. Filer skrivs av den som har innehållet; här sätts bara metadata.
void satt_metadata(const std::string &sokvag, u32 lage, i64 mtid_ns, bool ar_lank);

}  // namespace nvz
