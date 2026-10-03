add_dolphin_test(VertexLoaderTest VertexLoaderTest.cpp)

# Orca: D3D's FXC cache (ORCA.md, "Match start"). D3DCommon is built on Windows only; the cache needs
# Windows only for the compiler DLL's digest, so elsewhere the test builds its source itself.
if(WIN32)
  add_dolphin_test(FxcCacheTest FxcCacheTest.cpp)
else()
  add_dolphin_test(FxcCacheTest FxcCacheTest.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/VideoBackends/D3DCommon/FxcCache.cpp)
endif()
