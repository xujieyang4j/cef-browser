cmake_minimum_required(VERSION 3.24)

project(TrailBrowser VERSION 0.5.0 LANGUAGES C CXX)

include(CTest)

if(APPLE)
  # Objective-C++ must be enabled before loading CEF or creating any targets.
  # Enabling it later can leave Ninja without CMAKE_OBJCXX_COMPILE_OBJECT.
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

# Trail Browser currently ships without the platform sandbox support files.
# Keep CEF's build configuration aligned with the explicit no_sandbox setting
# in main.cpp (and avoid the Windows bootstrap-DLL build mode).
set(USE_SANDBOX OFF CACHE BOOL "Build CEF with sandbox support" FORCE)

set(CEF_ROOT "" CACHE PATH "Path to an extracted CEF binary distribution")
if(NOT CEF_ROOT AND DEFINED ENV{CEF_ROOT})
  file(TO_CMAKE_PATH "$ENV{CEF_ROOT}" CEF_ROOT)
endif()
if(NOT CEF_ROOT OR NOT EXISTS "${CEF_ROOT}/cmake/FindCEF.cmake")
  message(FATAL_ERROR
    "CEF_ROOT must point to an extracted CEF binary distribution. "
    "Run scripts/fetch_cef.py, then configure with -DCEF_ROOT=<path>.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets)

if(NOT EXISTS "${CEF_LIB_DEBUG}" AND EXISTS "${CEF_LIB_RELEASE}")
  message(STATUS "CEF Debug binaries not found; using Release runtime for Debug builds")
  set(CEF_LIB_DEBUG "${CEF_LIB_RELEASE}")
  if((GEN_NINJA OR GEN_MAKEFILES) AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CEF_BINARY_DIR "${CEF_BINARY_DIR_RELEASE}")
  endif()
endif()

if(UNIX AND NOT APPLE)
  # CEF's default Linux flag uses the process working directory. Runtime
  # assets are copied beside the executable, so resolve libcef from there.
  list(REMOVE_ITEM CEF_LINKER_FLAGS "-Wl,-rpath,.")
endif()

add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)
if(NOT APPLE)
  ADD_LOGICAL_TARGET(libcef_lib "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
endif()

set(TRAIL_BROWSER_SOURCES
  src/app/browser_app.cpp
  src/app/browser_app.h
  src/browser/browser_client.cpp
  src/browser/browser_client.h
  src/download/download_manager.cpp
  src/download/download_manager.h
  src/profile/browsing_data_store.cpp
  src/profile/browsing_data_store.h
  src/session/session_store.cpp
  src/session/session_store.h
  src/ui/browser_view.cpp
  src/ui/browser_view.h
  src/ui/download_panel.cpp
  src/ui/download_panel.h
  src/ui/main_window.cpp
  src/ui/main_window.h
)

if(APPLE)
  list(APPEND TRAIL_BROWSER_SOURCES
    src/platform/cef_application_mac.h
    src/platform/cef_application_mac.mm
  )
  set_source_files_properties(src/ui/browser_view.cpp PROPERTIES LANGUAGE OBJCXX)
  add_executable(trail-browser MACOSX_BUNDLE src/app/main.cpp ${TRAIL_BROWSER_SOURCES})
else()
  add_executable(trail-browser WIN32 src/app/main.cpp ${TRAIL_BROWSER_SOURCES})
endif()

SET_EXECUTABLE_TARGET_PROPERTIES(trail-browser)
target_include_directories(trail-browser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(trail-browser PRIVATE
  libcef_dll_wrapper
  $<$<NOT:$<PLATFORM_ID:Darwin>>:libcef_lib>
  ${CEF_STANDARD_LIBS}
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
)

if(WIN32)
  target_compile_definitions(trail-browser PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  target_link_libraries(trail-browser PRIVATE X11::X11)
  set_target_properties(trail-browser PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "$ORIGIN"
  )
endif()

if(APPLE)
  set_target_properties(trail-browser PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Trail Browser"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.trail.browser"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist.in"
    OUTPUT_NAME "Trail Browser"
  )

  foreach(helper_suffix_list IN LISTS CEF_HELPER_APP_SUFFIXES)
    string(REPLACE ":" ";" helper_suffix_list "${helper_suffix_list}")
    list(GET helper_suffix_list 0 helper_name_suffix)
    list(GET helper_suffix_list 1 helper_target_suffix)
    list(GET helper_suffix_list 2 helper_bundle_suffix)
    set(helper_target "trail-browser-helper${helper_target_suffix}")
    set(helper_output_name "Trail Browser Helper${helper_name_suffix}")

    set(HELPER_EXECUTABLE_NAME "${helper_output_name}")
    set(HELPER_BUNDLE_IDENTIFIER
        "dev.trail.browser.helper${helper_bundle_suffix}")
    set(helper_info_plist
        "${CMAKE_CURRENT_BINARY_DIR}/Helper-Info${helper_target_suffix}.plist")
    configure_file(resources/macos/Helper-Info.plist.in
                   "${helper_info_plist}" @ONLY)

    add_executable(${helper_target} MACOSX_BUNDLE
      src/app/helper_main.cpp
      src/app/browser_app.cpp
      src/app/browser_app.h
    )
    SET_EXECUTABLE_TARGET_PROPERTIES(${helper_target})
    target_include_directories(${helper_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${helper_target} PRIVATE
      libcef_dll_wrapper
      ${CEF_STANDARD_LIBS}
    )
    set_target_properties(${helper_target} PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST "${helper_info_plist}"
      OUTPUT_NAME "${helper_output_name}"
    )
    add_dependencies(trail-browser ${helper_target})
  endforeach()
endif()

include(cmake/CopyCefRuntime.cmake)
trail_copy_cef_runtime(trail-browser)

if(BUILD_TESTING AND UNIX AND NOT APPLE)
  find_program(XVFB_RUN_EXECUTABLE xvfb-run)
  if(XVFB_RUN_EXECUTABLE)
    add_test(NAME trail_browser_tabs
      COMMAND "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:trail-browser>
              --smoke-test-tabs "data:text/html,<title>First</title>")
    set_tests_properties(trail_browser_tabs PROPERTIES
      TIMEOUT 20
      PASS_REGULAR_EXPRESSION "TAB_SMOKE_OK"
    )
    add_test(NAME trail_browser_downloads
      COMMAND "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:trail-browser>
              --smoke-test-downloads)
    set_tests_properties(trail_browser_downloads PROPERTIES
      TIMEOUT 20
      PASS_REGULAR_EXPRESSION "DOWNLOAD_SMOKE_OK"
    )
    add_test(NAME trail_browser_failures
      COMMAND "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:trail-browser>
              --smoke-test-failures
              "data:text/html,<title>Recovery</title>")
    set_tests_properties(trail_browser_failures PROPERTIES
      TIMEOUT 20
      PASS_REGULAR_EXPRESSION "FAILURE_SMOKE_OK"
    )
    add_test(NAME trail_browser_session
      COMMAND "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:trail-browser>
              --smoke-test-session)
    set_tests_properties(trail_browser_session PROPERTIES
      TIMEOUT 20
      PASS_REGULAR_EXPRESSION "SESSION_SMOKE_OK"
    )
    add_test(NAME trail_browser_page_tools
      COMMAND "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:trail-browser>
              --smoke-test-page-tools
              "data:text/html,<title>Page Tools</title><p>trail trail trail</p>")
    set_tests_properties(trail_browser_page_tools PROPERTIES
      TIMEOUT 20
      PASS_REGULAR_EXPRESSION "PAGE_TOOLS_SMOKE_OK"
    )
    add_test(NAME trail_browser_profile
      COMMAND "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:trail-browser>
              --smoke-test-profile)
    set_tests_properties(trail_browser_profile PROPERTIES
      TIMEOUT 20
      PASS_REGULAR_EXPRESSION "PROFILE_SMOKE_OK"
    )
    add_test(NAME trail_browser_security
      COMMAND "${XVFB_RUN_EXECUTABLE}" -a $<TARGET_FILE:trail-browser>
              --smoke-test-security)
    set_tests_properties(trail_browser_security PROPERTIES
      TIMEOUT 20
      PASS_REGULAR_EXPRESSION "SECURITY_SMOKE_OK"
    )
  endif()
endif()

