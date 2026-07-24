# Discussion GitHub 操作

只执行当前任务需要的命令。任何写入仍须满足主 `SKILL.md` 的单次授权
边界。

## 搜索与分类

确认 GitHub 登录和仓库,查询分类及最近的 Discussion:

```bash
gh auth status
gh repo view --json nameWithOwner
gh api graphql -f query='
  query {
    repository(owner: "thun-res", name: "vlink") {
      id
      discussionCategories(first: 50) {
        nodes { id name slug description isAnswerable }
      }
      discussions(first: 100, orderBy: {field: UPDATED_AT, direction: DESC}) {
        nodes { number title bodyText url category { name slug } }
        pageInfo { hasNextPage endCursor }
      }
    }
  }'
```

存在下一页时用 `after` 游标继续查询,再以主题关键词搜索 open/closed
Issue。

## 读取完整上下文

先由 Discussion 编号取得 `id`,并保留每条评论的 `id` 和 `url`:

```bash
gh api graphql -F number=<编号> -f query='
  query($number: Int!) {
    repository(owner: "thun-res", name: "vlink") {
      discussion(number: $number) {
        id number url title body closed category { name isAnswerable }
        comments(first: 100) {
          nodes {
            id url body createdAt author { login }
            replies(first: 100) {
              nodes {
                id url body createdAt author { login }
                replyTo { id url }
              }
              pageInfo { hasNextPage endCursor }
            }
          }
          pageInfo { hasNextPage endCursor }
        }
      }
    }
  }'
```

任一连接存在下一页时使用对应 `after` 游标继续读取。

## 创建并回读

正文写入仓库外的 `mktemp` 临时文件。从查询结果取得 repository ID 和
分类 ID 后执行:

```bash
gh api graphql \
  -f query='
    mutation($repositoryId: ID!, $categoryId: ID!, $title: String!, $body: String!) {
      createDiscussion(input: {
        repositoryId: $repositoryId,
        categoryId: $categoryId,
        title: $title,
        body: $body
      }) {
        discussion { number url title category { name } }
      }
    }' \
  -F repositoryId="<repository-id>" \
  -F categoryId="<category-id>" \
  -f title="<标题>" \
  -F body=@"<临时正文文件>"
```

删除临时文件,再按返回编号回读:

```bash
gh api graphql -F number=<编号> -f query='
  query($number: Int!) {
    repository(owner: "thun-res", name: "vlink") {
      discussion(number: $number) {
        number url title body category { name } closed
      }
    }
  }'
```

## 发布回复并回读

回复正文写入仓库外的 `mktemp` 临时文件。顶层评论省略 `replyToId`;
回复指定评论时传入已核实的 `replyToId`:

```bash
gh api graphql \
  -f query='
    mutation(
      $discussionId: ID!,
      $replyToId: ID,
      $body: String!
    ) {
      addDiscussionComment(input: {
        discussionId: $discussionId,
        replyToId: $replyToId,
        body: $body
      }) {
        comment {
          id url body createdAt
          author { login }
          replyTo { id url }
        }
      }
    }' \
  -F discussionId="<discussion-id>" \
  -F replyToId="<目标-comment-id>" \
  -F body=@"<临时正文文件>"
```

发布顶层评论时不传 `-F replyToId=...`。保存响应中的 comment `id` 和
`url`,删除临时文件,再按返回 ID 回读:

```bash
gh api graphql -F id="<comment-id>" -f query='
  query($id: ID!) {
    node(id: $id) {
      ... on DiscussionComment {
        id url body createdAt
        author { login }
        discussion { number url }
        replyTo { id url }
      }
    }
  }'
```
