# nvz är C++ sedan 2026-11-16: EN binär med nvcm och nvjpg inlänkade och
# libzstd/liblzma/libz i process. Pythonversionen ligger kvar som referens i
# src/nvz.py men byggs inte och anropas inte av någonting.
# -O3 för C-kodekarna: nvcm är den varmaste slingan i läget `liten`, och -O3
# mättes till 14 % snabbare med byteidentisk utdata (samma för nvjpg).
CFLAGS ?= -O3 -Wall -Wextra -Wno-implicit-fallthrough
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CPPKALLOR = src/cpp/huvud.cpp src/cpp/yta.cpp src/cpp/arkiv.cpp src/cpp/kodek.cpp \
            src/cpp/omvandla.cpp src/cpp/trad.cpp src/cpp/json.cpp \
            src/cpp/sjalv.cpp src/cpp/verktyg.cpp
CPPOBJ = $(CPPKALLOR:src/cpp/%.cpp=src/cpp/%.o)

# GPU-delen byggs bara om maskinen har förutsättningarna. Vulkanhuvudena och
# glslc finns inte överallt, och resten av nvz ska gå att bygga utan dem.
HAR_GPU := $(shell [ -f /usr/include/vulkan/vulkan.h ] && command -v glslc >/dev/null 2>&1 && echo ja)
SPV = src/band.spv src/blake2b.spv src/matcher.spv

all: src/nvz src/nvjpg src/nvcm $(if $(HAR_GPU),gpu,utan-gpu)

# --- nvz: C++-binären -------------------------------------------------------
# Beroendena genereras av kompilatorn (-MMD -MP) och läses in nedan. Den
# gamla regeln lydde `%.o: %.cpp src/cpp/nvz.hpp` och tog alltså bara hänsyn
# till ETT huvud. Ett fält tillagt i `struct Val` i arkiv.hpp byggde då om
# huvud.o och arkiv.o men inte yta.o, och binären länkades ihop av
# objektfiler som var oense om strukturens layout: `nvz -t` kraschade med
# SIGSEGV medan `nvz packa` fungerade. Felet hittades av prov/slumpprov.py
# 2026-11-18 och var inte synligt i någon kompilatorvarning — därför står
# förklaringen här och inte i en commit-rad.
src/cpp/%.o: src/cpp/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(CPPOBJ:.o=.d)

# nvcm.c och nvjpg.c kompileras som C och länkas in via wrappers som byter
# bort deras main. De är samma filer som bygger de fristående binärerna.
src/cpp/cwrap_nvcm.o: src/cpp/cwrap_nvcm.c src/nvcm.c
	$(CC) $(CFLAGS) -c $< -o $@
src/cpp/cwrap_nvjpg.o: src/cpp/cwrap_nvjpg.c src/nvjpg.c
	$(CC) $(CFLAGS) -c $< -o $@

src/nvz: $(CPPOBJ) src/cpp/cwrap_nvcm.o src/cpp/cwrap_nvjpg.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lzstd -llzma -lz -lpthread

src/nvjpg: src/nvjpg.c
	$(CC) $(CFLAGS) -o $@ $<

gpu: src/nvgpu $(SPV)

utan-gpu:
	@echo "hoppar över gpu-delen: vulkan-huvuden eller glslc saknas (apt install libvulkan-dev glslc)"

src/nvgpu: src/nvgpu.c
	$(CC) $(CFLAGS) -o $@ $< -lvulkan -lpthread

# Shadrarna kompileras till spir-v och läses av nvgpu i körtid, ur samma
# katalog som binären.
src/%.spv: src/%.comp
	glslc -O -fshader-stage=compute $< -o $@

# blake2b-kärnan är genererad — utrullad, för att slippa scratch-minne.
src/blake2b.comp: src/gen-blake2b.py
	python3 $<

prov: src/nvjpg src/nvcm
	./src/nvz prova korpus/kod
	./prov/kontroll-jpg.sh korpus/foto
	@if [ -x src/nvgpu ]; then ./src/nvgpu prov --tyst && echo "nvgpu: proven gick igenom"; \
	 else echo "nvgpu inte byggd (make gpu) — gpu-proven hoppas över"; fi

# Installationen lägger de tre verktygen och deras man-sidor under PREFIX.
# nvz är ett skalskript som startar src/nvz.py, och python-delen måste
# följa med hel: png.py och jpeg.py läses in vid start.
PREFIX ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin
LIBDIR  = $(DESTDIR)$(PREFIX)/lib/nvz
MANDIR  = $(DESTDIR)$(PREFIX)/share/man/man1

installera: all
	install -d $(BINDIR) $(MANDIR)
	install -m 755 src/nvz src/nvcm src/nvjpg $(BINDIR)
	install -m 644 man/nvcm.1 man/nvz.1 man/nvjpg.1 $(MANDIR)
	@echo "installerat i $(PREFIX). Prova: man nvz"

avinstallera:
	rm -f $(BINDIR)/nvcm $(BINDIR)/nvjpg $(BINDIR)/nvz
	rm -rf $(LIBDIR)
	rm -f $(MANDIR)/nvcm.1 $(MANDIR)/nvz.1 $(MANDIR)/nvjpg.1

# Man-sidorna läses direkt ur trädet utan att installeras:
#   man -l man/nvcm.1
man: man/nvcm.1 man/nvz.1 man/nvjpg.1
	@for m in $^; do man --warnings -E UTF-8 -l $$m >/dev/null || exit 1; done
	@echo "man-sidorna är rena mot groffs varningar"

# EN FIL: det är nvz numera av sig själv — en statiskt länkbar C++-binär
# med nvcm och nvjpg inbyggda. zipapp-målet som buntade pythonversionen
# behövs inte längre och är borttaget.
pdf: rapport/nvz-rapport.pdf

rapport/nvz-rapport.pdf: rapport/bygg-tryck.py $(wildcard resultat/*.json)
	bash rapport/bygg-pdf.sh

rent:
	rm -f src/nvjpg src/nvcm src/nvgpu src/nvz src/cpp/*.o $(SPV)
	rm -rf dist .bygg-enfil

.PHONY: all prov pdf rent gpu utan-gpu installera avinstallera man

src/nvcm: src/nvcm.c
	$(CC) $(CFLAGS) -o $@ $<
