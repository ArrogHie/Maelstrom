include(FetchContent)

set(CMAKE_CXX_CLANG_TIDY "")

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.18.0          # 固定到当前最新稳定版
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(maelstrom_tests test_order_book.cpp test_order_book_w2.cpp test_matching_engine_w3.cpp test_matching_engine_w4.cpp)
target_link_libraries(maelstrom_tests PRIVATE
    maelstrom::maelstrom
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(maelstrom_tests)   # 让 ctest 自动发现每个 TEST