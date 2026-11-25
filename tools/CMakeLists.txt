# Developer tools.

# shader_tool: disassembles console shader code and reports statistics over shader dumps.
add_executable(shader_tool shader_tool.cpp)
target_link_libraries(shader_tool PRIVATE sludge_shader sludge_options)
