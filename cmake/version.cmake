# Writes OUT with the version string, from VERSION plus whatever of the three
# coordinates can be read here. Any of them missing is left out of the string.

set(revision "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${SRC}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
    WORKING_DIRECTORY "${SRC}"
    OUTPUT_VARIABLE revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE git_status)
  if(NOT git_status EQUAL 0)
    set(revision "")
  endif()
endif()

set(firefox "")
set(parity_header "${SRC}/cleartype/src/firefox_parity_data.h")
if(EXISTS "${parity_header}")
  file(STRINGS "${parity_header}" parity_head LIMIT_COUNT 4)
  foreach(line IN LISTS parity_head)
    if(line MATCHES "tree at ([A-Za-z0-9_.]+) \\(([0-9a-f]+)\\)")
      set(firefox_tag "${CMAKE_MATCH_1}")
      string(SUBSTRING "${CMAKE_MATCH_2}" 0 8 firefox_rev)
      if(firefox_tag MATCHES "^FIREFOX_([0-9]+)_([0-9]+)_RELEASE$")
        set(firefox_tag "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
      endif()
      set(firefox "firefox ${firefox_tag}/${firefox_rev}")
    endif()
  endforeach()
endif()

# The GNU build id of the vendored implementation, from its note: namesz 4,
# descsz 20, type 3, "GNU\0", then the hash.
set(implementation "")
set(implementation_so "${SRC}/third_party/office-android/libdwritecore.so")
if(EXISTS "${implementation_so}")
  file(READ "${implementation_so}" head HEX LIMIT 4096)
  string(FIND "${head}" "040000001400000003000000474e5500" note)
  if(NOT note EQUAL -1)
    math(EXPR note "${note} + 32")
    string(SUBSTRING "${head}" ${note} 8 implementation)
    set(implementation "impl ${implementation}")
  endif()
endif()

set(coordinates "")
foreach(part IN ITEMS "${revision}" "${firefox}" "${implementation}")
  if(part)
    list(APPEND coordinates "${part}")
  endif()
endforeach()

set(version "dwritecore-shim ${VERSION}")
if(coordinates)
  string(REPLACE ";" ", " coordinates "${coordinates}")
  set(version "${version} (${coordinates})")
endif()

set(text "#ifndef CLEARTYPE_VERSION_H_INCLUDED\n")
string(APPEND text "#define CLEARTYPE_VERSION_H_INCLUDED\n")
string(APPEND text "#define CLEARTYPE_VERSION_STRING \"${version}\"\n")
string(APPEND text "#endif  // CLEARTYPE_VERSION_H_INCLUDED\n")

set(previous "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" previous)
endif()
if(NOT previous STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
