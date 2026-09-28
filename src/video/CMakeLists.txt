# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8)
if(WIN32)
    target_sources(xbox_video PRIVATE fb_present.c)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
elseif(APPLE)
    target_sources(xbox_video PRIVATE fb_present_posix.m video_pump_posix.m)
    target_link_libraries(xbox_video PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Cocoa" "-framework AVFoundation")
else()
    # fb_present.c supplies the no-op fallback outside Windows and macOS.
    target_sources(xbox_video PRIVATE fb_present.c)
endif()
