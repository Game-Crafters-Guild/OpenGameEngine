#include "MovieRecorderToolbarMenu.h"

#include "MovieRecorderController.h"
#include "UI/EditorIcons.h"

#include <string>
#include <utility>

namespace GameEngine::Editor
{

std::vector<ContextMenuManipulator::Item> MovieRecorderMenuItems(
    MovieRecorderController& controller, std::function<void()> openRecordingSettings)
{
    std::vector<ContextMenuManipulator::Item> items;

    items.push_back({.Path = "Recording Settings",
                     .IconPath = EditorIcons::kSettings,
                     .OnActivate =
                         [openRecordingSettings = std::move(openRecordingSettings)]
                         {
                             if (openRecordingSettings)
                                 openRecordingSettings();
                         }});
    items.push_back({.Separator = true});

    items.push_back({.Path = "Preset", .IconPath = EditorIcons::kVideoCam});
    for (const MovieRecorderResolutionPreset& preset : GetMovieRecorderResolutionPresets())
    {
        items.push_back(
            {.Path = std::string("Preset/") + preset.Label,
             .IconPath = EditorIcons::kVideoCam,
             .OnActivate = [&controller, value = preset.Value]
             { controller.ApplyResolutionPreset(value); },
             .State = [&controller, value = preset.Value] {
                 return ContextMenuManipulator::ItemState{
                     .Checked = controller.GetResolutionPresetValue() == value};
             }});
    }

    items.push_back({.Path = "Fade", .IconPath = EditorIcons::kColorFilter});
    const struct
    {
        const char* Label;
        bool In;
        bool Out;
    } kFadeModes[] = {
        {"Fade/None", false, false},
        {"Fade/Fade In", true, false},
        {"Fade/Fade Out", false, true},
        {"Fade/Fade In + Out", true, true},
    };
    for (const auto& mode : kFadeModes)
    {
        items.push_back({.Path = mode.Label,
                         .IconPath = EditorIcons::kColorFilter,
                         .OnActivate = [&controller, in = mode.In, out = mode.Out]
                         { controller.SetFadeEnabled(in, out); },
                         .State = [&controller, in = mode.In, out = mode.Out] {
                             return ContextMenuManipulator::ItemState{
                                 .Checked = controller.GetFadeIn() == in &&
                                            controller.GetFadeOut() == out};
                         }});
    }
    items.push_back({.Path = "Fade", .Separator = true});
    items.push_back({.Path = "Fade/Black",
                     .IconPath = EditorIcons::kColorFilter,
                     .OnActivate = [&controller] { controller.SetFadeColor("black"); },
                     .State = [&controller] {
                         return ContextMenuManipulator::ItemState{.Checked =
                                                                      controller.GetFadeColor() !=
                                                                      "white"};
                     }});
    items.push_back({.Path = "Fade/White",
                     .IconPath = EditorIcons::kColorFilter,
                     .OnActivate = [&controller] { controller.SetFadeColor("white"); },
                     .State = [&controller] {
                         return ContextMenuManipulator::ItemState{.Checked =
                                                                      controller.GetFadeColor() ==
                                                                      "white"};
                     }});

    return items;
}

} // namespace GameEngine::Editor
