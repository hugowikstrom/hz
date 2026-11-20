# SPDX-License-Identifier: GPL-2.0-only
# Starting-point spec; adjust Release/changelog per distribution policy.
Name:           hz
Version:        2.0
Release:        1%{?dist}
Summary:        hyperZip - content-aware archiver that knows what it is packing
License:        GPL-2.0-only
URL:            https://nattviken.com/projekt/hz/
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc, gcc-c++, make, libzstd-devel, xz-devel
Requires:       libzstd, xz-libs

%description
hyperZip (command: hz) classifies its content and applies content-specific lossless
transforms - deduplication, similarity sorting, PNG unfolding with zlib
recipe search, lossless JPEG recoding and context-mixing compression -
before choosing a codec per block by probing a sample. Everything is
bit-exact; whatever cannot be reproduced exactly is stored raw.

%prep
%autosetup

%build
%make_build

%install
%make_install PREFIX=%{_prefix}

%check
make test

%files
%license LICENSE
%doc README.md
%{_bindir}/hz
%{_bindir}/hzcm
%{_bindir}/hzjpg
%{_mandir}/man1/hz.1*
%{_mandir}/man1/hzcm.1*
%{_mandir}/man1/hzjpg.1*
