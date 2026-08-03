cmake_minimum_required(VERSION 3.16)

add_executable(rpreda_analysis_driver AnalysisDriver.cpp)
target_link_libraries(rpreda_analysis_driver PRIVATE transpiler)
target_compile_definitions(
  rpreda_analysis_driver
  PRIVATE
  RPREDA_ENABLE_ANALYSIS_PROFILING=1
)
if(RPREDA_ENABLE_Z3)
  if(RPREDA_Z3_LIBRARY)
    target_link_libraries(
      rpreda_analysis_driver
      PRIVATE
      "${RPREDA_Z3_LIBRARY}"
    )
  elseif(TARGET z3::libz3)
    target_link_libraries(rpreda_analysis_driver PRIVATE z3::libz3)
  endif()
endif()
set_property(TARGET rpreda_analysis_driver PROPERTY CXX_STANDARD 17)

if(RPREDA_ENABLE_RUNTIME_TRACE)
  target_compile_definitions(
    rpreda_analysis_driver
    PRIVATE
    RPREDA_ENABLE_RUNTIME_TRACE=1
  )
endif()
if(RPREDA_ENABLE_RUNTIME_OPTIMIZATION)
  target_compile_definitions(
    rpreda_analysis_driver
    PRIVATE
    RPREDA_ENABLE_RUNTIME_OPTIMIZATION=1
  )
endif()
if(RPREDA_ENABLE_Z3)
  target_compile_definitions(
    rpreda_analysis_driver
    PRIVATE
    RPREDA_ENABLE_Z3=1
  )
endif()
if(RPREDA_ENABLE_BOUND_RELAY_MANIFEST)
  target_compile_definitions(
    rpreda_analysis_driver
    PRIVATE
    RPREDA_ENABLE_BOUND_RELAY_MANIFEST=1
  )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  target_link_libraries(rpreda_analysis_driver PRIVATE stdc++fs)
endif()

set_target_properties(rpreda_analysis_driver PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
)
set(RPREDA_ANALYSIS_DRIVER_Z3_RUNTIME_DIR "")
if(RPREDA_ENABLE_Z3 AND RPREDA_Z3_LIBRARY)
  get_filename_component(
    RPREDA_ANALYSIS_DRIVER_Z3_RUNTIME_DIR
    "${RPREDA_Z3_LIBRARY}"
    DIRECTORY
  )
endif()
if(UNIX AND NOT APPLE)
  set_target_properties(rpreda_analysis_driver PROPERTIES
    BUILD_RPATH "$ORIGIN;${RPREDA_ANALYSIS_DRIVER_Z3_RUNTIME_DIR}"
    INSTALL_RPATH "$ORIGIN;${RPREDA_ANALYSIS_DRIVER_Z3_RUNTIME_DIR}"
  )
elseif(APPLE)
  set_target_properties(rpreda_analysis_driver PROPERTIES
    BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path"
  )
endif()

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME rpreda_analysis_driver_tests
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_analysis_driver.py
        --driver $<TARGET_FILE:rpreda_analysis_driver>
        --fixture
          ${CMAKE_CURRENT_SOURCE_DIR}/../../transpiler/testcase/relay_protocol/named_address.prd
        --runtime-trace-enabled $<BOOL:${RPREDA_ENABLE_RUNTIME_TRACE}>
        --runtime-optimization-enabled
          $<BOOL:${RPREDA_ENABLE_RUNTIME_OPTIMIZATION}>
        --z3-enabled $<BOOL:${RPREDA_ENABLE_Z3}>
        --bound-manifest-enabled
          $<BOOL:${RPREDA_ENABLE_BOUND_RELAY_MANIFEST}>
    )
    set_tests_properties(rpreda_analysis_driver_tests PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )
    if(UNIX AND NOT APPLE)
      set_tests_properties(rpreda_analysis_driver_tests PROPERTIES
        ENVIRONMENT
          "PYTHONDONTWRITEBYTECODE=1;LD_LIBRARY_PATH=$<TARGET_FILE_DIR:transpiler>:${RPREDA_ANALYSIS_DRIVER_Z3_RUNTIME_DIR}:$ENV{LD_LIBRARY_PATH}"
      )
    elseif(APPLE)
      set_tests_properties(rpreda_analysis_driver_tests PROPERTIES
        ENVIRONMENT
          "PYTHONDONTWRITEBYTECODE=1;DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:transpiler>:${RPREDA_ANALYSIS_DRIVER_Z3_RUNTIME_DIR}:$ENV{DYLD_LIBRARY_PATH}"
      )
    endif()
  endif()
endif()
