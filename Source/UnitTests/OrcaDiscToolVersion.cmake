# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
file(STRINGS "${SOURCE}/Source/Core/Core/Orca/DiscLayout.h" format_line
  REGEX "^inline constexpr u64 FORMAT_VERSION = [0-9]+;")
string(REGEX REPLACE ".* = ([0-9]+);" "\\1" format_version "${format_line}")
execute_process(COMMAND "${TOOL}" --version
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0 OR NOT errors STREQUAL "" OR
   NOT output STREQUAL "orca-disc-tool ${VERSION} layout ${format_version}\n")
  message(FATAL_ERROR "Unexpected --version: exit ${result}, stdout '${output}', stderr '${errors}'")
endif()
