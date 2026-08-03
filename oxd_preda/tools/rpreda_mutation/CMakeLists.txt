cmake_minimum_required(VERSION 3.16)

add_library(rpreda_mutation_engine STATIC
  MutationKind.h
  MutationEngine.h
  MutationEngine.cpp
)
target_include_directories(rpreda_mutation_engine PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../3rdParty
)
set_property(TARGET rpreda_mutation_engine PROPERTY CXX_STANDARD 17)
set_property(TARGET rpreda_mutation_engine PROPERTY POSITION_INDEPENDENT_CODE ON)

add_executable(rpreda_mutation MutationMain.cpp)
target_link_libraries(rpreda_mutation PRIVATE rpreda_mutation_engine)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  target_link_libraries(rpreda_mutation PRIVATE stdc++fs)
endif()
set_property(TARGET rpreda_mutation PROPERTY CXX_STANDARD 17)
set_target_properties(rpreda_mutation PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
)

if(BUILD_TESTING)
  add_executable(rpreda_mutation_tests MutationEngineTests.cpp)
  target_link_libraries(rpreda_mutation_tests PRIVATE rpreda_mutation_engine)
  set_property(TARGET rpreda_mutation_tests PROPERTY CXX_STANDARD 17)
  add_test(NAME rpreda_mutation_tests COMMAND rpreda_mutation_tests)

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME rpreda_mutation_python_tests
      COMMAND ${Python3_EXECUTABLE} -m unittest -v test_mutation_runner.py
    )
    set_tests_properties(rpreda_mutation_python_tests PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )

    add_test(
      NAME rpreda_semantic_formula_python_tests
      COMMAND ${Python3_EXECUTABLE} -m unittest -v test_semantic_formula.py
    )
    set_tests_properties(rpreda_semantic_formula_python_tests PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )

    add_test(
      NAME rpreda_semantic_mutation_python_tests
      COMMAND ${Python3_EXECUTABLE} -m unittest -v test_semantic_mutation.py
    )
    set_tests_properties(rpreda_semantic_mutation_python_tests PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )
  endif()
endif()
