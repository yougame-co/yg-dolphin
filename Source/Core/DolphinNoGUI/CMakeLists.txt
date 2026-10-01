add_executable(dolphin-nogui
  Platform.cpp
  Platform.h
  PlatformHeadless.cpp
  MainNoGUI.cpp
)

if(X11_FOUND)
  target_sources(dolphin-nogui PRIVATE PlatformX11.cpp)
  target_link_libraries(dolphin-nogui PRIVATE PkgConfig::XRANDR PkgConfig::X11)
endif()

if(WIN32)
  target_sources(dolphin-nogui PRIVATE PlatformWin32.cpp)
endif()

if(APPLE)
  target_sources(dolphin-nogui PRIVATE PlatformMacos.mm)
  target_compile_options(dolphin-nogui PRIVATE -fobjc-arc)
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  target_sources(dolphin-nogui PRIVATE PlatformFBDev.cpp)
endif()

if (WIN32)
  target_sources(dolphin-nogui PRIVATE DolphinNoGUI.exe.manifest DolphinNoGUI.rc)

  set_target_properties(dolphin-nogui PROPERTIES
    DEBUG_POSTFIX    D
    OUTPUT_NAME      DolphinNoGUI
  )
else()
  set_target_properties(dolphin-nogui PROPERTIES OUTPUT_NAME dolphin-emu-nogui)
endif()

target_link_libraries(dolphin-nogui
PRIVATE
  core
  uicommon
  cpp-optparse
)

if(APPLE)
  target_link_libraries(dolphin-nogui
    PRIVATE
      ${APPKIT_LIBRARY}
      ${COREFOUNDATION_LIBRARY}
      ${IOK_LIBRARY}
  )
endif()

if(WIN32)
  # needed for adjusting window decorations with DwmSetWindowAttribute
  target_link_libraries(dolphin-nogui PRIVATE dwmapi.lib)
endif()

if(MSVC)
  # Add precompiled header
  target_link_libraries(dolphin-nogui PRIVATE use_pch)
endif()

if(USE_DISCORD_PRESENCE)
  target_compile_definitions(dolphin-nogui PRIVATE -DUSE_DISCORD_PRESENCE)
endif()

if(APPLE)
  # Orca: a headless macOS build looks for Sys at <exe dir>/Contents/Resources/Sys (the bundle layout),
  # which a plain build never creates. Link the source tree's Data/Sys there so game INIs and Orca
  # profiles are found in development builds.
  add_custom_command(TARGET dolphin-nogui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dolphin-nogui>/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:dolphin-nogui>/Contents/Resources/Sys"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/Data/Sys"
            "$<TARGET_FILE_DIR:dolphin-nogui>/Contents/Resources/Sys")
endif()

set(CPACK_PACKAGE_EXECUTABLES ${CPACK_PACKAGE_EXECUTABLES} dolphin-nogui)
if (NOT WIN32)
  install(TARGETS dolphin-nogui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
