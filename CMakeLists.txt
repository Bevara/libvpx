cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBVPX_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_vpx.c
)

SET(LIBVPX_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libvpx.a
)

SET(LIBVPX_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libvpx
        "${LIBVPX_SRC}"
        "${LIBVPX_LIB}"
        []
        ""
        "${LIBVPX_INC}"
        ""
        "1")
