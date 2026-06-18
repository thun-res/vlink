# OpenSceneGraph (bundled by build-conan.sh)

The Conan packaging flow optionally bundles the OpenSceneGraph (OSG) runtime,
dynamically linked into `vlink-viewer` / `vlink-analyzer` via the `libosg*.so`
(or `*.dylib`) files in `lib/` and the plugins under `lib/osgPlugins-*/`.

OpenSceneGraph is licensed under the **OpenSceneGraph Public License (OSGPL)**,
a modification of the **GNU LGPL 2.1** crafted to enable scene-graph use
without the standard LGPL re-linking constraints; see the upstream license at
<https://github.com/openscenegraph/OpenSceneGraph/blob/master/LICENSE.txt>.

Source code corresponding to the bundled binaries is available at the same
upstream repository tagged with the OSG version reported by the bundled
`libosg.so` SONAME (typically 3.6.5 for this distribution).
