# Packaging nvz for a Linux distribution

## What nvz is, packaging-wise

nvz is a **userspace** archiver — three C/C++ binaries and their man
pages. The road into a distribution is an ordinary package (deb/rpm),
not the kernel tree: nothing here runs in kernel space, and a
compression tool with half-gigabyte model tables has no business there.
What the kernel *spirit* contributes is the coding and comment style
(Documentation/process/coding-style.rst), which `src/` follows, and the
license.

## License

GPL-2.0-only (see `LICENSE`), the kernel's license, declared per file
with SPDX identifiers on line 1. Dependencies: libzstd (BSD) and
liblzma (0BSD/public domain) — both GPL-compatible, both in every
distribution's base set.

## Build and install

```sh
make                    # honors externally set CFLAGS/CXXFLAGS
make install DESTDIR=%{buildroot} PREFIX=/usr
make test               # self-test against the bundled code corpus
```

* No network access at build or test time.
* No bundled copies of libraries: zstd and lzma are linked from the
  system (`-lzstd -llzma`), the deflate side is nvz's own code.
* Reproducible: the archive format stores mtimes from the input only;
  the binaries embed no timestamps.
* `make dist` produces `dist/nvz-<version>.tar.gz` from `git archive` —
  the tarball a package should be built from.

## What a package should ship

| path | contents |
|:--|:--|
| `%{_bindir}/nvz` `nvcm` `nvjpg` | the three tools |
| `%{_mandir}/man1/*.1` | man pages |
| `%{_defaultdocdir}` | README.md |

`src/nvgpu` (CUDA) is deliberately **not** packaged: it requires a
non-free toolchain and loses to the CPU codecs on ratio (report §6).
`portable/nvzpy.py` targets macOS/Windows and is not for the Linux
package either.

A starting-point RPM spec is in `packaging/nvz.spec`; the Debian
equivalent is a stock `debhelper` rules file over the same
`make install` interface.

## Format stability contract

Archives written by any released nvz are readable by every later
release (v1 Python-JSON, v2 and v3 binary registers all still unpack).
The format strings inside archives are Swedish and frozen — see the
note at the top of `src/cpp/nvz.hpp`. A distribution upgrade never
orphans a user's archives.
