cmake_minimum_required(VERSION 3.19)
project(reaper_reamcusof LANGUAGES C CXX)

include(CheckCompilerFlag)
include(CheckCXXSourceCompiles)
include(CheckIncludeFileCXX)
include(CheckLinkerFlag)
include(FindGit)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  if(WIN32)
    set(USER_CONFIG_DIR "$ENV{APPDATA}")
  elseif(APPLE)
    set(USER_CONFIG_DIR "~/Library/Application Support")
  else()
    set(USER_CONFIG_DIR "~/.config")
  endif()

  set(CMAKE_INSTALL_PREFIX "${USER_CONFIG_DIR}/REAPER" CACHE PATH
    "REAPER resource path where to install reamcusof" FORCE)
endif()

if(CMAKE_OSX_ARCHITECTURES)
  list(JOIN CMAKE_OSX_ARCHITECTURES "-" ARCH_NAME)
elseif(MSVC_CXX_ARCHITECTURE_ID)
  set(ARCH_NAME ${MSVC_CXX_ARCHITECTURE_ID})
else()
  set(ARCH_NAME ${CMAKE_SYSTEM_PROCESSOR})
endif()

STRING(TOLOWER "${ARCH_NAME}" ARCH_NAME)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET "hidden")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL     ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE        ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

if(VCPKG_TOOLCHAIN)
  set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL Release)
  set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
endif()

if(NOT EXISTS ${PROJECT_SOURCE_DIR}/external/WDL)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive --depth 1 
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      )
    
endif()
file(CREATE_LINK ${PROJECT_SOURCE_DIR}/external/WDL/WDL ${PROJECT_SOURCE_DIR}/WDL SYMBOLIC)
if (NOT WIN32)
  # file(COPY_FILE ${PROJECT_SOURCE_DIR}/reaper-plugins/reaper_csurf/res.rc ${PROJECT_SOURCE_DIR}/WDL/swell/)
  execute_process(
    COMMAND ${PROJECT_SOURCE_DIR}/WDL/swell/swell_resgen.sh res.rc
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/reaper-plugins/reaper_csurf)
endif()

if(WIN32)
  foreach(arg
    CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG
    CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELEASE
    CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO
    CMAKE_C_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_MINSIZEREL
  )
    # Embed debug information in the object files
    string(REGEX REPLACE "/Z[iI]" "/Z7" ${arg} "${${arg}}")

    # Allow expansion of function not explicitly marked for no inlining
    string(REGEX REPLACE "/Ob1" "/Ob2" ${arg} "${${arg}}")
  endforeach()

  add_compile_options(
    /utf-8
    # Exception handling model
    /EHsc

    # Enhance optimized debugging
    $<$<NOT:$<CONFIG:Debug>>:/Zo>

    # Eliminate duplicate strings
    $<$<NOT:$<CONFIG:Debug>>:/GF>

    # Enable function-level linking
    $<$<NOT:$<CONFIG:Debug>>:/Gy>

    # Remove unreferenced COMDAT
    $<$<NOT:$<CONFIG:Debug>>:/Zc:inline>
  )

  add_link_options(
    # Remove unreferenced code
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>

    # Remove duplicate sections
    $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>

    # Use relative path to the PDB file to avoid exposing the full path
    # /PDBALTPATH:%_PDB%
  )
else()
  add_compile_options(
    -fsigned-char -fstack-protector-strong -fdiagnostics-color
  )

  if(APPLE)
    add_link_options(-Wl,-dead_strip)
  endif()

  check_linker_flag(CXX -Wl,--no-undefined HAVE_NO_UNDEFINED)
  if(HAVE_NO_UNDEFINED)
    add_link_options(-Wl,--no-undefined)
  endif()
endif()

add_library(common INTERFACE)
target_include_directories(
  common SYSTEM INTERFACE
  ${CMAKE_SOURCE_DIR}/sdk)
if(WIN32)
  target_compile_options(common INTERFACE
    /W3
    /WX
    /wd4996
    /wd4244
    )
   target_compile_definitions(common INTERFACE NOMINMAX)
else()
  target_compile_options(common INTERFACE
    -Wall -Wextra -Werror 
    -Wno-multichar
    -Wno-unused-parameter -Wno-missing-field-initializers -Wno-missing-braces
  )
endif()

check_include_file_cxx(optional HAS_STD_OPTIONAL)
if(HAS_STD_OPTIONAL)
  target_compile_definitions(common INTERFACE HAS_STD_OPTIONAL)
endif()

check_include_file_cxx(variant HAS_STD_VARIANT)
if(HAS_STD_VARIANT)
  target_compile_definitions(common INTERFACE HAS_STD_VARIANT)
endif()

add_library(${PROJECT_NAME} SHARED)

set_target_properties(${PROJECT_NAME} PROPERTIES
  PREFIX "" # disable the "lib" prefix
  OUTPUT_NAME "${PROJECT_NAME}-${ARCH_NAME}"
)
set(REAPER_USER_PLUGINS "UserPlugins")

install(TARGETS ${PROJECT_NAME}
  CONFIGURATIONS Debug Release
  RUNTIME DESTINATION "${REAPER_USER_PLUGINS}" # Windows .dll
  LIBRARY DESTINATION "${REAPER_USER_PLUGINS}" # Linux .so/macOS .dylib
)

if(WIN32)
  install(FILES $<TARGET_PDB_FILE:${PROJECT_NAME}> DESTINATION "${REAPER_USER_PLUGINS}" OPTIONAL)
endif()

add_subdirectory(${CMAKE_SOURCE_DIR}/reaper-plugins/reaper_csurf)
target_link_libraries(reamcusof common)
target_link_libraries(${PROJECT_NAME} reamcusof)

find_package(WDL REQUIRED)
target_link_libraries(reamcusof WDL::WDL)

if(NOT WIN32)
  find_package(SWELL REQUIRED)
  target_link_libraries(reamcusof SWELL::swell)
endif()

