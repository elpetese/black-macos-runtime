# test_guest_layout - checks the POSIX platform layer can build the Xbox guest
# address space the way xbox_MemoryLayoutInit needs it on a 16 KB-page host.
# POSIX only: it drives win32_compat's own reservation entry point, which the
# Windows build does not have.

if(NOT WIN32)
    add_executable(test_guest_layout test_guest_layout.c)
    target_link_libraries(test_guest_layout PRIVATE platform)
    target_include_directories(test_guest_layout PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src            # platform/xbox_winnt.h
    )
    add_test(NAME guest_layout COMMAND test_guest_layout)
endif()
