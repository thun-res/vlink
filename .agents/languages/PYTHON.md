# Python 编码规范

适用于 `languages/python_api/`、`conanfile.py`、仓库工具脚本和测试。人工整改
不触及第三方、预构建、构建目录与生成文件;当前 `/format` 不格式化
Python 文件。没有仓库配置支持的外部风格规则不得自行引入;先保持相邻
文件与 nanobind 导出层的一致性。

## 1. 基本风格

- 使用 4 空格缩进、UTF-8、`snake_case` 函数/变量、`PascalCase`
  类型、`UPPER_SNAKE_CASE` 常量。
- 导入顺序为标准库、第三方、本项目,分组间空一行。通配导入只允许
  `languages/python_api/vlink.py` 这种明确的绑定转发层,并保留 lint 说明。
- 公共入口提供简短 docstring;注释解释约束和原因,不逐行翻译代码。
- 类型标注跟随相邻公开 API;不得为了补 typing 引入运行时依赖或改变
  Python 兼容版本。
- 作用域内拥有且支持上下文协议的文件、进程和临时资源使用上下文
  管理器;跨作用域或长生命周期资源明确关闭责任。子进程必须检查返回码,
  路径优先使用 `pathlib.Path`。测试若专门断言非零状态,必须显式检查
  `returncode`,而不是设置 `check=True`。
- secret 不写入日志。既有环境变量的空白处理、TLS 参数和错误策略属于
  对外行为,不得在风格整改中改变。
- `conanfile.py` 保持 Conan 2 API、settings/options/default_options 与
  CMake 选项映射同步;新增依赖时同步锁定策略和打包验证。

## 2. VLink 绑定约束

- nanobind 新增/删除/改名后,同批同步 `vlink.py` 的显式导入、现有
  `__all__`(含可选符号的动态插入)、Python examples、测试与用户文档。
- 可选符号只捕获预期异常,例如缺少可选绑定使用 `ImportError`;不得用
  宽泛 `except Exception` 隐藏 ABI 或初始化错误。
- 保持 C++ 所有权、buffer 生命周期、线程回调与 GIL 语义;Python 包装
  不得引入隐藏深拷贝或改变 zerocopy 布局。
- nanobind 回调进入 Python 前必须持有 GIL;释放 GIL 的阻塞调用不得在
  无保护状态访问 Python 对象。返回 buffer/view 时明确 owner,禁止返回
  指向临时 C++ 对象的视图。
- 用户可见异常要包含操作和关键上下文,不得静默回退到不同后端。

## 3. 测试与兼容

- 测试独立、可重复,显式设置和恢复环境;临时文件优先使用 `tempfile`
  或相邻现有 helper。确需引入 pytest 时先同步依赖与测试入口。
- 浮点、异步和超时断言使用有依据的容差,不得用无界 sleep 掩盖竞态。
- 保持 CI 支持的 Python 版本语法兼容;引入新依赖前先确认构建镜像与
  package 配置。
- 只有维护者明确要求时运行 Python 测试;纯语法检查可使用
  `python -m py_compile`。AI 启动任何 Python 进程前必须设置
  `PYTHONPYCACHEPREFIX={project}/build-ai/<task_name>/__pycache__`。
  禁止在 `build-ai` 外生成、保留或迁移 `__pycache__`。
