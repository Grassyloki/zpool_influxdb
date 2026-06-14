cmake_minimum_required (VERSION 3.7)
project (zpool_influxdb C)

include(CheckCSourceCompiles)

# Distribution packages (Debian/Ubuntu zfsutils-linux, Arch/CachyOS, etc.)
# install the libzfs headers and libraries under /usr. Source installs of
# ZFSonLinux default to /usr/local. Override on the cmake command-line if
# yours differs, e.g.:
#      -D ZFS_INSTALL_BASE=/usr/local
set(ZFS_INSTALL_BASE /usr CACHE STRING "zfs installation base directory")

# Target CPU microarchitecture. Defaults to x86-64-v4 (AVX-512 baseline).
# NOTE: a v4 binary will SIGILL on CPUs older than x86-64-v4. To build a
# portable binary, or target a different level, override on the command line:
#      -D TARGET_ARCH=x86-64-v3      (AVX2 baseline)
#      -D TARGET_ARCH=               (compiler default, most portable)
set(TARGET_ARCH "x86-64-v4" CACHE STRING "value passed to the compiler -march= flag")

# to support unsigned 64-bit ints properly, uncomment below to set the
# SUPPORT_UINT64 flag at compile time.
# _LARGEFILE64_SOURCE is required so that the libspl headers can use the
# transitional stat64()/fstat64() interfaces with modern glibc.
set(CMAKE_C_FLAGS "-DSUPPORT_UINT64 -D_LARGEFILE64_SOURCE")
if(TARGET_ARCH)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=${TARGET_ARCH}")
endif()

include_directories(${ZFS_INSTALL_BASE}/include/libspl ${ZFS_INSTALL_BASE}/include/libzfs)
link_directories(${ZFS_INSTALL_BASE}/lib)

# The third argument of nvlist_lookup_string() gained a const qualifier in
# OpenZFS 2.2. Detect which signature this libzfs uses so the source can match
# it and build cleanly on both old (e.g. Debian 12 / ZFS 2.1) and new headers.
set(CMAKE_REQUIRED_INCLUDES ${ZFS_INSTALL_BASE}/include/libspl ${ZFS_INSTALL_BASE}/include/libzfs)
set(CMAKE_REQUIRED_FLAGS "-Werror -D_LARGEFILE64_SOURCE")
# Compile only (build a static lib): the test references nvlist_lookup_string
# but we don't want to require linking libnvpair just to probe its signature.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_c_source_compiles("
#include <libnvpair.h>
int probe(nvlist_t *nv) {
    const char *s;
    return nvlist_lookup_string(nv, \"x\", &s);
}" HAVE_CONST_NVLIST_LOOKUP_STRING)
unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_INCLUDES)
if(HAVE_CONST_NVLIST_LOOKUP_STRING)
    add_definitions(-DHAVE_CONST_NVLIST_LOOKUP_STRING)
endif()

add_executable(zpool_influxdb zpool_influxdb.c)
target_link_libraries(zpool_influxdb zfs nvpair)
set_property(TARGET zpool_influxdb PROPERTY C_STANDARD 99)
install(TARGETS zpool_influxdb DESTINATION ${ZFS_INSTALL_BASE}/bin)
