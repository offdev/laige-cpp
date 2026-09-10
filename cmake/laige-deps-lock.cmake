# ============================================================================
# laige-deps-lock.cmake — vendored dependency lock verification (M0-DEP-01)
#
# deps.lock (repo root) is the machine-readable record of every vendored
# dependency under deps/ (PRD §11, NFR-8.6, AGENTS DEP-005). Each entry:
#
#   name           stable dependency name (matches deps/<name>/)
#   version        upstream version string
#   path           vendored tree, relative to the repo root (deps/<name>)
#   owner          the module that owns (wraps) the dependency: 'tests'
#                  for dev-only deps used only by the test suite, or
#                  'src/laige-<module>' for engine modules (AGENTS DEP-004).
#                  tools/laige-include-lint (M0-CI-03) enforces that the
#                  dependency is only included from this boundary.
#   source_url     upstream repository URL
#   source_commit  upstream git commit the vendored tree was taken from
#   sha256         SHA-256 of the vendored tree (definition below)
#   license        upstream license
#   justification  PRD §11 row justifying the dependency
#
# laige_deps_verify_lock() is called from the root CMakeLists.txt on every
# configure. It re-hashes each vendored tree and fails loudly (FATAL_ERROR)
# on any mismatch, missing file, unlisted vendored directory, or malformed
# lock (CORE-008: no silent failure).
#
# deps.lock is canonical JSON: one key/value pair per line, entry fields in
# the fixed order above, the "entries" array spanning multiple lines, no
# embedded quotes. The parser below relies on that layout; keep the file in
# that order when editing it.
#
# Parser note: the lines are extracted with string(FIND)/string(SUBSTRING)
# and never via semicolon-list splitting. CMake's list separator is ";", and
# CMake 4.x additionally does not treat ";" inside "[...]" as a separator,
# so splitting JSON text on ";" is version-dependent and unreliable; plain
# string operations behave identically on CMake 3.22 through 4.x.
# ============================================================================

# laige_deps_tree_sha256(<dir> <out-var>)
#
# Deterministic, cross-platform tree hash (content-addressed):
#
#   for every regular file under <dir>, relative to <dir> with "/" path
#   separators, sorted ascending by relative path:
#     blob += hex(sha256(file content)) + "\n" + relpath + "\n"
#   <out-var> = sha256(blob)
#
# An empty tree hashes the empty blob. Hidden files are included; symlinks
# are neither followed nor hashed (the vendored trees contain none).
#
# The hash covers file content, so checkouts must be byte-identical on every
# platform. .gitattributes enforces this: deps/** is -text (never
# converted) and the repo default is text=auto eol=lf. A CRLF checkout
# (e.g. core.autocrlf=true on Windows without attributes) changes every
# file hash and fails here.
function(laige_deps_tree_sha256 dir out_var)
  set(_files "")
  file(GLOB_RECURSE _files "${dir}/*")
  set(_rels "")
  foreach(_f IN LISTS _files)
    if(NOT IS_DIRECTORY "${_f}" AND NOT IS_SYMLINK "${_f}")
      file(RELATIVE_PATH _rel "${dir}" "${_f}")
      string(REPLACE "\\" "/" _rel "${_rel}")
      list(APPEND _rels "${_rel}")
    endif()
  endforeach()
  list(SORT _rels)
  set(_blob "")
  foreach(_rel IN LISTS _rels)
    file(SHA256 "${dir}/${_rel}" _h)
    string(APPEND _blob "${_h}\n${_rel}\n")
  endforeach()
  string(SHA256 _hash "${_blob}")
  set(${out_var} "${_hash}" PARENT_SCOPE)
endfunction()

# laige_deps_verify_lock()
#
# Verifies deps.lock against the vendored trees. Must be called from the
# top-level CMakeLists (it resolves deps.lock and deps/ against the
# current source directory).
function(laige_deps_verify_lock)
  set(_lock_path "${CMAKE_CURRENT_SOURCE_DIR}/deps.lock")
  if(NOT EXISTS "${_lock_path}")
    message(FATAL_ERROR
      "laige-deps: deps.lock not found at ${_lock_path}.\n"
      "Every vendored dependency under deps/ must be listed in deps.lock "
      "(PRD §11, M0-DEP-01).")
  endif()

  file(READ "${_lock_path}" _content)
  string(REPLACE "\r\n" "\n" _content "${_content}")

  # --- line extraction (pure string operations; see Parser note) ---------
  set(_rest "${_content}")
  set(_line_no 0)
  set(_state "root")
  set(_in_entry 0)
  set(_n_fields 0)
  set(_saw_entries 0)
  set(_lock_version "")
  set(_entries 0)
  set(_names "")
  set(_checked_paths "")

  foreach(_iter RANGE 10000)
    string(FIND "${_rest}" "\n" _nl)
    if(_nl EQUAL -1)
      set(_line "${_rest}")
      set(_rest "")
      set(_last 1)
    else()
      string(SUBSTRING "${_rest}" 0 "${_nl}" _line)
      math(EXPR _after "${_nl} + 1")
      string(SUBSTRING "${_rest}" "${_after}" -1 _rest)
      set(_last 0)
    endif()
    math(EXPR _line_no "${_line_no} + 1")

    if(_line STREQUAL "")
      if(_last)
        break()
      endif()
      continue()
    endif()

    if(_state STREQUAL "root")
      if(_line MATCHES "^[ \t]*\\{[ \t]*$")
        set(_state "root-object")
      else()
        message(FATAL_ERROR "laige-deps: malformed deps.lock — expected "
                            "'{' on line ${_line_no}: '${_line}'")
      endif()
      continue()
    endif()

    if(_state STREQUAL "root-object")
      if(_line MATCHES "^[ \t]*\"version\"[ \t]*:[ \t]*([0-9]+),?[ \t]*$")
        set(_lock_version "${CMAKE_MATCH_1}")
      elseif(_line MATCHES "^[ \t]*\"entries\"[ \t]*:[ \t]*\\[[ \t]*$")
        set(_saw_entries 1)
        set(_state "entries")
      elseif(_line MATCHES "^[ \t]*\\}[ \t]*$")
        set(_state "closed")
      else()
        message(FATAL_ERROR "laige-deps: malformed deps.lock — unexpected "
                            "top-level line ${_line_no}: '${_line}'")
      endif()
      continue()
    endif()

    if(_state STREQUAL "entries")
      if(_line MATCHES "^[ \t]*\\{[ \t]*$")
        if(_in_entry)
          message(FATAL_ERROR "laige-deps: malformed deps.lock — entry "
                              "opened twice on line ${_line_no} (missing "
                              "'}' before a new '{').")
        endif()
        set(_in_entry 1)
        set(_n_fields 0)
        set(_f_name "" _f_version "" _f_path "" _f_owner ""
            _f_source_url "" _f_source_commit "" _f_sha256 ""
            _f_license "" _f_justification "")
      elseif(_line MATCHES "^[ \t]*\\}[ \t]*$")
        if(NOT _in_entry)
          message(FATAL_ERROR "laige-deps: malformed deps.lock — stray "
                              "'}' on line ${_line_no} in the entries "
                              "array.")
        endif()
        set(_in_entry 0)

        # --- validate one complete entry (fields in _f_* variables) -------
        if(NOT _n_fields EQUAL 9)
          message(FATAL_ERROR "laige-deps: deps.lock entry ${_entries} has "
                              "${_n_fields} fields (expected 9, each "
                              "exactly once) — duplicate or missing field.")
        endif()
        set(_missing "")
        foreach(_f IN ITEMS _f_name _f_version _f_path _f_owner
                        _f_source_url _f_source_commit _f_sha256
                        _f_license _f_justification)
          if(${_f} STREQUAL "")
            list(APPEND _missing "${_f}")
          endif()
        endforeach()
        if(NOT _missing STREQUAL "")
          message(FATAL_ERROR "laige-deps: deps.lock entry ${_entries} is "
                              "missing field(s): ${_missing}")
        endif()
        math(EXPR _entries "${_entries} + 1")
        list(FIND _names "${_f_name}" _dup)
        if(NOT _dup EQUAL -1)
          message(FATAL_ERROR "laige-deps: duplicate deps.lock entry name "
                              "'${_f_name}'.")
        endif()
        list(APPEND _names "${_f_name}")
        if(NOT _f_path MATCHES "^deps/[^/]+$")
          message(FATAL_ERROR "laige-deps: entry '${_f_name}' path must be "
                              "an immediate subdirectory of deps/, got "
                              "'${_f_path}'.")
        endif()
        if(NOT _f_owner MATCHES "^(tests|src/laige-[a-z0-9]+)$")
          message(FATAL_ERROR "laige-deps: entry '${_f_name}' owner must be "
                              "'tests' or 'src/laige-<module>' — the module "
                              "that owns (wraps) the dependency (AGENTS "
                              "DEP-004; enforced by tools/laige-include-lint) — "
                              "got '${_f_owner}'.")
        endif()
        if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${_f_path}")
          message(FATAL_ERROR "laige-deps: entry '${_f_name}' path "
                              "'${_f_path}' is not a directory.")
        endif()
        # CMake's regex has no {n} repetition, so validate class + length.
        string(LENGTH "${_f_sha256}" _sha_len)
        if(NOT _f_sha256 MATCHES "^[0-9a-f]+$" OR NOT _sha_len EQUAL 64)
          message(FATAL_ERROR "laige-deps: entry '${_f_name}' sha256 must "
                              "be a 64-char lowercase hex digest, got "
                              "'${_f_sha256}'.")
        endif()
        string(LENGTH "${_f_source_commit}" _commit_len)
        if(NOT _f_source_commit MATCHES "^[0-9a-f]+$" OR NOT _commit_len EQUAL 40)
          message(FATAL_ERROR "laige-deps: entry '${_f_name}' "
                              "source_commit must be a 40-char lowercase "
                              "git SHA, got '${_f_source_commit}'.")
        endif()

        # Re-hash the vendored tree and compare against the lock.
        laige_deps_tree_sha256(
          "${CMAKE_CURRENT_SOURCE_DIR}/${_f_path}" _actual)
        if(NOT _actual STREQUAL _f_sha256)
          message(FATAL_ERROR
            "laige-deps: VENDORED TREE MISMATCH for '${_f_name}' "
            "(${_f_path}).\n"
            "  expected (deps.lock): ${_f_sha256}\n"
            "  actual (on disk):     ${_actual}\n"
            "The vendored tree was modified, incomplete, or the lock was "
            "not regenerated. Re-vendor from the pinned source commit "
            "(source_url @ source_commit) and re-record the tree hash in "
            "deps.lock (DEP-005).")
        endif()
        list(APPEND _checked_paths "${_f_path}")
      elseif(_line MATCHES "^[ \t]*\\][ \t]*$")
        set(_state "entries-closed")
      elseif(_line MATCHES "^[ \t]*\"([a-z0-9_]+)\"[ \t]*:[ \t]*\"([^\"]*)\",?[ \t]*$")
        if(NOT _in_entry)
          message(FATAL_ERROR "laige-deps: malformed deps.lock — field "
                              "outside an entry on line ${_line_no}: "
                              "'${_line}'")
        endif()
        math(EXPR _n_fields "${_n_fields} + 1")
        if(CMAKE_MATCH_1 STREQUAL "name")
          set(_f_name "${CMAKE_MATCH_2}")
        elseif(CMAKE_MATCH_1 STREQUAL "version")
          set(_f_version "${CMAKE_MATCH_2}")
        elseif(CMAKE_MATCH_1 STREQUAL "path")
          set(_f_path "${CMAKE_MATCH_2}")
        elseif(CMAKE_MATCH_1 STREQUAL "owner")
          set(_f_owner "${CMAKE_MATCH_2}")
        elseif(CMAKE_MATCH_1 STREQUAL "source_url")
          set(_f_source_url "${CMAKE_MATCH_2}")
        elseif(CMAKE_MATCH_1 STREQUAL "source_commit")
          set(_f_source_commit "${CMAKE_MATCH_2}")
        elseif(CMAKE_MATCH_1 STREQUAL "sha256")
          set(_f_sha256 "${CMAKE_MATCH_2}")
        elseif(CMAKE_MATCH_1 STREQUAL "license")
          set(_f_license "${CMAKE_MATCH_2}")
        elseif(CMAKE_MATCH_1 STREQUAL "justification")
          set(_f_justification "${CMAKE_MATCH_2}")
        else()
          message(FATAL_ERROR "laige-deps: unknown field '${CMAKE_MATCH_1}' "
                              "in deps.lock entry on line ${_line_no}.")
        endif()
      else()
        message(FATAL_ERROR "laige-deps: malformed deps.lock — unexpected "
                            "line ${_line_no} in the entries array: "
                            "'${_line}'")
      endif()
      continue()
    endif()

    if(_state STREQUAL "entries-closed")
      if(_line MATCHES "^[ \t]*\\}[ \t]*$")
        set(_state "closed")
      else()
        message(FATAL_ERROR "laige-deps: malformed deps.lock — content "
                            "after the entries array on line ${_line_no}: "
                            "'${_line}'")
      endif()
      continue()
    endif()

    # state == closed
    message(FATAL_ERROR "laige-deps: malformed deps.lock — content after "
                        "the closing '}' on line ${_line_no}: '${_line}'")
  endforeach()

  if(NOT _state STREQUAL "closed")
    message(FATAL_ERROR "laige-deps: deps.lock is truncated — the root "
                        "object is never closed.")
  endif()
  if(NOT _lock_version STREQUAL "1")
    message(FATAL_ERROR "laige-deps: unsupported deps.lock version "
                        "'${_lock_version}' (supported: 1).")
  endif()
  if(NOT _saw_entries)
    message(FATAL_ERROR "laige-deps: deps.lock is missing the 'entries' "
                        "array.")
  endif()

  # Completeness: every immediate subdirectory of deps/ must be a listed
  # entry, so no vendored code can exist outside the lock (DEP-005).
  set(_dep_dirs "")
  file(GLOB _dep_dirs "${CMAKE_CURRENT_SOURCE_DIR}/deps/*")
  foreach(_d IN LISTS _dep_dirs)
    if(IS_DIRECTORY "${_d}" AND NOT IS_SYMLINK "${_d}")
      file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}/deps" "${_d}")
      string(REPLACE "\\" "/" _rel "${_rel}")
      list(FIND _checked_paths "deps/${_rel}" _idx)
      if(_idx EQUAL -1)
        message(FATAL_ERROR "laige-deps: directory 'deps/${_rel}' is not "
                            "listed in deps.lock. Add an entry (PRD §11, "
                            "DEP-003) or remove the directory.")
      endif()
    endif()
  endforeach()

  message(STATUS "laige-deps: deps.lock OK — ${_entries} vendored "
                "dependency(ies) verified")
endfunction()
