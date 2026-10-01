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

if(APPLE)
  option(ORCA_MACOS_BUNDLE "Build the NoGUI frontend as Orca.app, a self-contained bundle" OFF)
  set(ORCA_VERSION "0.1.0" CACHE STRING "The Orca version in the bundle's Info.plist")
endif()

if (WIN32)
  target_sources(dolphin-nogui PRIVATE DolphinNoGUI.exe.manifest DolphinNoGUI.rc)

  set_target_properties(dolphin-nogui PROPERTIES
    DEBUG_POSTFIX    D
    OUTPUT_NAME      DolphinNoGUI
  )
elseif(APPLE AND ORCA_MACOS_BUNDLE)
  # Orca.app: a session by default, its own user dir, and a disc picker (MainNoGUI.cpp).
  target_compile_definitions(dolphin-nogui PRIVATE ORCA_MACOS_BUNDLE)
  set_target_properties(dolphin-nogui PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Orca-Info.plist.in"
    OUTPUT_NAME Orca
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

if(APPLE AND ORCA_MACOS_BUNDLE)
  # Orca.app: Sys is copied into Contents/Resources (a release can't point at a source tree), without
  # the images and themes only the Qt UI uses; the OSD font stays. The GPL text travels with it.
  file(GLOB_RECURSE orca_sys RELATIVE "${CMAKE_SOURCE_DIR}/Data" "${CMAKE_SOURCE_DIR}/Data/Sys/*")
  list(FILTER orca_sys EXCLUDE REGEX "^Sys/Themes/")
  list(FILTER orca_sys EXCLUDE REGEX
       "^Sys/Resources/(Flag_|Platform_|achievements_|dolphin_logo|isoproperties_|nobanner)")
  foreach(res ${orca_sys})
    target_sources(dolphin-nogui PRIVATE "${CMAKE_SOURCE_DIR}/Data/${res}")
    get_filename_component(resdir "${res}" DIRECTORY)
    set_source_files_properties("${CMAKE_SOURCE_DIR}/Data/${res}" PROPERTIES
      MACOSX_PACKAGE_LOCATION "Resources/${resdir}")
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Data/Sys")
  set(orca_resources "${CMAKE_SOURCE_DIR}/COPYING" "${CMAKE_SOURCE_DIR}/Data/Orca.icns")
  target_sources(dolphin-nogui PRIVATE ${orca_resources})
  set_source_files_properties(${orca_resources} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  file(GLOB orca_licenses "${CMAKE_SOURCE_DIR}/LICENSES/*")
  target_sources(dolphin-nogui PRIVATE ${orca_licenses})
  set_source_files_properties(${orca_licenses} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/LICENSES)
elseif(APPLE)
  # Orca: a headless macOS build looks for Sys at <exe dir>/Contents/Resources/Sys (the bundle layout),
  # which a plain build never creates. Link the source tree's Data/Sys there so game INIs and Orca
  # profiles are found in development builds.
  add_custom_command(TARGET dolphin-nogui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dolphin-nogui>/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:dolphin-nogui>/Contents/Resources/Sys"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/Data/Sys"
            "$<TARGET_FILE_DIR:dolphin-nogui>/Contents/Resources/Sys")
elseif(NOT ANDROID AND NOT ENABLE_QT)
  # Orca: on Windows and Linux Sys is found next to the executable, and only DolphinQt's build puts
  # it there. A build without Qt links (or, without symlink rights on Windows, copies) it itself, or
  # no game INI or Orca profile is found. With Qt on, DolphinQt does it; doing it twice would race.
  add_custom_command(TARGET dolphin-nogui POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      -Dtarget="${CMAKE_SOURCE_DIR}/Data/Sys"
      -Dlink="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Sys"
      -P="${CMAKE_SOURCE_DIR}/CMake/DolphinSymlinkOrCopy.cmake"
  )
endif()

set(CPACK_PACKAGE_EXECUTABLES ${CPACK_PACKAGE_EXECUTABLES} dolphin-nogui)
if (NOT WIN32 AND NOT ORCA_MACOS_BUNDLE)
  install(TARGETS dolphin-nogui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
