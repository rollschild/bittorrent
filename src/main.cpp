#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "lib/nlohmann/json.hpp"

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

std::string perform_handshake(const std::string& ip, int port,
                              const std::string& info_hash,
                              const std::string& peer_id) {
    // create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        throw std::runtime_error("Failed to create socket!");
    }

    // connect to peer
    struct sockaddr_in peer_addr{};
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
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
    size_t total_recvd = 0;
    while (total_recvd < 68) {
        ssize_t recvd = recv(sock, response + total_recvd, 68 - total_recvd, 0);
        if (recvd < 0) {
            close(sock);
            throw std::runtime_error("Failed to receive handshake!");
        }
        total_recvd += recvd;
    }

    close(sock);

    // extract peer ID
    return std::string(response + 48, 20);
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
        std::string info_hash_raw = sha1_hash_raw(info_bencoded);  // why raw?
        std::string peer_id = "00112233445566778899";

        // perform handshake
        std::string rcvd_peer_id =
            perform_handshake(ip, port, info_hash_raw, peer_id);

        std::cout << "Peer ID: ";
        for (unsigned char c : rcvd_peer_id) {
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<int>(c);
        }
        std::cout << std::endl;
    } else {
        std::cerr << "unknown command: " << command << std::endl;
        return 1;
    }

    return 0;
}
