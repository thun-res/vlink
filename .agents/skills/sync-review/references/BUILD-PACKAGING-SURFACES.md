# 构建、安装与打包同步

从公开目标、源文件、功能开关或依赖变化出发,同时核对所有适用构建系统、
安装导出、包管理清单、平台入口和发布消费者。不同入口允许有明确的能力
子集,但差异必须有平台或交付场景依据,不得因长期未同步而静默缺失。

## 1. CMake、安装与导出

- 核对根及子目录 `CMakeLists.txt`、`cmake/**/*.cmake` 与模板中的 target、
  source、include、definition、dependency、option、默认值和平台门控。
- 新增、删除或改名库、CLI、模块、绑定、插件及示例时,同步构建接线、
  install component、export set、别名和 `find_package` 消费名称。
- `cmake/config.cmake.in`、`module.cmake.in`、`package.cmake`、许可证
  聚合、环境模板及卸载入口必须对应真实安装树和依赖传播。
- C++17 基线、高版本门控、共享/静态库定义和 Windows、Linux、macOS、
  QNX、Android 分支不得在不同入口中无依据分叉。

## 2. Conan 与 vcpkg

- `conanfile.py` 的 `settings`、`options`、`default_options` 与 CMake cache
  变量逐项对应;新增功能开关同时核对 `generate()` 的 toolchain 映射。
- `requirements()` 的条件依赖与 CPM/system dependency 分支一致;
  `package()` 的安装结果和 `package_info()` 的 component、library、
  definition、requirement、CMake target/alias 与实际导出一致。
- Conan 收集的运行库、许可证、平台目录和 `packup/build-conan.*` 的
  layout、输入参数及产物路径相互对应。
- `tools/vcpkg/vcpkg.json` 的 dependency、feature、host tool、supports
  与 CMake 功能和模块能力一致;baseline 变化单独说明依据。
- 版本只做一致性审计;未经维护者明确命令不得修改 `version.txt`、
  Conan/vcpkg 版本镜像或其他版本载体。

## 3. Android Soong

- 同时核对根 `Android.bp` 与 `examples/samples/Android.bp` 中的
  `cc_defaults`、library、binary、source、include、export、flag、
  shared/static dependency 和 C++ 标准。
- 核心源文件、模块、C API、Proxy、CLI 或示例变化时,检查对应 Soong
  target 是否需要增删或改名;生成/builtin 源路径必须真实存在。
- 将 Soong 与 CMake 的目标及宏按 Android 实际支持范围对照。能力子集是
  有意设计时记录不适用理由,不得机械补齐桌面、QNX 或不受支持的后端。

## 4. `packup/**` 与平台资源

- 核对 `build-conan.sh/.bat`、`build-deb.sh`、`build-rpm.sh`、
  `build-arch.sh` 的参数、环境变量、CMake 选项、依赖、架构、构建目录、
  install component、输出目录、文件名和扩展名。
- `cmake/package.cmake` 的 CPack generator、包拆分、组件依赖、系统依赖、
  license、prefix、libdir 和文件名必须与上述脚本及 `PKGINFO.in` 一致。
- Linux/macOS/Windows 的 install、uninstall、runtime setup、launcher、
  `qt.conf`、desktop/MIME/icon 和 Qt Installer Framework XML/QScript
  资源必须与真实安装树、可执行文件及组件名称一致。
- completion、环境设置模板和 platform toolchain 中的路径、变量、目标
  架构与 `doc/01`、`doc/13` 的用户入口一致。
- `packup/patch/*` 的依赖版本、应用入口、NOTICE/许可证与 CPM、Conan、
  CI cache key 和聚合许可证保持对应;未消费的补丁或资源必须报告。

## 5. CI、发布与文档消费者

- `.github/scripts/release-*`、runtime closure/staging 脚本与 release
  workflows 消费的构建目录、glob、artifact、checksum 和架构矩阵必须
  与打包生产者完全一致。
- `doc/01-started.md` 的构建、Conan、交叉编译、安装和发行包说明是主要
  用户文档;环境变量同步 `doc/13`,速查入口同步 `doc/14`。
- 用户可见 target、选项、依赖、包名、安装布局或支持平台变化时,再核对
  README、示例、CHANGELOG 和 `doc/15` 的适用说明。

## 6. 验证边界

- 静态检查每个路径、target、option、component、dependency、artifact
  和变量的生产者与消费者;区分有依据的平台差异和同步遗漏。
- 只有维护者明确授权时才执行 CMake/Conan/Soong 构建、CPack、安装器、
  包管理器或 release workflow;未运行不得写成动态通过。本地编译必须
  转用对应构建 skill,并遵守 `max(真实物理核心数 - 1, 1)` 的显式并行
  上限,不得在同步审计中另起无约束构建。
- 验证不得升级 dependency、baseline、toolchain、action、镜像或项目
  版本,也不得修改门禁、排除项或打包内容来掩盖失败。
