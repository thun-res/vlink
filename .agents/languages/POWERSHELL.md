# PowerShell 编码规范

适用于仓库内 `.ps1` 与 `.ps1.in`,尤其是 `.github/scripts/`。保持相邻
入口的参数、环境变量、输出和退出码接口,不得用版式整改改变工作流语义,
也不得顺手增加缓存协议或完整性门禁。

## 1. 文件与命名

- 人工整改不触及第三方、预构建、构建目录与生成文件;显式调用
  `/format` 时仅按该 skill 和 `tools/format.sh` 的既定范围统一行尾。
- 按 `.gitattributes` 保持 CRLF 行尾;兼容范围以实际调用方为准,
  `.github` CI 脚本兼容对应 workflow 使用的 `pwsh`。
- 注释沿用当前文件;风格整改不新增或改写说明性注释。
- 使用 2 空格缩进;新增函数优先使用 `Verb-Noun`,变量命名跟随相邻
  文件,环境变量通过 `$env:NAME` 访问。
- 新入口用 `param()` 声明参数;只有原入口已经按 `$PSCommandPath`
  解析路径时才保持 cwd-independent,不得以风格整改改变现有工作目录
  契约。

## 2. 错误与资源

- 入口设置 `$ErrorActionPreference = "Stop"`;会返回退出码的外部命令
  之后立即检查 `$LASTEXITCODE`,不能只依赖异常。
- 新增目录切换时使用 `Push-Location`/`Pop-Location` 和
  `try`/`finally`;原脚本依赖仓库根工作目录时不因风格整改改变接口。
- 捕获异常时保留原始上下文;只捕获能够处理的异常,不得用空
  `catch` 把失败伪装成成功。

## 3. 参数、编码与安全

- 路径使用 `Join-Path`;调用外部程序使用调用运算符 `&` 和参数数组,
  不拼接后交给 `Invoke-Expression`。
- 新增配置写入时显式选择编码;既有读写流程没有编码故障时不整体重写。
- secret 不写入日志或命令行回显;下载校验沿用现有发布流程,不以风格
  整改为由自行引入固定摘要或新的缓存 marker。
- 修改后至少做 PowerShell parser 语法检查;维护者要求行为验证时,再用
  临时目录与 mock/stub 覆盖受影响路径。缺少 PowerShell/pwsh 时明确
  记录未执行,最终由 Windows runner 验证。
