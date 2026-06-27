# vlink 仓库每日自动更新（systemd）

每天凌晨 0 点自动把 vlink 仓库强制同步到远端最新（`git fetch --all` + `git reset --hard`）。
采用 systemd **service + timer** 标准组合：service 负责执行更新动作，timer 负责定时触发。

> ⚠️ **强制同步会丢弃本地改动**：脚本对目标分支执行 `git reset --hard origin/<分支>`，
> 任何未推送的本地提交、未提交的工作区改动都会被**永久丢弃**。请仅在用作「只读部署副本」
> 的机器上启用，切勿在日常开发机上启用。

所有路径均不写死：`.service` / `.timer` 中用 `@REPO_DIR@` 占位，由 `install.sh`
在安装时根据仓库实际所在位置自动填充，因此源码部署在目标机器的任意路径都可用。

## 文件说明

| 文件 | 作用 |
| --- | --- |
| `vlink-repo-update.sh` | 更新脚本：`git fetch --all` + `checkout -f` + `git reset --hard origin/<分支>`（自动定位所在仓库根） |
| `vlink-repo-update.service` | oneshot 服务模板，调用上面的脚本 |
| `vlink-repo-update.timer` | 定时器模板，每天 `00:00` 触发 service |
| `install.sh` | 安装：解析真实路径、填充模板、写入 systemd、enable 定时器 |
| `uninstall.sh` | 卸载：停用定时器并删除已安装的 unit 文件 |

## 前置条件

目标机器需满足（否则更新会失败并被 systemd 标记 failed）：

- 已安装 `git`（脚本用 `command -v git` 查找，不限定路径）。
- 仓库存在名为 `origin` 的远端，且 `origin/<分支>` 可访问。
- 运行账号对仓库目录有读写权限。
- 该仓库为只读部署副本：脚本会 `checkout -f` 到目标分支并 `reset --hard`，
  本地任何改动/提交都会被丢弃（见顶部警告）。

## 安装（系统级，推荐）

在**目标机器**上，把仓库放到任意目录后，进入本目录执行：

```bash
./install.sh
```

它会自动：解析仓库根路径 → 填充 unit 模板 → **以仓库属主身份运行**（自动 `stat`
仓库 owner/group 写入 `User=`/`Group=`，避免 root 跑 git 触发 dubious-ownership 或
污染仓库文件属主）→ 写入 `/etc/systemd/system/` → `daemon-reload` → `enable --now` timer。
安装到 `/etc/*` 需要 root，非 root 时脚本自动用 `sudo`。

## 安装（用户级，免 root）

```bash
UNIT_DIR="$HOME/.config/systemd/user" ./install.sh
```

此模式下 `install.sh` 自动改用 `systemctl --user`，service 以当前用户运行（不注入 `User=`）。
**注意**：用户级 timer 在该用户未登录时不会触发，需开启 linger：

```bash
sudo loginctl enable-linger "$USER"
```

## 凭据与安全

`git fetch` 使用仓库 `origin` 现有的认证方式。请勿使用 **URL 内嵌 token** 的 https remote
（如 `https://<token>@github.com/...`）：token 会随 `.git/config` 散布到每台机器、可能出现在
journald 日志中，且 token 过期后每晚更新会静默失败。推荐使用**只读 SSH deploy key**
或 git credential helper。

## 卸载

```bash
./uninstall.sh
# 用户级安装则： UNIT_DIR="$HOME/.config/systemd/user" ./uninstall.sh
```

## 常用命令

```bash
# 查看下次触发时间
systemctl list-timers vlink-repo-update.timer

# 立即手动跑一次更新（不影响定时计划），首次安装当晚 0 点才会自动首跑，想立即验证用这条
sudo systemctl start vlink-repo-update.service

# 查看 service 状态 / 是否失败
systemctl status vlink-repo-update.service
systemctl is-failed vlink-repo-update.service

# 查看更新日志（脚本所有输出都进 journal）
journalctl -u vlink-repo-update.service -n 50 --no-pager
```

## 改时间 / 改分支

- 改分支：编辑 `.service` 的 `Environment=VLINK_REPO_BRANCH=`，重跑 `./install.sh`。
- 改时间：编辑 `.timer` 的 `OnCalendar=`，重跑 `./install.sh`。例如：
  - `*-*-* 00:00:00`  每天 0 点（默认）
  - `*-*-* 03:30:00`  每天凌晨 3:30
  - `Mon *-*-* 02:00:00`  每周一凌晨 2 点

可用 `systemd-analyze calendar '*-*-* 00:00:00'` 验证表达式与下次触发时间。
直接改 `/etc/systemd/system/` 下的安装件会被下次 `install.sh` 覆盖；临时调整可用
`systemctl edit vlink-repo-update.timer` 做 drop-in。
