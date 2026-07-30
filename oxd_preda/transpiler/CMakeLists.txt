# CMakeList.txt: preda_preoject
#
cmake_minimum_required(VERSION 3.16)

# Enable Hot Reload for MSVC compilers if supported.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

include(../CMake_rules.txt)
project("transpiler")

option(RPREDA_ENABLE_Z3 "Enable the optional R-PREDA Z3 solver backend" OFF)

set(RELAY_Z3_FILES)
set(RPREDA_Z3_TARGET)
set(RPREDA_Z3_RUNTIME_DIR)
if(RPREDA_ENABLE_Z3)
  find_package(Z3 CONFIG QUIET)
  if(TARGET z3::libz3)
    set(RPREDA_Z3_TARGET z3::libz3)
    set(RPREDA_Z3_RUNTIME_DIR "$<TARGET_FILE_DIR:z3::libz3>")
  else()
    find_path(RPREDA_Z3_INCLUDE_DIR
      NAMES z3++.h
      HINTS ${Z3_ROOT} ENV Z3_ROOT
      PATH_SUFFIXES include
    )
    find_library(RPREDA_Z3_LIBRARY
      NAMES z3 libz3
      HINTS ${Z3_ROOT} ENV Z3_ROOT
      PATH_SUFFIXES lib lib64
    )
    if(NOT RPREDA_Z3_INCLUDE_DIR OR NOT RPREDA_Z3_LIBRARY)
      message(FATAL_ERROR
        "RPREDA_ENABLE_Z3=ON requires the Z3 C++ headers and library. "
        "Set Z3_DIR to a Z3 CMake package directory, add its prefix to "
        "CMAKE_PREFIX_PATH, or set Z3_ROOT to the installation prefix."
      )
    endif()
    add_library(RPreda::Z3 INTERFACE IMPORTED)
    set_target_properties(RPreda::Z3 PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${RPREDA_Z3_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES "${RPREDA_Z3_LIBRARY}"
    )
    set(RPREDA_Z3_TARGET RPreda::Z3)
    get_filename_component(
      RPREDA_Z3_RUNTIME_DIR
      "${RPREDA_Z3_LIBRARY}"
      DIRECTORY
    )
  endif()

  set(RELAY_Z3_FILES
    ./relay_protocol/refinement/solver/z3/Z3FormulaEncoder.h
    ./relay_protocol/refinement/solver/z3/Z3FormulaEncoder.cpp
    ./relay_protocol/refinement/solver/z3/Z3SymbolEncoder.h
    ./relay_protocol/refinement/solver/z3/Z3SymbolEncoder.cpp
    ./relay_protocol/refinement/solver/z3/Z3RelaySolver.h
    ./relay_protocol/refinement/solver/z3/Z3RelaySolver.cpp
    ./relay_protocol/refinement/solver/z3/Z3ModelDecoder.h
    ./relay_protocol/refinement/solver/z3/Z3ModelDecoder.cpp
  )
endif()

# ############### build transpiler ###############
file(GLOB TRANSPILER_SOURCES
  ./*.cpp
  ./*.h
)
file(GLOB TRANSPILER_FILES
  ./transpiler/*.cpp
  ./transpiler/*.h
)
set(RELAY_PROTOCOL_FILES
  ./relay_protocol/RelayExprIR.h
  ./relay_protocol/RelayProtocolIR.h
  ./relay_protocol/RelayProtocolCollector.h
  ./relay_protocol/RelayProtocolCollector.cpp
  ./relay_protocol/RelayManifestEmitter.h
  ./relay_protocol/RelayManifestEmitter.cpp
  ./relay_protocol/analysis/RelayCardinalityExpr.h
  ./relay_protocol/analysis/RelayExpressionDependency.h
  ./relay_protocol/analysis/RelayDependencyAnalyzer.h
  ./relay_protocol/analysis/RelayDependencyAnalyzer.cpp
  ./relay_protocol/analysis/RelayDepthExpr.h
  ./relay_protocol/analysis/RelayProtocolSummary.h
  ./relay_protocol/analysis/RelaySummaryBuilder.h
  ./relay_protocol/analysis/RelaySummaryBuilder.cpp
  ./relay_protocol/analysis/RelaySummaryEmitter.h
  ./relay_protocol/analysis/RelaySummaryEmitter.cpp
  ./relay_protocol/refinement/RelayFormulaIR.h
  ./relay_protocol/refinement/RelayFormulaBuilder.h
  ./relay_protocol/refinement/RelayFormulaBuilder.cpp
  ./relay_protocol/refinement/RelayRefinementSymbol.h
  ./relay_protocol/refinement/RelayRefinementSymbolTable.h
  ./relay_protocol/refinement/RelayRefinementSymbolTable.cpp
  ./relay_protocol/refinement/RelayConstraint.h
  ./relay_protocol/refinement/RelayConstraintGenerator.h
  ./relay_protocol/refinement/RelayConstraintGenerator.cpp
  ./relay_protocol/refinement/RelayProofObligation.h
  ./relay_protocol/refinement/RelayRefinementEmitter.h
  ./relay_protocol/refinement/RelayRefinementEmitter.cpp
  ./relay_protocol/refinement/solver/RelaySolverBackend.h
  ./relay_protocol/refinement/solver/RelaySolverResult.h
  ./relay_protocol/refinement/solver/RelayProofRunner.h
  ./relay_protocol/refinement/solver/RelayProofRunner.cpp
  ${RELAY_Z3_FILES}
)
file(GLOB ANTLR_GEN
  ./antlr_generated/*.cpp
  ./antlr_generated/*.h
)
file(GLOB CRYSTILITY_ANTLR_GEN
  ./antlr_generated/crystality/*.cpp
  ./antlr_generated/crystality/*.h
)
source_group("transpiler" FILES ${TRANSPILER_FILES})
source_group("relay_protocol" FILES ${RELAY_PROTOCOL_FILES})
source_group("antlr_generated" FILES ${ANTLR_GEN})
source_group("antlr_generated/crystality" FILES ${CRYSTILITY_ANTLR_GEN})
add_library(transpiler SHARED ${TRANSPILER_SOURCES} ${TRANSPILER_FILES} ${RELAY_PROTOCOL_FILES} ${ANTLR_GEN} ${CRYSTILITY_ANTLR_GEN})
if(RPREDA_ENABLE_RUNTIME_TRACE)
  target_compile_definitions(transpiler PRIVATE RPREDA_ENABLE_RUNTIME_TRACE=1)
endif()
if(RPREDA_ENABLE_Z3)
  target_compile_definitions(transpiler PRIVATE RPREDA_ENABLE_Z3=1)
  target_link_libraries(transpiler PRIVATE ${RPREDA_Z3_TARGET})
endif()

# find_library(ANTLR4 antlr4-runtime ${CMAKE_SOURCE_DIR}/bin/bin_${CMAKE_BUILD_TYPE}/$<0:>)
if(WIN32)
  add_dependencies(transpiler antlr4-runtime)
  target_link_directories(transpiler PRIVATE
    ../../SFC/libs/win

    # ${CMAKE_SOURCE_DIR}/bin/bin_$<CONFIG>/$<0:>
  )
  target_link_libraries(transpiler PRIVATE
    antlr4-runtime
  )
  target_compile_definitions(transpiler PRIVATE ANTLR4CPP_STATIC _MBCS)
  win_sdk_rule(transpiler)

  if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "")
    set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} /DEBUG:FULL /INCREMENTAL")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE_INIT} /DEBUG:FULL /OPT:REF /OPT:ICF /LTCG:incremental")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi /W3 /sdl /MP /Gy /Gd /Oi /FC /permissive- /GL")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /ZI /JMC /sdl /MP /permissive- /FC")
  endif()

  set_property(TARGET transpiler PROPERTY CXX_STANDARD 17)
elseif(UNIX AND NOT APPLE)
  add_dependencies(transpiler antlr4_shared)
  target_include_directories(transpiler PRIVATE
    ../../SFC/libs/linux
  )
  target_compile_options(transpiler PRIVATE -fPIC -pthread -ldl -lX11)
  target_link_directories(transpiler PRIVATE ${CMAKE_BINARY_DIR}/bin)
  target_link_libraries(transpiler PRIVATE
    antlr4_shared
    ${IPP_LIB}
  )
  set_target_properties(transpiler PROPERTIES
    PREFIX ""
    SUFFIX ".so"
  )
  add_custom_command(TARGET transpiler POST_BUILD
  	COMMENT "Copy antlr4_shared library to libdirectory......."
  	COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:antlr4_shared> ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
  	COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_LINKER_FILE:antlr4_shared> ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
	)
elseif(APPLE)
  file(GLOB MAC_SOURCE_FILE ../../SFC/libs/mac/*.a)
  add_dependencies(transpiler antlr4_shared)
  target_link_libraries(transpiler PRIVATE
    antlr4_shared
    ${MAC_SOURCE_FILE}
  )

  # target_sources(transpiler PRIVATE ${MAC_SOURCE_FILE})
  set_target_properties(transpiler PROPERTIES
    PREFIX ""
    SUFFIX ".dylib"
  )
  add_custom_command(TARGET transpiler POST_BUILD
	COMMENT "Copy antlr4_shared library to libdirectory......."
	COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:antlr4_shared> ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
	COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_LINKER_FILE:antlr4_shared> ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
)
  mac_link_rule(transpiler)
endif()

target_include_directories(transpiler PRIVATE
  ../3rdParty/antlr4/runtime/Cpp/runtime/src
)

target_sources(transpiler PRIVATE ${TRANSPILER_SOURCES})
set_target_properties(transpiler PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
)

# target_link_libraries(transpiler PRIVATE antlr4-runtime)
set_property(TARGET transpiler PROPERTY CMAKE_POLICY_DEFAULT_CMP0079 NEW)

if(BUILD_TESTING)
  add_executable(relay_protocol_ir_tests
    ./test/RelayProtocolIRTests.cpp
  )
  target_link_libraries(relay_protocol_ir_tests PRIVATE transpiler)
  if(RPREDA_ENABLE_RUNTIME_TRACE)
    target_compile_definitions(
      relay_protocol_ir_tests
      PRIVATE
      RPREDA_ENABLE_RUNTIME_TRACE=1
    )
  endif()
  if(RPREDA_ENABLE_Z3)
    target_compile_definitions(relay_protocol_ir_tests PRIVATE RPREDA_ENABLE_Z3=1)
    target_link_libraries(relay_protocol_ir_tests PRIVATE ${RPREDA_Z3_TARGET})
  endif()
  set_property(TARGET relay_protocol_ir_tests PROPERTY CXX_STANDARD 17)
  add_test(
    NAME relay_protocol_ir
    COMMAND relay_protocol_ir_tests
      ${CMAKE_CURRENT_SOURCE_DIR}/testcase/relay_protocol
  )
  if(UNIX AND NOT APPLE)
    if(RPREDA_ENABLE_Z3)
      set_tests_properties(relay_protocol_ir PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:transpiler>:${RPREDA_Z3_RUNTIME_DIR}"
      )
    else()
      set_tests_properties(relay_protocol_ir PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:transpiler>"
      )
    endif()
  elseif(APPLE)
    if(RPREDA_ENABLE_Z3)
      set_tests_properties(relay_protocol_ir PROPERTIES
        ENVIRONMENT "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:transpiler>:${RPREDA_Z3_RUNTIME_DIR}"
      )
    else()
      set_tests_properties(relay_protocol_ir PROPERTIES
        ENVIRONMENT "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:transpiler>"
      )
    endif()
  endif()
endif()
