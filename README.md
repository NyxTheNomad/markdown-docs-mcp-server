# Markdown Docs MCP Server

A small, read-only Model Context Protocol server that turns any directory of Markdown files into a development knowledge base for coding agents. Built with C++20 and [mcp-cpp](https://github.com/Neumann-Labs/mcp-cpp).

文档由使用者自行维护。Server 不绑定任何具体框架，也不上传或修改你的文档；只需通过 `--docs` 指向 Markdown 根目录，即可让 MCP Agent 查询开发规范、API 用法和代码示例。

## Features

- Recursively indexes `.md` and `.markdown` files.
- Literal search with file paths, line numbers and snippets.
- Reads selected line ranges instead of flooding the model context.
- Supports Chinese text and case-insensitive ASCII search.
- Refreshes the in-memory index without restarting the process.
- Exposes both MCP tools and `docs:///{path}` resources.
- Includes a `develop-with-docs` prompt for documentation-first development.
- Never writes to the document directory.

## MCP capabilities

| Name | Type | Description |
| --- | --- | --- |
| `docs_list` | Tool | List indexed documents; optionally filter by path prefix |
| `docs_search` | Tool | Search all documents and return `path:line` matches |
| `docs_read` | Tool | Read a bounded range from one document |
| `docs_refresh` | Tool | Rebuild the index after documents change |
| `docs:///{path}` | Resource | Read an indexed Markdown file as an MCP resource |
| `develop-with-docs` | Prompt | Guide an agent to consult documentation before coding |

## Build

Requirements:

- macOS or Linux
- CMake 3.20+
- A C++20 compiler
- Git (CMake downloads the pinned `mcp-cpp` release)
- Optional: Ninja and Python 3

```bash
git clone https://github.com/NyxTheNomad/markdown-docs-mcp-server.git
cd markdown-docs-mcp-server
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The binary is generated at `build/markdown_docs_mcp_server`.

For offline development with an existing `mcp-cpp` checkout:

```bash
cmake -S . -B build \
  -DMCP_CPP_SOURCE_DIR=/absolute/path/to/mcp-cpp \
  -DMARKDOWN_DOCS_MCP_USE_SYSTEM_DEPS=ON
```

## Run with your own documents

Assume your documentation is organized like this:

```text
SF-Dev-Doc/
├── framework/
│   ├── VLstring.md
│   └── memory.md
├── coding-style/
│   └── cpp-style.md
├── architecture/
│   └── modules.md
└── examples/
    └── network-client.md
```

Start the server with the root directory:

```bash
./build/markdown_docs_mcp_server --docs /absolute/path/to/SF-Dev-Doc
```

The server communicates over stdio, so it is normally started by an MCP host rather than used as an interactive terminal program.

## Configure an MCP host

The exact settings screen or configuration filename depends on the host. The process configuration is equivalent to:

```json
{
  "mcpServers": {
    "development-docs": {
      "command": "/absolute/path/to/markdown_docs_mcp_server",
      "args": ["--docs", "/absolute/path/to/SF-Dev-Doc"]
    }
  }
}
```

Use absolute paths because an MCP host may start the process from a different working directory.

## Example: asking about `VLstring`

The agent can discover and read a custom framework API in several steps:

```text
docs_search({"query":"VLstring","limit":20})
docs_read({"path":"framework/VLstring.md","start_line":1,"max_lines":200})
docs_search({"query":"VLstring::Format"})
```

For reliable answers, the source Markdown should contain real header names, method signatures, examples, ownership rules, encoding behavior and prohibited usages. Saying only “VLstring is similar to CString” is not enough for an agent to safely infer its API.

## Recommended agent policy

Add a rule like this to your repository-level agent instructions:

```markdown
Before changing code, query the development-docs MCP server.

1. Search using the task, module and API names.
2. Read the complete sections around relevant matches.
3. List applicable rules with `path:line` references before implementation.
4. Never invent undocumented framework APIs.
5. Report documentation compliance and test results at the end.
```

MCP supplies context and workflow guidance; critical rules should still be enforced with formatters, linters, compilation checks and CI.

## Safety and limits

- The document directory is read-only.
- Symbolic links are skipped.
- Files larger than 2 MiB are skipped.
- `docs_read` returns at most 500 lines per call.
- The current search is literal and line-based. For thousands of documents, the repository layer can be replaced with SQLite FTS5, BM25 or vector search without changing the MCP interface.

More details are available in [docs/Markdown文档Server使用说明.md](docs/Markdown文档Server使用说明.md).

## License

MIT
