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

target_link_libraries(dbplayground_lib glog::glog _zlib)
