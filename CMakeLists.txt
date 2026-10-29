# Kairo: navegador sobre CEF (Chromium Embedded Framework) para Windows.
# Se compila dentro de https://github.com/chromiumembedded/cef-project
# (ver README.md: copia esta carpeta y añade add_subdirectory(kairo) en el CMakeLists raíz).

set(CEF_TARGET kairo)

set(KAIRO_SRCS
  src/main.cc
  src/app.cc
  src/app.h
  src/adblock.cc
  src/adblock.h
  src/shell.cc
  src/shell.h
)

add_executable(${CEF_TARGET} WIN32 ${KAIRO_SRCS})
SET_EXECUTABLE_TARGET_PROPERTIES(${CEF_TARGET})
add_dependencies(${CEF_TARGET} libcef_dll_wrapper)
target_link_libraries(${CEF_TARGET} libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
target_compile_features(${CEF_TARGET} PRIVATE cxx_std_20)

# DLLs y recursos de CEF junto al .exe
COPY_FILES("${CEF_TARGET}" "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
COPY_FILES("${CEF_TARGET}" "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")

# Interfaz (HTML/CSS/JS) y listas de filtros junto al .exe
add_custom_command(TARGET ${CEF_TARGET} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:${CEF_TARGET}>/ui")
