cmake_minimum_required(VERSION 3.22)
project(IndependentRecovery LANGUAGES CXX)
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(recovery_core STATIC
    core/model.h
    core/moc_reader.cpp
    core/moc_reader.h
    core/project_writer.cpp
    core/project_writer.h
    core/physics_xml.cpp
    core/physics_xml.h
    core/display_xml.cpp
    core/display_xml.h)
target_include_directories(recovery_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_link_libraries(recovery_core PUBLIC Qt6::Core Qt6::Gui)

add_executable(recovery-cli cli/main.cpp)
target_link_libraries(recovery-cli PRIVATE recovery_core)

add_executable(Live2DRecovery WIN32 app/main.cpp app/window.cpp app/window.h)
if(WIN32)
    target_sources(Live2DRecovery PRIVATE app/app.rc)
    target_include_directories(Live2DRecovery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
endif()
target_link_libraries(Live2DRecovery PRIVATE recovery_core Qt6::Widgets)

if(MSVC)
    foreach(component recovery_core recovery-cli Live2DRecovery)
        target_compile_options(${component} PRIVATE /utf-8 /W4)
    endforeach()
endif()

option(RECOVERY_BUILD_TESTS "Build generated-data regression tests" OFF)
if(RECOVERY_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(project-fixture tests/project_fixture.cpp)
    target_link_libraries(project-fixture PRIVATE recovery_core)
    if(MSVC)
        target_compile_options(project-fixture PRIVATE /utf-8 /W4)
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture-temp")
    add_test(NAME project-format COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_project_format.py" $<TARGET_FILE:project-fixture>)
    set_tests_properties(project-format PROPERTIES
        ENVIRONMENT "TMP=${CMAKE_CURRENT_BINARY_DIR}/fixture-temp;TEMP=${CMAKE_CURRENT_BINARY_DIR}/fixture-temp"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
