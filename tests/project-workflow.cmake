file(REMOVE_RECURSE "${TEST_ROOT}")
set(PROJECT_DIR "${TEST_ROOT}/sample-app")

execute_process(
  COMMAND "${CIVIC_EXECUTABLE}" new "${PROJECT_DIR}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "civic new failed: ${error}")
endif()
if(EXISTS "${PROJECT_DIR}/tests")
  message(FATAL_ERROR "new projects should not create a tests directory by default")
endif()

execute_process(
  COMMAND "${CIVIC_EXECUTABLE}" build
  WORKING_DIRECTORY "${PROJECT_DIR}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "debug project build failed: ${error}")
endif()
if(NOT EXISTS "${PROJECT_DIR}/target/debug/sample-app${CIVIC_EXECUTABLE_SUFFIX}")
  message(FATAL_ERROR "debug executable was not created in target/debug")
endif()

execute_process(
  COMMAND "${CIVIC_EXECUTABLE}" run -- "Civic User"
  WORKING_DIRECTORY "${PROJECT_DIR}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output STREQUAL "Hello, Civic User!\n")
  message(FATAL_ERROR "project run did not forward its argument: ${error}${output}")
endif()

execute_process(
  COMMAND "${CIVIC_EXECUTABLE}" build --release
  WORKING_DIRECTORY "${PROJECT_DIR}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "release project build failed: ${error}")
endif()
if(NOT EXISTS "${PROJECT_DIR}/target/release/sample-app${CIVIC_EXECUTABLE_SUFFIX}")
  message(FATAL_ERROR "release executable was not created in target/release")
endif()

execute_process(
  COMMAND "${CIVIC_EXECUTABLE}" run --manifest-path "${PROJECT_DIR}/civic.toml" -- "Manifest User"
  WORKING_DIRECTORY "${TEST_ROOT}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output STREQUAL "Hello, Manifest User!\n")
  message(FATAL_ERROR "manifest-path project run failed: ${error}${output}")
endif()

execute_process(
  COMMAND "${CIVIC_EXECUTABLE}" test
  WORKING_DIRECTORY "${PROJECT_DIR}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output STREQUAL "0 tests found\n")
  message(FATAL_ERROR "empty project test command failed: ${error}${output}")
endif()

set(OVERRIDE_ROOT "${TEST_ROOT}/override-root")
file(MAKE_DIRECTORY "${OVERRIDE_ROOT}/stdlib/std")
file(WRITE "${OVERRIDE_ROOT}/stdlib/std/custom.cv"
  "#header custom\nint custom_value() { return 73; }\n")
file(WRITE "${PROJECT_DIR}/src/main.cv"
  "#include <std/custom>\nint main() { assert(custom_value() == 73); return 0; }\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "CIVIC_ROOT=${OVERRIDE_ROOT}"
    "${CIVIC_EXECUTABLE}" run --manifest-path "${PROJECT_DIR}/civic.toml"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "CIVIC_ROOT override was not used: ${error}${output}")
endif()

set(INSTALL_ROOT "${TEST_ROOT}/install")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${CIVIC_BUILD_DIR}" --prefix "${INSTALL_ROOT}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Civic install failed: ${error}${output}")
endif()
if(NOT EXISTS "${INSTALL_ROOT}/share/civic/.civic-root" OR
   NOT EXISTS "${INSTALL_ROOT}/share/civic/stdlib/std/math.cv")
  message(FATAL_ERROR "install did not put root marker and standard library together")
endif()
file(WRITE "${PROJECT_DIR}/src/main.cv"
  "#include <std/math>\nint main() { assert(abs(-73) == 73); return 0; }\n")
execute_process(
  COMMAND "${INSTALL_ROOT}/bin/civic${CIVIC_EXECUTABLE_SUFFIX}" run
    --manifest-path "${PROJECT_DIR}/civic.toml"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "installed compiler could not locate its Civic root: ${error}${output}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
