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
./build/src/main

# Enable and run tests (GTest)
cmake -DENABLE_TESTING=ON -B build
cmake --build build
ctest --test-dir build

# Run a single test
ctest --test-dir build -R <test_name>

# Test DNS server manually (while server is running)
dig @127.0.0.1 -p 2053 example.com
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

DNS server using UDP sockets on port 2053. Entry point is `src/main.cpp` which sets up a UDP socket, binds to the port, and runs an event loop receiving DNS queries and sending responses.

Socket configuration: SO_REUSEPORT enabled, 512-byte buffer (standard DNS max).

**Current behavior**: Returns hardcoded IP 8.8.8.8 for all A record queries.

### DNS Message Structs

All defined in `src/main.cpp`:

- **DNSHeader**: 12-byte header with ID, flags, and section counts. Methods: `parse_from()`, `write_to()`, `create_response()`
- **DNSQuestion**: Query section with domain name (QNAME), type (QTYPE), and class (QCLASS). Handles label encoding (length-prefixed segments)
- **DNSAnswer**: Resource record with domain, type, class, TTL, and RDATA. Supports DNS compression (0xC00C pointer). Static factory: `create_a_record()`
- **DNSCompressionTable**: Maps domain names to message offsets for pointer-based compression (RFC 1035 Section 4.1.4). Methods: `record_name()`, `find_pointer()`

### DNS Message Format

All DNS communications use a single message format with 5 sections: header (12 bytes, big-endian), question, answer, authority, and additional.
