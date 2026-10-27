# Compile module tests against the same production runtime used by seekdb.
find_library(SEEKDB_GTEST_LIBRARY gtest PATHS "${DEP_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
add_custom_target(pretest)
foreach(module IN LISTS SEEKDB_PRETEST_MODULES)
  string(TOUPPER "${module}" module_upper)
  set(prefix "SEEKDB_PRETEST_${module_upper}")
  set(target "${${prefix}_TARGET}")
  file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS "${module}/test_*.cpp")
  add_executable(${target} EXCLUDE_FROM_ALL all_tests_main.cpp ${test_sources})
  target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}")
  target_link_libraries(${target} PRIVATE oceanbase "${SEEKDB_GTEST_LIBRARY}" pthread dl)
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${module}")
  add_dependencies(pretest ${target})
  math(EXPR last_shard "${${prefix}_SHARD_COUNT} - 1")
  foreach(shard RANGE ${last_shard})
    add_test(NAME ${target}_shard_${shard} COMMAND ${target})
    set_tests_properties(${target}_shard_${shard} PROPERTIES ENVIRONMENT
      "GTEST_TOTAL_SHARDS=${${prefix}_SHARD_COUNT};GTEST_SHARD_INDEX=${shard}")
  endforeach()
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/run_tests.sh"
  "#!/bin/sh\nexec ctest --test-dir '${CMAKE_BINARY_DIR}' \"$@\"\n")
file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/run_tests.sh" PERMISSIONS
  OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
