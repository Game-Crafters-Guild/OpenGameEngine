#include "AssistantTools.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
using Class = AssistantToolClass;

constexpr std::string_view kToolPrefix = "mcp__editor_assistant__";

// Every tool of mcp/src/tools/, in the server's order. A tool joins UndoableEdit only
// with a test showing one undo step per call. McpAssistantAttachment fails when a tool
// that writes files or starts processes is not marked actsOnHost in the server.
constexpr AssistantTool kTools[] = {
    {"get_build_status", "get_build_status", Class::Read, "Read the build status", {"", "", ""}},
    {"get_editor_state", "get_editor_state", Class::Read, "Read the editor state", {"", "", ""}},
    {"get_log", "get_log", Class::Read, "Read the log", {"filter", "minLevel", ""}},
    {"get_scene_hierarchy", "get_scene_hierarchy", Class::Read, "Read the scene hierarchy", {"", "", ""}},
    {"trigger_build", "trigger_build", Class::Gated, "Build the game", {"platform", "", ""}},
    {"get_build_settings", "get_build_settings", Class::Read, "Read the build settings", {"", "", ""}},
    {"execute_command", "execute_command", Class::Gated, "Run a command", {"name", "commandId", ""}},
    {"set_play_mode", "set_play_mode", Class::Gated, "Change play mode", {"action", "", ""}},
    {"focus_view", "focus_view", Class::Denied, "Focus a view", {"view", "", ""}, "it takes the operating system's foreground from the user"},
    {"create_entity", "create_entity", Class::UndoableEdit, "Create entity", {"name", "position", ""}},
    {"set_component", "set_component", Class::UndoableEdit, "Set component", {"entityId", "component", "values"}},
    {"delete_entity", "delete_entity", Class::UndoableEdit, "Delete entity", {"entityId", "", ""}},
    {"select_entity", "select_entity", Class::View, "Select entity", {"entityId", "", ""}},
    {"get_entity_components", "get_entity_components", Class::Read, "Read components", {"entityId", "", ""}},
    {"query_ecs", "query_ecs", Class::Read, "Query entities", {"hasComponent", "", ""}},
    {"save_scene", "save_scene", Class::Gated, "Save the scene", {"path", "", ""}},
    {"undo", "undo", Class::Denied, "Undo", {"", "", ""}, "the assistant cannot see what is on top of the shared undo stack; correct a change with a new edit, and the user undoes"},
    {"redo", "redo", Class::Denied, "Redo", {"", "", ""}, "the assistant cannot see what is on top of the shared undo stack; correct a change with a new edit, and the user undoes"},
    {"get_undo_stack", "get_undo_stack", Class::Read, "Read the undo history", {"", "", ""}},
    {"sample_ground_height", "sample_ground_height", Class::Read, "Sample ground height", {"", "", ""}},
    {"get_placement_ground_gap", "get_placement_ground_gap", Class::Read, "Measure the ground gap", {"entityId", "", ""}},
    {"markup_list", "markup_list", Class::Read, "List mark-ups", {"status", "", ""}},
    {"markup_get", "markup_get", Class::Read, "Read a mark-up", {"entityId", "", ""}},
    {"markup_create", "markup_create", Class::UndoableEdit, "Create mark-up", {"title", "kind", ""}},
    {"markup_update", "markup_update", Class::UndoableEdit, "Update mark-up", {"entityId", "title", ""}},
    {"markup_comment", "markup_comment", Class::UndoableEdit, "Comment on a mark-up", {"entityId", "", ""}},
    {"markup_contains", "markup_contains", Class::Read, "Test points against a mark-up", {"entityId", "", ""}},
    {"markup_frame", "markup_frame", Class::View, "Frame a mark-up", {"entityId", "", ""}},
    {"markup_set_visible", "markup_set_visible", Class::View, "Show or hide mark-ups", {"entityId", "visible", ""}},
    {"spawn_skinned_model", "spawn_humanoid_test_pair", Class::Gated, "Spawn a skinned model", {"modelPath", "name", ""}},
    {"get_skeleton_store_state", "get_skeleton_store_state", Class::Read, "Read the skeleton store", {"", "", ""}},
    {"get_ui_tree", "get_ui_tree", Class::Read, "Read the editor UI", {"rootId", "", ""}},
    {"get_panel_tree", "get_panel_tree", Class::Read, "Read a panel's UI", {"panelId", "", ""}},
    {"click_element", "click_element", Class::Input, "Click", {"elementId", "button", ""}},
    {"perform_drop", "perform_drop", Class::Input, "Drop assets", {"elementId", "paths", ""}},
    {"move_pointer", "move_pointer", Class::View, "Move the pointer", {"elementId", "", ""}},
    {"send_key", "send_key", Class::Input, "Press a key", {"key", "mods", ""}},
    {"input_text", "input_text", Class::Input, "Type text", {"elementId", "", ""}},
    {"open_asset", "open_asset", Class::View, "Open an asset", {"path", "", ""}},
    {"select_asset", "select_asset", Class::View, "Select an asset", {"path", "", ""}},
    {"generate_folder_thumbnails", "generate_folder_thumbnails", Class::Gated, "Generate thumbnails", {"path", "", ""}},
    {"get_asset_preview", "get_asset_preview", Class::Read, "Read the asset preview", {"", "", ""}},
    {"get_camera", "get_camera", Class::Read, "Read the camera", {"view", "", ""}},
    {"set_camera", "set_camera", Class::View, "Set the camera", {"", "", ""}},
    {"move_camera", "move_camera", Class::View, "Move the camera", {"", "", ""}},
    {"look_at", "look_at", Class::View, "Look at", {"entityId", "", ""}},
    {"set_gizmos_visibility", "set_gizmos_visibility", Class::View, "Show or hide gizmos", {"", "", ""}},
    {"take_screenshot", "take_screenshot", Class::Read, "Take a screenshot", {"target", "panelId", ""}},
    {"capture_resource", "capture_resource", Class::Read, "Read a render-graph resource", {"name", "", ""}},
    {"trigger_capture", "trigger_capture", Class::Gated, "Take a RenderDoc capture", {"", "", ""}},
    {"get_capture_status", "get_capture_status", Class::Read, "Read the capture status", {"", "", ""}},
    {"nsight_capture", "nsight_capture", Class::Gated, "Take an Nsight capture", {"", "", ""}},
    {"get_render_graph_overview", "get_render_graph_overview", Class::Read, "Read the render graph", {"", "", ""}},
    {"get_render_graph_passes", "get_render_graph_passes", Class::Read, "Read render-graph passes", {"", "", ""}},
    {"get_render_graph_resources", "get_render_graph_resources", Class::Read, "Read render-graph resources", {"type", "", ""}},
    {"get_render_graph_pass_detail", "get_render_graph_pass_detail", Class::Read, "Read a render-graph pass", {"passName", "", ""}},
    {"get_render_graph_dependencies", "get_render_graph_dependencies", Class::Read, "Read render-graph dependencies", {"", "", ""}},
    {"get_render_graph_validation", "get_render_graph_validation", Class::Read, "Read render-graph validation", {"", "", ""}},
    {"get_pass_descriptors", "get_pass_descriptors", Class::Read, "Read pass descriptors", {"passName", "", ""}},
    {"get_msaa", "get_msaa", Class::Read, "Read MSAA", {"", "", ""}},
    {"set_msaa", "set_msaa", Class::Gated, "Set MSAA", {"samples", "", ""}},
    {"set_aa_mode", "set_aa_mode", Class::Gated, "Set anti-aliasing", {"mode", "", ""}},
    {"set_taa_render_scale", "set_taa_render_scale", Class::Gated, "Set the TAA render scale", {"scale", "", ""}},
    {"set_dynamic_resolution", "set_dynamic_resolution", Class::Gated, "Set dynamic resolution", {"mode", "", ""}},
    {"set_lod", "set_lod", Class::Gated, "Set LOD", {"mode", "", ""}},
    {"set_shadow_quality", "set_shadow_quality", Class::Gated, "Set shadow quality", {"quality", "", ""}},
    {"set_shadow_pcss", "set_shadow_pcss", Class::Gated, "Set soft shadows", {"enabled", "", ""}},
    {"set_moments_resolution", "set_moments_resolution", Class::Gated, "Set the moments resolution", {"resolution", "", ""}},
    {"set_msm_blur_mode", "set_msm_blur_mode", Class::Gated, "Set the shadow blur", {"mode", "", ""}},
    {"get_hdr_output", "get_hdr_output", Class::Read, "Read HDR output", {"", "", ""}},
    {"set_hdr_output", "set_hdr_output", Class::Gated, "Set HDR output", {"enabled", "mode", ""}},
    {"get_render_stats", "get_render_stats", Class::Read, "Read render stats", {"", "", ""}},
    {"get_validation_stats", "get_validation_stats", Class::Read, "Read validation stats", {"", "", ""}},
    {"get_gpu_tooling", "get_gpu_tooling", Class::Read, "Read GPU tooling", {"", "", ""}},
    {"rgp_capture", "", Class::Gated, "Take a Radeon GPU Profiler capture", {"", "", ""}},
    {"get_terrain_stats", "get_terrain_stats", Class::Read, "Read terrain stats", {"", "", ""}},
    {"get_ui_profile_history", "get_ui_profile_history", Class::Read, "Read the UI profile", {"", "", ""}},
    {"get_cpu_profiler", "get_cpu_profiler", Class::Read, "Read the CPU profiler", {"", "", ""}},
    {"get_gpu_profiler", "get_gpu_profiler", Class::Read, "Read the GPU profiler", {"", "", ""}},
    {"toggle_shadow_debug", "toggle_shadow_debug", Class::View, "Toggle the shadow view", {"mode", "", ""}},
    {"set_parallax_steps_view", "set_parallax_steps_view", Class::View, "Toggle the parallax steps view", {"enable", "", ""}},
    {"toggle_shadow_thumbnails", "toggle_shadow_thumbnails", Class::View, "Toggle shadow thumbnails", {"enable", "", ""}},
    {"get_monitors", "get_monitors", Class::Read, "Read the monitors", {"", "", ""}},
    {"move_window", "move_window", Class::Denied, "Move a window", {"", "", ""}, "it takes the operating system's foreground from the user"},
    {"list_game_graph_actions", "", Class::Read, "List game graph actions", {"category", "", ""}},
    {"get_game_graph", "", Class::Read, "Read a game graph", {"path", "", ""}},
    {"add_game_graph_node", "", Class::Gated, "Add a game graph node", {"path", "typeId", ""}},
    {"update_game_graph_node", "", Class::Gated, "Update a game graph node", {"path", "nodeId", ""}},
    {"delete_game_graph_node", "", Class::Gated, "Delete a game graph node", {"path", "nodeId", ""}},
    {"link_game_graph_nodes", "", Class::Gated, "Link game graph nodes", {"path", "", ""}},
    {"write_game_graph_cpp", "", Class::Gated, "Write a game graph's C++", {"path", "outputPath", ""}},
    {"write_game_graph_action_code", "", Class::Gated, "Write a game graph action", {"typeId", "", ""}},
    {"create_game_system", "", Class::Gated, "Create a game system", {"name", "", ""}},
    {"create_entity_system", "", Class::Gated, "Create an entity system", {"name", "", ""}},
    {"create_component", "", Class::Gated, "Create a component", {"name", "", ""}},
    {"list_ecs_systems", "", Class::Read, "List ECS systems", {"", "", ""}},
    {"build_engine", "", Class::Denied, "Build the engine", {"", "", ""}, "it builds, starts or stops editors, which is not an action inside this editor"},
    {"run_tests", "", Class::Denied, "Run tests", {"", "", ""}, "it builds, starts or stops editors, which is not an action inside this editor"},
    {"launch_editor", "", Class::Denied, "Launch an editor", {"", "", ""}, "it builds, starts or stops editors, which is not an action inside this editor"},
    {"wait_for_editor", "", Class::Denied, "Wait for an editor", {"", "", ""}, "it builds, starts or stops editors, which is not an action inside this editor"},
    {"editor_full_cycle", "", Class::Denied, "Rebuild and relaunch the editor", {"", "", ""}, "it builds, starts or stops editors, which is not an action inside this editor"},
    {"shutdown_editor", "shutdown", Class::Denied, "Shut down an editor", {"", "", ""}, "it builds, starts or stops editors, which is not an action inside this editor"},
    {"list_debug_methods", "", Class::Denied, "List debug methods", {"", "", ""}, "it lists the editor's raw methods, which the assistant cannot call"},
};

// An argument's value as a summary shows it: strings bare, an object as its "key value"
// pairs ("Elevation 12, Intensity 3"), everything else as JSON.
std::string ArgumentText(const nlohmann::json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (!value.is_object())
        return value.dump();
    std::string pairs;
    for (const auto& [key, field] : value.items())
        pairs += (pairs.empty() ? "" : ", ") + key + " " + (field.is_string() ? field.get<std::string>() : field.dump());
    return pairs;
}
} // namespace

std::span<const AssistantTool> AssistantTools::All()
{
    return kTools;
}

const AssistantTool* AssistantTools::Find(std::string_view name)
{
    if (name.starts_with(kToolPrefix))
        name.remove_prefix(kToolPrefix.size());
    const auto it = std::find_if(std::begin(kTools), std::end(kTools),
                                 [name](const AssistantTool& tool) { return tool.Name == name; });
    return it == std::end(kTools) ? nullptr : &*it;
}

const AssistantTool* AssistantTools::FindByMethod(std::string_view method)
{
    if (method.empty())
        return nullptr;
    const auto it = std::find_if(std::begin(kTools), std::end(kTools),
                                 [method](const AssistantTool& tool) { return tool.Method == method; });
    return it == std::end(kTools) ? nullptr : &*it;
}

std::vector<std::string> AssistantTools::AttachedNames()
{
    std::vector<std::string> names;
    for (const AssistantTool& tool : kTools)
        if (tool.Class != AssistantToolClass::Denied)
            names.emplace_back(tool.Name);
    return names;
}

std::string AssistantTools::Subject(std::string_view name, const nlohmann::json& arguments)
{
    const AssistantTool* tool = Find(name);
    if (!tool || !arguments.is_object())
        return {};
    std::string subject;
    for (std::string_view argument : tool->SubjectArguments)
    {
        if (argument.empty())
            continue;
        const auto value = arguments.find(argument);
        if (value == arguments.end() || value->is_null() || (value->is_object() && value->empty()))
            continue;
        subject += (subject.empty() ? "" : " · ") + ArgumentText(*value);
    }
    return subject;
}
} // namespace GameEngine
