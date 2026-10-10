# The macOS Player bundle and the SDK template made from it (Apps/Player/CMakeLists.txt).
#  - Player.app stages the engine render pipelines (#2462) in its staged content
#    directory, Contents/Resources: the install assets root a Player run from the
#    build tree mounts as 'editor', which resolves ForwardPlus when game.config names
#    no pipeline.
#  - The template scrub removes them, wherever they were staged: an export ships only
#    the pipeline it resolves (AssetCollector::CollectRenderPipeline), so a pipeline
#    left in the template would ship in every exported game.
#  - With scripting enabled (PLAYER_STAGES_MANAGED), Player.app stages its managed set
#    in Contents/Resources/Managed, GameEngine.Scripting.Runtime among it: the GameSystem
#    runner ManagedSystemBridge resolves beside CoreBridge to tick C# GameSystems.
#  - Player.app stages every engine package beside its executable for development runs
#    (ge_stage_packages, Contents/MacOS/Packages): package sources and headers (the
#    extension set Tests/Player/MacBundleAssemblerTests.cpp names), editor-only package
#    modules and the marker naming this checkout. The template scrub removes it: an
#    export stages the packages a game uses under its content root,
#    Contents/Resources/Packages with packages.index, so the copy left in the template
#    would ship in every exported game.
foreach(var IN ITEMS PLAYER_APP PLAYER_TEMPLATE_APP)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} must name a bundle")
    endif()
endforeach()
if(NOT DEFINED PLAYER_STAGES_MANAGED)
    message(FATAL_ERROR "PLAYER_STAGES_MANAGED must be ON or OFF: whether the Player build stages the managed set")
endif()

set(pipeline "${PLAYER_APP}/Contents/Resources/Assets/RenderPipelines/ForwardPlus.rendergraph")
if(NOT EXISTS "${pipeline}")
    message(FATAL_ERROR "Player.app does not stage the dev render pipelines in its install assets root: missing ${pipeline}")
endif()

if(PLAYER_STAGES_MANAGED)
    set(runner "${PLAYER_APP}/Contents/Resources/Managed/GameEngine.Scripting.Runtime.dll")
    if(NOT EXISTS "${runner}")
        message(FATAL_ERROR "Player.app does not stage the GameSystem runner its C# GameSystems tick through: missing ${runner}")
    endif()
endif()

if(NOT IS_DIRECTORY "${PLAYER_APP}/Contents/MacOS/Packages")
    message(FATAL_ERROR "Player.app does not stage the engine packages for development runs: missing ${PLAYER_APP}/Contents/MacOS/Packages")
endif()

if(NOT IS_DIRECTORY "${PLAYER_TEMPLATE_APP}/Contents")
    message(FATAL_ERROR "No SDK Player.app template at ${PLAYER_TEMPLATE_APP}")
endif()
file(GLOB_RECURSE template_entries LIST_DIRECTORIES true RELATIVE "${PLAYER_TEMPLATE_APP}" "${PLAYER_TEMPLATE_APP}/*")
set(leftovers "")
foreach(entry IN LISTS template_entries)
    get_filename_component(name "${entry}" NAME)
    if(name STREQUAL "RenderPipelines" AND IS_DIRECTORY "${PLAYER_TEMPLATE_APP}/${entry}")
        list(APPEND leftovers "${entry}")
    endif()
endforeach()
if(leftovers)
    list(JOIN leftovers ", " leftovers)
    message(FATAL_ERROR "The SDK Player.app template still carries render pipelines (every export would ship them): ${leftovers}")
endif()

set(development_files "")
foreach(entry IN LISTS template_entries)
    get_filename_component(name "${entry}" NAME)
    if(entry STREQUAL "Contents/MacOS/Packages" OR name STREQUAL "EnginePackageAuthoringRoot.txt" OR
       name MATCHES "\\.Editor\\.(so|dylib)$" OR name MATCHES "\\.(c|cc|cpp|cxx|mm|h|hpp|inl)$")
        list(APPEND development_files "${entry}")
    endif()
endforeach()
if(development_files)
    list(LENGTH development_files count)
    list(SUBLIST development_files 0 10 shown)
    list(JOIN shown ", " shown)
    message(FATAL_ERROR "The SDK Player.app template still carries the Player's development packages "
                        "(every export would ship them): ${count} entries, among them ${shown}")
endif()
