# 定向 suite 与 case

## 列举与选择

`test/CMakeLists.txt` 将每个 `TEST_SUITE(...)` 注册成同名 CTest 用例。
先确认构建产物中的真实名称:

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR_REL=build-ai/skill_test
BUILD_DIR="$REPO_ROOT/$BUILD_DIR_REL"
ctest --test-dir "$BUILD_DIR" --show-only
```

精确运行一个或一组 CTest suite:

```bash
ctest --test-dir "$BUILD_DIR" --output-on-failure \
  --timeout 180 --parallel 1 --tests-regex '<suite-regex>'
```

按 doctest case 名、suite 通配符或排除 suite:

```bash
"$BUILD_DIR/output/bin/vlink-test" --test-suite="dds-*"
"$BUILD_DIR/output/bin/vlink-test" --test-case="*round*"
"$BUILD_DIR/output/bin/vlink-test" -tse="someip-*"
```

Windows 程序位于 `$BuildDir/output/bin/vlink-test.exe`,参数一致。

## 运行环境

- 运行前按平台 runner 设置动态库搜索路径和
  `VLINK_DDS_IP=127.0.0.1`。
- 涉及传输、Proxy 或跨进程行为时,按平台 runner 的参数启动
  `vlink-proxy`,确认未提前退出,测试结束后清理。
- CTest `--tests-regex` 适合 suite 过滤,可保留 CMake 注册的环境和资源
  锁;`--test-case` 只能直接运行测试程序。
- 直接运行测试程序不具备平台 runner 的单 suite 超时、完整排除项和
  Proxy 生命周期保障,必须在结果中说明。
- SOME/IP 测试依赖外部守护进程。缺失时可以按用户要求排除,但必须报告
  未覆盖范围。
- 先检查匹配数量;零用例不得判定为通过。定向运行通过只代表实际匹配的
  用例通过。
