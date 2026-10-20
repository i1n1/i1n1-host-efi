# SPDX-License-Identifier: GPL-2.0-only
#
# Registry of the sibling i1n1 repositories (including transitive ones) this repository builds against,
# and the resolver i1n1_require(). Resolution order for each repository:
#   1. already loaded in this CMake run;
#   2. -DI1N1_<NAME>_SOURCE_DIR=<local checkout> (CORE, ENGINE, PLATFORM, DRIVERZONE, HOST_EFI);
#   3. FetchContent of the pinned URL and commit below (FETCHCONTENT_SOURCE_DIR_I1N1_<NAME> also applies).
# Entries are stored in global properties and the first registration wins, so a dependency that loads its
# own copy of this file never hides the registry of the project that loaded it.
include(FetchContent)

function(_i1n1_register name url commit)
  get_property(_known GLOBAL PROPERTY I1N1_REPO_URL_${name} SET)
  if(NOT _known)
    set_property(GLOBAL PROPERTY I1N1_REPO_URL_${name} "${url}")
    set_property(GLOBAL PROPERTY I1N1_REPO_COMMIT_${name} "${commit}")
  endif()
endfunction()

_i1n1_register(core "https://github.com/i1n1/i1n1-core.git" "ab989e7ec2f8c8b0827f99370bee445ee1b91026")
_i1n1_register(engine "https://github.com/i1n1/i1n1-engine.git" "f34447e2d3a243073578a36acf1b059448723fd6")
_i1n1_register(platform "https://github.com/i1n1/i1n1-platform.git" "9644a35f570ecc066e2329a1b91a0ec0a34696dc")

function(i1n1_require name)
  string(TOUPPER "${name}" _up)
  get_property(_loaded GLOBAL PROPERTY I1N1_REPO_${_up}_LOADED)
  if(_loaded)
    return()
  endif()
  if(I1N1_${_up}_SOURCE_DIR)
    add_subdirectory("${I1N1_${_up}_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_i1n1/${name}")
    return()
  endif()
  get_property(_url GLOBAL PROPERTY I1N1_REPO_URL_${name})
  get_property(_commit GLOBAL PROPERTY I1N1_REPO_COMMIT_${name})
  if(NOT _url)
    message(FATAL_ERROR "i1n1_require: unknown repository '${name}'")
  endif()
  if(_commit STREQUAL "UNPINNED")
    message(FATAL_ERROR "i1n1 dependency '${name}' has no recorded commit. Configure with "
      "-DI1N1_${_up}_SOURCE_DIR=<path to a checkout of ${_url}>")
  endif()
  FetchContent_Declare(i1n1_${name}
    GIT_REPOSITORY "${_url}"
    GIT_TAG "${_commit}")
  FetchContent_MakeAvailable(i1n1_${name})
endfunction()
