cmake_minimum_required (VERSION 2.8 FATAL_ERROR)
project (tcmu-runner C)
set(VERSION 1.5.4)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror -Wall -Wdeclaration-after-statement -std=c99")

include(GNUInstallDirs)
include(CheckIncludeFile)

set(tcmu-runner_HANDLER_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/tcmu-runner")

option(with-glfs "build Gluster glfs handler" true)
option(with-qcow "build qcow handler" true)
option(with-rbd "build Ceph rbd handler" true)
option(with-zbc "build zbc handler" true)
option(with-fbo "build fbo handler" true)
option(with-tcmalloc "link against tcmalloc" true)
set(TCMU_CONFIG_DIR "/etc/tcmu" CACHE STRING "directory for tcmu configuration file")
set(TCMU_CONFIG_FILE "tcmu.conf" CACHE STRING "tcmu configuration file")

find_library(LIBNL_LIB nl-3)
find_library(LIBNL_GENL_LIB nl-genl-3)
set(LIBNL_LIBS
  ${LIBNL_LIB}
  ${LIBNL_GENL_LIB}
  )

find_path (LIBNL_INCLUDE_DIR
  NAMES
  netlink/netlink.h
  PATH_SUFFIXES
  libnl3
  )

find_package(PkgConfig)
pkg_check_modules(GLIB REQUIRED gio-unix-2.0)
pkg_check_modules(KMOD REQUIRED libkmod)

find_library(PTHREAD pthread)
find_library(DL dl)

if (with-tcmalloc)
  find_library(TCMALLOC_LIB tcmalloc)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
endif(with-tcmalloc)

# Stuff for building the shared library
add_library(tcmu
  SHARED
  strlcpy.c
  configfs.c
  api.c
  libtcmu.c
  libtcmu-register.c
  tcmuhandler-generated.c
  libtcmu_log.c
  libtcmu_config.c
  libtcmu_time.c
  )
set_target_properties(tcmu
  PROPERTIES
  VERSION 2.2
  SOVERSION "2"
  )
target_include_directories(tcmu
  PUBLIC ${LIBNL_INCLUDE_DIR}
  PUBLIC ${GLIB_INCLUDE_DIRS}
  PUBLIC ${PROJECT_SOURCE_DIR}/ccan
  )
target_link_libraries(tcmu
  ${LIBNL_LIB}
  ${LIBNL_GENL_LIB}
  ${GLIB_LIBRARIES}
  ${PTHREAD}
  ${TCMALLOC_LIB}
  )
target_compile_definitions(tcmu 
  PUBLIC TCMU_CONFIG_DIR="${TCMU_CONFIG_DIR}"
  PUBLIC TCMU_CONFIG_FILE="${TCMU_CONFIG_DIR}/${TCMU_CONFIG_FILE}"
  )
install(TARGETS tcmu LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

# Stuff for building the static library
add_library(tcmu_static
  strlcpy.c
  configfs.c
  api.c
  libtcmu.c
  libtcmu-register.c
  tcmuhandler-generated.c
  libtcmu_log.c
  libtcmu_config.c
  libtcmu_time.c
  )
target_include_directories(tcmu_static
  PUBLIC ${LIBNL_INCLUDE_DIR}
  PUBLIC ${GLIB_INCLUDE_DIRS}
  PUBLIC ${PROJECT_SOURCE_DIR}/ccan
  )
target_link_libraries(tcmu_static
  ${GLIB_LIBRARIES}
  ${TCMALLOC_LIB}
  )
target_compile_definitions(tcmu_static 
  PUBLIC TCMU_CONFIG_DIR="${TCMU_CONFIG_DIR}"
  PUBLIC TCMU_CONFIG_FILE="${TCMU_CONFIG_DIR}/${TCMU_CONFIG_FILE}"
  )

# Stuff for building the main binary
add_executable(tcmu-runner
  tcmur_work.c
  tcmur_cmd_handler.c
  tcmur_aio.c
  tcmur_device.c
  target.c
  alua.c
  scsi.c
  main.c
  tcmuhandler-generated.c
  )
target_link_libraries(tcmu-runner tcmu)
target_include_directories(tcmu-runner
  PUBLIC ${PROJECT_BINARY_DIR}
  PUBLIC ${GLIB_INCLUDE_DIRS}
  PUBLIC ${KMOD_INCLUDE_DIRS}
  PUBLIC ${PROJECT_SOURCE_DIR}/ccan
  )
target_link_libraries(tcmu-runner
  ${GLIB_LIBRARIES}
  ${PTHREAD}
  ${DL}
  ${KMOD_LIBRARIES}
  ${TCMALLOC_LIB}
  -Wl,--no-export-dynamic
  -Wl,--dynamic-list=${CMAKE_SOURCE_DIR}/main-syms.txt
  )
install(TARGETS tcmu-runner RUNTIME DESTINATION bin)

add_executable(tcmu-synthesizer
  scsi.c
  tcmu-synthesizer.c
  )
target_link_libraries(tcmu-synthesizer tcmu)
target_include_directories(tcmu-synthesizer
  PUBLIC ${PROJECT_BINARY_DIR}
  PUBLIC ${GLIB_INCLUDE_DIRS}
  PUBLIC ${PROJECT_SOURCE_DIR}/ccan
  )
target_link_libraries(tcmu-synthesizer
  ${GLIB_LIBRARIES}
  ${TCMALLOC_LIB}
  )

install(TARGETS RUNTIME DESTINATION bin)

add_custom_command(
  OUTPUT ${CMAKE_SOURCE_DIR}/tcmuhandler-generated.c ${CMAKE_SOURCE_DIR}/tcmuhandler-generated.h
  COMMAND gdbus-codegen ${CMAKE_SOURCE_DIR}/tcmu-handler.xml --generate-c-code ${CMAKE_SOURCE_DIR}/tcmuhandler-generated --c-generate-object-manager --interface-prefix org.kernel
  MAIN_DEPENDENCY tcmu-handler.xml
  )

add_custom_target(
  cscope
  COMMAND find -name '*.[ch]' > cscope.files
  COMMAND cscope -bq
  )
set_directory_properties(
  PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES
  "cscope.files;cscope.in.out;cscope.out;cscope.po.out"
  )

# Stuff for building the file handler
add_library(handler_file
  SHARED
  file_example.c
  )
set_target_properties(handler_file
  PROPERTIES
  PREFIX ""
  )
target_include_directories(handler_file
  PUBLIC ${PROJECT_SOURCE_DIR}/ccan
  )

if (with-fbo)
  # Stuff for building the file optical handler
  add_library(handler_file_optical
    SHARED
    scsi.c
    file_optical.c
    )
  
  set_target_properties(handler_file_optical
    PROPERTIES
    PREFIX ""
    )

  target_include_directories(handler_file_optical
    PUBLIC ${PROJECT_SOURCE_DIR}/ccan
    )
  target_link_libraries(handler_file_optical ${PTHREAD} ${TCMALLOC_LIB})
  install(TARGETS handler_file_optical DESTINATION ${CMAKE_INSTALL_LIBDIR}/tcmu-runner)
endif (with-fbo)

# The minimal library consumer
add_executable(consumer
  scsi.c
  consumer.c
  )
target_link_libraries(consumer tcmu)

if (with-zbc)
  # Stuff for building the file zbc handler
  add_library(handler_file_zbc
    SHARED
    scsi.c
    file_zbc.c
    )
  set_target_properties(handler_file_zbc
    PROPERTIES
    PREFIX ""
    )
  target_include_directories(handler_file_zbc
    PUBLIC ${PROJECT_SOURCE_DIR}/ccan
    )
  target_link_libraries(handler_file_zbc ${TCMALLOC_LIB})
  install(TARGETS handler_file_zbc DESTINATION ${CMAKE_INSTALL_LIBDIR}/tcmu-runner)
endif (with-zbc)

if (with-rbd)
  find_library(LIBRBD rbd)

  # Stuff for building the rbd handler
  add_library(handler_rbd
    SHARED
    rbd.c
    )
  set_target_properties(handler_rbd
    PROPERTIES
    PREFIX ""
    )
  target_include_directories(handler_rbd
    PUBLIC ${PROJECT_SOURCE_DIR}/ccan
    )
  target_link_libraries(handler_rbd
    ${LIBRBD}
    ${TCMALLOC_LIB}
    )
  install(TARGETS handler_rbd DESTINATION ${CMAKE_INSTALL_LIBDIR}/tcmu-runner)
endif (with-rbd)

if (with-glfs)
  find_library(GFAPI gfapi)

  set(GFAPI_VERSION760 0)

  pkg_check_modules(GFAPI760 glusterfs-api>=7.6 QUIET)
  if (GFAPI760_FOUND)
    set(GFAPI_VERSION760 1)
  endif (GFAPI760_FOUND)

  set(GFAPI_VERSION766 0)

  pkg_check_modules(GFAPI766 glusterfs-api>=7.6.6 QUIET)
  if (GFAPI766_FOUND)
    set(GFAPI_VERSION766 1)
  endif (GFAPI766_FOUND)

  # Stuff for building the glfs handler
  add_library(handler_glfs
    SHARED
    glfs.c
    )
  set_target_properties(handler_glfs
    PROPERTIES
    PREFIX ""
    )
  target_include_directories(handler_glfs
    PUBLIC ${PROJECT_SOURCE_DIR}/ccan
    )
  target_link_libraries(handler_glfs
    ${GFAPI}
    ${TCMALLOC_LIB}
    )
  install(TARGETS handler_glfs DESTINATION ${CMAKE_INSTALL_LIBDIR}/tcmu-runner)
endif (with-glfs)

if (with-qcow)
  find_package(ZLIB REQUIRED)

  # Stuff for building the qcow handler
  add_library(handler_qcow
    SHARED
    qcow.c
    )
  set_target_properties(handler_qcow
    PROPERTIES
    PREFIX ""
    )
  target_include_directories(handler_qcow
    PUBLIC ${PROJECT_SOURCE_DIR}/ccan
    )

  CHECK_INCLUDE_FILE("linux/falloc.h" HAVE_LINUX_FALLOC)
  if (HAVE_LINUX_FALLOC)
    set_target_properties(handler_qcow
      PROPERTIES
      COMPILE_FLAGS "-DHAVE_LINUX_FALLOC"
      )
  endif (HAVE_LINUX_FALLOC)
  target_link_libraries(handler_qcow
    ${ZLIB_LIBRARIES}
    ${TCMALLOC_LIB}
    )
  install(TARGETS handler_qcow DESTINATION ${CMAKE_INSTALL_LIBDIR}/tcmu-runner)
endif (with-qcow)

# stamp out a header file to pass some of the CMake settings
# to the source code
configure_file (
  "${PROJECT_SOURCE_DIR}/version.h.in"
  "${PROJECT_SOURCE_DIR}/version.h"
  )

configure_file (
  "${PROJECT_SOURCE_DIR}/tcmu.conf_install.cmake.in"
  "${PROJECT_SOURCE_DIR}/tcmu.conf_install.cmake"
  )
install(SCRIPT tcmu.conf_install.cmake)

configure_file (
  "${PROJECT_SOURCE_DIR}/logrotate.conf_install.cmake.in"
  "${PROJECT_SOURCE_DIR}/logrotate.conf_install.cmake"
  )
install(SCRIPT logrotate.conf_install.cmake)

install(FILES org.kernel.TCMUService1.service
  DESTINATION /usr/share/dbus-1/system-services)
install(FILES tcmu-runner.conf DESTINATION /etc/dbus-1/system.d)
if (SUPPORT_SYSTEMD)
  install(FILES tcmu-runner.service DESTINATION /usr/lib/systemd/system/)
endif (SUPPORT_SYSTEMD)
install(FILES tcmu-runner.8 DESTINATION ${CMAKE_INSTALL_PREFIX}/share/man/man8)
