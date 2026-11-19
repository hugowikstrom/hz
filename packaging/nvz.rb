# SPDX-License-Identifier: GPL-2.0-only
# Homebrew formula for the portable nvz. Install from a local clone:
#   brew install --formula packaging/nvz.rb
class Nvz < Formula
  desc "Content-aware archiver, portable pure-Python edition"
  homepage "https://nattviken.com/en/projects/nvz/"
  url "https://nattviken.com/filer/nvz/nvz-2.0-macos.tar.gz"
  sha256 "d94d96dd9b192642616a794de199e443ea1ea9df13900f1c684389e074e16ae8"
  license "GPL-2.0-only"

  depends_on "python@3.12"

  def install
    bin.install "nvz"
  end

  test do
    (testpath/"t/f.txt").write "hello nvz"
    system bin/"nvz", "pack", testpath/"t", testpath/"a.nv"
    system bin/"nvz", "test", testpath/"a.nv"
  end
end
