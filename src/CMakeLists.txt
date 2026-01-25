add_executable(main main.cpp)
target_link_libraries(main PRIVATE OpenSSL::Crypto)

target_sources(main PRIVATE main.cpp)
