#!/usr/bin/env bash
#
# Build vlink via Conan + CMake, then assemble a portable tgz + QtIFW installer.
#
# This is the "rich" packaging flow:
#   - Pulls all dependencies via Conan (no system libs required)
#   - Builds with viewer (Qt) + webviz (foxglove) + examples enabled
#   - Bundles Qt / OSG / FFmpeg / OpenSSL / SQLite / Protobuf / FlatBuffers /
#     zstd shared libraries into the output tree
#   - Produces:
#       1) portable archive  : build-conan/packup/<linux|darwin>/vlink-<ver>-<os>-<arch>.tgz
#       2) QtIFW installer   : build-conan/packup/<linux|darwin>/vlink-<ver>-<os>-<arch>(.app)
#       3) (macOS) bundle    : build-conan/packup/darwin/VLink Player.app
#
# For DEB / RPM / Arch (.pkg.tar.zst) distribution packages, use
# build-deb.sh / build-rpm.sh / build-arch.sh instead.
#
# Usage:
#   ./packup/build-conan.sh {project dir} [arch]
#
#   {project dir}   absolute or relative path to the vlink source tree
#   [arch]          (macOS only) x86_64 | arm64   selects Conan profile arch
#
# Required env (when building viewer / OSG support):
#   QT_DIR    path to a Qt 5.x or 6.x install (needed on macOS and Linux x86_64)
#   OSG_DIR   path to an OpenSceneGraph install (optional, enables 3D viewer)
#   QTIFW_DIR path to Qt Installer Framework root (optional; auto-detected)
#
# Required system tools:
#   python3 + pip (to bootstrap conan if missing)
#   cmake >= 3.15
#   strip, install_name_tool (macOS), lipo (macOS)

shopt -s extglob

WORK_DIR=$(cd $(dirname ${BASH_SOURCE:-$0}) && pwd)
PLATFORM_OS=$(uname -s)
PLATFORM_ARCH=$(uname -m)

GITHUB_URL=""
POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            echo -e "Usage:\n  build-conan.sh [--github_url <url>] {project dir} [arch]\n\nOptions:\n  --github_url <url>  override CPM_GITHUB_URL (default: https://github.com)"
            exit 0
            ;;
        --github_url)
            GITHUB_URL="$2"
            shift 2
            ;;
        *)
            POSITIONAL+=("$1")
            shift
            ;;
    esac
done

if [ ${#POSITIONAL[@]} -eq 0 ]; then
    echo -e "Usage:\n  build-conan.sh [--github_url <url>] {project dir} [arch]"
    exit 0
fi

if [ "$PLATFORM_OS" = "Darwin" ] || [ "$PLATFORM_ARCH" = "x86_64" ];then
    [ -z $QT_DIR ] && echo -e "QT_DIR env is empty!" && exit 1
fi

command -v conan &> /dev/null || pip install conan --user

[ $? -ne 0 ] && exit 2

conan profile detect &> /dev/null

if [ "$PLATFORM_OS" = "Darwin" ];then
    if [ "${POSITIONAL[1]}" = "x86_64" ];then
        sed -i '' 's/armv8/x86_64/g' "$HOME/.conan2/profiles/default"
    elif [ "${POSITIONAL[1]}" = "arm64" ];then
        sed -i '' 's/x86_64/armv8/g' "$HOME/.conan2/profiles/default"
    fi
    echo ""
else
    sed -i 's/gnu14/gnu17/g' "$HOME/.conan2/profiles/default"
    echo ""
fi

echo ""
echo "********************************************"
echo "*** conan install..."
echo "********************************************"
echo ""

SRC_DIR=${POSITIONAL[0]}
BUILD_DIR=${POSITIONAL[0]}/build-conan

INSTALL_DIR=$BUILD_DIR/install

if [ "$PLATFORM_OS" = "Darwin" ];then
    PACKUP_DIR=$BUILD_DIR/packup/darwin/vlink
else
    PACKUP_DIR=$BUILD_DIR/packup/linux/vlink
fi

[ -d $BUILD_DIR ] && rm -rf $BUILD_DIR

conan install ${POSITIONAL[0]} --output-folder=$BUILD_DIR --build=missing --profile default --profile $WORK_DIR/conan_profile

[ $? -ne 0 ] && exit 2

echo ""
echo "********************************************"
echo "*** cmake build..."
echo "********************************************"
echo ""

cmake -E make_directory $BUILD_DIR/output/lib
cmake -E make_directory $BUILD_DIR/output/bin

CMAKE_EXTRA_ARGS=()
[ -n "$GITHUB_URL" ] && CMAKE_EXTRA_ARGS+=("-DCPM_GITHUB_URL=$GITHUB_URL")

if [ ! -z $QT_DIR ];then
    cmake -S $SRC_DIR -B $BUILD_DIR -DCMAKE_TOOLCHAIN_FILE="conan/conan_toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_IGNORE_PATH="/usr;/usr/local" \
        -DCMAKE_INSTALL_PREFIX=$INSTALL_DIR \
        -DCMAKE_PREFIX_PATH=$QT_DIR \
        -DENABLE_SYMLINKS=ON \
        -DENABLE_COMPLETIONS=ON \
        -DENABLE_CPM=ON \
        -DENABLE_IOX_ROUDI=ON \
        -DENABLE_VIEWER=ON \
        -DENABLE_VIEWER_FFMPEG=ON \
        -DENABLE_VIEWER_OSG=ON \
        -DENABLE_WEBVIZ=ON \
        -DENABLE_WEBVIZ_FOXGLOVE=ON \
        -DENABLE_WEBVIZ_RERUN=OFF \
        "${CMAKE_EXTRA_ARGS[@]}"
else
    cmake -S $SRC_DIR -B $BUILD_DIR -DCMAKE_TOOLCHAIN_FILE="conan/conan_toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_IGNORE_PATH="/usr;/usr/local" \
        -DCMAKE_INSTALL_PREFIX=$INSTALL_DIR \
        -DENABLE_SYMLINKS=ON \
        -DENABLE_COMPLETIONS=ON \
        -DENABLE_CPM=ON \
        -DENABLE_IOX_ROUDI=ON \
        -DENABLE_VIEWER=ON \
        -DENABLE_VIEWER_FFMPEG=ON \
        -DENABLE_VIEWER_OSG=ON \
        -DENABLE_WEBVIZ=ON \
        -DENABLE_WEBVIZ_FOXGLOVE=ON \
        -DENABLE_WEBVIZ_RERUN=OFF \
        "${CMAKE_EXTRA_ARGS[@]}"
    fi

[ $? -ne 0 ] && exit 2

if [ "$PLATFORM_OS" = "Darwin" ];then
    cmake --build $BUILD_DIR --config Release -j$(sysctl -n hw.ncpu)
else
    cmake --build $BUILD_DIR --config Release -j$(nproc)
fi

[ $? -ne 0 ] && exit 2

strip $BUILD_DIR/output/bin/vlink-*
strip $BUILD_DIR/output/lib/libvlink-*

cmake --install $BUILD_DIR

[ $? -ne 0 ] && exit 2

echo ""
echo "********************************************"
echo "*** copy runtime files..."
echo "********************************************"
echo ""

rm -rf "$PACKUP_DIR"
mkdir -p "$PACKUP_DIR/bin" "$PACKUP_DIR/lib/cmake" "$PACKUP_DIR/include" "$PACKUP_DIR/etc" "$PACKUP_DIR/desktop"

for f in "$INSTALL_DIR"/bin/vlink-*;do
    [ -f "$f" ] && cp -f "$f" "$PACKUP_DIR/bin/"
done
for f in "$INSTALL_DIR"/bin/*;do
    [ -L "$f" ] || continue
    target=$(readlink "$f")
    case "$target" in vlink-*) cp -a "$f" "$PACKUP_DIR/bin/" ;; esac
done

for f in "$INSTALL_DIR"/lib/libvlink*;do
    case "$f" in *.a) continue;; esac
    [ -f "$f" ] || [ -L "$f" ] && cp -a "$f" "$PACKUP_DIR/lib/"
done

for d in "$INSTALL_DIR"/lib/cmake/vlink*;do
    [ -d "$d" ] && cp -rf "$d" "$PACKUP_DIR/lib/cmake/"
done

[ -d "$INSTALL_DIR/include/vlink" ] && cp -rf "$INSTALL_DIR/include/vlink" "$PACKUP_DIR/include/"
[ -d "$INSTALL_DIR/etc/vlink" ] && cp -rf "$INSTALL_DIR/etc/vlink" "$PACKUP_DIR/etc/"
rm -rf "$PACKUP_DIR/etc/vlink/licenses"

cmake -E copy $SRC_DIR/version.txt    $PACKUP_DIR/

if [ "$PLATFORM_OS" = "Darwin" ];then

    cmake -E copy $BUILD_DIR/output/lib/libavcodec.+([0-9]).dylib                                   $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libavformat.+(+([0-9])).dylib                               $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libavutil.+([0-9]).dylib                                    $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libswscale.+([0-9]).dylib                                   $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libcrypto.+([0-9]).dylib                                    $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libprotobuf.+([0-9]).dylib                                  $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libflatbuffers.+([0-9]).dylib                               $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libssl.+([0-9]).dylib                                       $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libsqlite3*                                                 $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libzstd*                                                    $PACKUP_DIR/lib/ 2>/dev/null || true

    if [ -f $BUILD_DIR/output/bin/iox-roudi ];then
        cmake -E copy $BUILD_DIR/output/bin/iox-roudi                                               $PACKUP_DIR/bin/
    elif [ -f $BUILD_DIR/iox-roudi ];then
        cmake -E copy $BUILD_DIR/iox-roudi                                                          $PACKUP_DIR/bin/
    fi

    if [ ! -z $QT_DIR ];then
        QT_VERSION=$($QT_DIR/bin/qmake -query QT_VERSION)
        echo "QT_VERSION=${QT_VERSION}"

        echo ""
        echo "********************************************"
        echo "*** copy qt files..."
        echo "********************************************"
        echo ""

        cmake -E make_directory $PACKUP_DIR/lib/platforms/
        cmake -E make_directory $PACKUP_DIR/lib/imageformats/
        cmake -E make_directory $PACKUP_DIR/lib/sqldrivers/

        if [[ ${QT_VERSION} = 5.* ]] || [[ $QT_VERSION = 6.* ]];then
            for _fw in QtCore QtGui QtWidgets QtOpenGL QtOpenGLWidgets QtNetwork QtSql QtSvg;do
                cmake -E make_directory $PACKUP_DIR/lib/${_fw}.framework/Versions/A
                [ -f $QT_DIR/lib/${_fw}.framework/Versions/A/${_fw} ] && \
                cmake -E copy $QT_DIR/lib/${_fw}.framework/Versions/A/${_fw}                        $PACKUP_DIR/lib/${_fw}.framework/Versions/A/
            done
            cmake -E copy $QT_DIR/plugins/platforms/libqcocoa.dylib                                 $PACKUP_DIR/lib/platforms/
            for _fmt in libqgif libqico libqjpeg libqsvg;do
                [ -f $QT_DIR/plugins/imageformats/${_fmt}.dylib ] && \
                cmake -E copy $QT_DIR/plugins/imageformats/${_fmt}.dylib                            $PACKUP_DIR/lib/imageformats/
            done
            cmake -E copy $QT_DIR/plugins/sqldrivers/libqsqlite.dylib                               $PACKUP_DIR/lib/sqldrivers/
        else
            echo "QT_VERSION error, QT_VERSION=$QT_VERSION"
            exit 3
        fi
    fi

    OSG_VERSION=3.6.5
    OSG_PREFIX1=3.3.1
    OSG_PREFIX2=21
    OSG_PREFIX3=161

    if [ ! -z $OSG_DIR ];then
        echo "OSG_VERSION=${OSG_VERSION}"

        echo ""
        echo "********************************************"
        echo "*** copy osg files..."
        echo "********************************************"
        echo ""

        cmake -E make_directory $PACKUP_DIR/lib/osgPlugins-${OSG_VERSION}/
        cmake -E copy $OSG_DIR/lib/libOpenThreads.${OSG_PREFIX1}.dylib                              $PACKUP_DIR/lib/libOpenThreads.${OSG_PREFIX2}.dylib
        for _osg in libosg libosgDB libosgGA libosgManipulator libosgUtil libosgText libosgViewer;do
            cmake -E copy $OSG_DIR/lib/${_osg}.${OSG_PREFIX3}.dylib                                 $PACKUP_DIR/lib/
        done
        for _plugin in osgdb_obj osgdb_osg osgdb_serializers_osg;do
            cmake -E copy $OSG_DIR/lib/osgPlugins-${OSG_VERSION}/${_plugin}.so                      $PACKUP_DIR/lib/osgPlugins-${OSG_VERSION}/
        done
    fi

    for f in run_viewer.sh run_player.sh run_analyzer.sh run_cmd.sh;do
        [ -f $WORK_DIR/darwin/$f ] && cmake -E copy $WORK_DIR/darwin/$f $PACKUP_DIR/bin/
    done
    [ -f $WORK_DIR/darwin/install.sh ] && cmake -E copy $WORK_DIR/darwin/install.sh $PACKUP_DIR/
    [ -f $WORK_DIR/darwin/uninstall.sh ] && cmake -E copy $WORK_DIR/darwin/uninstall.sh $PACKUP_DIR/
    [ -f $WORK_DIR/darwin/setup_runtime.sh ] && cmake -E copy $WORK_DIR/darwin/setup_runtime.sh $PACKUP_DIR/
    cmake -E copy $WORK_DIR/darwin/qt.conf $PACKUP_DIR/bin/
    for f in $(find $WORK_DIR/darwin -maxdepth 1 \( -name '*.png' -o -name '*.svg' \));do
        cmake -E copy $f $PACKUP_DIR/desktop/
    done
    chmod +x $PACKUP_DIR/bin/*.sh $PACKUP_DIR/*.sh 2>/dev/null

    echo ""
    echo "********************************************"
    echo "*** fix rpath for macOS binaries..."
    echo "********************************************"
    echo ""
    for f in "$PACKUP_DIR"/bin/vlink-*;do
        [ -f "$f" ] || continue
        file "$f" | grep -q Mach-O || continue
        if otool -l "$f" 2>/dev/null | grep -q LC_RPATH; then continue; fi
        install_name_tool -add_rpath @executable_path/../lib "$f" && \
        echo "Added rpath to: $(basename "$f")"
    done

    current_arch=${POSITIONAL[1]}
    [ "$current_arch" = "" ] && current_arch=$(uname -m)
    if [ "$current_arch" = "arm64" ]; then
        remove_arch="x86_64"
    elif [ "$current_arch" = "x86_64" ]; then
        remove_arch="arm64"
    else
        echo "Unsupported architecture: $current_arch"
        exit 1
    fi
    find $PACKUP_DIR/ -type f | while read -r file;do
        if lipo -info "$file" 2>/dev/null | grep -q "$remove_arch";then
            temp_file="${file}.tmp"
            lipo "$file" -remove "$remove_arch" -output "$temp_file" 2>/dev/null && \
            mv "$temp_file" "$file" && \
            codesign --force -s - "$file" && \
            echo "Removed $remove_arch and re-signed: $file"
        fi
    done

    rm -rf "$PACKUP_DIR/../VLink Player.app"
    MACOS_PACKUP_DIR="$PACKUP_DIR/../VLink Player.app/Contents/"
    mkdir -p "$MACOS_PACKUP_DIR/MacOS/"
    mkdir -p "$MACOS_PACKUP_DIR/Resources/"
    cp -rf $PACKUP_DIR/* "$MACOS_PACKUP_DIR/MacOS/"
    cp -rf $PACKUP_DIR/desktop/vlink-player.png "$MACOS_PACKUP_DIR/Resources/"
    cat > "$MACOS_PACKUP_DIR/Info.plist" <<EOL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>VLink Player</string>
    <key>CFBundleExecutable</key>
    <string>bin/run_player.sh</string>
    <key>CFBundleIconFile</key>
    <string>vlink-player.png</string>
    <key>CFBundleIdentifier</key>
    <string>com.vlink.player</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>LSMinimumSystemVersion</key>
    <string>12.0</string>
</dict>
</plist>
EOL
    ln -sf bin/vlink-proxy     "$MACOS_PACKUP_DIR/MacOS/proxy"
    ln -sf bin/vlink-info      "$MACOS_PACKUP_DIR/MacOS/info"
    ln -sf bin/vlink-monitor   "$MACOS_PACKUP_DIR/MacOS/monitor"
    ln -sf bin/vlink-bag       "$MACOS_PACKUP_DIR/MacOS/bag"
    ln -sf bin/vlink-list      "$MACOS_PACKUP_DIR/MacOS/list"
    ln -sf bin/vlink-eproto    "$MACOS_PACKUP_DIR/MacOS/eproto"
    ln -sf bin/vlink-efbs      "$MACOS_PACKUP_DIR/MacOS/efbs"
    ln -sf bin/vlink-dump      "$MACOS_PACKUP_DIR/MacOS/dump"
    ln -sf bin/vlink-check     "$MACOS_PACKUP_DIR/MacOS/check"
    ln -sf bin/vlink-bench     "$MACOS_PACKUP_DIR/MacOS/bench"
    ln -sf bin/vlink-foxglove  "$MACOS_PACKUP_DIR/MacOS/webviz_foxglove"
    ln -sf bin/vlink-bag2mcap  "$MACOS_PACKUP_DIR/MacOS/bag2mcap"
    ln -sf bin/run_viewer.sh   "$MACOS_PACKUP_DIR/MacOS/viewer"
    ln -sf bin/run_player.sh   "$MACOS_PACKUP_DIR/MacOS/player"
    ln -sf bin/run_analyzer.sh "$MACOS_PACKUP_DIR/MacOS/analyzer"

    ln -sf bin/vlink-foxglove  "$MACOS_PACKUP_DIR/MacOS/webviz"
    echo ""
else
    ln -sf vlink-foxglove      "$PACKUP_DIR/bin/webviz"
    cmake -E copy $BUILD_DIR/output/lib/libavcodec.so.+(+([0-9]))                                   $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libavformat.so.+(+([0-9]))                                  $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libavutil.so.+(+([0-9]))                                    $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libswscale.so.+(+([0-9]))                                   $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libcrypto.so.+(+([0-9]))                                    $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libprotobuf.so.+(+([0-9]))                                  $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libflatbuffers.so.+(+([0-9]))                               $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libssl.so.+(+([0-9]))                                       $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libsqlite3*                                                 $PACKUP_DIR/lib/ 2>/dev/null || true
    cmake -E copy $BUILD_DIR/output/lib/libzstd*                                                    $PACKUP_DIR/lib/ 2>/dev/null || true

    if [ -f $BUILD_DIR/output/bin/iox-roudi ];then
        cmake -E copy $BUILD_DIR/output/bin/iox-roudi                                               $PACKUP_DIR/bin/
    elif [ -f $BUILD_DIR/iox-roudi ];then
        cmake -E copy $BUILD_DIR/iox-roudi                                                          $PACKUP_DIR/bin/
    fi

    if [ ! -z $QT_DIR ];then
        QT_VERSION=$($QT_DIR/bin/qmake -query QT_VERSION)
        echo "QT_VERSION=${QT_VERSION}"

        echo ""
        echo "********************************************"
        echo "*** copy qt files..."
        echo "********************************************"
        echo ""

        for _dir in platforms platformthemes platforminputcontexts imageformats sqldrivers xcbglintegrations;do
            cmake -E make_directory $PACKUP_DIR/lib/$_dir/
        done

        if [[ ${QT_VERSION} = 5.* ]];then
            for _lib in Core Gui Widgets DBus Sql OpenGL XcbQpa Network Svg;do
                cmake -E copy $QT_DIR/lib/libQt5${_lib}.so.+([0-9])                                 $PACKUP_DIR/lib/ 2>/dev/null || true
            done
            cmake -E copy $QT_DIR/lib/libicudata.so.*+([0-9])                                       $PACKUP_DIR/lib/ 2>/dev/null || true
            cmake -E copy $QT_DIR/lib/libicui18n.so.*+([0-9])                                       $PACKUP_DIR/lib/ 2>/dev/null || true
            cmake -E copy $QT_DIR/lib/libicuuc.so.+([0-9])                                          $PACKUP_DIR/lib/ 2>/dev/null || true
            cmake -E copy $QT_DIR/plugins/platforms/libqxcb.so                                      $PACKUP_DIR/lib/platforms/            2>/dev/null || true
            for _fmt in libqgif libqico libqjpeg libqsvg;do
                cmake -E copy $QT_DIR/plugins/imageformats/${_fmt}.so                               $PACKUP_DIR/lib/imageformats/         2>/dev/null || true
            done
            cmake -E copy $QT_DIR/plugins/sqldrivers/libqsqlite.so                                  $PACKUP_DIR/lib/sqldrivers/           2>/dev/null || true
        elif [[ $QT_VERSION = 6.* ]];then
            for _lib in Core Gui Widgets DBus Sql OpenGL OpenGLWidgets XcbQpa Network Svg;do
                cmake -E copy $QT_DIR/lib/libQt6${_lib}.so.+([0-9])                                 $PACKUP_DIR/lib/ 2>/dev/null || true
            done
            cmake -E copy $QT_DIR/lib/libicudata.so.+([0-9])                                        $PACKUP_DIR/lib/ 2>/dev/null || true
            cmake -E copy $QT_DIR/lib/libicui18n.so.+([0-9])                                        $PACKUP_DIR/lib/ 2>/dev/null || true
            cmake -E copy $QT_DIR/lib/libicuuc.so.+([0-9])                                          $PACKUP_DIR/lib/ 2>/dev/null || true
            cmake -E copy $QT_DIR/plugins/platforms/libqxcb.so                                      $PACKUP_DIR/lib/platforms/            2>/dev/null || true
            for _fmt in libqgif libqico libqjpeg libqsvg;do
                cmake -E copy $QT_DIR/plugins/imageformats/${_fmt}.so                               $PACKUP_DIR/lib/imageformats/         2>/dev/null || true
            done
            cmake -E copy $QT_DIR/plugins/sqldrivers/libqsqlite.so                                  $PACKUP_DIR/lib/sqldrivers/           2>/dev/null || true
        else
            echo "QT_VERSION error, QT_VERSION=$QT_VERSION"
            exit 3
        fi

        for _dir in platformthemes platforminputcontexts xcbglintegrations;do
            src="$QT_DIR/plugins/$_dir"
            dst="$PACKUP_DIR/lib/$_dir"
            [ -d "$src" ] || continue
            mkdir -p "$dst"
            find "$src" \
                -maxdepth 1 \
                -type f \
                ! -name "*.debug" \
                ! -name "*.dbg" \
                -exec cp -f {} "$dst/" \;
        done
    fi

    OSG_VERSION=3.6.5
    OSG_PREFIX1=3.3.1
    OSG_PREFIX2=21
    OSG_PREFIX3=161

    if [ ! -z $OSG_DIR ];then
        echo "OSG_VERSION=${OSG_VERSION}"

        echo ""
        echo "********************************************"
        echo "*** copy osg files..."
        echo "********************************************"
        echo ""

        cmake -E make_directory $PACKUP_DIR/lib/osgPlugins-${OSG_VERSION}/
        cmake -E copy $OSG_DIR/lib/libOpenThreads.so.${OSG_PREFIX1}                                 $PACKUP_DIR/lib/libOpenThreads.so.${OSG_PREFIX2}
        for _osg in libosg libosgDB libosgGA libosgManipulator libosgUtil libosgText libosgViewer;do
            cmake -E copy $OSG_DIR/lib/${_osg}.so.${OSG_PREFIX3}                                    $PACKUP_DIR/lib/
        done
        for _plugin in osgdb_freetype osgdb_obj osgdb_osg osgdb_serializers_osg;do
            cmake -E copy $OSG_DIR/lib/osgPlugins-${OSG_VERSION}/${_plugin}.so                      $PACKUP_DIR/lib/osgPlugins-${OSG_VERSION}/
        done
    fi

    for f in run_viewer.sh run_player.sh run_analyzer.sh run_cmd.sh;do
        [ -f $WORK_DIR/linux/common/$f ] && cmake -E copy $WORK_DIR/linux/common/$f $PACKUP_DIR/bin/
    done
    [ -f $WORK_DIR/linux/common/install.sh ] && cmake -E copy $WORK_DIR/linux/common/install.sh $PACKUP_DIR/
    [ -f $WORK_DIR/linux/common/uninstall.sh ] && cmake -E copy $WORK_DIR/linux/common/uninstall.sh $PACKUP_DIR/
    [ -f $WORK_DIR/linux/common/setup_runtime.sh ] && cmake -E copy $WORK_DIR/linux/common/setup_runtime.sh $PACKUP_DIR/
    cmake -E copy $WORK_DIR/linux/common/qt.conf $PACKUP_DIR/bin/
    chmod +x $PACKUP_DIR/bin/*.sh $PACKUP_DIR/*.sh 2>/dev/null
    for f in $(find $WORK_DIR/linux/common -maxdepth 1 \( -name '*.desktop' -o -name '*.png' -o -name '*.svg' -o -name '*.xml' \));do
        cmake -E copy $f $PACKUP_DIR/desktop/
    done
    cmake -E copy_directory $WORK_DIR/linux/$PLATFORM_ARCH $PACKUP_DIR/lib/
fi

echo ""
echo "********************************************"
echo "*** aggregating third-party license files..."
echo "********************************************"
echo ""

LICENSES_DIR="$PACKUP_DIR/licenses"
mkdir -p "$LICENSES_DIR"

for _src in "$INSTALL_DIR"/*/vlink/licenses; do
    if [ -d "$_src" ]; then
        cp -af "$_src"/. "$LICENSES_DIR/"
        break
    fi
done

if [ -d "$BUILD_DIR/output/licenses" ]; then
    for d in "$BUILD_DIR"/output/licenses/*/; do
        [ -d "$d" ] || continue
        name=$(basename "$d")
        mkdir -p "$LICENSES_DIR/$name"
        cp -af "$d"/. "$LICENSES_DIR/$name/"
    done
fi

if [ -n "$QT_DIR" ]; then
    _has_qt=0
    case "$PLATFORM_OS" in
        Darwin) [ -f "$PACKUP_DIR/lib/QtCore.framework/Versions/A/QtCore" ] && _has_qt=1 ;;
        *)      ls "$PACKUP_DIR"/lib/libQt*Core.so* >/dev/null 2>&1 && _has_qt=1 ;;
    esac
    if [ $_has_qt -eq 1 ]; then
        mkdir -p "$LICENSES_DIR/qt"
        [ -f "$WORK_DIR/licenses/qt/README.md" ] && cp -f "$WORK_DIR/licenses/qt/README.md" "$LICENSES_DIR/qt/"
        for f in "$QT_DIR"/LICENSE* "$QT_DIR"/LGPL* "$QT_DIR"/GPL* "$QT_DIR"/COPYING*; do
            [ -f "$f" ] && cp -f "$f" "$LICENSES_DIR/qt/"
        done
        for d in "$QT_DIR"/licenses "$QT_DIR"/Licenses "$QT_DIR"/../Licenses; do
            [ -d "$d" ] && cp -af "$d"/. "$LICENSES_DIR/qt/" && break
        done
        if ls "$PACKUP_DIR"/lib/libicu* >/dev/null 2>&1; then
            mkdir -p "$LICENSES_DIR/icu"
            [ -f "$WORK_DIR/licenses/icu/README.md" ] && cp -f "$WORK_DIR/licenses/icu/README.md" "$LICENSES_DIR/icu/"
        fi
    fi
fi

if [ -n "$OSG_DIR" ]; then
    _has_osg=0
    case "$PLATFORM_OS" in
        Darwin) ls "$PACKUP_DIR"/lib/libosg.*.dylib >/dev/null 2>&1 && _has_osg=1 ;;
        *)      ls "$PACKUP_DIR"/lib/libosg.so.* >/dev/null 2>&1 && _has_osg=1 ;;
    esac
    if [ $_has_osg -eq 1 ]; then
        mkdir -p "$LICENSES_DIR/osg"
        [ -f "$WORK_DIR/licenses/osg/README.md" ] && cp -f "$WORK_DIR/licenses/osg/README.md" "$LICENSES_DIR/osg/"
        for f in "$OSG_DIR"/LICENSE* "$OSG_DIR"/COPYING* \
                 "$OSG_DIR"/share/OpenSceneGraph/LICENSE* \
                 "$OSG_DIR"/doc/LICENSE*; do
            [ -f "$f" ] && cp -f "$f" "$LICENSES_DIR/osg/"
        done
    fi
fi

echo "Licenses aggregated to: $LICENSES_DIR"
ls -1 "$LICENSES_DIR" 2>/dev/null

echo ""
echo "********************************************"
echo "*** creating portable archive..."
echo "********************************************"
echo ""

VERSION=$(cat "$SRC_DIR/version.txt" | tr -d '[:space:]')
ARCH=${POSITIONAL[1]:-$PLATFORM_ARCH}
if [ "$PLATFORM_OS" = "Darwin" ];then
    PLATFORM_TAG="macos-${ARCH}"
    ARCHIVE_DIR=$BUILD_DIR/packup/darwin
else
    PLATFORM_TAG="linux-${ARCH}"
    ARCHIVE_DIR=$BUILD_DIR/packup/linux
fi

ARCHIVE_NAME="vlink-${VERSION}-${PLATFORM_TAG}.tgz"
ARCHIVE_PATH="$ARCHIVE_DIR/${ARCHIVE_NAME}"

tar -czf "$ARCHIVE_PATH" -C "$(dirname "$PACKUP_DIR")" "$(basename "$PACKUP_DIR")"

echo "Portable archive: $ARCHIVE_PATH ($(du -sh "$ARCHIVE_PATH" | cut -f1))"

echo ""
echo "********************************************"
echo "*** creating installer package..."
echo "********************************************"
echo ""

BINARYCREATOR=""

if [ ! -z "$QTIFW_DIR" ];then
    BINARYCREATOR="$QTIFW_DIR/bin/binarycreator"
elif command -v binarycreator &> /dev/null;then
    BINARYCREATOR="binarycreator"
else
    for dir in \
        "$HOME/Qt/Tools/QtInstallerFramework"/*/bin \
        "/opt/Qt/Tools/QtInstallerFramework"/*/bin \
        "/usr/local/Qt/Tools/QtInstallerFramework"/*/bin \
    ;do
        if [ -x "$dir/binarycreator" ];then
            BINARYCREATOR="$dir/binarycreator"
            break
        fi
    done
fi

if [ -z "$BINARYCREATOR" ];then
    echo "Warning: binarycreator not found, skipping installer creation."
    echo "Install Qt Installer Framework and set QTIFW_DIR or add binarycreator to PATH."
    echo ""
    echo "Packup directory: $PACKUP_DIR"
    echo "Done."
    exit 0
fi

if [ ! -f "$WORK_DIR/installer/config/config.xml" ];then
    echo "Warning: Installer templates not found, skipping installer creation."
    echo "Packup directory: $PACKUP_DIR"
    echo "Done."
    exit 0
fi

echo "Using binarycreator: $BINARYCREATOR"

VERSION=$(cat "$SRC_DIR/version.txt" | tr -d '[:space:]')
DATE=$(date +%Y-%m-%d)

INSTALLER_DIR=$BUILD_DIR/installer
INSTALLER_CONFIG=$INSTALLER_DIR/config
INSTALLER_PKG=$INSTALLER_DIR/packages/com.vlink
INSTALLER_META=$INSTALLER_PKG/meta
INSTALLER_DATA=$INSTALLER_PKG/data

rm -rf "$INSTALLER_DIR"
mkdir -p "$INSTALLER_CONFIG"
mkdir -p "$INSTALLER_META"
mkdir -p "$INSTALLER_DATA"

cp -rf "$WORK_DIR/installer/config/"* "$INSTALLER_CONFIG/"

DEFAULT_TARGET_DIR="@HomeDir@/vlink"

if [ "$PLATFORM_OS" = "Darwin" ]; then
    MAINTENANCE_NAME="vlink-uninstall"
else
    MAINTENANCE_NAME="vlink-uninstall.run"
fi

sed -e "s|@VERSION@|$VERSION|g" \
    -e "s|@DEFAULT_TARGET_DIR@|$DEFAULT_TARGET_DIR|g" \
    -e "s|@MAINTENANCE_NAME@|$MAINTENANCE_NAME|g" \
    "$WORK_DIR/installer/config/config.xml" > "$INSTALLER_CONFIG/config.xml"

sed -e "s|@VERSION@|$VERSION|g" \
    -e "s|@DATE@|$DATE|g" \
    "$WORK_DIR/installer/packages/com.vlink/meta/package.xml" > "$INSTALLER_META/package.xml"

cp -f "$WORK_DIR/installer/packages/com.vlink/meta/installscript.qs" "$INSTALLER_META/"
cp -f "$SRC_DIR/LICENSE" "$INSTALLER_META/"
for _src in "$LICENSES_DIR/LICENSE_NOTICES.md" "$BUILD_DIR/LICENSE_NOTICES.md"; do
    if [ -f "$_src" ]; then
        cp -f "$_src" "$INSTALLER_META/LICENSE_NOTICES.md"
        break
    fi
done

echo "Copying packup files to installer data directory..."
cp -rf "$PACKUP_DIR"/* "$INSTALLER_DATA/"

ARCH=${POSITIONAL[1]:-$PLATFORM_ARCH}
if [ "$PLATFORM_OS" = "Darwin" ];then
    PLATFORM_TAG="macos-${ARCH}"
    OUTPUT_DIR=$BUILD_DIR/packup/darwin
else
    PLATFORM_TAG="linux-${ARCH}"
    OUTPUT_DIR=$BUILD_DIR/packup/linux
fi

if [ "$PLATFORM_OS" = "Darwin" ]; then
    OUTPUT_NAME="vlink-${VERSION}-${PLATFORM_TAG}"
else
    OUTPUT_NAME="vlink-${VERSION}-${PLATFORM_TAG}.run"
fi

OUTPUT_PATH="$OUTPUT_DIR/${OUTPUT_NAME}"

echo "Creating installer: ${OUTPUT_PATH}"
echo ""

"$BINARYCREATOR" \
    --offline-only \
    -c "$INSTALLER_CONFIG/config.xml" \
    -p "$INSTALLER_DIR/packages" \
    "$OUTPUT_PATH"

if [ $? -ne 0 ];then
    echo "Error: binarycreator failed!"
    echo "Packup directory: $PACKUP_DIR"
    exit 2
else
    if [ -d "${OUTPUT_PATH}.app" ];then
        ACTUAL_OUTPUT="${OUTPUT_PATH}.app"
    elif [ -f "${OUTPUT_PATH}" ];then
        ACTUAL_OUTPUT="${OUTPUT_PATH}"
    else
        ACTUAL_OUTPUT="${OUTPUT_PATH}"
    fi
    echo ""
    echo "********************************************"
    echo "*** Installer created: $ACTUAL_OUTPUT"
    echo "*** Size: $(du -sh "$ACTUAL_OUTPUT" | cut -f1)"
    echo "********************************************"
fi

echo ""
echo "Done."

exit 0
