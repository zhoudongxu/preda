cmake_minimum_required(VERSION 3.16)

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME rpreda_coverage_scalability_python_tests
      COMMAND ${Python3_EXECUTABLE} -m unittest -v test_evaluation.py
    )
    set_tests_properties(rpreda_coverage_scalability_python_tests PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;RPREDA_REPO_ROOT=${CMAKE_SOURCE_DIR}"
    )
  endif()
endif()

