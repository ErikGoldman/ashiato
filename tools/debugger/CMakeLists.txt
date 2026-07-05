find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

function(ashiato_debugger_use_cxx17 target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /std:c++17)
    else()
        target_compile_options(${target_name} PRIVATE -std=c++17)
    endif()
endfunction()

add_library(ashiato_debugger_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(ashiato_debugger_imgui
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(ashiato_debugger_imgui
    PUBLIC
        glfw
        OpenGL::GL
)

ashiato_debugger_use_cxx17(ashiato_debugger_imgui)

add_library(ashiato_debugger_core STATIC
    debugger_core.cpp
    debugger_core.hpp
)

target_include_directories(ashiato_debugger_core
    PRIVATE
        ${nlohmann_json_SOURCE_DIR}/include
)

target_link_libraries(ashiato_debugger_core
    PUBLIC
        Threads::Threads
)

if(WIN32)
    target_link_libraries(ashiato_debugger_core
        PUBLIC
            ws2_32
    )
endif()

ashiato_debugger_use_cxx17(ashiato_debugger_core)

add_executable(ashiato_debugger
    debugger_app.cpp
    debugger_app.hpp
    debugger_app_internal.hpp
    debugger_panels.cpp
    debugger_refresh.cpp
    debugger_widgets.cpp
    main.cpp
)

target_link_libraries(ashiato_debugger
    PRIVATE
        ashiato_debugger_core
        ashiato_debugger_imgui
)

ashiato_debugger_use_cxx17(ashiato_debugger)

if(TARGET Catch2::Catch2WithMain)
    add_executable(ashiato_debugger_core_tests
        debugger_core.test.cpp
    )

    target_link_libraries(ashiato_debugger_core_tests
        PRIVATE
            ashiato_debugger_core
            Catch2::Catch2WithMain
    )

    ashiato_debugger_use_cxx17(ashiato_debugger_core_tests)

    add_test(NAME ashiato_debugger_core_tests COMMAND ashiato_debugger_core_tests)
    if(TARGET tests)
        add_dependencies(tests ashiato_debugger_core_tests)
    endif()
endif()
