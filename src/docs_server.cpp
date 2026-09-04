// Read-only Markdown knowledge-base MCP server built with mcp-cpp.
//
// The server indexes *.md / *.markdown files under --docs and exposes them as
// tools plus docs:// resources. stdout is protocol-only; diagnostics use stderr.

#include <mcp/mcp.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::uintmax_t kMaxDocumentBytes = 2U * 1024U * 1024U;
constexpr std::string_view kDocsUriPrefix = "docs:///";

struct Document {
    std::string path;
    std::string content;
    std::vector<std::string> lines;
};

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool is_markdown(const fs::path& path) {
    const std::string extension = ascii_lower(path.extension().string());
    return extension == ".md" || extension == ".markdown";
}

std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream input{content};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string shorten(std::string text, std::size_t max_bytes = 300) {
    if (text.size() <= max_bytes) {
        return text;
    }
    text.resize(max_bytes);
    return text + "...";
}

bool is_unreserved(unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~' ||
           ch == '/';
}

std::string uri_encode_path(std::string_view path) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char ch : path) {
        if (is_unreserved(ch)) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[(ch >> 4U) & 0x0FU]);
            encoded.push_back(kHex[ch & 0x0FU]);
        }
    }
    return encoded;
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::optional<std::string> uri_decode_path(std::string_view encoded) {
    std::string decoded;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '%') {
            decoded.push_back(encoded[i]);
            continue;
        }
        if (i + 2 >= encoded.size()) return std::nullopt;
        const int high = hex_value(encoded[i + 1]);
        const int low = hex_value(encoded[i + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return decoded;
}

std::string normalize_relative_path(const std::string& raw) {
    const fs::path path{raw};
    if (path.empty() || path.is_absolute()) return {};
    const std::string normalized = path.lexically_normal().generic_string();
    if (normalized == "." || normalized == ".." || normalized.starts_with("../")) {
        return {};
    }
    return normalized;
}

mcp::CallToolResult error_result(std::string message) {
    return {
        .content = {mcp::TextContent{.text = std::move(message)}},
        .is_error = true,
    };
}

mcp::CallToolResult json_result(std::string summary, json structured) {
    return {
        .content = {mcp::TextContent{.text = std::move(summary)}},
        .structured_content = std::move(structured),
    };
}

std::optional<std::size_t> bounded_integer(const json& args,
                                           const char* name,
                                           std::size_t default_value,
                                           std::size_t max_value) {
    if (!args.contains(name)) return default_value;
    if (!args[name].is_number_integer()) return std::nullopt;
    if (args[name].is_number_unsigned()) {
        const auto value = args[name].get<unsigned long long>();
        if (value < 1 || value > max_value) return std::nullopt;
        return static_cast<std::size_t>(value);
    }
    const auto value = args[name].get<long long>();
    if (value < 1 || static_cast<unsigned long long>(value) > max_value) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

class DocsRepository {
public:
    explicit DocsRepository(fs::path root) {
        std::error_code error;
        root_ = fs::canonical(std::move(root), error);
        if (error || !fs::is_directory(root_)) {
            throw std::runtime_error("文档目录不存在或不可读: " + root_.string());
        }
        reload();
    }

    std::size_t reload() {
        std::vector<Document> updated;
        std::error_code iterator_error;
        fs::recursive_directory_iterator iterator{
            root_, fs::directory_options::skip_permission_denied, iterator_error};
        const fs::recursive_directory_iterator end;

        while (!iterator_error && iterator != end) {
            const fs::directory_entry entry = *iterator;
            iterator.increment(iterator_error);

            std::error_code entry_error;
            if (entry.is_symlink(entry_error) || !entry.is_regular_file(entry_error) ||
                !is_markdown(entry.path())) {
                continue;
            }
            const auto file_size = entry.file_size(entry_error);
            if (entry_error || file_size > kMaxDocumentBytes) {
                std::cerr << "skip oversized/unreadable document: " << entry.path() << '\n';
                continue;
            }

            const fs::path relative = fs::relative(entry.path(), root_, entry_error);
            if (entry_error) continue;

            std::ifstream input{entry.path(), std::ios::binary};
            if (!input) continue;
            std::string content{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
            updated.push_back(Document{
                .path = relative.generic_string(),
                .content = content,
                .lines = split_lines(content),
            });
        }

        std::sort(updated.begin(), updated.end(), [](const Document& left, const Document& right) {
            return left.path < right.path;
        });
        const std::size_t count = updated.size();
        {
            std::lock_guard lock{mutex_};
            documents_ = std::move(updated);
        }
        return count;
    }

    [[nodiscard]] std::string root_string() const { return root_.string(); }

    json list(const std::string& prefix) const {
        json documents = json::array();
        std::lock_guard lock{mutex_};
        for (const Document& document : documents_) {
            if (!prefix.empty() && !document.path.starts_with(prefix)) continue;
            documents.push_back({
                {"path", document.path},
                {"uri", std::string{kDocsUriPrefix} + uri_encode_path(document.path)},
                {"bytes", document.content.size()},
                {"lines", document.lines.size()},
            });
        }
        return {{"root", root_.string()}, {"documents", std::move(documents)}};
    }

    json search(const std::string& query, std::size_t limit) const {
        const std::string needle = ascii_lower(query);
        json matches = json::array();
        std::lock_guard lock{mutex_};
        for (const Document& document : documents_) {
            for (std::size_t i = 0; i < document.lines.size(); ++i) {
                if (ascii_lower(document.lines[i]).find(needle) == std::string::npos) continue;
                matches.push_back({
                    {"path", document.path},
                    {"uri", std::string{kDocsUriPrefix} + uri_encode_path(document.path)},
                    {"line", i + 1},
                    {"snippet", shorten(document.lines[i])},
                });
                if (matches.size() >= limit) {
                    return {{"query", query}, {"matches", std::move(matches)}, {"truncated", true}};
                }
            }
        }
        return {{"query", query}, {"matches", std::move(matches)}, {"truncated", false}};
    }

    std::optional<json> read(const std::string& raw_path,
                             std::size_t start_line,
                             std::size_t max_lines) const {
        const std::string path = normalize_relative_path(raw_path);
        if (path.empty()) return std::nullopt;

        std::lock_guard lock{mutex_};
        const auto found = std::find_if(documents_.begin(), documents_.end(), [&](const Document& doc) {
            return doc.path == path;
        });
        if (found == documents_.end()) return std::nullopt;

        const std::size_t first_index = std::min(start_line - 1, found->lines.size());
        const std::size_t end_index = std::min(first_index + max_lines, found->lines.size());
        std::ostringstream content;
        for (std::size_t i = first_index; i < end_index; ++i) {
            if (i != first_index) content << '\n';
            content << found->lines[i];
        }

        return json{
            {"path", found->path},
            {"uri", std::string{kDocsUriPrefix} + uri_encode_path(found->path)},
            {"startLine", first_index + 1},
            {"endLine", end_index},
            {"totalLines", found->lines.size()},
            {"content", content.str()},
        };
    }

private:
    fs::path root_;
    mutable std::mutex mutex_;
    std::vector<Document> documents_;
};

json object_schema(json properties, json required = json::array()) {
    json schema{
        {"type", "object"},
        {"properties", std::move(properties)},
        {"additionalProperties", false},
    };
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

void register_tools(mcp::Server& server, const std::shared_ptr<DocsRepository>& repository) {
    const mcp::ToolAnnotations read_only{
        .read_only_hint = true,
        .destructive_hint = false,
        .idempotent_hint = true,
        .open_world_hint = false,
    };

    server.tool(
        "docs_list",
        object_schema({
            {"prefix", {{"type", "string"}, {"description", "可选的相对路径前缀"}}},
        }),
        [repository](const json& args) {
            if (!args.is_object()) return error_result("参数必须是 JSON object");
            if (args.contains("prefix") && !args["prefix"].is_string()) {
                return error_result("prefix 必须是字符串");
            }
            const std::string prefix = args.value("prefix", std::string{});
            const json result = repository->list(prefix);
            return json_result("找到 " + std::to_string(result["documents"].size()) + " 篇文档", result);
        },
        "列出开发文档",
        "列出已索引的 Markdown 文档。首次使用或不知道文件名时先调用它。",
        read_only);

    server.tool(
        "docs_search",
        object_schema(
            {
                {"query", {{"type", "string"}, {"minLength", 1},
                           {"description", "要检索的原文词语或短语；支持中文"}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50},
                           {"default", 10}}},
            },
            json::array({"query"})),
        [repository](const json& args) {
            if (!args.is_object() || !args.contains("query") || !args["query"].is_string()) {
                return error_result("query 必须是非空字符串");
            }
            const std::string query = args["query"].get<std::string>();
            if (query.empty()) return error_result("query 不能为空");
            const auto limit = bounded_integer(args, "limit", 10, 50);
            if (!limit) return error_result("limit 必须是 1 到 50 的整数");

            const json result = repository->search(query, *limit);
            std::ostringstream summary;
            summary << "检索 “" << query << "” 得到 " << result["matches"].size() << " 条结果";
            for (const auto& match : result["matches"]) {
                summary << "\n- " << match["path"].get<std::string>() << ':'
                        << match["line"].get<std::size_t>() << " — "
                        << match["snippet"].get<std::string>();
            }
            return json_result(summary.str(), result);
        },
        "检索开发规范",
        "在全部 Markdown 文档中做不区分 ASCII 大小写的逐行原文检索。开发前用任务关键词、模块名或规则词检索，然后通过 docs_read 阅读完整上下文。",
        read_only);

    server.tool(
        "docs_read",
        object_schema(
            {
                {"path", {{"type", "string"}, {"description", "docs_list/docs_search 返回的相对路径"}}},
                {"start_line", {{"type", "integer"}, {"minimum", 1}, {"default", 1}}},
                {"max_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500},
                               {"default", 200}}},
            },
            json::array({"path"})),
        [repository](const json& args) {
            if (!args.is_object() || !args.contains("path") || !args["path"].is_string()) {
                return error_result("path 必须是文档相对路径");
            }
            const auto start_line = bounded_integer(args, "start_line", 1, 100000000);
            const auto max_lines = bounded_integer(args, "max_lines", 200, 500);
            if (!start_line || !max_lines) {
                return error_result("start_line 必须为正整数，max_lines 必须是 1 到 500 的整数");
            }
            const auto result = repository->read(args["path"].get<std::string>(), *start_line, *max_lines);
            if (!result) return error_result("文档不存在，或 path 不是安全的相对路径");
            const std::string header = (*result)["path"].get<std::string>() + ":" +
                                       std::to_string((*result)["startLine"].get<std::size_t>()) + "-" +
                                       std::to_string((*result)["endLine"].get<std::size_t>());
            return json_result(header + "\n\n" + (*result)["content"].get<std::string>(), *result);
        },
        "分段读取开发文档",
        "按相对路径和行号读取 Markdown。检索命中后必须读取相关段落的完整上下文，不要只依据单行 snippet 下结论。",
        read_only);

    server.tool(
        "docs_refresh",
        object_schema(json::object()),
        [repository](const json&) {
            const std::size_t count = repository->reload();
            return json_result("文档索引已刷新，共 " + std::to_string(count) + " 篇",
                               {{"documents", count}});
        },
        "刷新文档索引",
        "开发文档发生变化后调用，重新扫描 Markdown 文件；不会修改任何文档。",
        read_only);
}

void register_resources(mcp::Server& server, const std::shared_ptr<DocsRepository>& repository) {
    server.resource_template(mcp::ResourceTemplate{
        .uri_template = "docs:///{path}",
        .name = "development-documents",
        .title = "开发文档",
        .description = "读取 --docs 目录下的 Markdown；path 使用 docs_list 返回的相对路径",
        .mime_type = "text/markdown",
    });

    server.fallback_resource_handler([repository](const std::string& uri) {
        if (!uri.starts_with(kDocsUriPrefix)) {
            throw mcp::Error{mcp::error_code::invalid_params, "不支持的资源 URI: " + uri};
        }
        const auto decoded = uri_decode_path(std::string_view{uri}.substr(kDocsUriPrefix.size()));
        if (!decoded) {
            throw mcp::Error{mcp::error_code::invalid_params, "资源 URI 包含无效的百分号编码"};
        }
        const auto document = repository->read(*decoded, 1, 100000000);
        if (!document) {
            throw mcp::Error{mcp::error_code::invalid_params, "文档不存在: " + *decoded};
        }
        return mcp::ReadResourceResult{
            .contents = {
                mcp::TextResourceContents{
                    .uri = uri,
                    .mime_type = "text/markdown",
                    .text = (*document)["content"].get<std::string>(),
                },
            },
        };
    });
}

void register_prompt(mcp::Server& server) {
    server.prompt(
        mcp::Prompt{
            .name = "develop-with-docs",
            .title = "基于项目文档开发",
            .description = "要求 Agent 在修改代码前检索规范，并在交付时说明遵循情况",
            .arguments = std::vector<mcp::PromptArgument>{
                {.name = "task", .description = "本次开发任务", .required = true},
                {.name = "scope", .description = "可选模块、目录或技术栈范围", .required = false},
            },
        },
        [](const std::unordered_map<std::string, std::string>& args) {
            const auto task = args.find("task");
            const auto scope = args.find("scope");
            const std::string task_text = task == args.end() ? "当前开发任务" : task->second;
            const std::string scope_text = scope == args.end() ? "未指定" : scope->second;
            return mcp::GetPromptResult{
                .description = "文档约束开发流程",
                .messages = {
                    mcp::PromptMessage{
                        .role = mcp::Role::user,
                        .content = mcp::TextContent{
                            .text =
                                "请完成以下任务：" + task_text + "\n"
                                "范围：" + scope_text + "\n\n"
                                "必须遵循此流程：\n"
                                "1. 先调用 docs_list 了解文档结构。\n"
                                "2. 用任务关键词、模块名和‘必须/禁止/规范’等词调用 docs_search。\n"
                                "3. 对命中结果调用 docs_read 阅读完整上下文。\n"
                                "4. 编码前列出适用于本任务的文档约束及来源 path:line。\n"
                                "5. 按约束实施并验证；有冲突或缺失时明确指出，不自行虚构规范。\n"
                                "6. 最终答复给出文档遵循清单和验证结果。",
                        },
                    },
                },
            };
        });
}

fs::path parse_docs_path(int argc, char* argv[]) {
    fs::path docs = fs::current_path() / "docs";
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--docs" && i + 1 < argc) {
            docs = argv[++i];
        } else if (argument == "--help" || argument == "-h") {
            std::cerr << "usage: markdown_docs_mcp_server [--docs /path/to/markdown/root]\n";
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::runtime_error("未知参数: " + argument);
        }
    }
    return docs;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        auto repository = std::make_shared<DocsRepository>(parse_docs_path(argc, argv));
        mcp::Server server{mcp::Implementation{
            .name = "markdown-development-docs",
            .title = "开发文档知识库",
            .version = "0.1.0",
            .description = "只读检索和读取 Markdown 开发规范",
        }};
        server.set_instructions(
            "这是项目开发文档的只读知识库。处理代码任务前先用 docs_list/docs_search 找到相关规范，"
            "再用 docs_read 阅读完整段落；引用规则时带上 path:line。文档变更后调用 docs_refresh。");

        register_tools(server, repository);
        register_resources(server, repository);
        register_prompt(server);

        std::cerr << "markdown_docs_mcp_server indexed Markdown under "
                  << repository->root_string() << '\n';
        server.run(std::make_unique<mcp::StdioTransport>());
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "markdown_docs_mcp_server failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
