# Qt (bundled by build-conan.sh)

The Conan packaging flow bundles a subset of the Qt framework, dynamically
linked into `vlink-viewer`, `vlink-player`, and `vlink-analyzer` via the
`.so`/`.dylib`/`.framework` files in `lib/`.

Qt is licensed under **GNU LGPL v3**. Source code corresponding to the exact
binaries shipped in this distribution is available from
<https://www.qt.io/download-open-source> and
<https://download.qt.io/archive/qt/>. Use the `qmake -query QT_VERSION`
output of the bundled libraries to select the matching tarball.

LGPL §6 is satisfied by the dynamic-library form: end users may replace the
bundled Qt shared libraries with their own LGPL-compatible build of the same
SONAME without modifying the rest of the vlink distribution.

The full text of the LGPL v3 is included as `LGPL-3.0.txt`; this is the
upstream Qt licensing terms that apply to the binaries in `lib/`.
