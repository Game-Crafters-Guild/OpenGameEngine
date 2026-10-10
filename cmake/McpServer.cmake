# The checked-in MCP client configuration points at mcp/dist/index.js.
# Register an optional, explicit build target; configure never runs npm.
function(ge_add_mcp_server_target)
    set(mcp_directory "${CMAKE_SOURCE_DIR}/mcp")
    if(NOT EXISTS "${mcp_directory}/package.json")
        return()
    endif()

    # Only PATH availability enables this target. Do not retain a cached
    # executable after npm has been removed from the configure environment.
    find_program(mcp_npm NAMES npm.cmd npm PATHS ENV PATH NO_DEFAULT_PATH NO_CACHE)
    if(NOT mcp_npm)
        message(STATUS "npm not found; optional McpServer target is unavailable (install Node.js >= 20 to enable it)")
        return()
    endif()

    set(dependency_stamp "${mcp_directory}/node_modules/.package-lock.json")
    add_custom_command(
        OUTPUT "${dependency_stamp}"
        COMMAND "${mcp_npm}" ci
        DEPENDS "${mcp_directory}/package.json" "${mcp_directory}/package-lock.json"
        WORKING_DIRECTORY "${mcp_directory}"
        COMMENT "Installing MCP server dependencies"
        VERBATIM)

    file(GLOB_RECURSE mcp_sources CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${mcp_directory}/src/*")
    set(server_entry "${mcp_directory}/dist/index.js")
    add_custom_command(
        OUTPUT "${server_entry}"
        COMMAND "${mcp_npm}" run build
        DEPENDS "${dependency_stamp}" "${mcp_directory}/tsconfig.json" ${mcp_sources}
        WORKING_DIRECTORY "${mcp_directory}"
        COMMENT "Building MCP server"
        VERBATIM)
    add_custom_target(McpServer DEPENDS "${server_entry}")
endfunction()

# The editor's AI Assistant runs the server from beside the editor executable
# (<editor dir>/mcp/): the built dist/ and its production dependencies only. Staged
# whenever the McpServer target builds; without that target the assistant's acting
# modes say to build it. Call it from the directory that created McpServer.
function(ge_stage_mcp_server_beside target)
    if(NOT TARGET McpServer)
        return()
    endif()
    find_program(mcp_npm NAMES npm.cmd npm PATHS ENV PATH NO_DEFAULT_PATH NO_CACHE)
    set(mcp_directory "${CMAKE_SOURCE_DIR}/mcp")
    set(staged_directory "$<TARGET_FILE_DIR:${target}>/mcp")
    add_custom_command(TARGET McpServer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${mcp_directory}/dist" "${staged_directory}/dist"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${mcp_directory}/package.json" "${mcp_directory}/package-lock.json" "${staged_directory}"
        COMMAND "${mcp_npm}" ci --prefix "${staged_directory}" --omit=dev --ignore-scripts --no-audit --no-fund
        COMMENT "Staging the MCP server beside ${target}"
        VERBATIM)
endfunction()
