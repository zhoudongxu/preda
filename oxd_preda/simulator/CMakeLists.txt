# CMakeList.txt: preda_preoject
#
cmake_minimum_required(VERSION 3.8)

# Enable Hot Reload for MSVC compilers if supported.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

include(../CMake_rules.txt)
project(chain_simulator)

# add options for macros
option(ENABLE_VERIFY_SIG OFF)
option(ENABLE_AFFINITY_SET OFF)

# ############## build chain_simulator #################
file(GLOB_RECURSE CHAIN_SIMULATOR_FILE
  ../../SFC/core/ext/bignum/big_num.*
  ../../SFC/core/ext/bignum/precision_num.*
  ../../SFC/core/ext/botan/botan.*
  ../../SFC/core/ext/lib_inc.c
  ../../SFC/core/ext/bignum/ttmath/ttmathuint_x86_64_msvc.asm
  ../../SFC/essentials.cpp
  main.cpp
)
file(GLOB_RECURSE TEST EXCLUDE contracts/scope.*
  contracts/*.prdts
  contracts/*.prd
)
file(GLOB SIMULATOR
	chain_simu.h
	chain_simu.cpp
	core_contracts.h
	core_contracts.cpp
	shard_data.h
	shard_data.cpp
	simu_global.h
	simu_global.cpp
	simu_script.h
	simu_script.cpp
	simu_shard.h
	simu_shard.cpp
)

set(RELAY_PLAN)
set(RELAY_PLAN_TEST)
if(RPREDA_ENABLE_BOUND_RELAY_MANIFEST)
  file(GLOB RELAY_PLAN
    ../runtime/relay_plan/*.h
    ../runtime/relay_plan/*.cpp
  )
  set(
    RELAY_PLAN_TEST
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/relay_plan/RelayPlanTests.cpp
  )
  list(REMOVE_ITEM RELAY_PLAN ${RELAY_PLAN_TEST})
  find_package(Threads REQUIRED)
endif()

set(RELAY_RUNTIME_TRACE)
set(RELAY_RUNTIME_TRACE_TEST)
set(RELAY_TRACE_FAULT_INJECTOR
  ${CMAKE_CURRENT_SOURCE_DIR}/relay_trace/RelayTraceFaultInjector.h
  ${CMAKE_CURRENT_SOURCE_DIR}/relay_trace/RelayTraceFaultInjector.cpp
)
set(RELAY_TRACE_FAULT_INJECTOR_TEST
  ${CMAKE_CURRENT_SOURCE_DIR}/relay_trace/RelayTraceFaultInjectorTests.cpp
)
if(RPREDA_ENABLE_RUNTIME_TRACE)
  file(GLOB RELAY_RUNTIME_TRACE
    relay_trace/*.h
    relay_trace/*.cpp
  )
  set(
    RELAY_RUNTIME_TRACE_TEST
    ${CMAKE_CURRENT_SOURCE_DIR}/relay_trace/RelayRuntimeTraceTests.cpp
  )
  list(REMOVE_ITEM RELAY_RUNTIME_TRACE
    ${RELAY_RUNTIME_TRACE_TEST}
    ${RELAY_TRACE_FAULT_INJECTOR}
    ${RELAY_TRACE_FAULT_INJECTOR_TEST}
  )
  if(RPREDA_ENABLE_TRACE_FAULT_INJECTION)
    list(APPEND RELAY_RUNTIME_TRACE ${RELAY_TRACE_FAULT_INJECTOR})
    list(APPEND RELAY_RUNTIME_TRACE_TEST ${RELAY_TRACE_FAULT_INJECTOR_TEST})
  endif()
endif()

set(RELAY_OPTIMIZATION)
set(RELAY_OPTIMIZATION_TEST)
set(PENDING_TXNS_BATCH_TEST)
if(RPREDA_ENABLE_RUNTIME_OPTIMIZATION)
  file(GLOB RELAY_OPTIMIZATION
    relay_optimization/*.h
    relay_optimization/*.cpp
  )
  set(
    RELAY_OPTIMIZATION_TEST
    ${CMAKE_CURRENT_SOURCE_DIR}/relay_optimization/RelayOptimizationTests.cpp
  )
  set(
    PENDING_TXNS_BATCH_TEST
    ${CMAKE_CURRENT_SOURCE_DIR}/relay_optimization/PendingTxnsBatchTests.cpp
  )
  list(
    REMOVE_ITEM
    RELAY_OPTIMIZATION
    ${RELAY_OPTIMIZATION_TEST}
    ${PENDING_TXNS_BATCH_TEST}
  )
endif()
file(GLOB ANTLR_GENERATED
  antlr_generated/*.cpp
  antlr_generated/*.h
)
file(GLOB proj_win
  proj.win/*.h
  proj.win/preda.ico
  proj.win/*.rc
)
file(GLOB RVM ../native/abi/*.h ../native/abi/*.cpp)
file(GLOB RVM_TYPE ../native/types/*.h ../native/types/*.cpp)
source_group("test" FILES ${TEST})
source_group("simulator" FILES ${SIMULATOR})
source_group("runtime/relay_plan" FILES ${RELAY_PLAN})
source_group("simulator/relay_trace" FILES ${RELAY_RUNTIME_TRACE})
source_group("simulator/relay_optimization" FILES ${RELAY_OPTIMIZATION})
source_group("simulator/antlr_generated" FILES ${ANTLR_GENERATED})
source_group("Source Files" FILES ${proj_win})

# source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/../native PREFIX "rvm" FILES ${RVM})
source_group("rvm" FILES ${RVM})
source_group("rvm/types" FILES ${RVM_TYPE})
source_group("main" FILES ${CHAIN_SIMULATOR_FILE})
add_executable(
  chain_simulator
  ${CHAIN_SIMULATOR_FILE}
  ${TEST}
  ${SIMULATOR}
  ${RELAY_PLAN}
  ${RELAY_RUNTIME_TRACE}
  ${RELAY_OPTIMIZATION}
  ${ANTLR_GENERATED}
  ${RVM}
  ${RVM_TYPE}
  ${proj_win}
)

if(RPREDA_ENABLE_RUNTIME_TRACE AND BUILD_TESTING)
  add_executable(
    relay_runtime_trace_tests
    ${RELAY_RUNTIME_TRACE}
    ${RELAY_PLAN}
    ${RELAY_RUNTIME_TRACE_TEST}
  )
  target_compile_definitions(
    relay_runtime_trace_tests
    PRIVATE
    RPREDA_ENABLE_BOUND_RELAY_MANIFEST=1
    RPREDA_ENABLE_RUNTIME_TRACE=1
    RPREDA_RUNTIME_TRACE_STANDALONE_TEST_MAIN=1
  )
  if(RPREDA_ENABLE_TRACE_FAULT_INJECTION)
    target_compile_definitions(
      relay_runtime_trace_tests
      PRIVATE
      RPREDA_ENABLE_TRACE_FAULT_INJECTION=1
    )
  endif()
  target_link_directories(
    relay_runtime_trace_tests
    PRIVATE
    ../../oxd_libsec/lib
  )
  set_property(
    TARGET relay_runtime_trace_tests
    PROPERTY CXX_STANDARD 17
  )
  add_dependencies(
    relay_runtime_trace_tests
    oxd_libsec
  )
  add_test(
    NAME relay_runtime_trace_tests
    COMMAND relay_runtime_trace_tests
  )
endif()

if(RPREDA_ENABLE_BOUND_RELAY_MANIFEST AND BUILD_TESTING)
  add_executable(
    relay_plan_tests
    ${RELAY_PLAN}
    ${RELAY_PLAN_TEST}
  )
  target_compile_definitions(
    relay_plan_tests
    PRIVATE
    RPREDA_ENABLE_BOUND_RELAY_MANIFEST=1
    RPREDA_RELAY_PLAN_TEST_MAIN=1
  )
  target_link_directories(
    relay_plan_tests
    PRIVATE
    ../../oxd_libsec/lib
  )
  target_link_libraries(
    relay_plan_tests
    PRIVATE
    Threads::Threads
  )
  set_property(
    TARGET relay_plan_tests
    PROPERTY CXX_STANDARD 17
  )
  add_dependencies(
    relay_plan_tests
    oxd_libsec
  )
  add_test(
    NAME relay_plan_tests
    COMMAND relay_plan_tests
  )
endif()

if(RPREDA_ENABLE_RUNTIME_OPTIMIZATION AND BUILD_TESTING)
  add_executable(
    relay_optimization_tests
    ${RELAY_OPTIMIZATION}
    ${RELAY_OPTIMIZATION_TEST}
  )
  target_compile_definitions(
    relay_optimization_tests
    PRIVATE
    RPREDA_ENABLE_BOUND_RELAY_MANIFEST=1
    RPREDA_ENABLE_RUNTIME_OPTIMIZATION=1
    RPREDA_RUNTIME_OPTIMIZATION_TESTING=1
  )
  target_link_libraries(
    relay_optimization_tests
    PRIVATE
    Threads::Threads
  )
  set_property(
    TARGET relay_optimization_tests
    PROPERTY CXX_STANDARD 17
  )
  add_test(
    NAME relay_optimization_tests
    COMMAND relay_optimization_tests
  )

  add_executable(
    pending_txns_batch_tests
    ${PENDING_TXNS_BATCH_TEST}
    shard_data.cpp
    ../../SFC/core/rt/mem_alloc.cpp
  )
  target_compile_definitions(
    pending_txns_batch_tests
    PRIVATE
    RPREDA_PENDING_TXNS_BATCH_TESTING=1
    RPREDA_PENDING_TXNS_BATCH_STANDALONE_TEST_MAIN=1
  )
  target_link_libraries(
    pending_txns_batch_tests
    PRIVATE
    Threads::Threads
  )
  target_include_directories(
    pending_txns_batch_tests
    PRIVATE
    ../3rdParty/antlr4/runtime/Cpp/runtime/src
  )
  set_property(
    TARGET pending_txns_batch_tests
    PROPERTY CXX_STANDARD 17
  )
  if(MSVC)
    target_compile_options(
      pending_txns_batch_tests
      PRIVATE
      /Gy
    )
    target_link_options(
      pending_txns_batch_tests
      PRIVATE
      /OPT:REF
    )
  elseif(APPLE)
    target_compile_options(
      pending_txns_batch_tests
      PRIVATE
      -ffunction-sections
      -fdata-sections
    )
    target_link_options(
      pending_txns_batch_tests
      PRIVATE
      -Wl,-dead_strip
    )
  elseif(UNIX)
    target_compile_options(
      pending_txns_batch_tests
      PRIVATE
      -ffunction-sections
      -fdata-sections
    )
    target_link_options(
      pending_txns_batch_tests
      PRIVATE
      -Wl,--gc-sections
    )
  endif()
  add_test(
    NAME pending_txns_batch_tests
    COMMAND pending_txns_batch_tests
  )
endif()

# Add macro definitions based on options
if(ENABLE_VERIFY_SIG)
    target_compile_definitions(chain_simulator PRIVATE VERIFY_SIG=1)
endif()

if(ENABLE_AFFINITY_SET)
    target_compile_definitions(chain_simulator PRIVATE AFFINITY_SET=1)
endif()

if(RPREDA_ENABLE_RUNTIME_TRACE)
    target_compile_definitions(
      chain_simulator
      PRIVATE
      RPREDA_ENABLE_RUNTIME_TRACE=1
    )
endif()

if(RPREDA_ENABLE_TRACE_FAULT_INJECTION)
    target_compile_definitions(
      chain_simulator
      PRIVATE
      RPREDA_ENABLE_TRACE_FAULT_INJECTION=1
    )
endif()

if(RPREDA_ENABLE_BOUND_RELAY_MANIFEST)
    target_compile_definitions(
      chain_simulator
      PRIVATE
      RPREDA_ENABLE_BOUND_RELAY_MANIFEST=1
    )
endif()

if(RPREDA_ENABLE_RUNTIME_OPTIMIZATION)
    target_compile_definitions(
      chain_simulator
      PRIVATE
      RPREDA_ENABLE_RUNTIME_OPTIMIZATION=1
    )
endif()

target_sources(chain_simulator PRIVATE
  ../../SFC/essentials.cpp
  ../../SFC/core/ext/lib_inc.c
)
target_link_directories(chain_simulator PRIVATE ../../oxd_libsec/lib)
target_include_directories(chain_simulator PRIVATE ../3rdParty/antlr4/runtime/Cpp/runtime/src)
target_link_libraries(chain_simulator PRIVATE "oxd_libsec$<$<CONFIG:Debug>:_d>")

if(WIN32)
  target_sources(chain_simulator PRIVATE ../../SFC/core/ext/bignum/ttmath/ttmathuint_x86_64_msvc.asm)
  set_source_files_properties(../../SFC/core/ext/bignum/ttmath/ttmathuint_x86_64_msvc.asm PROPERTIES LANGUAGE ASM_MASM)
  target_link_directories(chain_simulator PRIVATE ../../SFC/libs/win)
  target_link_libraries(chain_simulator PRIVATE ${IPP_LIB} antlr4-runtime)
  win_sdk_rule(chain_simulator)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi /W3 /sdl /MP /Gy /O2 /Oi /GL /GS /FC /permissive /bigobj")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /ZI /JMC /W3 /MP /sdl /RTC1 /Od /GS /FC /permissive /bigobj")
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /OPT:ICF /OPT:REF /DEBUG /LTCG:incremental")
endif()
elseif(UNIX AND NOT APPLE)
  add_dependencies(chain_simulator antlr4_shared)
  linux_link_ipp_rule(chain_simulator)
  target_link_directories(chain_simulator PRIVATE ../../SFC/libs/linux)
  target_link_libraries(chain_simulator PRIVATE ${IPP_LIB} antlr4_shared)
  target_compile_options(chain_simulator PRIVATE -fPIC -pthread)
  if(CMAKE_BUILD_TYPE MATCHES Debug)
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath -Wl,${CMAKE_SOURCE_DIR}/bin/bin_debug/")
  else()
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath -Wl,${CMAKE_SOURCE_DIR}/bin/bin_release/")
  endif()
elseif(APPLE)
  add_dependencies(chain_simulator antlr4_shared)
  target_sources(chain_simulator PRIVATE ../../SFC/core/os/objc_wrap.mm)
  target_link_libraries(chain_simulator
    PRIVATE
    ${IPP_LIB}
	antlr4_shared
  )
  mac_link_rule(chain_simulator)
  if(CMAKE_BUILD_TYPE MATCHES Debug)
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath -Wl,${CMAKE_SOURCE_DIR}/bin/bin_debug/")
  else()
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath -Wl,${CMAKE_SOURCE_DIR}/bin/bin_release/")
  endif()
endif()

if(RPREDA_ENABLE_RUNTIME_TRACE AND BUILD_TESTING)
  if(WIN32)
    target_link_options(
      relay_runtime_trace_tests
      PRIVATE
      "/DEFAULTLIB:$<IF:$<CONFIG:Debug>,oxd_libsec_d.lib,oxd_libsec.lib>"
    )
    target_link_directories(
      relay_runtime_trace_tests
      PRIVATE
      ../../SFC/libs/win
    )
    target_link_libraries(
      relay_runtime_trace_tests
      PRIVATE
      ${IPP_LIB}
    )
    win_sdk_rule(relay_runtime_trace_tests)
  elseif(UNIX AND NOT APPLE)
    target_link_libraries(
      relay_runtime_trace_tests
      PRIVATE
      "-l:liboxd_libsec.a"
    )
    target_link_directories(
      relay_runtime_trace_tests
      PRIVATE
      ../../SFC/libs/linux
    )
    linux_link_ipp_rule(relay_runtime_trace_tests)
    target_compile_options(
      relay_runtime_trace_tests
      PRIVATE
      -fPIC
      -pthread
    )
  elseif(APPLE)
    target_link_libraries(
      relay_runtime_trace_tests
      PRIVATE
      "-loxd_libsec"
    )
    target_link_directories(
      relay_runtime_trace_tests
      PRIVATE
      ../../SFC/libs/mac
    )
    target_link_libraries(
      relay_runtime_trace_tests
      PRIVATE
      ${IPP_LIB}
    )
    mac_link_rule(relay_runtime_trace_tests)
  endif()
endif()

if(RPREDA_ENABLE_BOUND_RELAY_MANIFEST AND BUILD_TESTING)
  if(WIN32)
    target_link_options(
      relay_plan_tests
      PRIVATE
      "/DEFAULTLIB:$<IF:$<CONFIG:Debug>,oxd_libsec_d.lib,oxd_libsec.lib>"
    )
    target_link_directories(
      relay_plan_tests
      PRIVATE
      ../../SFC/libs/win
    )
    target_link_libraries(
      relay_plan_tests
      PRIVATE
      ${IPP_LIB}
    )
    win_sdk_rule(relay_plan_tests)
  elseif(UNIX AND NOT APPLE)
    target_link_libraries(
      relay_plan_tests
      PRIVATE
      "-l:liboxd_libsec.a"
    )
    target_link_directories(
      relay_plan_tests
      PRIVATE
      ../../SFC/libs/linux
    )
    linux_link_ipp_rule(relay_plan_tests)
    target_compile_options(
      relay_plan_tests
      PRIVATE
      -fPIC
      -pthread
    )
  elseif(APPLE)
    target_link_libraries(
      relay_plan_tests
      PRIVATE
      "-loxd_libsec"
    )
    target_link_directories(
      relay_plan_tests
      PRIVATE
      ../../SFC/libs/mac
    )
    target_link_libraries(
      relay_plan_tests
      PRIVATE
      ${IPP_LIB}
    )
    mac_link_rule(relay_plan_tests)
  endif()
endif()

target_compile_definitions(chain_simulator PRIVATE _VIZ _CONSOLE ANTLR4CPP_STATIC NOGDI)
set_target_properties(chain_simulator PROPERTIES OUTPUT_NAME "chsimu")
set_property(TARGET chain_simulator PROPERTY CXX_STANDARD 17)
set_target_properties(chain_simulator PROPERTIES LINK_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
set_target_properties(chain_simulator PROPERTIES CMAKE_POLICY_DEFAULT_CMP0069 NEW)
set_target_properties(chain_simulator PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/bin_$<LOWER_CASE:$<CONFIG>>/$<0:>
)

if(CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET chain_simulator PROPERTY CXX_STANDARD 17)
  cmake_policy(SET CMP0057 NEW)
endif()
