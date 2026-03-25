cmake_minimum_required(VERSION 3.16)
project(ili9341_tests C)

# --- ILI9341 driver tests (need ESP-IDF mocks) ---
add_executable(test_ili9341
    test_ili9341.c
    mocks/esp_idf_mock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ili9341/ili9341.c
)
target_include_directories(test_ili9341 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ili9341/include
)

# --- Framebuffer tests (no mocks needed, pure logic) ---
add_executable(test_framebuffer
    test_framebuffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/framebuffer/framebuffer.c
)
target_include_directories(test_framebuffer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/framebuffer/include
)

# --- Audio driver tests ---
add_executable(test_audio
    test_audio.c
    mocks/esp_idf_mock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/audio/audio.c
)
target_include_directories(test_audio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/audio/include
)
target_link_libraries(test_audio m)
