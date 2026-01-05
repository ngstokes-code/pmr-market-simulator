add_executable(spsc_ring_test spsc_ring_test.cpp)
target_link_libraries(spsc_ring_test PRIVATE marketsim)
target_include_directories(spsc_ring_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME spsc_ring_test COMMAND spsc_ring_test)

add_executable(order_book_test order_book_test.cpp)
target_link_libraries(order_book_test PRIVATE marketsim)
target_include_directories(order_book_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME order_book_test COMMAND order_book_test)
