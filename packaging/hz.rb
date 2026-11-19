# SPDX-License-Identifier: GPL-2.0-only
# Homebrew formula for the portable hz. Install from a local clone:
#   brew install --formula packaging/hz.rb
class Hz < Formula
  desc "hZ - content-aware archiver, portable pure-Python edition"
  homepage "https://nattviken.com/en/projects/hz/"
  url "https://nattviken.com/filer/hz/hz-2.0-macos.tar.gz"
  sha256 "c8e45868fb4681224321e8c845c024de7d695982205a6d71df8887cc3cf0de79"
  license "GPL-2.0-only"

  depends_on "python@3.12"

  def install
    bin.install "hz"
  end

  test do
    (testpath/"t/f.txt").write "hello hz"
    system bin/"hz", "pack", testpath/"t", testpath/"a.hz"
    system bin/"hz", "test", testpath/"a.hz"
  end
end
