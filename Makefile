# nvz: python-delen behöver inget bygge, C-delen gör det.
CFLAGS ?= -O2 -Wall -Wextra -Wno-implicit-fallthrough

# GPU-delen byggs bara om maskinen har förutsättningarna. Vulkanhuvudena och
# glslc finns inte överallt, och resten av nvz ska gå att bygga utan dem.
HAR_GPU := $(shell [ -f /usr/include/vulkan/vulkan.h ] && command -v glslc >/dev/null 2>&1 && echo ja)
SPV = src/band.spv src/blake2b.spv src/matcher.spv

all: src/nvjpg src/nvcm $(if $(HAR_GPU),gpu,utan-gpu)

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
	install -d $(BINDIR) $(LIBDIR) $(MANDIR)
	install -m 755 src/nvcm src/nvjpg $(BINDIR)
	install -m 644 src/nvz.py src/png.py src/jpeg.py $(LIBDIR)
	printf '#!/bin/sh\nexec python3 %s/nvz.py "$$@"\n' "$(PREFIX)/lib/nvz" > $(BINDIR)/nvz
	chmod 755 $(BINDIR)/nvz
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

# EN FIL. gzip är ett program man kopierar; nvz är tre pythonmoduler och två
# hjälpbinärer. `make enfil` buntar pythondelen till dist/nvz — en körbar fil
# med shebang, byggd som zipapp. Hjälpbinärerna nvcm och nvjpg söks då i PATH
# (se hjälpbinär() i nvz.py). Saknas de packar nvz ändå, men utan sina egna
# omvandlingar — det är skillnaden mot gzip, och den står i README.
enfil: dist/nvz

dist/nvz: src/nvz.py src/png.py src/jpeg.py
	@rm -rf .bygg-enfil && mkdir -p .bygg-enfil dist
	# Koden får ett MODULNAMN och __main__ blir ett skal som importerar det.
	# Process-poolen plockar isär sina uppgifter med pickle, och pickle sparar
	# en funktion som "modul + namn". Ligger koden i __main__ kan barnprocessen
	# inte importera den igen ur zipen, och poolen dör med BrokenProcessPool.
	cp src/nvz.py .bygg-enfil/nvzkod.py
	cp src/png.py src/jpeg.py .bygg-enfil/
	printf 'import sys\nfrom nvzkod import main\nsys.exit(main())\n' > .bygg-enfil/__main__.py
	python3 -m zipapp .bygg-enfil -o $@ -p "/usr/bin/env python3"
	@chmod 755 $@ && rm -rf .bygg-enfil
	@echo "$@  $$(stat -c%s $@) byte  (kräver python3; nvcm och nvjpg söks i PATH)"

pdf: rapport/nvz-rapport.pdf

rapport/nvz-rapport.pdf: rapport/bygg-tryck.py $(wildcard resultat/*.json)
	bash rapport/bygg-pdf.sh

rent:
	rm -f src/nvjpg src/nvcm src/nvgpu $(SPV)
	rm -rf dist .bygg-enfil

.PHONY: all prov pdf rent gpu utan-gpu installera avinstallera man enfil

src/nvcm: src/nvcm.c
	$(CC) $(CFLAGS) -o $@ $<
