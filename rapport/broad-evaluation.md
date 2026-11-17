# nvz — broad evaluation

**Empirical evaluation of a content-aware archiver: compression ratio,
wall-clock cost, and correctness across eight data types and two standard
corpora.**

*2026-11-16 · Ryzen AI 9 HX 470 (24 threads), 28 GiB RAM, NVMe ·
source code, measurement data, and this report: <https://github.com/hugowikstrom/nvz> ·
every figure is generated from `resultat/*.json` and can be reproduced per appendix A ·
[svensk version](bred-provning.md)*

## Abstract

nvz is an archiver that classifies its input and applies content-specific,
lossless transforms — deduplication, similarity ordering, PNG inflation with
recipe recovery, JPEG re-encoding to arithmetic code, and context-mixing
compression (nvcm) — before selecting a codec per block by sampling. This
report evaluates nvz against seven established compressors on eleven corpora
and tests two hypotheses:

- **H1 (compression):** content awareness yields measurably smaller archives
  than generic stream compression on data with exploitable structure,
  without degrading the result on data that lacks it.
- **H2 (correctness):** every archive reproduces the source tree bit-exactly
  — contents, permissions, timestamps, symbolic links — and a damaged
  archive is rejected rather than yielding incorrect data.

The results support both hypotheses, with one quantified deviation from H1.
In summary:

| observation | measured | reference |
|:--|--:|:--|
| log files, ratio | **23.8×** (10.03 of 238.3 MiB) | xz −9e: 20.1× |
| log files, vs. best classical | **−15.3 %** | vs. xz −9e |
| Silesia, nvcm (single thread) | **1.677 bits/byte** | xz −9e: 1.829 |
| enwik8, nvcm (single thread) | **1.713 bits/byte** | xz −9e: 1.986 |
| video, all compressors | within 1 % of raw size | expected: already coded |
| archives-of-archives, nvz vs. zstd −22 | **−0.1 %** after remedy (+20.0 % before) | diagnosed and fixed, section 4.2 |
| restoration, 7 corpora | bit-exact, all entries | section 5 |
| damage tests, 5 damage classes | all rejected | section 5.2 |

The initially measured deviation from H1 (+20.0 % on the corpus of
already-compressed archives) was diagnosed as the codec probe's sample
selection, remedied during the evaluation, and re-measured: with the rule
of section 4.2, nvz is 0.1 % *smaller* than the best classical tool there
as well. H1 thus stands without a known counterexample in this material.

## 1. Introduction

A conventional archiving pipeline (`tar | zstd`) treats the tree as an
unordered byte stream: the compressor sees neither file duplicates outside
its window, nor embedded compression layers (deflate in PNG, Huffman in
JPEG), nor the opportunity to place similar files adjacently. nvz exploits
all three, and every transform is verified at pack time by full
re-synthesis and comparison against the original; a transform that cannot
be proven reversible falls back to raw storage.

Previous measurements covered three homogeneous corpora (source code, PNG,
JPEG photographs) and one mixed project tree. They do not answer how the
archiver behaves on data without exploitable structure (video, GIF,
already-compressed archives), on highly redundant log data, or in
comparison with published results. The present evaluation extends the
evidence with four corpora of real data, two standard corpora (Silesia,
enwik8), and a systematic correctness assessment.

## 2. Method

### 2.1 Reference compressors

Seven configurations of four established tools: gzip (−6, −9 via pigz),
bzip2 (−9), zstd (−3, −19 `--long=27`, −22 `--ultra --long=31`), and
xz (−9e). `tar | zstd −3` is labelled *current* and serves as the baseline,
as it matches the machine's existing archival script. nvz is measured in
modes `snabb` ("fast": zstd −3, no transforms), `normal` (transforms, codec
probe per block), and `liten` ("small": additionally nvcm as a candidate;
the default), plus `utan knep` ("no tricks": mode normal with
deduplication and transforms disabled) to isolate the transforms'
contribution.

### 2.2 Measurement procedure

Each data point is a complete cycle: pack, unpack into an empty directory,
recursive comparison against the source tree
(`diff -rq --no-dereference`). The *exact* column in the tables is the
outcome of that comparison, not an assumption. Times are wall-clock;
packing may use all 24 threads. Measurements ran under `nice 19` and
`ionice -c 3` on an otherwise loaded machine with idle cores; control runs
showed no measurable effect of the priority. The codec-level measurement
(section 3.4) instead follows the field's convention: one file at a time,
one thread, no tar, no deduplication.

Archive size is not deterministic across runs: two identical
`--läge normal` runs produced 8 597 884 and 8 597 603 bytes
(a 3·10⁻⁵ difference). Differences within a few hundred bytes are
therefore not significant.

### 2.3 Corpora

The six below complement the earlier three (code 55.3 MiB/5 974 files,
PNG 431.1/4 618, photo 408.5/1 143). Local corpora are built as hard links
by `korpus/bygg-korpus.py`; the log corpus is instead copied once, since
log files are written during measurement and a mutating corpus invalidates
the cycle comparison. Files modified within the last 15 minutes are
excluded.

| corpus | size | contents | rationale |
|:--|--:|:--|:--|
| `gif` | 105 files, 27.4 MiB | web graphics, animations, duplicates across several `node_modules` | LZW entropy layer that nvz does not open; high duplicate share |
| `film` | 67 files, 536.7 MiB | h264 (TikTok), VP9 (screen recording) | transform-coded data; expected null result |
| `arkiv` | 133 files, 391.8 MiB | zip and tar.gz, incl. two bit-identical pairs and eight daily database backups | compressed data with redundancy *between* files |
| `logg` | 76 files, 238.3 MiB | syslog, kern.log, app logs, JSONL transcripts | maximally repetitive text; a growing archive class |
| `silesia` | 12 files, 211 938 580 B | the Silesia corpus | comparability: standard in lzbench et al. |
| `enwik8` | 1 file, 10⁸ B | English Wikipedia, first 10⁸ bytes | comparability: Large Text Compression Benchmark |

The duplicate share per corpus is quantified below, since it determines how
much of a ratio is explained by deduplication rather than compression.

<!-- DUBBLETTER -->

| corpus | files | unique files | raw MiB | exact duplicates MiB | share |
|:--|--:|--:|--:|--:|--:|
| kod | 5,974 | 5,898 | 55.3 | 0.4 | 0.7 % |
| png | 4,618 | 1,963 | 431.1 | 123.8 | 28.7 % |
| foto | 1,143 | 1,142 | 408.5 | 0.0 | 0.0 % |
| gif | 105 | 59 | 27.4 | 13.0 | 47.3 % |
| film | 67 | 67 | 536.7 | 0.0 | 0.0 % |
| arkiv | 133 | 130 | 391.8 | 62.8 | 16.0 % |
| logg | 76 | 74 | 238.3 | 0.0 | 0.0 % |
| silesia | 12 | 12 | 202.1 | 0.0 | 0.0 % |
| enwik8 | 1 | 1 | 95.4 | 0.0 | 0.0 % |

<!-- SLUT DUBBLETTER -->

## 3. Results

### 3.1 Overview

Best nvz mode against the baseline and against the best classical tool per
corpus. Negative values denote smaller archives.

<!-- ÖVERSIKT -->

| corpus | raw MiB | current MiB | best classical MiB | best nvz MiB | vs. current | vs. best klassiska | pack time nvz/classical |
|:--|--:|--:|--:|--:|--:|--:|--:|
| Source code | 55 | 11.5 | 8.2 (xz -9e) | 7.3 (liten) | −36.5 % | −11.6 % | 70 / 47 s |
| Images (PNG) | 431 | 362.9 | 245.2 (zst -22) | 208.8 (liten) | −42.5 % | −14.8 % | 265 / 156 s |
| Photos (JPEG) | 409 | 406.3 | 404.9 (bz2 -9) | 345.1 (normal) | −15.1 % | −14.8 % | 49 / 57 s |
| Images (GIF) | 27 | 27.0 | 14.2 (zst -22) | 14.2 (normal) | −47.6 % | −0.0 % | 8 / 5 s |
| Video (h264 and VP9) | 537 | 533.9 | 532.0 (zst -19) | 532.0 (normal) | −0.3 % | +0.0 % | 61 / 57 s |
| Archives (zip and tar.gz) | 392 | 381.8 | 242.8 (zst -22) | 242.5 (liten) | −36.5 % | −0.1 % | 192 / 154 s |
| Loggfiles | 238 | 22.4 | 11.8 (xz -9e) | 10.0 (liten) | −55.2 % | −15.3 % | 197 / 897 s |
| Silesia (archive level) | 202 | 63.1 | 46.2 (xz -9e) | 43.5 (liten) | −31.0 % | −5.7 % | 102 / 158 s |
| enwik8 (archive level) | 95 | 33.8 | 23.7 (xz -9e) | 20.4 (liten) | −39.6 % | −13.7 % | 131 / 121 s |

<!-- SLUT ÖVERSIKT -->

### 3.2 Full matrix

All compressors across all corpora, in three panels: size, pack time,
unpack time. The silesia and enwik8 columns refer to the archive level
(whole corpus through a tar-equivalent path), not the codec level of 3.4.

<!-- MATRIS -->

**Archive size**

| compressor | kod | png | foto | gif | film | arkiv | logg | silesia | enwik8 |
|:--|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| tar (okomprimerad) | 60.8 | 438.4 | 409.4 | 27.6 | 536.8 | 391.9 | 238.4 | 202.1 | 95.4 |
| tar.gz -6 | 12.3 | 401.7 | 405.8 | 27.2 | 532.4 | 380.0 | 30.5 | 65.1 | 34.8 |
| tar.gz -9 (pigz) | 12.2 | 401.9 | 406.0 | 27.2 | 532.5 | 380.1 | 29.0 | 64.5 | 34.8 |
| tar.bz2 -9 | 9.5 | 385.4 | 404.9 | 27.3 | 535.5 | 384.5 | 18.9 | 52.0 | 27.7 |
| tar.zst -3 (dagens) | 11.5 | 362.9 | 406.3 | 27.0 | 533.9 | 381.8 | 22.4 | 63.1 | 33.8 |
| tar.zst -19 --long | 8.5 | 252.7 | 405.1 | 14.2 | **532.0** | 261.4 | 13.1 | 50.2 | 25.2 |
| tar.zst -22 --ultra | 8.5 | 245.2 | 405.1 | 14.2 | 532.0 | 242.8 | 12.9 | 49.8 | 24.1 |
| tar.xz -9e | 8.2 | 266.4 | 405.4 | 14.2 | 532.9 | 251.7 | 11.8 | 46.2 | 23.7 |
| **nvz snabb** | 11.4 | 279.3 | 408.5 | 14.4 | 536.8 | 329.0 | 22.4 | 63.2 | 33.8 |
| **nvz utan knep** | 8.3 | 256.5 | 406.4 | 14.2 | 532.0 | 242.5 | 12.0 | 46.2 | 23.7 |
| **nvz normal** | 8.3 | 208.8 | **345.1** | **14.2** | 532.0 | 242.5 | 12.0 | 46.2 | 23.7 |
| **nvz liten** | **7.3** | **208.8** | **345.1** | **14.2** | 532.0 | **242.5** | **10.0** | **43.5** | **20.4** |

*MiB per corpus; column minimum in bold. Raw sizes: kod 55, png 431, foto 409, gif 27, film 537, arkiv 392, logg 238, silesia 202, enwik8 95 MiB.*

**Pack time**

| compressor | kod | png | foto | gif | film | arkiv | logg | silesia | enwik8 |
|:--|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| tar (okomprimerad) | **0** | 3 | 2 | 0 | 1 | 1 | 0 | **0** | **0** |
| tar.gz -6 | 2 | 20 | 19 | 1 | 24 | 18 | 4 | 12 | 7 |
| tar.gz -9 (pigz) | 0 | 2 | 1 | 0 | 2 | 1 | 1 | 2 | 1 |
| tar.bz2 -9 | 7 | 74 | 57 | 4 | 78 | 56 | 32 | 24 | 11 |
| tar.zst -3 (dagens) | 0 | **1** | **0** | **0** | **0** | **0** | **0** | 0 | 0 |
| tar.zst -19 --long | 35 | 40 | 53 | 4 | 57 | 47 | 113 | 63 | 61 |
| tar.zst -22 --ultra | 56 | 156 | 225 | 5 | 266 | 154 | 986 | 179 | 118 |
| tar.xz -9e | 47 | 128 | 157 | 8 | 154 | 144 | 897 | 158 | 121 |
| **nvz snabb** | 1 | 2 | 1 | 0 | 3 | 2 | 2 | 1 | 2 |
| **nvz utan knep** | 41 | 42 | 12 | 11 | 59 | 191 | 854 | 94 | 125 |
| **nvz normal** | 41 | 268 | 49 | 8 | 61 | 203 | 864 | 94 | 125 |
| **nvz liten** | 70 | 265 | 55 | 9 | 60 | 192 | 197 | 102 | 131 |

*s per corpus; column minimum in bold. Raw sizes: kod 55, png 431, foto 409, gif 27, film 537, arkiv 392, logg 238, silesia 202, enwik8 95 MiB.*

**Uppack time**

| compressor | kod | png | foto | gif | film | arkiv | logg | silesia | enwik8 |
|:--|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| tar (okomprimerad) | **0** | **0** | **0** | **0** | **0** | **0** | **0** | **0** | **0** |
| tar.gz -6 | 0 | 3 | 2 | 0 | 2 | 2 | 1 | 1 | 1 |
| tar.gz -9 (pigz) | 0 | 3 | 2 | 0 | 2 | 2 | 1 | 1 | 1 |
| tar.bz2 -9 | 2 | 44 | 41 | 3 | 54 | 39 | 6 | 10 | 6 |
| tar.zst -3 (dagens) | 0 | 0 | **0** | **0** | 0 | 1 | 0 | 0 | 0 |
| tar.zst -19 --long | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| tar.zst -22 --ultra | 0 | 1 | 0 | 0 | 1 | 1 | 0 | 0 | 0 |
| tar.xz -9e | 1 | 5 | 4 | 0 | 5 | 7 | 1 | 3 | 1 |
| **nvz snabb** | 0 | 1 | 1 | 0 | 2 | 1 | 1 | 1 | 1 |
| **nvz utan knep** | 1 | 1 | 1 | 0 | 2 | 1 | 2 | 2 | 2 |
| **nvz normal** | 1 | 10 | 17 | 0 | 2 | 1 | 2 | 2 | 2 |
| **nvz liten** | 68 | 9 | 17 | 0 | 2 | 1 | 203 | 101 | 135 |

*s per corpus; column minimum in bold. Raw sizes: kod 55, png 431, foto 409, gif 27, film 537, arkiv 392, logg 238, silesia 202, enwik8 95 MiB.*

<!-- SLUT MATRIS -->

### 3.3 Per corpus

Complete series per corpus. *Mot dagens* ("vs. current") is the size change
relative to `tar.zst −3`; *exakt* is the bit-exactness outcome.

<!-- TABELLER -->

### Images (GIF) — 27.4 MiB i 105 filer

| compressor | size MiB | kvot | vs. current | pack time | uppack time | exact |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 27.62 | 0.99× | +2.1 % | 0 s | 0 s | yes |
| tar.gz -6 | 27.18 | 1.01× | +0.5 % | 1 s | 0 s | yes |
| tar.gz -9 (pigz) | 27.19 | 1.01× | +0.6 % | 0 s | 0 s | yes |
| tar.bz2 -9 | 27.28 | 1.00× | +0.9 % | 4 s | 3 s | yes |
| tar.zst -3 (dagens) | 27.04 | 1.01× | +0.0 % | **0 s** | 0 s | yes |
| tar.zst -19 --long | 14.18 | 1.93× | −47.6 % | 4 s | 0 s | yes |
| tar.zst -22 --ultra | 14.18 | 1.93× | −47.6 % | 5 s | 0 s | yes |
| tar.xz -9e | 14.19 | 1.93× | −47.5 % | 8 s | 0 s | yes |
| **nvz snabb** | 14.44 | 1.90× | −46.6 % | 0 s | 0 s | yes |
| **nvz utan knep** | 14.18 | 1.93× | −47.6 % | 11 s | 0 s | yes |
| **nvz normal** | **14.18** | 1.93× | −47.6 % | 8 s | 0 s | yes |
| **nvz liten** | **14.18** | 1.93× | −47.6 % | 9 s | 0 s | yes |

### Video (h264 and VP9) — 536.7 MiB i 67 filer

| compressor | size MiB | kvot | vs. current | pack time | uppack time | exact |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 536.82 | 1.00× | +0.6 % | 1 s | 0 s | yes |
| tar.gz -6 | 532.39 | 1.01× | −0.3 % | 24 s | 2 s | yes |
| tar.gz -9 (pigz) | 532.54 | 1.01× | −0.2 % | 2 s | 2 s | yes |
| tar.bz2 -9 | 535.53 | 1.00× | +0.3 % | 78 s | 54 s | yes |
| tar.zst -3 (dagens) | 533.86 | 1.01× | +0.0 % | **0 s** | 0 s | yes |
| tar.zst -19 --long | **532.02** | 1.01× | −0.3 % | 57 s | 0 s | yes |
| tar.zst -22 --ultra | 532.03 | 1.01× | −0.3 % | 266 s | 1 s | yes |
| tar.xz -9e | 532.86 | 1.01× | −0.2 % | 154 s | 5 s | yes |
| **nvz snabb** | 536.75 | 1.00× | +0.5 % | 3 s | 2 s | yes |
| **nvz utan knep** | 532.04 | 1.01× | −0.3 % | 59 s | 2 s | yes |
| **nvz normal** | 532.04 | 1.01× | −0.3 % | 61 s | 2 s | yes |
| **nvz liten** | 532.04 | 1.01× | −0.3 % | 60 s | 2 s | yes |

### Archives (zip and tar.gz) — 391.8 MiB i 133 filer

| compressor | size MiB | kvot | vs. current | pack time | uppack time | exact |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 391.93 | 1.00× | +2.7 % | 1 s | 0 s | yes |
| tar.gz -6 | 380.05 | 1.03× | −0.5 % | 18 s | 2 s | yes |
| tar.gz -9 (pigz) | 380.14 | 1.03× | −0.4 % | 1 s | 2 s | yes |
| tar.bz2 -9 | 384.49 | 1.02× | +0.7 % | 56 s | 39 s | yes |
| tar.zst -3 (dagens) | 381.78 | 1.03× | +0.0 % | **0 s** | 1 s | yes |
| tar.zst -19 --long | 261.37 | 1.50× | −31.5 % | 47 s | 0 s | yes |
| tar.zst -22 --ultra | 242.77 | 1.61× | −36.4 % | 154 s | 1 s | yes |
| tar.xz -9e | 251.75 | 1.56× | −34.1 % | 144 s | 7 s | yes |
| **nvz snabb** | 329.03 | 1.19× | −13.8 % | 2 s | 1 s | yes |
| **nvz utan knep** | 242.53 | 1.62× | −36.5 % | 191 s | 1 s | yes |
| **nvz normal** | 242.53 | 1.62× | −36.5 % | 203 s | 1 s | yes |
| **nvz liten** | **242.53** | 1.62× | −36.5 % | 192 s | 1 s | yes |

### Log files — 238.3 MiB i 76 filer

| compressor | size MiB | kvot | vs. current | pack time | uppack time | exact |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 238.39 | 1.00× | +965.0 % | 0 s | 0 s | yes |
| tar.gz -6 | 30.46 | 7.82× | +36.1 % | 4 s | 1 s | yes |
| tar.gz -9 (pigz) | 29.03 | 8.21× | +29.7 % | 1 s | 1 s | yes |
| tar.bz2 -9 | 18.92 | 12.60× | −15.5 % | 32 s | 6 s | yes |
| tar.zst -3 (dagens) | 22.38 | 10.65× | +0.0 % | **0 s** | 0 s | yes |
| tar.zst -19 --long | 13.09 | 18.21× | −41.5 % | 113 s | 0 s | yes |
| tar.zst -22 --ultra | 12.92 | 18.45× | −42.3 % | 986 s | 0 s | yes |
| tar.xz -9e | 11.84 | 20.12× | −47.1 % | 897 s | 1 s | yes |
| **nvz snabb** | 22.41 | 10.63× | +0.1 % | 2 s | 1 s | yes |
| **nvz utan knep** | 11.97 | 19.91× | −46.5 % | 854 s | 2 s | yes |
| **nvz normal** | 11.97 | 19.91× | −46.5 % | 864 s | 2 s | yes |
| **nvz liten** | **10.03** | 23.77× | −55.2 % | 197 s | 203 s | yes |

<!-- SLUT TABELLER -->

### 3.4 Standard corpora, codec level

This comparison isolates the nvcm compressor — one file at a time, one
thread — since published figures for these corpora are defined that way.
nvz as an archiver appears in 3.1–3.3.

<!-- STANDARD -->

**Silesia** — 211,938,580 byte

| codec | size bytes | kvot | bits/byte | pack MiB/s | unpack MiB/s | exact |
|:--|--:|--:|--:|--:|--:|:-:|
| gzip -9 | 67,631,990 | 3.134× | 2.553 | 6.8 | 157.9 | yes |
| bzip2 -9 | 54,506,769 | 3.888× | 2.058 | 8.1 | 19.2 | yes |
| zstd -19 | 52,895,350 | 4.007× | 1.997 | 1.6 | 374.3 | yes |
| zstd -22 --ultra | 52,369,178 | 4.047× | 1.977 | 1.2 | 360.9 | yes |
| brotli -q 11 | 49,564,563 | 4.276× | 1.871 | 0.3 | 175.8 | yes |
| xz -9e | 48,456,004 | 4.374× | 1.829 | 1.3 | 66.5 | yes |
| **nvcm** | 44,439,240 | 4.769× | 1.677 | 0.9 | 0.8 | yes |

**enwik8** — 100,000,000 byte

| codec | size bytes | kvot | bits/byte | pack MiB/s | unpack MiB/s | exact |
|:--|--:|--:|--:|--:|--:|:-:|
| gzip -9 | 36,445,248 | 2.744× | 2.916 | 10.2 | 138.2 | yes |
| bzip2 -9 | 29,008,758 | 3.447× | 2.321 | 8.7 | 16.5 | yes |
| zstd -19 | 26,944,227 | 3.711× | 2.155 | 1.1 | 340.6 | yes |
| brotli -q 11 | 25,742,001 | 3.885× | 2.059 | 0.3 | 207.3 | yes |
| zstd -22 --ultra | 25,272,471 | 3.957× | 2.022 | 0.8 | 381.5 | yes |
| xz -9e | 24,831,648 | 4.027× | 1.986 | 0.8 | 69.6 | yes |
| **nvcm** | 21,417,184 | 4.669× | 1.713 | 0.8 | 0.7 | yes |

<!-- SLUT STANDARD -->

Per file on Silesia, whose heterogeneity is the point of the corpus:

<!-- SILESIA-PER-FIL -->

| file | contents | raw bytes | zstd -19 | brotli -q 11 | xz -9e | nvcm | nvcm vs. best |
|:--|:--|--:|--:|--:|--:|--:|--:|
| `dickens` | English literature, text | 10,193,352 | 2,849,945 | 2,827,777 | 2,831,212 | 2,234,894 | −20.2 % |
| `mozilla` | executables (tar) | 51,215,653 | 15,071,672 | 13,872,265 | 13,376,240 | 14,366,242 | +7.4 % |
| `mr` | medical 3D image | 9,970,424 | 3,106,737 | 2,823,136 | 2,751,892 | 2,205,358 | −9.7 % |
| `nci` | chemical structures, text | 33,553,317 | 1,664,988 | 1,519,768 | 1,449,272 | 1,505,038 | +3.8 % |
| `ooffice` | executable DLL | 6,151,755 | 2,595,377 | 2,478,855 | 2,427,224 | 2,379,789 | −2.0 % |
| `osdb` | database dump, binary | 10,084,646 | 3,100,177 | 2,816,278 | 2,844,556 | 2,395,972 | −14.5 % |
| `reymont` | Polish literature, PDF source | 6,627,493 | 1,348,462 | 1,332,158 | 1,315,592 | 1,053,488 | −15.5 % |
| `samba` | source code (tar) | 21,606,321 | 3,894,675 | 3,766,340 | 3,739,524 | 3,377,043 | −9.7 % |
| `sao` | star catalogue, binary | 7,250,892 | 5,000,576 | 4,586,092 | 4,425,664 | 4,509,261 | +1.9 % |
| `webster` | dictionary, text | 41,458,740 | 8,679,705 | 8,428,575 | 8,368,672 | 6,317,041 | −24.5 % |
| `x-ray` | X-ray, 16-bit | 8,475,800 | 5,129,827 | 4,682,753 | 4,491,264 | 3,742,075 | −7.6 % |
| `xml` | XML | 5,345,363 | 453,209 | 430,566 | 434,892 | 353,039 | −18.0 % |

<!-- SLUT SILESIA-PER-FIL -->

As a reference against published results (LTCB; other machines and program
versions; not our measurements):

| program | enwik8 (bytes) | bits/byte | method |
|:--|--:|--:|:--|
| gzip 1.3.5 | 36,445,248 | 2.92 | dictionary |
| bzip2 1.0.2 | 29,008,736 | 2.32 | block sorting |
| zstd 0.6.0 | 25,405,601 | 2.03 | dictionary |
| xz 5.2.1 | 24,703,772 | 1.98 | dictionary (LZMA) |
| nanozip 0.09a | 18,594,163 | 1.49 | context mixing |
| zpaq 6.42 | 17,855,729 | 1.43 | context mixing |
| paq8px_v206fix1 | 15,849,084 | 1.27 | context mixing |
| cmix v21 | 14,623,723 | 1.17 | context mixing |

<!-- ENWIK8-PLATS -->

Our own measurement: **nvcm compresses enwik8 to 21,417,184 bytes, 1.713 bits/byte** — 13.8 % smaller than `xz -9e` on the same machine (24,831,648 bytes, 1.986 bpb).

Med 1.71 bits/byte hamnar nvcm mellan ordbokspackarna och de tunga kontextblandarna i tabellen ovan: den tar hem det en ordbokspackare inte kan se, men den har åtta modeller där cmix har hundratals, och den kör i ett par MiB/s där cmix kör i kilobyte per sekund.

<!-- SLUT ENWIK8-PLATS -->

### 3.5 Throughput

Seconds are not comparable across corpora of different size; the table
therefore reports MiB/s of raw data through the wall clock.

<!-- TIDER -->

| corpus | nvz normal pack | nvz normal unpack | zstd -3 pack | zstd -19 pack | xz -9e pack |
|:--|--:|--:|--:|--:|--:|
| Source code | 1 | 86 | 325 | 2 | 1 |
| Images (PNG) | 2 | 45 | 743 | 11 | 3 |
| Photos (JPEG) | 8 | 24 | 869 | 8 | 3 |
| Images (GIF) | 3 | 274 | 304 | 6 | 3 |
| Video (h264 and VP9) | 9 | 333 | 1,220 | 9 | 3 |
| Archives (zip and tar.gz) | 2 | 363 | 1,059 | 8 | 3 |
| Loggfiles | 0 | 128 | 1,254 | 2 | 0 |
| Silesia (archive level) | 2 | 83 | 612 | 3 | 1 |
| enwik8 (archive level) | 1 | 50 | 502 | 2 | 1 |

*MiB per second, raw data through the wall clock.*

<!-- SLUT TIDER -->

## 4. Analysis

### 4.1 Log data: compressor choice dominates

The log corpus exhibits the evaluation's highest ratio, 23.8×, and the
source can be isolated through the mode comparison: `normal` and `liten`
differ only in whether nvcm participates in the codec probe.

| mode | size | pack time | active difference |
|:--|--:|--:|:--|
| `normal` | 11.97 MiB | 864 s | codec probe zstd/xz |
| `liten` | **10.03 MiB** | **197 s** | nvcm added |

nvcm won the probe for every text block, which the archive register
confirms (2 blocks, both nvcm; 192.03 → 9.00 MiB and 46.29 → 1.03 MiB).
That the smaller archive also packs faster is a parallelism effect: nvz's
blocks compress independently, whereas `xz -9e -T0` on this input
effectively runs single-threaded due to its per-thread memory budget. The
blocks' differing ratios (21.4× vs. 45.1×) illustrate adaptation: a model
that has seen 192 MiB of syslog predicts the next block substantially
better.

### 4.2 Already-compressed archives: the limit of sampling

On the archive corpus, nvz is 20.0 % larger than
`zstd −22 --ultra --long=31` (291.3 vs. 242.8 MiB). Two explanatory
hypotheses were tested experimentally (`experiment/arkiv-fonster.sh`):
(a) block partitioning prevents cross-block matching; (b) the codec probe
selects the wrong codec.

| configuration | size |
|:--|--:|
| zstd −19, default window (8 MiB) | 365.3 MiB |
| zstd −19 `--long=27`, whole stream | 261.4 MiB |
| zstd −22 `--ultra --long=31`, whole stream | 242.8 MiB |
| nvz, probe selects | 291.3 MiB |
| nvz, `--kodek zstd19` | 260.2 MiB |
| nvz, `--kodek zstd22` | **244.6 MiB** |

Hypothesis (a) is falsified: with a manually chosen codec, partitioning
costs 1.2 MiB (0.7 %). Hypothesis (b) is confirmed: the probe compresses
4 MiB sampled across the block, while the corpus's redundancy lies in
nearly identical files hundreds of megabytes apart — a property a sparse
sample cannot observe. The probe therefore classifies the block as poorly
compressible and selects a cheap codec. Deduplication captures the exact
copies (62.8 MiB, 16.0 %); the remainder consists of *near*-copies
(distinct gzip streams of similar raw data), visible only to long-range
matching over the whole stream.

**Remedy and re-measurement.** The diagnosis was implemented as two
rules: (i) opaque blocks are not split at 192 MiB — on opaque data, block
partitioning is a compression bound rather than a memory optimization,
and zstd parallelizes within the block; (ii) for opaque blocks the
sample classifies as compressible, the sampled choice is replaced by a
two-stage rule, since a sample cannot distinguish video from
archives-of-archives — both are locally dense, and the difference only
shows over the whole block. The block is first compressed with zstd −19
(2²⁷ window); a gain above 10 % indicates inter-file redundancy, and the
block is recompressed with zstd −22 (2³¹ window), which collects the
remainder. Video stops at around one percent and never pays for the
expensive path. Re-measured:

| configuration | size |
|:--|--:|
| zstd −22 `--ultra --long=31`, whole stream | 242.8 MiB |
| nvz after remedy (1 block, escalated to zstd22) | **242.5 MiB** |

The video corpus exercises the rule's other branch: escalation does not
trigger (zstd −19 gains 0.9 %), and pack time stays at 60 s against the
255 s an unconditional zstd −22 would have cost.

nvz thus moves from +20.0 % to −0.1 % against the best classical tool on
this corpus; the margin is deduplication's 62.8 MiB, which the classical
pipeline must rediscover as matches rather than never storing.

### 4.3 Video: the null result and its price

All twelve configurations land within 1 % of raw size — expected, since
transform-coded video has no entropy layer accessible to generic
compression. The relevant comparison is thus the cost of establishing
this: nvz `normal` reaches the same size as `zstd −22` in 16 % of the time
(61 vs. 266 s), because the codec probe classifies the blocks as
raw-storage cases from samples instead of compressing them in full.

### 4.4 GIF: ratio without compression contribution

The GIF corpus's 1.93× ratio is fully explained by duplicates (47.3 % of
the corpus) and by the corpus (27.4 MiB) fitting inside every modern
compressor's window: nvz, zstd −19/−22 and xz reach the same size, and
`nvz utan knep` is identical to `nvz normal`. nvz's transforms contribute
nothing on LZW-coded content — the format is not opened. A GIF
counterpart to nvjpg (LZW → arithmetic coding of the frames) is thus an
identified but unmeasured opportunity; the corpus to measure it now
exists.

## 5. Correctness

### 5.1 Restoration

Recursive diff answers a weaker question than H2 requires: it compares
contents of files that exist on both sides, but is blind to permissions,
timestamps, empty directories, and entries silently dropped.
`prov/aterskapning.py` therefore computes a complete fingerprint —
SHA-256, mode, mtime in nanoseconds, link target, entry type — before
packing and after unpacking, requires identity, and verifies that the
archive register's entry count matches the tree's.

<!-- ÅTERSKAPNING -->

| corpus | files | register entries | SHA-256 equal | mode and times equal | hard links left | deviations |
|:--|--:|--:|:-:|:-:|:-:|:--|
| gif | 105 | yes | ja | yes | no | none |
| arkiv | 133 | yes | ja | yes | no | none |
| logg | 76 | yes | ja | yes | no | none |
| film | 67 | yes | ja | yes | no | none |
| png | 4,618 | yes | ja | yes | no | none |
| foto | 1,143 | yes | ja | yes | no | none |
| kod | 5,974 | yes | ja | yes | no | none |

Damage tests on the archive from corpus **gif**:

| damage | outcome | passed |
|:--|:--|:-:|
| magin bytt | rejected | yes |
| byte i blockkropp | rejected | yes |
| sista kilobyten bort | rejected | yes |
| registerpekaren noll | rejected | yes |
| en byte i registret | rejected | yes |

<!-- SLUT ÅTERSKAPNING -->

In addition, `prov/prova-allt.sh` exercises edge cases — empty files,
duplicates, relative, broken, and directory-targeting symlinks, filenames
with newlines and non-ASCII, permissions 400–755, timestamps in 1985 and
2031 — in every mode, requiring full metadata identity. The suite
comprises 44 tests and passes without exception.

### 5.2 Damage tolerance

An archiver that returns incorrect data without a failure indication is
worse than one that refuses: the former is discovered only at use. Five
damage classes were injected into a healthy archive; the requirement is
rejection or provably intact output, never silent corruption.

| damage class | outcome |
|:--|:--|
| replaced magic | rejected |
| bit flip in block body | rejected |
| truncation (final 1,024 bytes) | rejected |
| zeroed register pointer | rejected |
| bit flip in register | rejected |

### 5.3 Version compatibility

The C++ implementation (section 6) reads the Python implementation's
archives (JSON register, 8-byte hash prefix) bit-exactly, including PNG-
and JPEG-transformed objects; this is part of the test suite. The reverse
is unsupported: the v2 register is binary, and the Python implementation
is retired.

## 6. Implementation

During the evaluation, nvz was rewritten from Python into a standalone
C++17 binary (287 kB): nvcm and nvjpg are linked in and executed as worker
processes via `/proc/self/exe`, zstd/xz/zlib are called as in-process
libraries, and `liten` is the default mode. Compression results are
equivalent within sampling noise (the codecs are the same libraries); the
difference is the time profile wherever Python participated per file:

| data point | Python | C++ |
|:--|--:|--:|
| code, mode `snabb` (5,974 files) | 2.7 s | 0.8 s |
| gif, mode `snabb` | 0.6 s | 0.1 s |
| 516 JPEG transforms | ≈8 s | 2.8 s |
| photo, unpack (1,143 transformed) | 7.8 s | 17.4 s |

The remaining regression on photo unpacking stems from worker-process
startup cost per image and is the single point where the Python
implementation's process pool was faster.

The port exposed three defects of the kind that only manifest under
concurrency or format migration, documented here for their generality:

1. **Allocation between `fork` and `exec` in a threaded process.** The
   child inherits lock state from threads that do not survive the fork; an
   allocation in the child can then block permanently. Symptom: workers at
   0 % CPU for 84 minutes. Remedy: the argument vector is built before
   `fork`; between `fork` and `exec` only async-signal-safe calls occur.
2. **File-descriptor inheritance between parallel workers.** A child
   forked while another thread's pipes are open inherits them too; the
   peer's read end then never sees EOF. Remedy: `pipe2(O_CLOEXEC)`.
3. **Hash length in the v1 register.** The Python register stores the
   first 16 hex characters of the object hash — 8 bytes. A reader
   comparing 16 bytes rejects healthy archives.

## 7. Limitations and future work

Ordered by estimated return:

1. **Sample selection for non-opaque classes:** the remedy in 4.2 covers
   opaque blocks only, where a rule can replace the probe. For text and
   binary classes, whether a sample growing with block size improves
   codec choice remains unmeasured.
2. **GIF re-encoding** (section 4.4): the nvjpg construction applied to
   the LZW layer.
3. **nvcm on opaque blocks:** mode `liten` sends only text classes to
   nvcm; whether opaque data pays off is unmeasured.

Outside this evaluation: progressive JPEG (nvjpg covers baseline
sequential only), git packfiles (the PNG recipe approach, untried),
archives exceeding working memory, and the GPU mode — measured separately
with the conclusion that LZ77 matching accelerates 2.5× but serial entropy
coding bounds the whole (Amdahl; see README).

## Appendix A: Reproduction

Source code and all measurement data are published at
<https://github.com/hugowikstrom/nvz>. Dependencies: g++ (C++17),
libzstd-dev, liblzma-dev, zlib1g-dev; for the reference measurements also
gzip, pigz, bzip2, zstd, and xz.

```bash
git clone https://github.com/hugowikstrom/nvz && cd nvz
make                                                  # builds nvz, nvcm, nvjpg
python3 korpus/bygg-korpus.py gif film arkiv logg     # local corpora
export NVPACK_SKRAP=$PWD/.skrap
python3 matning/matning.py <corpus>                   # archive level, per corpus
python3 matning/standardkorpus.py silesia enwik8      # codec level, one thread
python3 prov/aterskapning.py gif arkiv logg film png foto kod
bash prov/prova-allt.sh                               # edge cases, 44 tests
bash experiment/arkiv-fonster.sh                      # hypothesis test of 4.2
python3 experiment/dubbletter.py
python3 rapport/bred-provning.py                      # fills the tables here
```
