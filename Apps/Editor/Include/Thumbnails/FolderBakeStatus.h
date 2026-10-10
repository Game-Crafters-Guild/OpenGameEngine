#pragma once

#include "Thumbnails/FolderBakeReport.h"
#include "UI/UIElement.h"

#include <memory>

namespace GameEngine
{
class Button;
class IThumbnailProvider;
class Label;

// The progress and the outcome of a "Generate Thumbnails" folder bake, shown
// under the asset browser's views: "Generating thumbnails: 120 of 333" while
// it runs, then how many were generated, already current and failed, until
// the user dismisses it. Hidden while no bake has run.
//
// The plate, the text and the dismiss button are declared in
// UI/thumbnails/FolderBakeStatus/FolderBakeStatus.uxml and styled by
// FolderBakeStatus.css beside it.
class FolderBakeStatus : public UIElement
{
public:
    // Listens to `provider` for the rest of its life; the listener does
    // nothing once this element is destroyed.
    explicit FolderBakeStatus(IThumbnailProvider& provider);

private:
    void Show(const FolderBakeReport& report);
    void Dismiss();

    Label* m_Text = nullptr;
    Button* m_Dismiss = nullptr;
    std::shared_ptr<FolderBakeStatus*> m_Self;
};

} // namespace GameEngine
