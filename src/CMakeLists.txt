file(GLOB_RECURSE DBPLAYGROUND_SOURCES ${PROJECT_SOURCE_DIR}/src/*/*.cpp ${PROJECT_SOURCE_DIR}/src/*/*/*.cpp Core/MiniKV.cpp)
list(REMOVE_ITEM DBPLAYGROUND_SOURCES ${PROJECT_SOURCE_DIR}/src/Container/LinearProbeHashTable.h ${PROJECT_SOURCE_DIR}/src/Container/LinearProbeHashTable.cpp)
list(REMOVE_ITEM DBPLAYGROUND_SOURCES ${PROJECT_SOURCE_DIR}/src/Storage/Page/HashTablePage.h ${PROJECT_SOURCE_DIR}/src/Storage/Page/HashTablePage.cpp)
list(REMOVE_ITEM DBPLAYGROUND_SOURCES ${PROJECT_SOURCE_DIR}/src/Storage/Page/HashTableHeaderPage.h ${PROJECT_SOURCE_DIR}/src/Storage/Page/HashTableHeaderPage.cpp)

if(DBPLAYGROUND_BUILD_SHARED)
    add_library(dbplayground_lib SHARED ${DBPLAYGROUND_SOURCES})
else()
    add_library(dbplayground_lib STATIC ${DBPLAYGROUND_SOURCES})
endif()

target_include_directories(dbplayground_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)

# S3 adapters (src/Cloud/S3) need an HTTP client and crypto for AWS SigV4.
# libcurl is the system copy; OpenSSL supplies SHA256/HMAC. On macOS OpenSSL is
# keg-only under Homebrew, so hint its prefix before find_package.
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    foreach(_ssl_prefix /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
        if(EXISTS ${_ssl_prefix})
            set(OPENSSL_ROOT_DIR ${_ssl_prefix})
            break()
        endif()
    endforeach()
endif()
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

# NB: link the curl library file directly rather than the CURL::libcurl imported
# target. That target's INTERFACE include dir is the macOS SDK's usr/include,
# which -- when added as -I to every source -- breaks Homebrew LLVM libc++'s
# header ordering (<cwchar> can't find <wchar.h>). <curl/curl.h> resolves via
# the default SDK sysroot without an explicit include dir.
target_link_libraries(dbplayground_lib glog::glog _zlib ${CURL_LIBRARIES} OpenSSL::Crypto)
