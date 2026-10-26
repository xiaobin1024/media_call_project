set(CMAKE_AUTOMOC ON)

set(SOURCES
    main.cpp
    ui/main_window.cpp
    ui/call_window.cpp
    signaling/signaling_client.cpp
    media/audio_capture.cpp
    media/audio_playback.cpp
    media/video_capture.cpp
    media/video_capture_worker.cpp
    media/video_render.cpp
    transport/rtp_packet.cpp
    transport/rtp_session.cpp
    transport/jitter_buffer.cpp
    call/call_manager.cpp
)

set(HEADERS
    ui/main_window.h
    ui/call_window.h
    signaling/signaling_client.h
    media/media_device.h
    media/audio_capture.h
    media/audio_playback.h
    media/video_capture.h
    media/video_capture_worker.h
    media/video_render.h
    transport/rtp_packet.h
    transport/rtp_session.h
    transport/jitter_buffer.h
    call/call_manager.h
)

add_executable(call_client ${SOURCES} ${HEADERS})

target_include_directories(call_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(call_client PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Multimedia
    Qt${QT_VERSION_MAJOR}::MultimediaWidgets
)

set_target_properties(call_client PROPERTIES
    WIN32_EXECUTABLE TRUE
)
