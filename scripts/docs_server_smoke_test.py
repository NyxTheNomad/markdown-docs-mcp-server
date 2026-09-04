#!/usr/bin/env python3
"""End-to-end stdio protocol test for markdown_docs_mcp_server."""

import json
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} SERVER DOCS_DIR", file=sys.stderr)
        return 2

    process = subprocess.Popen(
        [sys.argv[1], "--docs", sys.argv[2]],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    assert process.stdin is not None
    assert process.stdout is not None

    def request(request_id: int, method: str, params=None):
        frame = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            frame["params"] = params
        process.stdin.write(json.dumps(frame, ensure_ascii=False) + "\n")
        process.stdin.flush()
        response = json.loads(process.stdout.readline())
        assert response["id"] == request_id, response
        assert "error" not in response, response
        return response["result"]

    initialized = request(
        1,
        "initialize",
        {
            "protocolVersion": "2025-11-25",
            "capabilities": {},
            "clientInfo": {"name": "docs-smoke-test", "version": "0.1.0"},
        },
    )
    process.stdin.write(
        json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n"
    )
    process.stdin.flush()

    tools = request(2, "tools/list", {})
    listing = request(3, "tools/call", {"name": "docs_list", "arguments": {}})
    search = request(
        4, "tools/call", {"name": "docs_search", "arguments": {"query": "stdout"}}
    )
    read = request(
        5,
        "tools/call",
        {
            "name": "docs_read",
            "arguments": {"path": "coding-style/cpp-style.md", "start_line": 1},
        },
    )
    resource = request(
        6, "resources/read", {"uri": "docs:///coding-style/cpp-style.md"}
    )
    prompt = request(
        7,
        "prompts/get",
        {"name": "develop-with-docs", "arguments": {"task": "新增一个工具"}},
    )

    assert initialized["serverInfo"]["name"] == "markdown-development-docs"
    assert {item["name"] for item in tools["tools"]} == {
        "docs_list", "docs_search", "docs_read", "docs_refresh"
    }
    assert len(listing["structuredContent"]["documents"]) == 4
    assert search["structuredContent"]["matches"][0]["path"] == "coding-style/cpp-style.md"
    assert "C++20" in read["structuredContent"]["content"]
    assert "stdout" in resource["contents"][0]["text"]
    assert "docs_search" in prompt["messages"][0]["content"]["text"]

    process.stdin.close()
    stderr = process.stderr.read() if process.stderr is not None else ""
    return_code = process.wait(timeout=5)
    if return_code != 0:
        raise RuntimeError(f"server exited with {return_code}: {stderr}")

    print("docs server smoke test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
