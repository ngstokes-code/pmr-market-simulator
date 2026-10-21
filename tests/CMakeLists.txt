function(msim_test name)
  cmake_parse_arguments(T "" "" "LIBS;ARGS" ${ARGN})
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${T_LIBS} msim_flags)
  add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
endfunction()

msim_test(order_book_test LIBS marketsim_core)
msim_test(flat_hash_test LIBS marketsim_core)
msim_test(tick_test LIBS marketsim_core)
if(MSIM_SANITIZE)
  msim_test(differential_test LIBS marketsim_verify ARGS --seeds 0:0)
else()
  msim_test(differential_test LIBS marketsim_verify)
endif()
msim_test(metamorphic_test LIBS marketsim_verify)
msim_test(exhaustive_test LIBS marketsim_verify)
msim_test(workload_test LIBS marketsim_verify)
msim_test(itch_test LIBS marketsim_core)
if(MSIM_SANITIZE)
  msim_test(feed_book_test LIBS marketsim_core ARGS --bounded)
else()
  msim_test(feed_book_test LIBS marketsim_core)
endif()
msim_test(ref_map_test LIBS marketsim_core)
msim_test(replay_test LIBS marketsim_replay)

add_executable(fuzz_driver ${PROJECT_SOURCE_DIR}/fuzz/fuzz_driver.cpp
                           ${PROJECT_SOURCE_DIR}/fuzz/differential_fuzz.cpp)
target_link_libraries(fuzz_driver PRIVATE marketsim_verify msim_flags)
add_test(NAME fuzz_driver COMMAND fuzz_driver --runs 20000)

# Hand-kept regression traces: each must replay with both books agreeing.
file(GLOB msim_traces ${CMAKE_CURRENT_SOURCE_DIR}/traces/*.trace)
foreach(trace ${msim_traces})
  get_filename_component(name ${trace} NAME_WE)
  add_test(NAME trace_${name} COMMAND diff_replay ${trace} --quiet)
endforeach()

msim_test(obligation_test LIBS marketsim_verify)
if(MSIM_SANITIZE MATCHES "thread")
  add_executable(replay_race_probe replay_race_probe.cpp)
  target_link_libraries(replay_race_probe PRIVATE msim_flags Threads::Threads)
endif()

msim_test(closed_loop_test LIBS marketsim_core)

msim_test(volume_test LIBS marketsim_core)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME volume_cli_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/volume_cli_test.py $<TARGET_FILE:itch_volume>
  ${PROJECT_SOURCE_DIR}/scripts/check_volume.py)
add_test(NAME replay_boundary_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/replay_boundary_test.py $<TARGET_FILE:itch_replay>)
add_test(NAME release_regression_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/release_regression_test.py
  ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/scripts/run.py)

add_test(NAME research_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/research_test.py $<TARGET_FILE:itch_observe>
  ${PROJECT_SOURCE_DIR}/scripts $<TARGET_FILE:itch_volume>)

add_test(NAME benchmark_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/benchmark_test.py $<TARGET_FILE:book_bench>
  $<TARGET_FILE:structure_bench> $<TARGET_FILE:replay_bench>
  ${PROJECT_SOURCE_DIR}/scripts)

# Labels describe obligations; inventory.json is an independently checked list.
set_tests_properties(order_book_test tick_test PROPERTIES LABELS matching_policy_and_capacity)
set_tests_properties(differential_test metamorphic_test exhaustive_test workload_test
  fuzz_driver obligation_test trace_capacity-exact-fill trace_freed-slot
  trace_sweep-and-rest PROPERTIES LABELS independent_matching)
set_tests_properties(flat_hash_test ref_map_test PROPERTIES LABELS concrete_storage)
set_tests_properties(itch_test PROPERTIES LABELS itch_framing)
set_tests_properties(feed_book_test closed_loop_test PROPERTIES LABELS feed_reconstruction)
set_tests_properties(replay_test replay_boundary_test PROPERTIES LABELS replay_ownership_and_failure)
set_tests_properties(volume_test volume_cli_test research_test PROPERTIES LABELS accounting_and_research)
set_tests_properties(release_regression_test benchmark_test PROPERTIES LABELS evidence_integrity)
