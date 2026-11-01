# libgraphrel — Graph reliability extension for TeDDy

add_library(
    graphrel
        src/common/graph.cpp
        src/common/graph_io.cpp
        src/common/variable_ordering.cpp
        src/common/validation.cpp
        src/lbl/lbl_calculator.cpp
        src/frontier/frontier.cpp
        src/diagram/diagram.cpp
)

add_library(
    teddy::graphrel ALIAS graphrel
)

target_include_directories(
    graphrel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

find_package(Threads REQUIRED)

target_link_libraries(
    graphrel
    PUBLIC teddy
    PRIVATE Threads::Threads
)

target_compile_features(
    graphrel
    PUBLIC cxx_std_20
)

set_target_properties(
    graphrel
    PROPERTIES
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS        NO
)

target_compile_options(
    graphrel
    PRIVATE ${LIBTEDDY_COMPILE_OPTIONS}
)

target_link_options(
    graphrel
    PRIVATE ${LIBTEDDY_LINK_OPTIONS}
)
