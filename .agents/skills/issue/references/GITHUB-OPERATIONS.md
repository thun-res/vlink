# Issue GitHub 操作

只执行当前任务需要的命令。任何写入仍须满足主 `SKILL.md` 的单次授权
边界。

## 搜索与读取

确认 GitHub 登录和仓库,搜索 open 与 closed Issue:

```bash
gh auth status
gh repo view --json nameWithOwner
gh issue list --repo thun-res/vlink --state all --limit 100 \
  --search "<组件名 关键符号 错误文本> in:title,body"
gh issue view <候选编号> --repo thun-res/vlink --comments
```

草拟或发布回复前读取目标 Issue 和全部评论:

```bash
gh api "repos/thun-res/vlink/issues/<编号>"
gh api --paginate "repos/thun-res/vlink/issues/<编号>/comments?per_page=100"
```

## 创建并回读

正文写入仓库外的 `mktemp` 临时文件。根据模板类型执行:

```bash
gh issue create --repo thun-res/vlink \
  --title "[Bug] <标题>" --body-file "<临时文件>" --label bug

gh issue create --repo thun-res/vlink \
  --title "[Feature] <标题>" --body-file "<临时文件>" --label enhancement
```

若对应 label 不存在,省略 `--label`,不得创建新 label。删除临时文件,
再按返回编号回读:

```bash
gh issue view <编号> --repo thun-res/vlink \
  --json number,url,title,state,body,labels
```

## 发布回复并回读

回复正文写入仓库外的 `mktemp` 临时文件,复查目标编号与正文后发布:

```bash
gh api --method POST \
  "repos/thun-res/vlink/issues/<编号>/comments" \
  -F body=@"<临时正文文件>"
```

保存响应中的 `id` 和 `html_url`,删除临时文件,再按返回 ID 回读:

```bash
gh api "repos/thun-res/vlink/issues/comments/<comment-id>" \
  --jq '{id,html_url,body,user:.user.login,created_at}'
```
