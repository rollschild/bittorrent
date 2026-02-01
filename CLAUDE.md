# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Context

Whenever working with this codebase (or any codebase), ALWAYS ask me first about any changes or modifications you are trying to make, and do NOT make those changes without me explicitly allowing so.

ALWAYS show me the code/implementation without me having to type `/plan` myself.

## Build Commands

```bash
# Enter Nix development environment
nix develop

# Build with CMake
cmake . -B build
cmake --build build

# Run
./build/src/main decode <encoded_value>

# Run tests (GTest) - Note: tests directory currently commented out in CMakeLists.txt
cmake --build build
ctest --test-dir build

# Run a single test
ctest --test-dir build -R <test_name>

# Format code
clang-format -i src/*.cpp
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

## Dependencies

- **OpenSSL**: SHA1 hashing for info hash computation
- **libcurl**: HTTP requests to tracker
- **nlohmann/json**: JSON serialization (vendored in `src/lib/nlohmann/json.hpp`)

## Architecture

BitTorrent client implementation. Single-file architecture in `src/main.cpp` with CLI interface.

### CLI Commands

- `decode <value>` - Decode bencoded value to JSON
- `info <torrent_file>` - Display torrent metadata (tracker URL, length, info hash, pieces)
- `peers <torrent_file>` - Fetch and display peer list from tracker
- `handshake <torrent_file> <ip>:<port>` - Perform BitTorrent handshake with a peer
- `download_piece -o <output_path> <torrent_file> <piece_index>` - Download a single piece from peers
- `download -o <output_path> <torrent_file>` - Download complete file from peers
- `magnet_parse <magnet_link>` - Parse magnet link and extract tracker URL and info hash

### Key Functions

- `decode_bencoded_value()` - Recursive bencode parser (strings, integers, lists, dicts)
- `extract_bencoded_value()` - Extract raw bencoded data for a dictionary key (used for info hash)
- `sha1_hash()` / `sha1_hash_raw()` - SHA1 hashing (hex string vs raw bytes)
- `url_encode()` - URL-encode binary data for tracker requests
- `fetch_url()` - HTTP GET using libcurl
- `get_first_peer()` - Fetch peer list from tracker and return first peer's IP/port
- `perform_handshake()` - TCP socket connection and BitTorrent protocol handshake
- `wait_for_unchoke()` - Wait for bitfield, send interested, wait for unchoke message
- `download_piece()` - Download and verify a piece using block requests (16KB blocks)
- `send_message()` / `recv_message()` - BitTorrent peer wire protocol message framing

### BitTorrent Protocol Constants

Message IDs follow BEP 0003: `MSG_CHOKE (0)`, `MSG_UNCHOKE (1)`, `MSG_INTERESTED (2)`, `MSG_NOT_INTERESTED (3)`, `MSG_BITFIELD (5)`, `MSG_REQUEST (6)`, `MSG_PIECE (7)`. Block size is 16KB (2^14).
