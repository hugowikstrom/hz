# SPDX-License-Identifier: GPL-2.0-only
# Homebrew formula for the portable hz. Install from a local clone:
#   brew install --formula packaging/hz.rb
class Hz < Formula
  desc "Content-aware archiver, portable pure-Python edition"
  homepage "https://nattviken.com/en/projects/hz/"
  url "https://nattviken.com/filer/hz/hz-2.0-macos.tar.gz"
  sha256 "af8ce316fc826aa730769efc8399f58ffda1702ddeb2a361085f7560eb512552"
  license "GPL-2.0-only"

  depends_on "python@3.12"

  def install
    bin.install "hz"
  end

  test do
    (testpath/"t/f.txt").write "hello hz"
    system bin/"hz", "pack", testpath/"t", testpath/"a.nv"
    system bin/"hz", "test", testpath/"a.nv"
  end
end
