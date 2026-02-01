#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "lib/nlohmann/json.hpp"

// Message IDs
// corresponds to https://www.bittorrent.org/beps/bep_0003.html#peer-messages
constexpr uint8_t MSG_CHOKE = 0;
constexpr uint8_t MSG_UNCHOKE = 1;
constexpr uint8_t MSG_INTERESTED = 2;
constexpr uint8_t MSG_NOT_INTERESTED = 3;
constexpr uint8_t MSG_BITFIELD = 5;
constexpr uint8_t MSG_REQUEST = 6;
constexpr uint8_t MSG_PIECE = 7;

constexpr size_t BLOCK_SIZE = 16384;  // 2^14, 16KB

using json = nlohmann::json;

json decode_bencoded_value(const std::string& encoded_value, size_t& index) {
    char c = encoded_value[index];
    if (std::isdigit(c)) {
        // Example: "5:hello" -> "hello"
        size_t colon_index = encoded_value.find(':', index);
        if (colon_index != std::string::npos) {
            std::string number_string =
                encoded_value.substr(index, colon_index - index);
            int64_t number = std::atoll(number_string.c_str());
            std::string str = encoded_value.substr(colon_index + 1, number);
            index = colon_index + number + 1;
            return json(str);
        } else {
            throw std::runtime_error("Invalid encoded value: " + encoded_value);
        }
    } else if (c == 'i') {
        // maybe an encoded integer
        size_t end_index = encoded_value.find('e', index);
        if (end_index == std::string::npos) {
            throw std::runtime_error("Invalid encoded integer: missing 'e'");
        }
        int64_t number = std::atoll(
            encoded_value.substr(index + 1, end_index - index - 1).c_str());
        index = end_index + 1;
        return json(number);
    } else if (c == 'l') {
        // might be a list `l<contents>e`
        index++;  // skip `l`
        json list = json::array();
        char type = encoded_value[index];
        while (type != 'e') {
            list.push_back(decode_bencoded_value(encoded_value, index));
            type = encoded_value[index];
        }
        index++;  // skip the list-ending 'e'
        return list;
    } else if (c == 'd') {
        // might be a dictionary
        // {"hello": 52, "foo":"bar"}
        // d3:foo3:bar5:helloi52ee
        index++;  // skip 'd'
        json dict = json::object();
        while (encoded_value[index] != 'e') {
            const json key = decode_bencoded_value(encoded_value, index);
            const json value = decode_bencoded_value(encoded_value, index);
            dict[key.get<std::string>()] = value;
        }
        index++;
        return dict;
    } else {
        throw std::runtime_error("Unhandled encoded value: " + encoded_value);
    }
}

std::string read_file(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);  // binary?
    if (!file) {
        throw std::runtime_error("Could not open file: " + filename);
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

/**
 * Helper to find and extract raw bencoded value for a dictionary key
 */
std::string extract_bencoded_value(const std::string& data,
                                   const std::string& key) {
    // e.g. search for "4:info" in the raw torrent data
    std::string bencoded_key = std::to_string(key.length()) + ":" + key;
    // search in raw data
    size_t key_pos = data.find(bencoded_key);
    if (key_pos == std::string::npos) {
        throw std::runtime_error("Key not found: " + key);
    }
    // position after "4:info"
    size_t value_start = key_pos + bencoded_key.length();
    size_t index = value_start;
    decode_bencoded_value(data, index);  // advances index past the value
    return data.substr(value_start, index - value_start);
}

std::string sha1_hash(const std::string& data) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
         hash);
    std::ostringstream ss;
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<int>(hash[i]);
    }
    return ss.str();
}

/**
 * Get SHA1 hash (binary, not hex)
 *   - Raw binary (20 bytes): each byte can be any value 0 - 255, including
 * non-printable chars and null bytes
 *     -
 * \xd6\x9f\x91\xe6\xb2\xae\x4c\x54\x24\x68\xd1\x07\x3a\x71\xd4\xea\x13\x87\x9a\x7f
 *   - Hex string (40 characters): each byte represented as two hex digits (0-9,
 * a-f)
 *     - d69f91e6b2ae4c542468d1073a71d4ea13879a7f
 */
std::string sha1_hash_raw(const std::string& data) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
         hash);
    return std::string(reinterpret_cast<char*>(hash), SHA_DIGEST_LENGTH);
}

/**
 * Callback for curl to write response data
 *   nmemb: number of elements received
 *   size: size of each element (always 1 for bytes)
 */
size_t write_callback(void* contents, size_t size, size_t nmemb,
                      std::string* output) {
    size_t total_size = size * nmemb;
    output->append(static_cast<char*>(contents), total_size);
    return total_size;
}

/**
 * URL-encode binary data (for info_hash)
 */
std::string url_encode(const std::string& data) {
    std::ostringstream encoded;
    // std::uppercase flag set in order to follow RFC 3986 convetion
    // percent-encodings use uppercase hex digits
    encoded << std::hex << std::uppercase;
    for (unsigned char c : data) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << c;
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<int>(c);
        }
    }
    return encoded.str();
}

std::string fetch_url(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl!");
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("Curl request failed: " +
                                 std::string(curl_easy_strerror(res)));
    }

    return response;
}

void recv_all(int sock, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t recvd =
            recv(sock, static_cast<char*>(buf) + total, len - total, 0);
        if (recvd <= 0) {
            throw std::runtime_error("Connection closed or error during recv!");
        }
        total += recvd;
    }
}

int perform_handshake(const std::string& ip, int port,
                      const std::string& info_hash, const std::string& peer_id,
                      bool keep_open = false) {
    // create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        throw std::runtime_error("Failed to create socket!");
    }

    // connect to peer
    struct sockaddr_in peer_addr{};
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    // internet representation to network
    // converts IP address from human-readable string to binary network format
    inet_pton(AF_INET, ip.c_str(), &peer_addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&peer_addr),
                sizeof(peer_addr)) < 0) {
        close(sock);
        throw std::runtime_error("Failed to connect to peer!");
    }

    // build handshake message
    std::string handshake;
    handshake += static_cast<char>(19);  // protocl len
    handshake += "BitTorrent protocol";  // 19 bytes
    handshake += std::string(8, '\0');   // reserved bytes
    handshake += info_hash;              // 20 bytes
    handshake += peer_id;                // 20 bytes

    // send handshake
    if (send(sock, handshake.c_str(), handshake.length(), 0) !=
        static_cast<ssize_t>(handshake.length())) {
        close(sock);
        throw std::runtime_error("Failed to send handshake!");
    }

    // receive peer's handshake (68 bytes)
    char response[68];
    recv_all(sock, response, 68);

    if (!keep_open) {
        close(sock);
    }

    // extract peer ID
    // return std::string(response + 48, 20);
    return sock;
}

void send_message(int sock, uint8_t id, const std::string& payload = "") {
    // the length prefix includes the ID byte but _NOT_ itself
    // len = 1 + payload.size()
    uint32_t len = htonl(1 + payload.size());
    send(sock, &len, 4, 0);  // message length prefix (4 bytes)
    send(sock, &id, 1, 0);   // message id (1 byte)
    if (!payload.empty()) {
        send(sock, payload.c_str(), payload.size(), 0);
    }
}
std::pair<uint8_t, std::string> recv_message(int sock) {
    uint32_t len;
    // first, the length-prefix (4 bytes)
    recv_all(sock, &len, 4);
    len = ntohl(len);
    if (len == 0) {
        return {255, ""};  // keep-alive
    }

    // then, the ID byte
    uint8_t id;
    recv_all(sock, &id, 1);

    std::string payload(len - 1, '\0');
    if (len > 1) {
        recv_all(sock, payload.data(), len - 1);
    }

    return {id, payload};
}

void wait_for_unchoke(int sock) {
    // wait for bitfield
    auto [bf_id, bf_payload] = recv_message(sock);
    // send interested
    send_message(sock, MSG_INTERESTED);
    // wait for unchoke
    while (true) {
        auto [id, payload] = recv_message(sock);
        if (id == MSG_UNCHOKE) {
            break;
        }
    }
}

std::string download_piece(int sock, int piece_index, int piece_len,
                           const std::string& piece_hash) {
    // request all blocks
    std::string piece_data(piece_len, '\0');
    // REMEMBER this
    int num_blocks = (piece_len + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // a piece is too large to request in one message
    for (int i = 0; i < num_blocks; ++i) {
        int offset = i * BLOCK_SIZE;
        // to handle the last block where the actual size is smaller than
        // BLOCK_SIZE
        int block_len =
            std::min(static_cast<int>(BLOCK_SIZE), piece_len - offset);

        // request message format:
        //   - index: 4 bytes
        //   - begin (byte offset within piece): 4 bytes
        //   - length: 4 bytes
        std::string req_payload(12, '\0');
        // 32-bit unsigned -> network byte BIG endian
        uint32_t idx = htonl(piece_index);
        uint32_t begin = htonl(offset);
        uint32_t len = htonl(block_len);

        std::memcpy(req_payload.data(), &idx, 4);
        std::memcpy(req_payload.data() + 4, &begin, 4);
        std::memcpy(req_payload.data() + 8, &len, 4);

        // All requests are sent up front before waiting for any responses.
        // This is a form of **pipelining** -- it avoids a round-trip per block.
        send_message(sock, MSG_REQUEST, req_payload);
    }

    // receive all blocks
    int blocks_recvd = 0;
    while (blocks_recvd < num_blocks) {
        auto [id, payload] = recv_message(sock);
        if (id == MSG_PIECE) {
            // format:
            //   - id
            //   - payload
            //     - index (4 bytes)
            //     - begin (4 bytes)
            //     - block (variable, at offset 8)

            // payload is std::string
            // to read 4 bytes at `uint32_t`, we need to reinterpret the raw
            // bytes as a pointer to uint32_t
            // payload.data() is char*
            // reinterpret_cast works with pointers _ONLY_
            // then dereference uint32_t* to uint32_t
            uint32_t begin =
                ntohl(*reinterpret_cast<const uint32_t*>(payload.data() + 4));
            std::memcpy(piece_data.data() + begin, payload.data() + 8,
                        payload.size() - 8);
            blocks_recvd++;
        }
    }

    // verify hash
    std::string computed_hash = sha1_hash_raw(piece_data);
    if (computed_hash != piece_hash) {
        throw std::runtime_error("Piece hash mismatch!");
    }

    return piece_data;
}

std::pair<std::string, int> get_first_peer(const json& torrent,
                                           const std::string& info_hash_raw) {
    std::string tracker_url = torrent["announce"].get<std::string>();
    int64_t len = torrent["info"]["length"].get<int64_t>();
    std::string peer_id = "00112233445566778899";

    std::string url = tracker_url + "?info_hash=" + url_encode(info_hash_raw) +
                      "&peer_id=" + peer_id + "&port=6881" + "&uploaded=0" +
                      "&downloaded=0" + "&left=" + std::to_string(len) +
                      "&compact=1";

    std::string res = fetch_url(url);
    size_t idx = 0;
    json tracker_res = decode_bencoded_value(res, idx);
    // parse compact peers (6 bytes each: 4 IP + 2 port)
    std::string peers = tracker_res["peers"].get<std::string>();
    int ip1 = static_cast<unsigned char>(peers[0]);
    int ip2 = static_cast<unsigned char>(peers[1]);
    int ip3 = static_cast<unsigned char>(peers[2]);
    int ip4 = static_cast<unsigned char>(peers[3]);
    int port = (static_cast<unsigned char>(peers[4]) << 8 |
                static_cast<unsigned char>(peers[5]));

    std::string ip = std::to_string(ip1) + "." + std::to_string(ip2) + "." +
                     std::to_string(ip3) + "." + std::to_string(ip4);
    return {ip, port};
}
std::pair<std::string, int> get_first_peer(const std::string& tracker_url,
                                           const std::string& info_hash_raw) {
    std::string peer_id = "00112233445566778899";

    std::string url = tracker_url + "?info_hash=" + url_encode(info_hash_raw) +
                      "&peer_id=" + peer_id + "&port=6881" + "&uploaded=0" +
                      "&downloaded=0" + "&left=999" + "&compact=1";

    std::string res = fetch_url(url);
    size_t idx = 0;
    json tracker_res = decode_bencoded_value(res, idx);
    // parse compact peers (6 bytes each: 4 IP + 2 port)
    std::string peers = tracker_res["peers"].get<std::string>();
    int ip1 = static_cast<unsigned char>(peers[0]);
    int ip2 = static_cast<unsigned char>(peers[1]);
    int ip3 = static_cast<unsigned char>(peers[2]);
    int ip4 = static_cast<unsigned char>(peers[3]);
    int port = (static_cast<unsigned char>(peers[4]) << 8 |
                static_cast<unsigned char>(peers[5]));

    std::string ip = std::to_string(ip1) + "." + std::to_string(ip2) + "." +
                     std::to_string(ip3) + "." + std::to_string(ip4);
    return {ip, port};
}

struct magnet_link {
    // magnet link format:
    // `magnet:?xt=urn:btih:<info-hash>&dn=<name>&tr=<tracker-url>&x.pe=<peer-address>`
    std::string info_hash_hex;
    std::string info_hash_raw;
    std::string tracker_url;
    std::string peer_addr_str;

    void info_hash_hex_to_raw() {
        if (!info_hash_hex.empty()) {
            for (size_t i = 0; i < info_hash_hex.length(); i += 2) {
                int byte_val;
                std::istringstream iss(info_hash_hex.substr(i, 2));
                iss >> std::hex >> byte_val;
                info_hash_raw += static_cast<char>(byte_val);
            }
        }
    }
};

magnet_link parse_magnet_link(std::string mag_link_str) {
    magnet_link mag_link_struct;

    // parse query parameters
    size_t query_start = mag_link_str.find('?');
    if (query_start == std::string::npos) {
        throw std::runtime_error("INVALID magnet link format!");
    }

    std::string query = mag_link_str.substr(query_start + 1);
    // split by '&' and parse each parameter
    size_t pos = 0;
    while (pos < query.length()) {
        size_t amp_pos = query.find('&', pos);
        std::string param;
        if (amp_pos == std::string::npos) {
            param = query.substr(pos);
            pos = query.length();
        } else {
            param = query.substr(pos, amp_pos - pos);
            pos = amp_pos + 1;
        }

        size_t eq_pos = param.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = param.substr(0, eq_pos);
            std::string value = param.substr(eq_pos + 1);

            if (key == "xt" && value.substr(0, 9) == "urn:btih:") {
                // extract info hash
                mag_link_struct.info_hash_hex = value.substr(9);
            } else if (key == "tr") {
                // URL-encoded the tracker URL
                std::string decoded;
                for (size_t i = 0; i < value.length(); ++i) {
                    if (value[i] == '%' && i + 2 < value.length()) {
                        int hex_val;
                        std::istringstream iss(value.substr(i + 1, 2));
                        iss >> std::hex >> hex_val;
                        decoded += static_cast<char>(hex_val);
                        i += 2;
                    } else {
                        decoded += value[i];
                    }
                }
                mag_link_struct.tracker_url = decoded;
            } else if (key == "x.pe") {
                mag_link_struct.peer_addr_str = value;
            }
        }
    }

    mag_link_struct.info_hash_hex_to_raw();

    return mag_link_struct;
}

int main(int argc, char* argv[]) {
    // Flush after every std::cout / std::cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " decode <encoded_value>"
                  << std::endl;
        return 1;
    }

    std::string command = argv[1];

    if (command == "decode") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " decode <encoded_value>"
                      << std::endl;
            return 1;
        }
        // You can use print statements as follows for debugging, they'll be
        // visible when running tests.
        std::cerr << "Logs from your program will appear here!" << std::endl;

        std::string encoded_value = argv[2];
        size_t index = 0;
        json decoded_value = decode_bencoded_value(encoded_value, index);
        std::cout << decoded_value.dump() << std::endl;
    } else if (command == "info") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " info <torrent_file>"
                      << std::endl;
            return 1;
        }
        std::string filename = argv[2];
        std::string contents = read_file(filename);
        std::string info_bencoded = extract_bencoded_value(contents, "info");
        size_t index = 0;
        json torrent = decode_bencoded_value(contents, index);

        std::cout << "Tracker URL: " << torrent["announce"].get<std::string>()
                  << std::endl;
        std::cout << "Length: " << torrent["info"]["length"].get<int64_t>()
                  << std::endl;
        std::cout << "Info Hash: " << sha1_hash(info_bencoded) << std::endl;
        std::cout << "Piece Length: "
                  << torrent["info"]["piece length"].get<int64_t>()
                  << std::endl;
        std::cout << "Pieces:" << std::endl;
        std::string pieces = torrent["info"]["pieces"].get<std::string>();
        for (size_t i = 0; i < pieces.length(); i += 20) {
            std::ostringstream ss;
            for (size_t j = 0; j < 20; ++j) {
                ss << std::hex << std::setfill('0')
                   << std::setw(2)
                   // inner `static_cast` prevents sign extension
                   // e.g. 0xAB is -85 for signed char
                   // outer `static_cast` prints 0xAB as a number, not a char
                   << static_cast<int>(
                          static_cast<unsigned char>(pieces[i + j]));
            }
            std::cout << ss.str() << std::endl;
        }
    } else if (command == "peers") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " peers <torrent_file>"
                      << std::endl;
            return 1;
        }

        std::string filename = argv[2];
        std::string contents = read_file(filename);
        size_t index = 0;
        json torrent = decode_bencoded_value(contents, index);

        std::string info_bencoded = extract_bencoded_value(contents, "info");
        std::string info_hash_raw = sha1_hash_raw(info_bencoded);

        std::string tracker_url = torrent["announce"].get<std::string>();
        int64_t length = torrent["info"]["length"].get<int64_t>();

        // build tracker request URL
        std::string peer_id = "00112233445566778899";  // 20-byte peer ID
        std::string url =
            tracker_url + "?info_hash=" + url_encode(info_hash_raw) +
            "&peer_id=" + peer_id + "&port=6881" + "&uploaded=0" +
            "&downloaded=0" + "&left=" + std::to_string(length) + "&compact=1";

        std::string res = fetch_url(url);
        index = 0;
        json tracker_res = decode_bencoded_value(res, index);

        // parse compact peers (6 bytes each: 4 IP + 2 port)
        std::string peers = tracker_res["peers"].get<std::string>();
        for (size_t i = 0; i < peers.length(); i += 6) {
            int ip1 = static_cast<unsigned char>(peers[i]);
            int ip2 = static_cast<unsigned char>(peers[i + 1]);
            int ip3 = static_cast<unsigned char>(peers[i + 2]);
            int ip4 = static_cast<unsigned char>(peers[i + 3]);
            int port = (static_cast<unsigned char>(peers[i + 4]) << 8 |
                        static_cast<unsigned char>(peers[i + 5]));

            std::cout << ip1 << "." << ip2 << "." << ip3 << "." << ip4 << ":"
                      << port << std::endl;
        }

    } else if (command == "handshake") {
        if (argc < 4) {
            std::cerr << "Usage: " << argv[0]
                      << " handshake <torrent_file> <peer_ip>:<peer_port>"
                      << std::endl;
            return 1;
        }

        std::string filename = argv[2];
        std::string peer_addr = argv[3];

        // parse peer address
        size_t colon_pos = peer_addr.find(':');
        if (colon_pos == std::string::npos) {
            std::cerr << "Invalid peer address format" << std::endl;
            return 1;
        }

        std::string ip = peer_addr.substr(0, colon_pos);
        int port = std::stoi(peer_addr.substr(colon_pos + 1));

        // get info hash from content
        std::string contents = read_file(filename);
        std::string info_bencoded = extract_bencoded_value(contents, "info");
        std::string info_hash_raw = sha1_hash_raw(info_bencoded);
        std::string peer_id = "00112233445566778899";

        // perform handshake
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            throw std::runtime_error("Failed to create socket!");
        }

        struct sockaddr_in peer_addr_struct{};
        peer_addr_struct.sin_family = AF_INET;
        peer_addr_struct.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &peer_addr_struct.sin_addr);
        if (connect(sock, reinterpret_cast<sockaddr*>(&peer_addr_struct),
                    sizeof(peer_addr_struct)) < 0) {
            close(sock);
            throw std::runtime_error("Fialed to connect to peer!");
        }

        std::string handshake;
        handshake += static_cast<char>(19);  // protocl len
        handshake += "BitTorrent protocol";  // 19 bytes
        handshake += std::string(8, '\0');   // reserved bytes
        handshake += info_hash_raw;          // 20 bytes
        handshake += peer_id;                // 20 bytes

        if (send(sock, handshake.c_str(), handshake.length(), 0) !=
            static_cast<ssize_t>(handshake.length())) {
            close(sock);
            throw std::runtime_error("Failed to send handshake!");
        }

        char response[68];
        recv_all(sock, response, 68);
        close(sock);

        std::string rcvd_peer_id(response + 48, 20);

        std::cout << "Peer ID: ";
        for (unsigned char c : rcvd_peer_id) {
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<int>(c);
        }
        std::cout << std::endl;
    } else if (command == "download_piece") {
        if (argc < 6 || std::string(argv[2]) != "-o") {
            std::cerr << "Usage: " << argv[0]
                      << " download_piece -o <output_path> <torrent_file> "
                         "<piece_index>"
                      << std::endl;
            return 1;
        }

        std::string output_path = argv[3];
        std::string filename = argv[4];
        int piece_index = std::stoi(argv[5]);

        // parse torrent
        std::string contents = read_file(filename);
        size_t index = 0;
        json torrent = decode_bencoded_value(contents, index);

        std::string info_bencoded = extract_bencoded_value(contents, "info");
        std::string info_hash_raw = sha1_hash_raw(info_bencoded);

        // calculate piece length
        int64_t total_len = torrent["info"]["length"].get<int64_t>();
        int64_t piece_len = torrent["info"]["piece length"].get<int64_t>();
        int num_pieces = (total_len + piece_len - 1) / piece_len;

        // last piece may be smaller
        // so `this_piece_len` <= piece_len
        int this_piece_len = piece_len;
        if (piece_index == num_pieces - 1) {
            this_piece_len = total_len - (piece_index * piece_len);
        }

        // get piece hash (20 bytes per piece)
        // `pieces` is an entire string of multiple hashes
        std::string pieces = torrent["info"]["pieces"].get<std::string>();
        std::string piece_hash = pieces.substr(piece_index * 20, 20);

        // get peer and connect
        auto [ip, port] = get_first_peer(torrent, info_hash_raw);
        std::string peer_id = "00112233445566778899";

        // 1. handshake
        // 2. download data
        int sock = perform_handshake(ip, port, info_hash_raw, peer_id, true);

        wait_for_unchoke(sock);

        // download piece
        std::string piece_data =
            download_piece(sock, piece_index, this_piece_len, piece_hash);
        close(sock);

        // write to file
        std::ofstream out(output_path, std::ios::binary);
        out.write(piece_data.c_str(), piece_data.size());
        out.close();

        std::cout << "Piece " << piece_index << " downloaded to " << output_path
                  << "." << std::endl;

    } else if (command == "download") {
        if (argc < 5 || std::string(argv[2]) != "-o") {
            std::cerr << "Usage: " << argv[0]
                      << " download -o <output_path> <torrent_file>"
                      << std::endl;
            return 1;
        }

        std::string output_path = argv[3];
        std::string filename = argv[4];

        std::string contents = read_file(filename);
        size_t index = 0;
        json torrent = decode_bencoded_value(contents, index);

        std::string info_bencoded = extract_bencoded_value(contents, "info");
        std::string info_hash_raw = sha1_hash_raw(info_bencoded);

        int64_t total_len = torrent["info"]["length"].get<int64_t>();
        int64_t piece_len = torrent["info"]["piece length"].get<int64_t>();
        int num_pieces = (total_len + piece_len - 1) / piece_len;
        std::string pieces = torrent["info"]["pieces"].get<std::string>();

        // connect to peer
        auto [ip, port] = get_first_peer(torrent, info_hash_raw);
        std::string peer_id = "00112233445566778899";
        int sock = perform_handshake(ip, port, info_hash_raw, peer_id, true);
        wait_for_unchoke(sock);

        // download all pieces
        std::string file_data;
        file_data.reserve(total_len);

        for (int i = 0; i < num_pieces; ++i) {
            int this_piece_len = (i == num_pieces - 1)
                                     ? (total_len - (i * piece_len))
                                     : piece_len;

            // get piece hash
            std::string piece_hash = pieces.substr(i * 20, 20);

            // download piece
            std::string piece_data =
                download_piece(sock, i, this_piece_len, piece_hash);
            file_data += piece_data;
        }

        close(sock);

        // write complete file
        std::ofstream out(output_path, std::ios::binary);
        out.write(file_data.c_str(), file_data.size());
        out.close();
    } else if (command == "magnet_parse") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0]
                      << " magnet_parse -o <magnet-link>" << std::endl;
            return 1;
        }

        std::string mag_link_str = argv[2];
        magnet_link mag_link_struct = parse_magnet_link(mag_link_str);

        std::cout << "Tracker URL: " << mag_link_struct.tracker_url
                  << std::endl;
        std::cout << "Info Hash: " << mag_link_struct.info_hash_hex
                  << std::endl;

    } else if (command == "magnet_handshake") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0]
                      << " magnet_handshake <magnet-link>" << std::endl;
            return 1;
        }

        std::string mag_link_str = argv[2];
        magnet_link mag_link_struct = parse_magnet_link(mag_link_str);

        // parse peer address from x.pe parameter
        std::string ip;
        int port{};
        if (mag_link_struct.peer_addr_str.empty()) {
            std::tie(ip, port) = get_first_peer(mag_link_struct.tracker_url,
                                                mag_link_struct.info_hash_raw);
        } else {
            size_t colon_pos = mag_link_struct.peer_addr_str.find(':');
            if (colon_pos == std::string::npos) {
                throw std::runtime_error(
                    "Invalid peer address in magnet link!");
            }

            ip = mag_link_struct.peer_addr_str.substr(0, colon_pos);
            port =
                std::stoi(mag_link_struct.peer_addr_str.substr(colon_pos + 1));
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            throw std::runtime_error("Failed to create socket!");
        }

        struct sockaddr_in peer_addr{};
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &peer_addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&peer_addr),
                    sizeof(peer_addr)) < 0) {
            close(sock);
            throw std::runtime_error("Failed to connect to peer!");
        }

        // build handshake with extension support
        // reserved byte 5, bit 4 (0x10) indeicates extension protocol support
        // 00 00 00 00 00 10 00 00
        std::string reserved(8, '\0');
        reserved[5] = 0x10;

        std::string peer_id = "00112233445566778899";
        std::string handshake;
        handshake += static_cast<char>(19);          // protocl len
        handshake += "BitTorrent protocol";          // 19 bytes
        handshake += reserved;                       // reserved bytes
        handshake += mag_link_struct.info_hash_raw;  // 20 bytes
        handshake += peer_id;                        // 20 bytes

        // send handshake
        if (send(sock, handshake.c_str(), handshake.length(), 0) !=
            static_cast<ssize_t>(handshake.length())) {
            close(sock);
            throw std::runtime_error("Failed to send handshake!");
        }

        // receive peer's handshake
        char response[68];
        recv_all(sock, response, 68);

        // extract and print peer ID
        std::string rcvd_peer_id(response + 48, 20);
        std::cout << "Peer ID: ";
        for (unsigned char c : rcvd_peer_id) {
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<int>(c);
        }
        std::cout << std::endl;

        // check if peer supports extensions
        bool peer_supports_extensions =
            (static_cast<unsigned char>(response[25]) & 0x10) != 0;
        if (peer_supports_extensions) {
            // send extension handshake (BEP 10)
            // message ID 20 = extended;
            // extended ID 0 = handshake
            // payload: bencoded dict with "m" containing supported extensions
            // we advertise support for `ut_metadata` (BEP 9) with ID 1
            std::string ext_handshake_payload = "d1:md11:ut_metadatai1eee";

            // build extended message:
            //   - msg_id (20)
            //   - ext_id (0)
            //   - payload
            std::string ext_msg;
            ext_msg +=
                static_cast<char>(0);  // extended message ID 0 = handshake
            ext_msg += ext_handshake_payload;

            // send as message with ID 20
            constexpr uint8_t MSG_EXTENDED = 20;
            send_message(sock, MSG_EXTENDED, ext_msg);

            // receive messages until we get the extension handshake
            // peer might send bitfield or other messages first
            while (true) {
                // receive peer's extension handshake
                auto [msg_id, payload] = recv_message(sock);
                if (msg_id == MSG_EXTENDED && !payload.empty()) {
                    uint8_t ext_msg_id = static_cast<uint8_t>(payload[0]);

                    if (ext_msg_id == 0) {
                        // extension handshake received
                        std::string ext_payload = payload.substr(1);
                        size_t idx = 0;
                        json ext_dict = decode_bencoded_value(ext_payload, idx);

                        // print peer metadata extension ID if available
                        if (ext_dict.contains("m") &&
                            ext_dict["m"].contains("ut_metadata")) {
                            std::cout << std::dec
                                      <<  // reset to decimal mode since hex
                                          // mode is sticky!
                                "Peer Metadata Extension ID: "
                                      << ext_dict["m"]["ut_metadata"].get<int>()
                                      << std::endl;
                        }
                        break;
                    }
                }
            }
        }

        close(sock);
    } else if (command == "magnet_info") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " magnet_info <magnet-link>"
                      << std::endl;
            return 1;
        }

        std::string mag_link_str = argv[2];
        magnet_link mag_link_struct = parse_magnet_link(mag_link_str);

        // parse peer address from x.pe parameter
        std::string ip;
        int port{};
        if (mag_link_struct.peer_addr_str.empty()) {
            std::tie(ip, port) = get_first_peer(mag_link_struct.tracker_url,
                                                mag_link_struct.info_hash_raw);
        } else {
            size_t colon_pos = mag_link_struct.peer_addr_str.find(':');
            if (colon_pos == std::string::npos) {
                throw std::runtime_error(
                    "Invalid peer address in magnet link!");
            }

            ip = mag_link_struct.peer_addr_str.substr(0, colon_pos);
            port =
                std::stoi(mag_link_struct.peer_addr_str.substr(colon_pos + 1));
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            throw std::runtime_error("Failed to create socket!");
        }

        struct sockaddr_in peer_addr{};
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &peer_addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&peer_addr),
                    sizeof(peer_addr)) < 0) {
            close(sock);
            throw std::runtime_error("Failed to connect to peer!");
        }

        // build handshake with extension support
        // reserved byte 5, bit 4 (0x10) indeicates extension protocol support
        // 00 00 00 00 00 10 00 00
        std::string reserved(8, '\0');
        reserved[5] = 0x10;

        std::string peer_id = "00112233445566778899";
        std::string handshake;
        handshake += static_cast<char>(19);          // protocl len
        handshake += "BitTorrent protocol";          // 19 bytes
        handshake += reserved;                       // reserved bytes
        handshake += mag_link_struct.info_hash_raw;  // 20 bytes
        handshake += peer_id;                        // 20 bytes

        // send handshake
        if (send(sock, handshake.c_str(), handshake.length(), 0) !=
            static_cast<ssize_t>(handshake.length())) {
            close(sock);
            throw std::runtime_error("Failed to send handshake!");
        }

        // receive peer's handshake
        char response[68];
        recv_all(sock, response, 68);

        // check if peer supports extensions
        bool peer_supports_extensions =
            (static_cast<unsigned char>(response[25]) & 0x10) != 0;
        if (!peer_supports_extensions) {
            close(sock);
            std::cerr << "Peer does not support extensions!" << std::endl;
            return 0;
        }
        // send extension handshake (BEP 10)
        // message ID 20 = extended;
        // extended ID 0 = handshake
        // payload: bencoded dict with "m" containing supported extensions
        // we advertise support for `ut_metadata` (BEP 9) with ID 1
        std::string ext_handshake_payload = "d1:md11:ut_metadatai1eee";

        // build extended message:
        //   - msg_id (20)
        //   - ext_id (0)
        //   - payload
        std::string ext_msg;
        ext_msg += static_cast<char>(0);  // extended message ID 0 = handshake
        ext_msg += ext_handshake_payload;

        // send as message with ID 20
        constexpr uint8_t MSG_EXTENDED = 20;
        send_message(sock, MSG_EXTENDED, ext_msg);

        int peer_metadata_id = 0;
        int metadata_size = 0;

        // receive messages until we get the extension handshake
        // peer might send bitfield or other messages first
        while (true) {
            // receive peer's extension handshake
            auto [msg_id, payload] = recv_message(sock);

            // handle bitfield message
            if (msg_id == MSG_BITFIELD) {
                continue;
            }

            if (msg_id == MSG_EXTENDED && !payload.empty()) {
                uint8_t ext_msg_id = static_cast<uint8_t>(payload[0]);

                if (ext_msg_id == 0) {
                    // extension handshake received
                    std::string ext_payload = payload.substr(1);
                    size_t idx = 0;
                    json ext_dict = decode_bencoded_value(ext_payload, idx);

                    // print peer metadata extension ID if available
                    if (ext_dict.contains("m") &&
                        ext_dict["m"].contains("ut_metadata")) {
                        peer_metadata_id =
                            ext_dict["m"]["ut_metadata"].get<int>();
                    }
                    // not inside the "m" dict
                    if (ext_dict.contains("metadata_size")) {
                        metadata_size = ext_dict["metadata_size"].get<int>();
                    }
                    break;
                }
            }
        }

        if (peer_metadata_id == 0 || metadata_size == 0) {
            close(sock);
            std::cerr << "Peer does not support metadata extension!"
                      << std::endl;
            return 0;
        }

        // request metadata pieces
        // metadata split into 16KB pieces
        int num_piece = (metadata_size + 16384 - 1) / 16384;
        std::string metadata;
        metadata.reserve(metadata_size);
        for (int piece = 0; piece < num_piece; ++piece) {
            // send metadata request: d8:msg_typei0e5:piecei<piece>ee
            // {'msg_type': 0, 'piece': 0}
            std::string request_payload =
                "d8:msg_typei0e5:piecei" + std::to_string(piece) + "ee";
            std::string request_msg;
            // extension message id (1 byte)
            // This will be the peer's metadata extension ID, which you received
            // during the extension handshake
            request_msg += static_cast<char>(peer_metadata_id);
            request_msg += request_payload;
            send_message(sock, MSG_EXTENDED, request_msg);

            // receive metadata response
            while (true) {
                auto [msg_id, payload] = recv_message(sock);
                if (msg_id == MSG_BITFIELD) {
                    continue;
                }

                if (msg_id == MSG_EXTENDED && !payload.empty()) {
                    uint8_t ext_msg_id = static_cast<uint8_t>(payload[0]);
                    // our advertised ut_metadata ID is 1
                    if (ext_msg_id == 1) {
                        // parse bencoded header to find where data start
                        std::string ext_payload = payload.substr(1);
                        size_t idx = 0;
                        json response_dict =
                            decode_bencoded_value(ext_payload, idx);
                        int msg_type = response_dict["msg_type"].get<int>();

                        if (msg_type == 1) {
                            // data
                            // data follows the bencoded dict
                            std::string piece_data = ext_payload.substr(idx);
                            metadata += piece_data;
                            break;
                        } else if (msg_type == 2) {
                            // reject
                            close(sock);
                            throw std::runtime_error(
                                "Metadata request rejected!");
                        }
                    }
                }
            }
        }

        close(sock);

        // verify metadata hash
        std::string computed_hash = sha1_hash_raw(metadata);
        if (computed_hash != mag_link_struct.info_hash_raw) {
            throw std::runtime_error("Metadata hash mismatch!");
        }

        // parse info dictionary
        size_t idx = 0;
        json info = decode_bencoded_value(metadata, idx);

        // print results
        std::cout << "Tracker URL: " << mag_link_struct.tracker_url
                  << std::endl;
        std::cout << "Length: " << info["length"].get<int64_t>() << std::endl;
        std::cout << "Info Hash: " << mag_link_struct.info_hash_hex
                  << std::endl;
        std::cout << "Piece Length: " << info["piece length"].get<int64_t>()
                  << std::endl;
        std::cout << "Piece Hashes: " << std::endl;

        auto pieces = info["pieces"].get<std::string>();
        for (size_t i = 0; i < pieces.length(); i += 20) {
            std::ostringstream ss;
            for (size_t j = 0; j < 20; ++j) {
                ss << std::hex << std::setfill('0') << std::setw(2)
                   << static_cast<int>(
                          static_cast<unsigned char>(pieces[i + j]));
                // pieces[i + j] is a char, which is signed on most platforms.
                // a byte like 0xAB is stored as -85 in signed char.
                // when you cast a signed char directly to int, sign extension
                // occurs -85 (0xAB) becomes -85 or 0xFFFFFFAB.
                // then std::cout << std::hex would print ffffffab instead of ab
                // outer cast to int is needed because std::cout << std::hex
                // would print an unsigned char as a character, not as a hex
                // number
            }
            std::cout << ss.str() << std::endl;
        }
    } else if (command == "magnet_download_piece") {
        if (argc < 6 || std::string(argv[2]) != "-o") {
            std::cerr
                << "Usage: " << argv[0]
                << " magnet_download_piece -o <output_path> <magnet_link> "
                   "<piece_index>"
                << std::endl;
            return 1;
        }

        std::string output_path = argv[3];
        std::string mag_link_str = argv[4];
        int piece_index = std::stoi(argv[5]);

        magnet_link mag_link_struct = parse_magnet_link(mag_link_str);

        // parse peer address from x.pe parameter
        std::string ip;
        int port{};
        if (mag_link_struct.peer_addr_str.empty()) {
            std::tie(ip, port) = get_first_peer(mag_link_struct.tracker_url,
                                                mag_link_struct.info_hash_raw);
        } else {
            size_t colon_pos = mag_link_struct.peer_addr_str.find(':');
            if (colon_pos == std::string::npos) {
                throw std::runtime_error(
                    "Invalid peer address in magnet link!");
            }

            ip = mag_link_struct.peer_addr_str.substr(0, colon_pos);
            port =
                std::stoi(mag_link_struct.peer_addr_str.substr(colon_pos + 1));
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            throw std::runtime_error("Failed to create socket!");
        }

        struct sockaddr_in peer_addr{};
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &peer_addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&peer_addr),
                    sizeof(peer_addr)) < 0) {
            close(sock);
            throw std::runtime_error("Failed to connect to peer!");
        }

        // build handshake with extension support
        // reserved byte 5, bit 4 (0x10) indeicates extension protocol support
        // 00 00 00 00 00 10 00 00
        std::string reserved(8, '\0');
        reserved[5] = 0x10;

        std::string peer_id = "00112233445566778899";
        std::string handshake;
        handshake += static_cast<char>(19);          // protocl len
        handshake += "BitTorrent protocol";          // 19 bytes
        handshake += reserved;                       // reserved bytes
        handshake += mag_link_struct.info_hash_raw;  // 20 bytes
        handshake += peer_id;                        // 20 bytes

        // send handshake
        if (send(sock, handshake.c_str(), handshake.length(), 0) !=
            static_cast<ssize_t>(handshake.length())) {
            close(sock);
            throw std::runtime_error("Failed to send handshake!");
        }

        // receive peer's handshake
        char response[68];
        recv_all(sock, response, 68);

        // check if peer supports extensions
        bool peer_supports_extensions =
            (static_cast<unsigned char>(response[25]) & 0x10) != 0;
        if (!peer_supports_extensions) {
            close(sock);
            std::cerr << "Peer does not support extensions!" << std::endl;
            return 0;
        }
        // send extension handshake (BEP 10)
        // message ID 20 = extended;
        // extended ID 0 = handshake
        // payload: bencoded dict with "m" containing supported extensions
        // we advertise support for `ut_metadata` (BEP 9) with ID 1
        std::string ext_handshake_payload = "d1:md11:ut_metadatai1eee";

        // build extended message:
        //   - msg_id (20)
        //   - ext_id (0)
        //   - payload
        std::string ext_msg;
        ext_msg += static_cast<char>(0);  // extended message ID 0 = handshake
        ext_msg += ext_handshake_payload;

        // send as message with ID 20
        constexpr uint8_t MSG_EXTENDED = 20;
        send_message(sock, MSG_EXTENDED, ext_msg);

        int peer_metadata_id = 0;
        int metadata_size = 0;

        // receive messages until we get the extension handshake
        // peer might send bitfield or other messages first
        while (true) {
            // receive peer's extension handshake
            auto [msg_id, payload] = recv_message(sock);

            // handle bitfield message
            if (msg_id == MSG_BITFIELD) {
                continue;
            }

            if (msg_id == MSG_EXTENDED && !payload.empty()) {
                uint8_t ext_msg_id = static_cast<uint8_t>(payload[0]);

                if (ext_msg_id == 0) {
                    // extension handshake received
                    std::string ext_payload = payload.substr(1);
                    size_t idx = 0;
                    json ext_dict = decode_bencoded_value(ext_payload, idx);

                    // print peer metadata extension ID if available
                    if (ext_dict.contains("m") &&
                        ext_dict["m"].contains("ut_metadata")) {
                        peer_metadata_id =
                            ext_dict["m"]["ut_metadata"].get<int>();
                    }
                    // not inside the "m" dict
                    if (ext_dict.contains("metadata_size")) {
                        metadata_size = ext_dict["metadata_size"].get<int>();
                    }
                    break;
                }
            }
        }

        if (peer_metadata_id == 0 || metadata_size == 0) {
            close(sock);
            std::cerr << "Peer does not support metadata extension!"
                      << std::endl;
            return 0;
        }

        // request metadata pieces
        // metadata split into 16KB pieces
        int num_metadata_pieces = (metadata_size + 16384 - 1) / 16384;
        std::string metadata;
        metadata.reserve(metadata_size);
        for (int piece = 0; piece < num_metadata_pieces; ++piece) {
            // send metadata request: d8:msg_typei0e5:piecei<piece>ee
            // {'msg_type': 0, 'piece': 0}
            std::string request_payload =
                "d8:msg_typei0e5:piecei" + std::to_string(piece) + "ee";
            std::string request_msg;
            // extension message id (1 byte)
            // This will be the peer's metadata extension ID, which you received
            // during the extension handshake
            request_msg += static_cast<char>(peer_metadata_id);
            request_msg += request_payload;
            send_message(sock, MSG_EXTENDED, request_msg);

            // receive metadata response
            while (true) {
                auto [msg_id, payload] = recv_message(sock);
                if (msg_id == MSG_BITFIELD) {
                    continue;
                }

                if (msg_id == MSG_EXTENDED && !payload.empty()) {
                    uint8_t ext_msg_id = static_cast<uint8_t>(payload[0]);
                    // our advertised ut_metadata ID is 1
                    if (ext_msg_id == 1) {
                        // parse bencoded header to find where data start
                        std::string ext_payload = payload.substr(1);
                        size_t idx = 0;
                        json response_dict =
                            decode_bencoded_value(ext_payload, idx);
                        int msg_type = response_dict["msg_type"].get<int>();

                        if (msg_type == 1) {
                            // data
                            // data follows the bencoded dict
                            std::string piece_data = ext_payload.substr(idx);
                            metadata += piece_data;
                            break;
                        } else if (msg_type == 2) {
                            // reject
                            close(sock);
                            throw std::runtime_error(
                                "Metadata request rejected!");
                        }
                    }
                }
            }
        }

        // verify metadata hash
        std::string computed_hash = sha1_hash_raw(metadata);
        if (computed_hash != mag_link_struct.info_hash_raw) {
            throw std::runtime_error("Metadata hash mismatch!");
        }

        // parse info dictionary
        size_t idx = 0;
        json info = decode_bencoded_value(metadata, idx);

        int64_t total_len = info["length"].get<int64_t>();
        int64_t piece_len = info["piece length"].get<int64_t>();
        int num_pieces = (total_len + piece_len - 1) / piece_len;

        if (piece_index < 0 || piece_index >= num_pieces) {
            close(sock);
            throw std::runtime_error("Invalid piece index!");
        }

        int this_piece_len = piece_len;
        if (piece_index == num_pieces - 1) {
            this_piece_len = total_len - (piece_index * piece_len);
        }

        // get piece hash
        std::string pieces = info["pieces"].get<std::string>();
        std::string piece_hash = pieces.substr(piece_index * 20, 20);

        // send interested and wait for unchoke
        send_message(sock, MSG_INTERESTED);

        while (true) {
            auto [msg_id, payload] = recv_message(sock);
            if (msg_id == MSG_UNCHOKE) {
                break;
            }
        }

        std::string piece_data =
            download_piece(sock, piece_index, this_piece_len, piece_hash);
        close(sock);

        // write to file
        std::ofstream out(output_path, std::ios::binary);
        out.write(piece_data.c_str(), piece_data.size());
        out.close();

        std::cout << "Piece " << piece_index << " downloaded to " << output_path
                  << "." << std::endl;
    } else if (command == "magnet_download") {
        if (argc < 5 || std::string(argv[2]) != "-o") {
            std::cerr << "Usage: " << argv[0]
                      << " magnet_download -o <output_path> <magnet_link>"
                      << std::endl;
            return 1;
        }

        std::string output_path = argv[3];
        std::string mag_link_str = argv[4];

        magnet_link mag_link_struct = parse_magnet_link(mag_link_str);

        // parse peer address from x.pe parameter
        std::string ip;
        int port{};
        if (mag_link_struct.peer_addr_str.empty()) {
            std::tie(ip, port) = get_first_peer(mag_link_struct.tracker_url,
                                                mag_link_struct.info_hash_raw);
        } else {
            size_t colon_pos = mag_link_struct.peer_addr_str.find(':');
            if (colon_pos == std::string::npos) {
                throw std::runtime_error(
                    "Invalid peer address in magnet link!");
            }

            ip = mag_link_struct.peer_addr_str.substr(0, colon_pos);
            port =
                std::stoi(mag_link_struct.peer_addr_str.substr(colon_pos + 1));
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            throw std::runtime_error("Failed to create socket!");
        }

        struct sockaddr_in peer_addr{};
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &peer_addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&peer_addr),
                    sizeof(peer_addr)) < 0) {
            close(sock);
            throw std::runtime_error("Failed to connect to peer!");
        }

        // build handshake with extension support
        // reserved byte 5, bit 4 (0x10) indeicates extension protocol support
        // 00 00 00 00 00 10 00 00
        std::string reserved(8, '\0');
        reserved[5] = 0x10;

        std::string peer_id = "00112233445566778899";
        std::string handshake;
        handshake += static_cast<char>(19);          // protocl len
        handshake += "BitTorrent protocol";          // 19 bytes
        handshake += reserved;                       // reserved bytes
        handshake += mag_link_struct.info_hash_raw;  // 20 bytes
        handshake += peer_id;                        // 20 bytes

        // send handshake
        if (send(sock, handshake.c_str(), handshake.length(), 0) !=
            static_cast<ssize_t>(handshake.length())) {
            close(sock);
            throw std::runtime_error("Failed to send handshake!");
        }

        // receive peer's handshake
        char response[68];
        recv_all(sock, response, 68);

        // check if peer supports extensions
        bool peer_supports_extensions =
            (static_cast<unsigned char>(response[25]) & 0x10) != 0;
        if (!peer_supports_extensions) {
            close(sock);
            std::cerr << "Peer does not support extensions!" << std::endl;
            return 0;
        }
        // send extension handshake (BEP 10)
        // message ID 20 = extended;
        // extended ID 0 = handshake
        // payload: bencoded dict with "m" containing supported extensions
        // we advertise support for `ut_metadata` (BEP 9) with ID 1
        std::string ext_handshake_payload = "d1:md11:ut_metadatai1eee";

        // build extended message:
        //   - msg_id (20)
        //   - ext_id (0)
        //   - payload
        std::string ext_msg;
        ext_msg += static_cast<char>(0);  // extended message ID 0 = handshake
        ext_msg += ext_handshake_payload;

        // send as message with ID 20
        constexpr uint8_t MSG_EXTENDED = 20;
        send_message(sock, MSG_EXTENDED, ext_msg);

        int peer_metadata_id = 0;
        int metadata_size = 0;

        // receive messages until we get the extension handshake
        // peer might send bitfield or other messages first
        while (true) {
            // receive peer's extension handshake
            auto [msg_id, payload] = recv_message(sock);

            // handle bitfield message
            if (msg_id == MSG_BITFIELD) {
                continue;
            }

            if (msg_id == MSG_EXTENDED && !payload.empty()) {
                uint8_t ext_msg_id = static_cast<uint8_t>(payload[0]);

                if (ext_msg_id == 0) {
                    // extension handshake received
                    std::string ext_payload = payload.substr(1);
                    size_t idx = 0;
                    json ext_dict = decode_bencoded_value(ext_payload, idx);

                    // print peer metadata extension ID if available
                    if (ext_dict.contains("m") &&
                        ext_dict["m"].contains("ut_metadata")) {
                        peer_metadata_id =
                            ext_dict["m"]["ut_metadata"].get<int>();
                    }
                    // not inside the "m" dict
                    if (ext_dict.contains("metadata_size")) {
                        metadata_size = ext_dict["metadata_size"].get<int>();
                    }
                    break;
                }
            }
        }

        if (peer_metadata_id == 0 || metadata_size == 0) {
            close(sock);
            std::cerr << "Peer does not support metadata extension!"
                      << std::endl;
            return 0;
        }

        // request metadata pieces
        // metadata split into 16KB pieces
        int num_metadata_pieces = (metadata_size + 16384 - 1) / 16384;
        std::string metadata;
        metadata.reserve(metadata_size);
        for (int piece = 0; piece < num_metadata_pieces; ++piece) {
            // send metadata request: d8:msg_typei0e5:piecei<piece>ee
            // {'msg_type': 0, 'piece': 0}
            std::string request_payload =
                "d8:msg_typei0e5:piecei" + std::to_string(piece) + "ee";
            std::string request_msg;
            // extension message id (1 byte)
            // This will be the peer's metadata extension ID, which you received
            // during the extension handshake
            request_msg += static_cast<char>(peer_metadata_id);
            request_msg += request_payload;
            send_message(sock, MSG_EXTENDED, request_msg);

            // receive metadata response
            while (true) {
                auto [msg_id, payload] = recv_message(sock);
                if (msg_id == MSG_BITFIELD) {
                    continue;
                }

                if (msg_id == MSG_EXTENDED && !payload.empty()) {
                    uint8_t ext_msg_id = static_cast<uint8_t>(payload[0]);
                    // our advertised ut_metadata ID is 1
                    if (ext_msg_id == 1) {
                        // parse bencoded header to find where data start
                        std::string ext_payload = payload.substr(1);
                        size_t idx = 0;
                        json response_dict =
                            decode_bencoded_value(ext_payload, idx);
                        int msg_type = response_dict["msg_type"].get<int>();

                        if (msg_type == 1) {
                            // data
                            // data follows the bencoded dict
                            std::string piece_data = ext_payload.substr(idx);
                            metadata += piece_data;
                            break;
                        } else if (msg_type == 2) {
                            // reject
                            close(sock);
                            throw std::runtime_error(
                                "Metadata request rejected!");
                        }
                    }
                }
            }
        }

        // verify metadata hash
        std::string computed_hash = sha1_hash_raw(metadata);
        if (computed_hash != mag_link_struct.info_hash_raw) {
            throw std::runtime_error("Metadata hash mismatch!");
        }

        // parse info dictionary
        size_t idx = 0;
        json info = decode_bencoded_value(metadata, idx);

        int64_t total_len = info["length"].get<int64_t>();
        int64_t piece_len = info["piece length"].get<int64_t>();
        int num_pieces = (total_len + piece_len - 1) / piece_len;

        std::string file_data;
        file_data.reserve(total_len);

        // get piece hash
        std::string pieces = info["pieces"].get<std::string>();

        // send interested and wait for unchoke
        send_message(sock, MSG_INTERESTED);

        while (true) {
            auto [msg_id, payload] = recv_message(sock);
            if (msg_id == MSG_UNCHOKE) {
                break;
            }
        }

        for (int i = 0; i < num_pieces; ++i) {
            int this_piece_len = (i == num_pieces - 1)
                                     ? (total_len - (i * piece_len))
                                     : piece_len;
            std::string piece_hash = pieces.substr(i * 20, 20);
            std::string piece_data =
                download_piece(sock, i, this_piece_len, piece_hash);
            file_data += piece_data;
        }

        close(sock);

        // write to file
        std::ofstream out(output_path, std::ios::binary);
        out.write(file_data.c_str(), file_data.size());
        out.close();

        std::cout << "File downloaded to " << output_path << "." << std::endl;
    } else {
        std::cerr << "unknown command: " << command << std::endl;
        return 1;
    }

    return 0;
}
