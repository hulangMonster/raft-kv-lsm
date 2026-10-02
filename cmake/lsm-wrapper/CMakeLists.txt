# M6.10 N4: wrapper subdirectory that pulls in the pinned lsm source tree while
# swallowing its CTest registration.
#
# Why: the lsm CMakeLists calls a bare enable_testing() + add_test(lsm_unit) and
# does not consult BUILD_TESTING, while its lsm_tests binary is EXCLUDE_FROM_ALL
# (we deliberately never build it). That leaked into our build: `ctest -N`
# reported "Could not find executable .../bin/lsm_tests", and neither
# -DBUILD_TESTING=OFF nor clearing the directory's TESTS property nor setting
# DISABLED stops CTest from resolving the executable.
#
# add_test/function definitions are directory-scoped and do not propagate to the
# parent, so shadowing add_test *here* silences only the lsm subtree; our own
# add_test(raftkv_unit/raftkv_raft) calls are unaffected. The lsm tree itself is
# not modified. Targets defined below (lsm) stay visible to the parent, which is
# all we need for target_link_libraries.
function(add_test)
endfunction()

add_subdirectory("${RAFTKV_LSM_DIR}" "${CMAKE_BINARY_DIR}/lsm-build" EXCLUDE_FROM_ALL)
