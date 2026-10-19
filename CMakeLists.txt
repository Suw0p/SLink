cmake_minimum_required(VERSION 3.25)

project(SLink
    VERSION 1.1.1
    DESCRIPTION "SLink"
    LANGUAGES C CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "SLink targets Windows 10/11 only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SLink supports x64 builds only.")
endif()

option(SLINK_BUILD_TESTS "Build and register unit tests" OFF)
option(SLINK_BUILD_LIVE_EDITOR_CHECK
    "Build the opt-in live remote editor save check" OFF)
option(SLINK_ENABLE_ADDRESS_SANITIZER
    "Instrument SLink and tests with MSVC AddressSanitizer" OFF)
option(SLINK_ENABLE_PERF_TRACE
    "Emit development-only p50/p95 timing summaries to the debugger" OFF)

# The public source package intentionally omits the private test sources.
if(SLINK_BUILD_TESTS OR SLINK_BUILD_LIVE_EDITOR_CHECK)
    message(FATAL_ERROR
        "This source package does not include tests; disable test options.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Windows 属性、manifest 与设置页共用 project(VERSION)，发布时只改上面的版本号。
set(SLINK_WINDOWS_VERSION
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")
set(SLINK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SLINK_MANIFEST_PATH "${SLINK_GENERATED_DIR}/SLink.manifest")
set(SLINK_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/SLink.ico")
set(SLINK_RC_PATH "${SLINK_GENERATED_DIR}/SLink.rc")
file(MAKE_DIRECTORY "${SLINK_GENERATED_DIR}")
configure_file(resources/SLink.manifest.in "${SLINK_MANIFEST_PATH}" @ONLY)
configure_file(resources/SLink.rc.in "${SLINK_RC_PATH}" @ONLY)
configure_file(resources/slink_version.hpp.in
    "${SLINK_GENERATED_DIR}/slink_version.hpp" @ONLY)
set_source_files_properties("${SLINK_RC_PATH}" PROPERTIES
    OBJECT_DEPENDS "${SLINK_MANIFEST_PATH}")

set(SWB_3RD_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/3rd/include")
set(SWB_3RD_LIB "${CMAKE_CURRENT_SOURCE_DIR}/3rd/lib")

set(SWB_LIBCRYPTO "${SWB_3RD_LIB}/libcrypto.lib")
set(SWB_LIBSODIUM "${SWB_3RD_LIB}/libsodium.lib")
set(SWB_LIBSSH2 "${SWB_3RD_LIB}/libssh2.lib")
set(SWB_LIBSQLITE "${SWB_3RD_LIB}/sqlite3.lib")
set(SWB_LIBZ "${SWB_3RD_LIB}/zs.lib")
set(SWB_SCINTILLA_DLL "${SWB_3RD_LIB}/Scintilla.dll")
set(SWB_LEXILLA_DLL "${SWB_3RD_LIB}/Lexilla.dll")

set(SWB_REQUIRED_3RD_FILES
    "${SWB_3RD_INCLUDE}/libssh2.h"
    "${SWB_3RD_INCLUDE}/sqlite3.h"
    "${SWB_3RD_INCLUDE}/sodium.h"
    "${SWB_3RD_INCLUDE}/openssl/evp.h"
    "${SWB_3RD_INCLUDE}/vterm.h"
    "${SWB_LIBCRYPTO}"
    "${SWB_LIBSODIUM}"
    "${SWB_LIBSSH2}"
    "${SWB_LIBSQLITE}"
    "${SWB_LIBZ}"
    "${SWB_SCINTILLA_DLL}"
    "${SWB_LEXILLA_DLL}")
foreach(required_file IN LISTS SWB_REQUIRED_3RD_FILES)
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Required bundled dependency is missing: ${required_file}")
    endif()
endforeach()

add_library(swb_options INTERFACE)
target_compile_features(swb_options INTERFACE cxx_std_20)
target_compile_definitions(swb_options INTERFACE
    UNICODE
    _UNICODE
    $<$<BOOL:${SLINK_ENABLE_PERF_TRACE}>:SLINK_ENABLE_PERF_TRACE=1>
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
if(MSVC)
    target_compile_options(swb_options INTERFACE
        /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc
        $<$<CONFIG:Release>:/O2 /GL /Zi>)
    target_link_options(swb_options INTERFACE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
endif()
if(SLINK_ENABLE_ADDRESS_SANITIZER)
    if(NOT MSVC)
        message(FATAL_ERROR "SLINK_ENABLE_ADDRESS_SANITIZER currently supports MSVC only.")
    endif()
    target_compile_options(swb_options INTERFACE /fsanitize=address)
    target_link_options(swb_options INTERFACE /INFERASANLIBS /INCREMENTAL:NO)
endif()

# Build libvterm from the bundled, auditable source instead of linking an
# opaque prebuilt archive. SLink carries a bounds fix in screen.c and keeps
# terminal reflow disabled until that path has passed sustained resize tests.
add_library(swb_libvterm STATIC
    3rd/libvterm/src/encoding.c
    3rd/libvterm/src/keyboard.c
    3rd/libvterm/src/mouse.c
    3rd/libvterm/src/parser.c
    3rd/libvterm/src/pen.c
    3rd/libvterm/src/screen.c
    3rd/libvterm/src/state.c
    3rd/libvterm/src/unicode.c
    3rd/libvterm/src/vterm.c)
target_include_directories(swb_libvterm
    PUBLIC "${SWB_3RD_INCLUDE}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/3rd/libvterm/src")
if(MSVC)
    target_compile_options(swb_libvterm PRIVATE
        /W3
        $<$<CONFIG:Release>:/O2 /GL /Zi>)
endif()
if(SLINK_ENABLE_ADDRESS_SANITIZER)
    target_compile_options(swb_libvterm PRIVATE /fsanitize=address)
endif()

add_library(swb_storage STATIC
    src/storage/application_repository.cpp
    src/storage/app_paths.cpp
    src/storage/crash_reporter.cpp
    src/storage/rotating_log.cpp
    src/storage/secure_buffer.cpp
    src/storage/state_store.cpp
    src/storage/vault.cpp)
target_include_directories(swb_storage
    PUBLIC include
    PRIVATE "${SWB_3RD_INCLUDE}")
target_compile_definitions(swb_storage PRIVATE
    SLINK_USE_SQLITE=1
    SLINK_USE_LIBSODIUM=1)
target_link_libraries(swb_storage
    PUBLIC swb_options
    PRIVATE
        "${SWB_LIBSQLITE}"
        "${SWB_LIBSODIUM}"
        dbghelp)
if(MSVC)
    set_source_files_properties(src/storage/crash_reporter.cpp
        PROPERTIES COMPILE_OPTIONS /WX)
endif()

add_library(swb_transfer STATIC
    src/transfer/file_uri.cpp
    src/transfer/local_endpoint.cpp
    src/transfer/pane_model.cpp
    src/transfer/sftp_endpoint.cpp
    src/transfer/parallel_transfer.cpp)
target_include_directories(swb_transfer
    PUBLIC include
    PRIVATE "${SWB_3RD_INCLUDE}")
target_compile_definitions(swb_transfer PRIVATE
    SLINK_USE_OPENSSL=1)
target_link_libraries(swb_transfer
    PUBLIC swb_options
    PRIVATE "${SWB_LIBCRYPTO}" crypt32 advapi32)

add_library(swb_terminal STATIC
    src/terminal/terminal_buffer.cpp
    src/terminal/terminal_input.cpp
    src/terminal/terminal_session_codec.cpp
    src/terminal/libvterm_adapter.cpp)
target_include_directories(swb_terminal
    PUBLIC include
    PRIVATE "${SWB_3RD_INCLUDE}")
target_compile_definitions(swb_terminal PUBLIC
    SLINK_USE_LIBVTERM=1)
target_link_libraries(swb_terminal
    PUBLIC swb_options
    PRIVATE swb_libvterm)

add_library(swb_ui STATIC
    src/ui/retained_ui.cpp
    src/ui/file_icon_provider.cpp
    src/ui/local_shortcut.cpp
    src/ui/table_control.cpp
    src/ui/sidebar_control.cpp
    src/ui/selector_control.cpp
    src/ui/main_window.cpp
    src/ui/terminal_control.cpp
    src/ui/terminal_page.cpp
    src/ui/transfer_conflict_dialog.cpp
    src/ui/file_operation_dialogs.cpp
    src/ui/transfer_page.cpp
    src/ui/transfer_file_rows.cpp
    src/ui/transfer_pane_history.cpp
    src/ui/transfer_page_surface.cpp
    src/ui/transfer_queue_model.cpp
    src/ui/editor_page.cpp
    src/ui/port_forward_page.cpp)
target_include_directories(swb_ui
    PUBLIC include
    PRIVATE resources "${SLINK_GENERATED_DIR}")
target_link_libraries(swb_ui
    PUBLIC swb_options swb_terminal swb_transfer
    PRIVATE comctl32 d2d1 dwrite dwmapi uxtheme shlwapi imm32 shell32 ole32)

add_library(swb_ssh STATIC
    src/ssh/network_reactor.cpp
    src/ssh/reactor_endpoint.cpp
    src/ssh/session.cpp)
target_include_directories(swb_ssh
    PUBLIC include
    PRIVATE "${SWB_3RD_INCLUDE}")
target_compile_definitions(swb_ssh PRIVATE
    SLINK_USE_LIBSSH2=1)
target_link_libraries(swb_ssh
    PUBLIC swb_options swb_transfer
    PRIVATE
        "${SWB_LIBSSH2}"
        "${SWB_LIBCRYPTO}"
        "${SWB_LIBZ}"
        crypt32
        ws2_32)

add_library(swb_editor STATIC
    src/editor/remote_document.cpp)
target_include_directories(swb_editor PUBLIC include)
target_link_libraries(swb_editor
    PUBLIC swb_options swb_storage swb_transfer
    PRIVATE bcrypt)

add_executable(SLink WIN32
    src/app/main.cpp
    src/app/application.cpp
    src/app/remote_initial_directory.cpp
    src/app/editor_controller.cpp
    src/app/editor_work_scheduler.cpp
    src/app/file_session_controller.cpp
    src/app/transfer_persistence_coordinator.cpp
    src/app/transfer_session_pool.cpp
    src/app/firewall_helper.cpp
    src/app/server_manager_dialog.cpp
    src/app/port_forward_controller.cpp
    src/app/local_terminal_session.cpp
    src/app/terminal_session_controller.cpp
    "${SLINK_RC_PATH}")
target_include_directories(SLink PRIVATE include resources)
target_link_libraries(SLink PRIVATE
    swb_options
    swb_storage
    swb_transfer
    swb_ui
    swb_ssh
    swb_editor
    shell32
    ole32
    oleaut32)
if(MSVC)
    set(SWB_SYMBOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/symbols")
    file(MAKE_DIRECTORY "${SWB_SYMBOL_DIR}")
    target_link_options(SLink PRIVATE
        /MANIFEST:NO
        $<$<CONFIG:Release>:/DEBUG:FULL>
        $<$<CONFIG:Release>:/MAP:${SWB_SYMBOL_DIR}/SLink.map>
        $<$<CONFIG:Release>:/MAPINFO:EXPORTS>)
endif()

set(SWB_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/out/bin")
set_target_properties(SLink PROPERTIES
    OUTPUT_NAME "SLink"
    PDB_NAME "SLink"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/symbols"
    RUNTIME_OUTPUT_DIRECTORY "${SWB_RUNTIME_DIR}")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(SLink PROPERTIES
        "PDB_OUTPUT_DIRECTORY_${config_upper}" "${CMAKE_CURRENT_BINARY_DIR}/symbols"
        "RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${SWB_RUNTIME_DIR}")
endforeach()

add_custom_command(TARGET SLink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SWB_SCINTILLA_DLL}"
        "$<TARGET_FILE_DIR:SLink>/Scintilla.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SWB_LEXILLA_DLL}"
        "$<TARGET_FILE_DIR:SLink>/Lexilla.dll"
    COMMENT "Preparing the portable runtime directory")

if(SLINK_BUILD_TESTS)
    enable_testing()

    function(swb_enable_test_assertions target_name)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${target_name} PRIVATE -UNDEBUG)
        endif()
    endfunction()

    add_executable(storage_tests
        tests/storage/storage_tests.cpp
        src/storage/application_repository.cpp
        src/storage/app_paths.cpp
        src/storage/crash_reporter.cpp
        src/storage/rotating_log.cpp
        src/storage/secure_buffer.cpp
        src/storage/state_store.cpp
        src/storage/vault.cpp)
    target_include_directories(storage_tests PRIVATE
        include
        "${SWB_3RD_INCLUDE}")
    target_compile_definitions(storage_tests PRIVATE
        SLINK_USE_SQLITE=1
        SLINK_USE_LIBSODIUM=1)
    target_link_libraries(storage_tests PRIVATE
        swb_options
        "${SWB_LIBSQLITE}"
        "${SWB_LIBSODIUM}"
        dbghelp)
    swb_enable_test_assertions(storage_tests)
    add_test(NAME storage_tests COMMAND storage_tests)

    add_executable(crash_reporter_tests
        tests/storage/crash_reporter_tests.cpp)
    target_include_directories(crash_reporter_tests PRIVATE include)
    target_link_libraries(crash_reporter_tests PRIVATE
        swb_options swb_storage dbghelp)
    if(MSVC)
        target_compile_options(crash_reporter_tests PRIVATE /WX)
    endif()
    swb_enable_test_assertions(crash_reporter_tests)
    add_test(NAME crash_reporter_tests COMMAND crash_reporter_tests)

    add_executable(transfer_tests
        tests/transfer/transfer_tests.cpp
        src/transfer/file_uri.cpp
        src/transfer/local_endpoint.cpp
        src/transfer/pane_model.cpp
        src/transfer/sftp_endpoint.cpp
        src/transfer/parallel_transfer.cpp)
    target_include_directories(transfer_tests PRIVATE
        include
        "${SWB_3RD_INCLUDE}")
    target_compile_definitions(transfer_tests PRIVATE
        SLINK_USE_OPENSSL=1)
    target_link_libraries(transfer_tests PRIVATE
        swb_options "${SWB_LIBCRYPTO}" crypt32 advapi32)
    swb_enable_test_assertions(transfer_tests)
    add_test(NAME transfer_tests COMMAND transfer_tests)

    add_executable(terminal_tests
        tests/terminal/terminal_buffer_tests.cpp
        src/terminal/terminal_buffer.cpp
        src/terminal/terminal_input.cpp
        src/terminal/terminal_session_codec.cpp
        src/terminal/libvterm_adapter.cpp)
    target_include_directories(terminal_tests PRIVATE
        include
        "${SWB_3RD_INCLUDE}")
    target_compile_definitions(terminal_tests PRIVATE
        SLINK_USE_LIBVTERM=1)
    target_link_libraries(terminal_tests PRIVATE
        swb_options swb_libvterm)
    swb_enable_test_assertions(terminal_tests)
    add_test(NAME terminal_tests COMMAND terminal_tests)
    add_executable(terminal_render_check
        tests/terminal/terminal_render_check.cpp)
    target_include_directories(terminal_render_check PRIVATE include)
    target_link_libraries(terminal_render_check PRIVATE
        swb_options swb_ui gdi32 user32 ole32)
    swb_enable_test_assertions(terminal_render_check)
    add_test(NAME terminal_render_check
        COMMAND terminal_render_check
            "${CMAKE_CURRENT_SOURCE_DIR}/out/verification/terminal-cjk-check.bmp")

    add_executable(version_info_tests tests/app/version_info_tests.cpp)
    target_include_directories(version_info_tests PRIVATE
        resources "${SLINK_GENERATED_DIR}")
    target_link_libraries(version_info_tests PRIVATE swb_options version)
    swb_enable_test_assertions(version_info_tests)
    add_dependencies(version_info_tests SLink)
    add_test(NAME version_info_tests
        COMMAND version_info_tests $<TARGET_FILE:SLink>)

    add_executable(local_terminal_session_tests WIN32
        tests/app/local_terminal_session_tests.cpp
        src/app/local_terminal_session.cpp)
    target_include_directories(local_terminal_session_tests PRIVATE include)
    target_link_libraries(local_terminal_session_tests PRIVATE
        swb_options swb_ui user32 ole32)
    swb_enable_test_assertions(local_terminal_session_tests)
    add_test(NAME local_terminal_session_tests
        COMMAND local_terminal_session_tests)

    add_executable(editor_tests
        tests/editor/remote_document_tests.cpp)
    target_include_directories(editor_tests PRIVATE include)
    target_link_libraries(editor_tests PRIVATE swb_options swb_editor)
    swb_enable_test_assertions(editor_tests)
    add_test(NAME editor_tests COMMAND editor_tests)

    add_executable(editor_work_scheduler_tests
        tests/app/editor_work_scheduler_tests.cpp
        src/app/editor_work_scheduler.cpp)
    target_include_directories(editor_work_scheduler_tests PRIVATE include)
    target_link_libraries(editor_work_scheduler_tests PRIVATE swb_options)
    swb_enable_test_assertions(editor_work_scheduler_tests)
    add_test(NAME editor_work_scheduler_tests COMMAND editor_work_scheduler_tests)

    add_executable(transfer_persistence_coordinator_tests
        tests/app/transfer_persistence_coordinator_tests.cpp
        src/app/transfer_persistence_coordinator.cpp)
    target_include_directories(transfer_persistence_coordinator_tests PRIVATE include)
    target_link_libraries(transfer_persistence_coordinator_tests PRIVATE
        swb_options swb_storage swb_transfer)
    swb_enable_test_assertions(transfer_persistence_coordinator_tests)
    add_test(NAME transfer_persistence_coordinator_tests
        COMMAND transfer_persistence_coordinator_tests)

    add_executable(transfer_queue_model_tests
        tests/ui/transfer_queue_model_tests.cpp
        src/ui/transfer_queue_model.cpp)
    target_include_directories(transfer_queue_model_tests PRIVATE include)
    target_link_libraries(transfer_queue_model_tests PRIVATE swb_options)
    swb_enable_test_assertions(transfer_queue_model_tests)
    add_test(NAME transfer_queue_model_tests COMMAND transfer_queue_model_tests)

    add_executable(transfer_page_parts_tests
        tests/ui/transfer_page_parts_tests.cpp)
    target_include_directories(transfer_page_parts_tests PRIVATE include)
    target_link_libraries(transfer_page_parts_tests PRIVATE
        swb_options swb_ui gdi32 user32 ole32)
    swb_enable_test_assertions(transfer_page_parts_tests)
    add_test(NAME transfer_page_parts_tests COMMAND transfer_page_parts_tests)

    add_executable(ssh_disabled_tests
        tests/ssh/ssh_disabled_tests.cpp)
    target_include_directories(ssh_disabled_tests PRIVATE include)
    target_link_libraries(ssh_disabled_tests PRIVATE swb_options swb_ssh)
    swb_enable_test_assertions(ssh_disabled_tests)
    add_test(NAME ssh_disabled_tests COMMAND ssh_disabled_tests)

    add_executable(network_reactor_tests
        tests/ssh/network_reactor_tests.cpp)
    target_include_directories(network_reactor_tests PRIVATE include)
    target_link_libraries(network_reactor_tests PRIVATE swb_options swb_ssh)
    swb_enable_test_assertions(network_reactor_tests)
    add_test(NAME network_reactor_tests COMMAND network_reactor_tests)

    add_executable(reactor_endpoint_tests
        tests/ssh/reactor_endpoint_tests.cpp)
    target_include_directories(reactor_endpoint_tests PRIVATE include)
    target_link_libraries(reactor_endpoint_tests PRIVATE swb_options swb_ssh)
    swb_enable_test_assertions(reactor_endpoint_tests)
    add_test(NAME reactor_endpoint_tests COMMAND reactor_endpoint_tests)

    add_executable(file_session_controller_tests
        tests/app/file_session_controller_tests.cpp
        src/app/file_session_controller.cpp
        src/app/remote_initial_directory.cpp)
    target_include_directories(file_session_controller_tests PRIVATE include)
    target_link_libraries(file_session_controller_tests PRIVATE
        swb_options swb_storage swb_ssh)
    swb_enable_test_assertions(file_session_controller_tests)
    add_test(NAME file_session_controller_tests
        COMMAND file_session_controller_tests)

    add_executable(transfer_session_pool_tests
        tests/app/transfer_session_pool_tests.cpp
        src/app/transfer_session_pool.cpp)
    target_include_directories(transfer_session_pool_tests PRIVATE include)
    target_link_libraries(transfer_session_pool_tests PRIVATE
        swb_options swb_storage swb_transfer swb_ssh)
    swb_enable_test_assertions(transfer_session_pool_tests)
    add_test(NAME transfer_session_pool_tests
        COMMAND transfer_session_pool_tests)

    add_executable(port_forward_controller_tests
        tests/app/port_forward_controller_tests.cpp
        src/app/port_forward_controller.cpp)
    target_include_directories(port_forward_controller_tests PRIVATE include)
    target_link_libraries(port_forward_controller_tests PRIVATE
        swb_options swb_ui swb_ssh ws2_32 iphlpapi)
    swb_enable_test_assertions(port_forward_controller_tests)
    add_test(NAME port_forward_controller_tests
        COMMAND port_forward_controller_tests)

    add_executable(credential_codec_tests
        tests/app/credential_codec_tests.cpp
        src/app/server_manager_dialog.cpp
        src/ui/table_control.cpp
        src/ui/selector_control.cpp)
    target_include_directories(credential_codec_tests PRIVATE include)
    target_link_libraries(credential_codec_tests PRIVATE
        swb_options swb_storage comdlg32)
    swb_enable_test_assertions(credential_codec_tests)
    add_test(NAME credential_codec_tests
        COMMAND credential_codec_tests)
endif()

if(SLINK_BUILD_TESTS AND SLINK_ENABLE_ADDRESS_SANITIZER)
    # /fsanitize=address uses a dynamic runtime. Give every CTest case its own
    # explicit runtime search path; inheriting PATH is unreliable across
    # sequential test launches on some Windows CTest builds.
    get_filename_component(SLINK_MSVC_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_property(SLINK_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
    set_property(TEST ${SLINK_REGISTERED_TESTS} PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${SLINK_MSVC_RUNTIME_DIR}")
endif()

if(SLINK_BUILD_LIVE_EDITOR_CHECK)
    add_executable(live_editor_save_check
        tests/editor/live_editor_save_check.cpp
        src/app/file_session_controller.cpp
        src/app/server_manager_dialog.cpp
        src/ui/table_control.cpp
        src/ui/selector_control.cpp)
    target_include_directories(live_editor_save_check PRIVATE include)
    target_link_libraries(live_editor_save_check PRIVATE
        swb_options
        swb_storage
        swb_ssh
        swb_editor
        comdlg32)
endif()

# Localized MSVC /showIncludes output can be missed by Ninja. Project headers
# are also explicit dependencies so public ABI changes always rebuild callers.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    file(GLOB_RECURSE SWB_LOCAL_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.inl")
    file(GLOB_RECURSE SWB_LOCAL_CPP CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    foreach(source_file IN LISTS SWB_LOCAL_CPP)
        set_property(SOURCE "${source_file}" APPEND PROPERTY OBJECT_DEPENDS
            ${SWB_LOCAL_HEADERS})
    endforeach()
endif()
