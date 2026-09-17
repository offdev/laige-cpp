# hello line-budget check (M1-SAMPLE-01; PRD §9.4, NFR-13.5).
#
# The game-code budget: the game source (samples/hello/hello.cpp) is
# < 100 lines of CODE. A code line is a line that is neither blank nor
# a // comment (the file uses only // comments): the NFR-13.5
# "heavily commented" requirement lives in the comment lines and does
# not count against the budget (PRD §9.4: "< 100 lines of code").
# Platform-independent: CMake core only, no compiler involved.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{SAMPLE_HELLO_SRC})
  message(FATAL_ERROR "hello_line_budget: SAMPLE_HELLO_SRC environment "
                     "variable is not set (configure error)")
endif()

file(READ "$ENV{SAMPLE_HELLO_SRC}" _content)
string(REPLACE "\r\n" "\n" _content "${_content}")
# Protect C++ semicolons before using the semicolon as the CMake list
# separator: a naive newline split would let every `;` in a line become
# an extra list element (and multi-`;` lines would count as several code
# lines). The sentinel must not itself contain a semicolon (it would
# become a list separator) and cannot occur in a C++ source file.
string(REPLACE ";" "@S@" _protected "${_content}")
string(REPLACE "\n" ";" _lines "${_protected}")

set(_code 0)
set(_comments 0)
foreach(_line IN LISTS _lines)
  # CMake's regex engine has no POSIX character classes, so the
  # whitespace class is spelled explicitly (spaces/tabs — the file's
  # indentation alphabet).
  if(_line MATCHES "^[ \t]*(//.*)?$")
    if(NOT _line MATCHES "^[ \t]*$")
      math(EXPR _comments "${_comments} + 1")
    endif()
  else()
    math(EXPR _code "${_code} + 1")
  endif()
endforeach()

if(_code GREATER_EQUAL 100)
  message(FATAL_ERROR
    "hello line budget: ${_code} code lines in hello.cpp "
    "(the PRD §9.4 budget is < 100 lines of code; "
    "${_comments} comment lines do not count)")
endif()

message(STATUS "hello_line_budget: OK (${_code} code lines < 100; "
              "${_comments} comment lines)")
