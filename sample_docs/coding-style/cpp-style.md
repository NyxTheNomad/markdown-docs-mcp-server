# C++ development rules

## Language version

The project must use C++20. Every new target must explicitly declare `cxx_std_20`.

## Error handling

Recoverable domain failures must be returned as structured errors instead of terminating the process.

## Logging

An MCP stdio server must never write logs to stdout. All diagnostics must go to stderr.
