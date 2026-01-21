# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Context

Whenever working with this codebase (or any codebase), ALWAYS ask me first about any changes or modifications you are trying to make, and do NOT make those changes without me explicitly allowing so.

ALWAYS show me the code/implemention without me having to type `/plan` myself.

## Build Commands

```bash
# Enter Nix development environment
nix develop

# Build with CMake
cmake . -B build
cmake --build build

# Run
./build/src/main decode <encoded_value>

# Enable and run tests (GTest)
cmake -DENABLE_TESTING=ON -B build
cmake --build build
ctest --test-dir build

# Run a single test
ctest --test-dir build -R <test_name>
```

## Development Environment

- **Language**: C++23 (required standard)
- **Build System**: CMake 3.29+
- **Package Manager**: Nix Flakes
- **Test Framework**: Google Test (GTest), Catch2 v3 available
- **Code Style**: Google style, 4-space indent (see `.clang-format`)

## Compiler Configuration

- Compiler: GCC
- Flags: `-Wall -Wfatal-errors -Wextra -Werror -g -O1`

## Architecture

BitTorrent client implementation. Entry point is `src/main.cpp` which provides a CLI interface.

### Current Implementation

- **Bencode decoding**: `decode_bencoded_value()` parses bencoded strings (length-prefixed format like `5:hello`)
- **CLI**: Accepts `decode <value>` command to decode and output bencoded values as JSON
- **JSON**: Uses nlohmann/json (vendored in `src/lib/nlohmann/json.hpp`) for JSON output
