# Markdown 开发文档 MCP Server

`markdown_docs_mcp_server` 将一个已有的 Markdown 目录转换为只读 MCP 知识库。它不复制也不修改源文档，启动参数直接指向文档根目录。

## 1. 启动方式

```bash
/absolute/path/to/build/markdown_docs_mcp_server \
  --docs /absolute/path/to/your/development-docs
```

Server 会递归加载 `.md` 和 `.markdown`。文档更新后不需要重启，可以让 Agent 调用 `docs_refresh`。

## 2. 暴露给 Agent 的能力

| 名称 | 类型 | 用途 |
| --- | --- | --- |
| `docs_list` | Tool | 了解文档目录结构，可按相对路径前缀过滤 |
| `docs_search` | Tool | 逐行检索关键词，返回文件、行号和 snippet |
| `docs_read` | Tool | 按路径、起始行、最大行数读取完整上下文 |
| `docs_refresh` | Tool | 文档变更后重新建立内存索引 |
| `docs:///{path}` | Resource template | 将任意已索引文档作为 Markdown Resource 读取 |
| `develop-with-docs` | Prompt | 生成“先查规范、再编码、最后给遵循清单”的任务消息 |

推荐调用链：

```text
开发任务
   ↓
docs_list（了解有哪些规范）
   ↓
docs_search（任务词、模块名、必须/禁止/规范）
   ↓
docs_read（读取命中位置的完整上下文）
   ↓
整理 path:line 约束清单
   ↓
实现 + 测试 + 文档遵循报告
```

## 3. MCP Host 配置

不同 Host 的配置入口不同，核心都是让 Host 启动这个二进制并传入文档目录。JSON 风格的示意配置如下：

```json
{
  "mcpServers": {
    "development-docs": {
      "command": "/absolute/path/to/markdown-docs-mcp-server/build/markdown_docs_mcp_server",
      "args": [
        "--docs",
        "/absolute/path/to/your/development-docs"
      ]
    }
  }
}
```

路径应使用绝对路径，因为 MCP Host 启动子进程时的工作目录通常不是本项目目录。

连接后可以对 Agent 说：

> 使用 develop-with-docs 流程完成登录模块的错误处理；实现前先查询相关开发文档，并在最终答复中列出引用的 path:line。

## 4. 建议添加到项目级 Agent 指令

仅仅“提供 MCP 工具”不能从技术上强制模型每次都调用它。若希望形成稳定流程，应在项目的 Agent 指令文件或系统提示中加入类似规则：

```markdown
## 开发文档规范

修改代码前必须使用 development-docs MCP：

1. 调用 docs_search 检索任务、模块及相关规范关键词。
2. 调用 docs_read 阅读命中段落的完整上下文。
3. 实现前列出适用约束及来源 path:line。
4. 文档冲突或没有覆盖时必须明确说明，不得虚构规范。
5. 最终回复包含文档遵循清单和测试结果。
```

如果某些规则必须被严格强制，例如格式、依赖边界、禁止 API、安全检查，还应将其实现成 formatter、linter、编译检查或 CI gate。MCP 提供上下文和工作流引导，但不能替代确定性的工程检查。

## 5. 检索行为与适用规模

当前版本采用内存索引和逐行字面检索：

- 中文按原文子串匹配。
- 英文字母不区分 ASCII 大小写。
- 每条命中返回 `path`、`line`、`snippet` 和资源 URI。
- `docs_read` 默认读取 200 行，最多读取 500 行，避免一次塞入过多上下文。
- 单文件最大 2 MiB；符号链接会跳过，避免意外读取文档根目录之外的文件。
- 工具只读，不提供创建、更新或删除文档的能力。

这适合几十到数百篇以规范和 API 说明为主的文档。如果文档达到数千篇、存在大量同义词，或需要按语义而非原文搜索，可在保持 MCP API 不变的前提下，把 `DocsRepository::search()` 替换为 SQLite FTS5、BM25 或向量检索。

## 6. 文档组织建议

为了让 Agent 更稳定地检索到规则：

- 一个文件聚焦一个主题，例如 `cpp-style.md`、`error-handling.md`、`database.md`。
- 标题和正文使用项目中的真实模块名、类型名与术语。
- 明确使用“必须”“禁止”“推荐”“例外”等级词。
- 规则同时给出正例、反例和适用范围。
- 重大规则避免只藏在长篇背景说明里。
- 对冲突规则标注优先级和生效版本。

示例：

```markdown
# 错误处理规范

适用范围：所有 C++ service target。

## 必须

- 可恢复的业务错误必须返回 `Result<T>`。
- 日志必须包含 request id，不得记录 token。

## 禁止

- 禁止在库代码中调用 `std::exit()`。

## 例外

- 进程启动阶段遇到不可恢复的配置错误，可以返回非零退出码。
```

## 7. 验证 Server

项目自带真实 stdio JSON-RPC 测试：

```bash
ctest --test-dir build --output-on-failure
```

只运行文档 Server 测试：

```bash
python3 scripts/docs_server_smoke_test.py \
  build/markdown_docs_mcp_server sample_docs
```

测试覆盖初始化、列出工具、列文档、检索、分段读取、Resource 读取和 Prompt 获取。
