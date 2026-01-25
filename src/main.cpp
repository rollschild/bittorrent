#include <openssl/sha.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
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
    } else {
        std::cerr << "unknown command: " << command << std::endl;
        return 1;
    }

    return 0;
}
