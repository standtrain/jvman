cmake_minimum_required(VERSION 3.16)
project(jvman VERSION 0.4.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(jvman
    src/main.c
    src/manager.c
    src/download_source.c
    src/discovery.c
    src/update.c
    src/platform.c
    src/util.c
    src/i18n.c
    src/sha256.c
    src/env_persist.c
    src/rc_writer.c
)
target_include_directories(jvman PRIVATE src resources installer)

if(WIN32)
    target_sources(jvman PRIVATE
        installer/environment.c
        installer/pathlist.c
    )
endif()

if(MSVC)
    target_compile_options(jvman PRIVATE /W4 /utf-8)
else()
    target_compile_options(jvman PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    enable_language(RC)
    target_compile_definitions(jvman PRIVATE _WIN32_WINNT=0x0601)
    target_sources(jvman PRIVATE resources/jvman.rc)
else()
    target_compile_definitions(jvman PRIVATE _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
endif()

# The Windows installer is a native, per-user setup bundle.  The setup stub
# and packer remain separate build targets so the final executable is produced
# from the actual CMake target paths (including non-default build directories).
if(WIN32)
    set(JVMAN_INSTALLER_SOURCES
        installer/setup.c
        installer/environment.c
        installer/files.c
        installer/pathlist.c
        installer/package.c
        installer/lang.c
        src/sha256.c
    )
    add_executable(jvman_setup_stub WIN32
        ${JVMAN_INSTALLER_SOURCES}
        installer/installer.rc
    )
    set_target_properties(jvman_setup_stub PROPERTIES
        OUTPUT_NAME "jvman-setup-stub"
    )
    target_include_directories(jvman_setup_stub PRIVATE src installer)
    target_compile_definitions(jvman_setup_stub PRIVATE _WIN32_WINNT=0x0601)
    if(MSVC)
        target_compile_options(jvman_setup_stub PRIVATE /W4 /utf-8)
    else()
        target_compile_options(jvman_setup_stub PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(MINGW)
        # setup.c uses the Unicode Win32 entry point (wWinMain).
        target_link_options(jvman_setup_stub PRIVATE -municode)
    endif()
    target_link_libraries(jvman_setup_stub PRIVATE
        advapi32 shell32 user32 ole32
    )

    add_executable(pack_setup
        installer/pack_setup.c
        installer/package.c
        src/sha256.c
    )
    target_include_directories(pack_setup PRIVATE src installer)
    if(MSVC)
        target_compile_options(pack_setup PRIVATE /W4 /utf-8)
    else()
        target_compile_options(pack_setup PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # A cross compiler cannot execute the host packer.  It can still build the
    # stub and packer targets; the self-contained setup bundle is enabled only
    # when CMake is building for the current Windows host.
    if(NOT CMAKE_CROSSCOMPILING)
        set(JVMAN_SETUP_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/jvman-setup.exe")
        add_custom_command(
            OUTPUT "${JVMAN_SETUP_OUTPUT}"
            COMMAND $<TARGET_FILE:pack_setup>
                    "$<TARGET_FILE:jvman_setup_stub>"
                    "$<TARGET_FILE:jvman>"
                    "${JVMAN_SETUP_OUTPUT}"
            DEPENDS jvman jvman_setup_stub pack_setup
            COMMENT "Packing jvman-setup.exe"
            VERBATIM
        )
        add_custom_target(jvman_setup_bundle ALL
            DEPENDS "${JVMAN_SETUP_OUTPUT}"
        )
        install(FILES "${JVMAN_SETUP_OUTPUT}" DESTINATION bin)
    else()
        message(STATUS "Skipping jvman-setup.exe bundle: CMAKE_CROSSCOMPILING is enabled")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(test_unit
        tests/test_unit.c
        src/download_source.c
        src/discovery.c
        src/platform.c
        src/util.c
        src/sha256.c
    )
    target_include_directories(test_unit PRIVATE src)
    if(MSVC)
        target_compile_options(test_unit PRIVATE /W4 /utf-8)
    else()
        target_compile_options(test_unit PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(WIN32)
        target_compile_definitions(test_unit PRIVATE _WIN32_WINNT=0x0601)
        target_link_libraries(test_unit PRIVATE advapi32 winhttp shell32)
    else()
        target_compile_definitions(test_unit PRIVATE _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
    endif()
    add_test(NAME unit COMMAND test_unit)

    add_executable(test_update
        tests/test_update.c
        src/update.c
        src/platform.c
        src/util.c
        src/i18n.c
        src/sha256.c
    )
    target_include_directories(test_update PRIVATE src)
    if(MSVC)
        target_compile_options(test_update PRIVATE /W4 /utf-8)
    else()
        target_compile_options(test_update PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(WIN32)
        target_compile_definitions(test_update PRIVATE _WIN32_WINNT=0x0601)
        target_link_libraries(test_update PRIVATE advapi32 winhttp shell32)
    else()
        target_compile_definitions(test_update PRIVATE
            _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
    endif()
    add_test(NAME update_unit COMMAND test_update)

    add_executable(test_update_flow
        tests/test_update_flow.c
        src/update.c
        src/util.c
        src/i18n.c
    )
    target_include_directories(test_update_flow PRIVATE src)
    if(MSVC)
        target_compile_options(test_update_flow PRIVATE /W4 /utf-8)
    else()
        target_compile_options(test_update_flow PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    if(WIN32)
        target_compile_definitions(test_update_flow PRIVATE _WIN32_WINNT=0x0601)
        target_link_libraries(test_update_flow PRIVATE advapi32)
    else()
        target_compile_definitions(test_update_flow PRIVATE
            _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
    endif()
    add_test(NAME update_flow COMMAND test_update_flow)

    add_executable(test_update_helper
        tests/test_update_helper.c
        src/platform.c
        src/util.c
        src/sha256.c
    )
    target_include_directories(test_update_helper PRIVATE src)
    if(MSVC)
        target_compile_options(test_update_helper PRIVATE /W4 /utf-8)
    else()
        target_compile_options(test_update_helper PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    if(WIN32)
        target_compile_definitions(test_update_helper PRIVATE
            _WIN32_WINNT=0x0601)
        target_link_libraries(test_update_helper PRIVATE
            advapi32 winhttp shell32)
        add_test(NAME update_helper
            COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_helper.ps1
                    -Helper $<TARGET_FILE:test_update_helper>)
    else()
        target_compile_definitions(test_update_helper PRIVATE
            _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
        add_test(NAME update_helper
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_helper.sh
                    $<TARGET_FILE:test_update_helper>)
    endif()
    set_tests_properties(update_helper PROPERTIES TIMEOUT 60)

    if(WIN32)
        add_test(NAME cli
            COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.ps1
                    -Binary $<TARGET_FILE:jvman>)

        add_executable(test_installer_package
            tests/test_installer_package.c
            installer/package.c
            src/sha256.c
        )
        target_include_directories(test_installer_package PRIVATE src installer)
        target_compile_definitions(test_installer_package PRIVATE _WIN32_WINNT=0x0601)
        if(MSVC)
            target_compile_options(test_installer_package PRIVATE /W4 /utf-8)
        else()
            target_compile_options(test_installer_package PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME installer_package COMMAND test_installer_package)

        add_executable(test_installer_pathlist
            tests/test_installer_pathlist.c
            installer/pathlist.c
        )
        target_include_directories(test_installer_pathlist PRIVATE installer)
        target_compile_definitions(test_installer_pathlist PRIVATE _WIN32_WINNT=0x0601)
        if(MSVC)
            target_compile_options(test_installer_pathlist PRIVATE /W4 /utf-8)
        else()
            target_compile_options(test_installer_pathlist PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        if(MINGW)
            # The test intentionally uses wmain so UTF-16 PATH cases are
            # exercised without a lossy console conversion.
            target_link_options(test_installer_pathlist PRIVATE -municode)
        endif()
        add_test(NAME installer_pathlist COMMAND test_installer_pathlist)

        add_executable(test_installer_lang
            tests/test_lang.c
            installer/lang.c
        )
        target_include_directories(test_installer_lang PRIVATE installer)
        target_compile_definitions(test_installer_lang PRIVATE _WIN32_WINNT=0x0601)
        if(MSVC)
            target_compile_options(test_installer_lang PRIVATE /W4 /utf-8)
        else()
            target_compile_options(test_installer_lang PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        target_link_libraries(test_installer_lang PRIVATE user32)
        add_test(NAME installer_lang COMMAND test_installer_lang)

        add_executable(test_installer_data_cleanup
            tests/test_installer_data_cleanup.c
            installer/files.c
            installer/package.c
            src/platform.c
            src/util.c
            src/sha256.c
        )
        target_include_directories(test_installer_data_cleanup PRIVATE src installer)
        target_compile_definitions(test_installer_data_cleanup PRIVATE _WIN32_WINNT=0x0601)
        if(MSVC)
            target_compile_options(test_installer_data_cleanup PRIVATE /W4 /utf-8)
        else()
            target_compile_options(test_installer_data_cleanup PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        target_link_libraries(test_installer_data_cleanup PRIVATE
            advapi32 winhttp shell32 ole32
        )
        add_test(NAME installer_data_cleanup COMMAND test_installer_data_cleanup)

        if(NOT CMAKE_CROSSCOMPILING)
            add_test(NAME installer
                COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                        -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_installer.ps1
                        -Setup "${JVMAN_SETUP_OUTPUT}"
                        -Binary $<TARGET_FILE:jvman>)
            set_tests_properties(installer PROPERTIES
                DEPENDS "installer_package;installer_pathlist"
            )
        endif()
    else()
        add_test(NAME cli
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.sh
                    $<TARGET_FILE:jvman>)
    endif()
endif()

if(WIN32)
    target_link_libraries(jvman PRIVATE advapi32 winhttp shell32 user32)
endif()

install(TARGETS jvman RUNTIME DESTINATION bin)
