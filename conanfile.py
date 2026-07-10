#
# Copyright (C) 2026 by Thun Lu. All rights reserved.
#

from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout, CMakeToolchain
import os
import shutil

class VLinkConan(ConanFile):
    name = "vlink"
    version = "2.1.0"
    author = "Thun Lu <thun.lu@zohomail.cn>"
    license = "Apache-2.0"
    url = "https://github.com/thun-res/vlink"
    homepage = "https://vlink.work"
    description = "High-performance middleware for autonomous driving and embodied AI"
    topics = ("automotive", "middleware", "communication")
    package_type = "library"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps"
    exports_sources = "*"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "enable_cpm": [True, False],
        "enable_cpm_all": [True, False],
        "enable_doc": [True, False],
        "enable_cxx_std_20": [True, False],
        "enable_c_api": [True, False],
        "enable_python_api": [True, False],
        "enable_security": [True, False],
        "enable_zstd": [True, False],
        "enable_sqlite": [True, False],
        "enable_cli_info": [True, False],
        "enable_cli_bag": [True, False],
        "enable_cli_trigger": [True, False],
        "enable_cli_eproto": [True, False],
        "enable_cli_efbs": [True, False],
        "enable_cli_list": [True, False],
        "enable_cli_monitor": [True, False],
        "enable_cli_dump": [True, False],
        "enable_cli_check": [True, False],
        "enable_cli_bench": [True, False],
        "enable_proxy": [True, False],
        "enable_viewer": [True, False],
        "enable_viewer_ffmpeg": [True, False],
        "enable_viewer_osg": [True, False],
        "enable_webviz": [True, False],
        "enable_webviz_foxglove": [True, False],
        "enable_webviz_rerun": [True, False],
        "enable_exprtk": [True, False],
        "enable_symlinks": [True, False],
        "enable_completions": [True, False],
        "enable_examples": [True, False],
        "enable_test": [True, False],
        "enable_test_warn": [True, False],
        "enable_test_sanitize": [True, False],
        "enable_test_coverage": [True, False],
        "select_log_backend": ["spdlog", "quill", "dlt", "native"],
        "install_config_dir": ["etc/vlink", "share/vlink"],
    }
    default_options = {
        "fPIC": True,
        "shared": False,
        "enable_cpm": False,
        "enable_cpm_all": False,
        "enable_doc": False,
        "enable_cxx_std_20": False,
        "enable_c_api": True,
        "enable_python_api": False,
        "enable_security": True,
        "enable_zstd": True,
        "enable_sqlite": True,
        "enable_cli_info": True,
        "enable_cli_bag": True,
        "enable_cli_trigger": True,
        "enable_cli_eproto": True,
        "enable_cli_efbs": True,
        "enable_cli_list": True,
        "enable_cli_monitor": True,
        "enable_cli_dump": True,
        "enable_cli_check": True,
        "enable_cli_bench": True,
        "enable_proxy": True,
        "enable_viewer": False,
        "enable_viewer_ffmpeg": False,
        "enable_viewer_osg": False,
        "enable_webviz": False,
        "enable_webviz_foxglove": False,
        "enable_webviz_rerun": False,
        "enable_exprtk": True,
        "enable_symlinks": True,
        "enable_completions": True,
        "enable_examples": False,
        "enable_test": False,
        "enable_test_warn": False,
        "enable_test_sanitize": False,
        "enable_test_coverage": False,
        "select_log_backend": "spdlog",
        "install_config_dir": "etc/vlink",
    }

    def set_version(self):
        version_file = os.path.join(self.recipe_folder, "version.txt")
        if os.path.isfile(version_file):
            with open(version_file, "r") as f:
                self.version = f.read().strip()

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def requirements(self):
        if not self.options.enable_cpm and not self.options.enable_cpm_all:
            self.requires("zlib/1.3.2")
            self.requires("zstd/1.5.7")
            self.requires("sqlite3/3.51.3")
            self.requires("openssl/3.0.21")
            self.requires("protobuf/3.21.12")
            self.requires("flatbuffers/25.12.19")
            self.requires("fast-dds/2.11.2")
            self.requires("cyclonedds/0.10.5")
            self.requires("iceoryx/2.0.6")
        elif not self.options.enable_cpm_all:
            self.requires("zlib/1.3.2")
            self.requires("zstd/1.5.7")
            self.requires("sqlite3/3.51.3")
            self.requires("openssl/3.0.21")
            self.requires("protobuf/3.21.12")
            self.requires("flatbuffers/25.12.19")
        if self.options.enable_viewer:
            if self.options.enable_viewer_ffmpeg:
                self.requires("ffmpeg/8.1.2")
            if self.options.enable_viewer_osg and not os.environ.get("OSG_DIR"):
                self.requires("openscenegraph/3.6.5")

    def layout(self):
        cmake_layout(self)
        self.folders.build = "conan"
        self.folders.generators = "conan"

    def _copy_dependencies(self, scan_dirs, output_dir, is_windows):
        with os.scandir(scan_dirs) as it:
            for entry in it:
                src_path = entry.path
                name = entry.name
                if entry.is_dir(follow_symlinks=False):
                    continue
                if entry.is_symlink():
                    if ".so" not in src_path and ".dylib" not in src_path:
                        continue
                    dst_path = os.path.join(output_dir, name)
                    try:
                        if os.path.lexists(dst_path):
                            if os.path.isdir(dst_path) and not os.path.islink(dst_path):
                                shutil.rmtree(dst_path)
                            else:
                                os.remove(dst_path)
                        target = os.readlink(src_path)
                        os.symlink(target, dst_path)
                    except Exception:
                        shutil.copy2(src_path, dst_path)
                    continue
                if is_windows and not src_path.endswith(".dll"):
                    continue
                if not is_windows and ".so" not in src_path and ".dylib" not in src_path:
                    continue
                shutil.copy2(src_path, os.path.join(output_dir, name))

    def generate(self):
        is_windows = str(self.settings.os).lower() == "windows"
        output_bin_dir = os.path.join("..", "output", "bin")
        output_lib_dir = os.path.join("..", "output", "lib")
        output_lic_dir = os.path.join("..", "output", "licenses")
        os.makedirs(output_bin_dir, exist_ok=True)
        os.makedirs(output_lib_dir, exist_ok=True)
        os.makedirs(output_lic_dir, exist_ok=True)

        for dep in self.dependencies.host.values():
            pkg = getattr(dep, "package_folder", None)
            if not pkg:
                continue

            if is_windows:
                output_dir = output_bin_dir
                scan_dirs  = os.path.join(pkg, "bin")
            else:
                output_dir = output_lib_dir
                scan_dirs  = os.path.join(pkg, "lib")
            if os.path.isdir(scan_dirs):
                self._copy_dependencies(scan_dirs, output_dir, is_windows)

            lic_src = os.path.join(pkg, "licenses")
            if os.path.isdir(lic_src):
                try:
                    name = str(dep.ref.name)
                except Exception:
                    name = os.path.basename(pkg)
                lic_dst = os.path.join(output_lic_dir, name)
                os.makedirs(lic_dst, exist_ok=True)
                for item in os.listdir(lic_src):
                    src_item = os.path.join(lic_src, item)
                    if os.path.isfile(src_item):
                        shutil.copy2(src_item, os.path.join(lic_dst, item))

        tc = CMakeToolchain(self)
        tc.variables["ENABLE_CCACHE_BUILD"]    = "OFF"
        tc.variables["ENABLE_CPM"]             = "ON" if (self.options.enable_cpm or self.options.enable_cpm_all) else "OFF"
        tc.variables["ENABLE_CPM_ALL"]         = "ON" if self.options.enable_cpm_all else "OFF"
        tc.variables["ENABLE_DOC"]             = "ON" if self.options.enable_doc else "OFF"
        tc.variables["ENABLE_CXX_STD_20"]      = "ON" if self.options.enable_cxx_std_20 else "OFF"
        tc.variables["ENABLE_C_API"]           = "ON" if self.options.enable_c_api else "OFF"
        tc.variables["ENABLE_PYTHON_API"]      = "ON" if self.options.enable_python_api else "OFF"
        tc.variables["ENABLE_SECURITY"]        = "ON" if self.options.enable_security else "OFF"
        tc.variables["ENABLE_ZSTD"]            = "ON" if self.options.enable_zstd else "OFF"
        tc.variables["ENABLE_SQLITE"]          = "ON" if self.options.enable_sqlite else "OFF"
        tc.variables["ENABLE_CLI_INFO"]        = "ON" if self.options.enable_cli_info else "OFF"
        tc.variables["ENABLE_CLI_BAG"]         = "ON" if self.options.enable_cli_bag else "OFF"
        tc.variables["ENABLE_CLI_TRIGGER"]     = "ON" if self.options.enable_cli_trigger else "OFF"
        tc.variables["ENABLE_CLI_EPROTO"]      = "ON" if self.options.enable_cli_eproto else "OFF"
        tc.variables["ENABLE_CLI_EFBS"]        = "ON" if self.options.enable_cli_efbs else "OFF"
        tc.variables["ENABLE_CLI_LIST"]        = "ON" if self.options.enable_cli_list else "OFF"
        tc.variables["ENABLE_CLI_MONITOR"]     = "ON" if self.options.enable_cli_monitor else "OFF"
        tc.variables["ENABLE_CLI_DUMP"]        = "ON" if self.options.enable_cli_dump else "OFF"
        tc.variables["ENABLE_CLI_CHECK"]       = "ON" if self.options.enable_cli_check else "OFF"
        tc.variables["ENABLE_CLI_BENCH"]       = "ON" if self.options.enable_cli_bench else "OFF"
        tc.variables["ENABLE_PROXY"]           = "ON" if self.options.enable_proxy else "OFF"
        tc.variables["ENABLE_VIEWER"]          = "ON" if self.options.enable_viewer else "OFF"
        tc.variables["ENABLE_VIEWER_FFMPEG"]   = "ON" if self.options.enable_viewer_ffmpeg else "OFF"
        tc.variables["ENABLE_VIEWER_OSG"]      = "ON" if self.options.enable_viewer_osg else "OFF"
        tc.variables["ENABLE_WEBVIZ"]          = "ON" if self.options.enable_webviz else "OFF"
        tc.variables["ENABLE_WEBVIZ_FOXGLOVE"] = "ON" if self.options.enable_webviz_foxglove else "OFF"
        tc.variables["ENABLE_WEBVIZ_RERUN"]    = "ON" if self.options.enable_webviz_rerun else "OFF"
        tc.variables["ENABLE_EXPRTK"]          = "ON" if self.options.enable_exprtk else "OFF"
        tc.variables["ENABLE_SYMLINKS"]        = "ON" if self.options.enable_symlinks else "OFF"
        tc.variables["ENABLE_COMPLETIONS"]     = "ON" if self.options.enable_completions else "OFF"
        tc.variables["ENABLE_EXAMPLES"]        = "ON" if self.options.enable_examples else "OFF"
        tc.variables["ENABLE_TEST"]            = "ON" if self.options.enable_test else "OFF"
        tc.variables["ENABLE_TEST_WARN"]       = "ON" if self.options.enable_test_warn else "OFF"
        tc.variables["ENABLE_TEST_SANITIZE"]   = "ON" if self.options.enable_test_sanitize else "OFF"
        tc.variables["ENABLE_TEST_COVERAGE"]   = "ON" if self.options.enable_test_coverage else "OFF"
        tc.variables["SELECT_LOG_BACKEND"]     = str(self.options.select_log_backend)
        tc.variables["INSTALL_CONFIG_DIR"]     = str(self.options.install_config_dir)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "vlink")

        core_deps_available = not self.options.enable_cpm_all
        module_deps_available = not self.options.enable_cpm and not self.options.enable_cpm_all

        self.cpp_info.components["vlink"].libs = ["vlink"]
        if not self.options.shared:
            self.cpp_info.components["vlink"].defines = ["VLINK_LIBRARY_STATIC"]
        if core_deps_available:
            if self.options.enable_security:
                self.cpp_info.components["vlink"].requires.append("openssl::crypto")
            if self.options.enable_sqlite:
                self.cpp_info.components["vlink"].requires.append("sqlite3::sqlite")
            if self.options.enable_zstd:
                self.cpp_info.components["vlink"].requires.append("zstd::zstdlib")
        self.cpp_info.components["vlink"].set_property("cmake_target_name", "vlink::vlink")
        self.cpp_info.components["vlink"].set_property("cmake_target_aliases", ["vlink"])

        self.cpp_info.components["dds"].libs = ["vlink-dds"]
        self.cpp_info.components["dds"].requires = ["vlink"]
        if module_deps_available:
            self.cpp_info.components["dds"].requires.append("fast-dds::fastrtps")
        self.cpp_info.components["dds"].defines = ["VLINK_SUPPORT_DDS"]
        self.cpp_info.components["dds"].set_property("cmake_target_name", "vlink::dds")
        self.cpp_info.components["dds"].set_property("cmake_target_aliases", ["vlink-dds"])

        self.cpp_info.components["ddsc"].libs = ["vlink-ddsc"]
        self.cpp_info.components["ddsc"].requires = ["vlink"]
        if module_deps_available:
            self.cpp_info.components["ddsc"].requires.append("cyclonedds::CycloneDDS")
        self.cpp_info.components["ddsc"].defines = ["VLINK_SUPPORT_DDSC"]
        self.cpp_info.components["ddsc"].set_property("cmake_target_name", "vlink::ddsc")
        self.cpp_info.components["ddsc"].set_property("cmake_target_aliases", ["vlink-ddsc"])

        self.cpp_info.components["shm"].libs = ["vlink-shm"]
        self.cpp_info.components["shm"].requires = ["vlink"]
        if module_deps_available:
            self.cpp_info.components["shm"].requires.append("iceoryx::iceoryx_posh")
        self.cpp_info.components["shm"].defines = ["VLINK_SUPPORT_SHM"]
        self.cpp_info.components["shm"].set_property("cmake_target_name", "vlink::shm")
        self.cpp_info.components["shm"].set_property("cmake_target_aliases", ["vlink-shm"])

        self.cpp_info.components["intra"].libs = ["vlink-intra"]
        self.cpp_info.components["intra"].requires = ["vlink"]
        self.cpp_info.components["intra"].defines = ["VLINK_SUPPORT_INTRA"]
        self.cpp_info.components["intra"].set_property("cmake_target_name", "vlink::intra")
        self.cpp_info.components["intra"].set_property("cmake_target_aliases", ["vlink-intra"])

        if self.options.enable_c_api:
            self.cpp_info.components["c_api"].libs = ["vlink-c_api"]
            self.cpp_info.components["c_api"].requires = ["vlink", "dds", "ddsc", "shm", "intra"]
            if not self.options.shared:
                self.cpp_info.components["c_api"].defines = ["VLINK_C_API_LIBRARY_STATIC"]
            self.cpp_info.components["c_api"].set_property("cmake_target_name", "vlink::c_api")
            self.cpp_info.components["c_api"].set_property("cmake_target_aliases", ["vlink-c_api"])

        if self.options.enable_proxy:
            self.cpp_info.components["proxy_api"].libs = ["vlink-proxy_api"]
            self.cpp_info.components["proxy_api"].requires = ["vlink", "dds", "ddsc", "shm", "intra"]
            self.cpp_info.components["proxy_api"].defines = ["VLINK_ENABLE_PROXY"]
            if not self.options.shared:
                self.cpp_info.components["proxy_api"].defines.append("VLINK_PROXY_API_LIBRARY_STATIC")
            self.cpp_info.components["proxy_api"].set_property("cmake_target_name", "vlink::proxy_api")
            self.cpp_info.components["proxy_api"].set_property("cmake_target_aliases", ["vlink-proxy_api"])

            self.cpp_info.components["proxy_server"].libs = ["vlink-proxy_server"]
            self.cpp_info.components["proxy_server"].requires = ["vlink", "dds", "ddsc", "shm", "intra"]
            self.cpp_info.components["proxy_server"].defines = ["VLINK_ENABLE_PROXY"]
            if not self.options.shared:
                self.cpp_info.components["proxy_server"].defines.append("VLINK_PROXY_SERVER_LIBRARY_STATIC")
            self.cpp_info.components["proxy_server"].set_property("cmake_target_name", "vlink::proxy_server")
            self.cpp_info.components["proxy_server"].set_property("cmake_target_aliases", ["vlink-proxy_server"])

        if self.options.enable_exprtk and (
            self.options.enable_cli_dump
            or self.options.enable_viewer
            or self.options.enable_webviz
        ):
            self.cpp_info.components["exprtk_api"].libs = ["vlink-exprtk_api"]
            self.cpp_info.components["exprtk_api"].requires = ["vlink"]
            self.cpp_info.components["exprtk_api"].defines = ["VLINK_ENABLE_EXPRTK"]
            if not self.options.shared:
                self.cpp_info.components["exprtk_api"].defines.append("VLINK_EXPRTK_API_LIBRARY_STATIC")
            self.cpp_info.components["exprtk_api"].set_property("cmake_target_name", "vlink::exprtk_api")
            self.cpp_info.components["exprtk_api"].set_property("cmake_target_aliases", ["vlink-exprtk_api"])

        self.cpp_info.components["all"].libs = []
        self.cpp_info.components["all"].requires = ["vlink", "dds", "ddsc", "shm", "intra"]
        self.cpp_info.components["all"].set_property("cmake_target_name", "vlink::all")
        self.cpp_info.components["all"].set_property("cmake_target_aliases", ["vlink-all"])
