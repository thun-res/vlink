# ICU (bundled by build-conan.sh, transitive Qt dependency)

When the Qt bundle ships through the Conan packaging flow, the ICU runtime
libraries (`libicudata.so.*`, `libicui18n.so.*`, `libicuuc.so.*`) are copied
into `lib/` to satisfy Qt's I18N/Unicode dependencies.

ICU is licensed under a permissive Unicode license; versions prior to 74 use
**Unicode-DFS-2016**, versions 74+ use **Unicode-3.0**. See
<https://www.unicode.org/copyright.html> for the applicable text.

When ICU is also pulled in via Conan as a transitive dependency, its actual
upstream LICENSE file is copied to `icu/LICENSE` automatically by the Conan
flow; otherwise this README serves as the attribution.
