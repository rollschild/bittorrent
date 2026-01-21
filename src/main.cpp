#include <cctype>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "lib/nlohmann/json.hpp"

using json = nlohmann::json;

json decode_bencoded_value(const std::string& encoded_value, size_t& index) {
    char c = encoded_value[index];
    if (std::isdigit(c)) {
        // Example: "5:hello" -> "hello"
        size_t colon_index = encoded_value.find(':');
        if (colon_index != std::string::npos) {
            std::string number_string =
                encoded_value.substr(index, colon_index);
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
    } else {
        throw std::runtime_error("Unhandled encoded value: " + encoded_value);
    }
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
    } else {
        std::cerr << "unknown command: " << command << std::endl;
        return 1;
    }

    return 0;
}
