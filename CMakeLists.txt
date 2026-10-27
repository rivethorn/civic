cmake_minimum_required(VERSION 3.20)
project(Civic VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs)

find_package(LLVM REQUIRED CONFIG)
find_program(CIVIC_CLANG_EXECUTABLE NAMES clang++ clang REQUIRED)
file(TO_CMAKE_PATH "${CIVIC_CLANG_EXECUTABLE}" CIVIC_CLANG_CMAKE_PATH)

add_executable(civic src/main.cpp)
target_compile_features(civic PRIVATE cxx_std_17)
set_target_properties(civic PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(civic SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
separate_arguments(CIVIC_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
list(REMOVE_ITEM CIVIC_LLVM_DEFINITIONS "-D_GNU_SOURCE")
foreach(definition IN LISTS CIVIC_LLVM_DEFINITIONS)
  string(REGEX REPLACE "^-D" "" definition "${definition}")
  target_compile_definitions(civic PRIVATE "${definition}")
endforeach()
target_compile_definitions(civic PRIVATE
  "CIVIC_CLANG_PATH=\"${CIVIC_CLANG_CMAKE_PATH}\""
  "CIVIC_EXECUTABLE_SUFFIX=\"${CMAKE_EXECUTABLE_SUFFIX}\""
)

if(TARGET LLVM)
  target_link_libraries(civic PRIVATE LLVM)
else()
  llvm_map_components_to_libnames(CIVIC_LLVM_LIBS Core Support TargetParser)
  target_link_libraries(civic PRIVATE ${CIVIC_LLVM_LIBS})
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(NAME civic-integration
    COMMAND civic test "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME civic-project-workflow
    COMMAND ${CMAKE_COMMAND}
      "-DCIVIC_EXECUTABLE=$<TARGET_FILE:civic>"
      "-DCIVIC_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      "-DCIVIC_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/project-workflow-test"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/project-workflow.cmake")
  add_test(NAME civic-lsp
    COMMAND ${CMAKE_COMMAND}
      "-DCIVIC_EXECUTABLE=$<TARGET_FILE:civic>"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/lsp-test"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsp.cmake")
endif()

install(TARGETS civic RUNTIME DESTINATION bin)
install(DIRECTORY stdlib/ DESTINATION "${CMAKE_INSTALL_DATADIR}/civic/stdlib")
install(CODE
  "file(WRITE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/civic/.civic-root\" \"\")")
