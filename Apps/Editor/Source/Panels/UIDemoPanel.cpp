#include "Panels/UIDemoPanel.h"

#include <filesystem>

#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Logger/Logger.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIManager.h"
#include "UI/UIElement.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"

namespace GameEngine
{
namespace
{
// Attach order IS the cascade order at equal specificity: the control sheets go on first
// so the panel sheet can override them.
constexpr std::size_t kStylesheetCount = 3;

std::filesystem::path StylesheetPath(std::size_t index)
{
    switch (index)
    {
    case 0:
        return std::filesystem::path("UI") / "Controls" / "Foldout.css";
    case 1:
        return std::filesystem::path("UI") / "Controls" / "Accordion.css";
    default:
        return std::filesystem::path("UI") / "UIDemoPanel.css";
    }
}
} // namespace

UIDemoPanel::UIDemoPanel()
    : DockPanel("UI Demo")
{
    // Allow the Editor theme (and demo-local stylesheet) to target this panel.
    AddClass("ui-demo-panel");

    // Create a placeholder native dropdown immediately so EditorApplication can
    // install the OS menu invoker during initialization. When the data-driven
    // layout is applied, the hot-reload reconciler will reuse this instance by
    // id ("UIDemoDropdownNative") and keep the installed invoker intact.
    auto nativeDropdown = std::make_unique<Dropdown>();
    m_NativeDropdown = nativeDropdown.get();
    m_NativeDropdown->SetId("UIDemoDropdownNative");
    m_NativeDropdown->AddClass("dropdown-native");
    m_NativeDropdown->SetMode(Dropdown::Mode::Native);
    AddChild(std::move(nativeDropdown));
}

UIDemoPanel::~UIDemoPanel()
{
    // The pending asset callbacks capture this panel; withdraw them before it goes
    // away (destroying an AssetLoadHandle does not).
    if (m_LoadHandle)
        m_LoadHandle->Cancel();
}

void UIDemoPanel::OnPostLayout()
{
    // Defer binding to a safe point (dispatcher drain) to avoid mutating the UI
    // tree during layout/geometry traversal.
    if (m_BindApplied || m_BindPending || m_BindFailed)
        return;

    if (!GetOwnerManager())
        return;

    m_BindPending = true;
    // A dropped action never runs, so it would never release the latch: release it here
    // instead and let the next laid-out frame try again.
    if (!this->PostAction([this]()
                          { this->BindFromAssetsDeferred(); }))
        m_BindPending = false;
}

void UIDemoPanel::BindFromAssetsDeferred()
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
    {
        // Detached between scheduling and draining: transient, so release the latch and
        // let the next laid-out frame schedule a fresh attempt.
        m_BindPending = false;
        return;
    }

    auto& am = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path layoutPath = std::filesystem::path("UI") / "UIDemoPanel.xml";
    const GUID layoutGuid = am.ResolveAssetGuid(layoutPath, GameEngine::kAssetSourceAliasEditor);
    if (layoutGuid.IsNull())
        return MarkBindFailed("the layout guid did not resolve from the editor asset source");

    // Loads are asynchronous: a blocking future.get() here would stall the UI thread, and
    // on the failure path it would do so once per layout pass.
    m_LoadHandle = std::make_unique<AssetLoadHandle>(am.LoadAsset(
        layoutGuid,
        [this, post = GetPostHandle(), layoutGuid](Result<SharedPtr<Asset>, AssetError> r)
        {
            // The load completes on a worker; every latch mutation below runs on the UI
            // thread through PostAction.
            const bool loaded = r.IsOk() && r.Value() && r.Value()->GetType() == AssetType::UILayout;
            post.Post(
                [this, layoutGuid, loaded]()
                {
                    if (!loaded)
                        return MarkBindFailed("the layout asset did not load");
                    UIManager* ui2 = GetOwnerManager();
                    if (!ui2)
                    {
                        m_BindPending = false;
                        return;
                    }
                    auto& am2 = EngineCore::GetInstance().GetAssetManager();
                    auto a = am2.GetAsset(layoutGuid);
                    if (!a || a->GetType() != AssetType::UILayout)
                        return MarkBindFailed("the loaded layout asset is not a UILayout");
                    if (!ui2->BindLayoutToSubtreeChildrenFromAsset(
                            this, *static_cast<UILayoutAsset*>(a.get())))
                        return MarkBindFailed("binding the layout into the panel subtree was rejected");

                    m_BindApplied = true;
                    m_BindPending = false;
                    // The UXML children exist only now, so this is where the control
                    // pointers resolve.
                    RefreshControlPointers();
                    // Stylesheets are optional decoration on a panel that already bound;
                    // the chain runs off m_BindApplied, not off the layout latch.
                    AttachStylesheet(0);
                });
        },
        AssetLoadPriority::High));
}

void UIDemoPanel::MarkBindFailed(std::string_view reason)
{
    m_BindPending = false;
    m_BindFailed = true;
    Logger::Log::Error(
        "UIDemoPanel: layout bind failed ({}). The panel stays unbound; "
        "check that UI/UIDemoPanel.xml is staged under the editor asset mount.",
        reason);
}

void UIDemoPanel::AttachStylesheet(std::size_t index)
{
    if (index >= kStylesheetCount)
        return;

    auto& am = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path path = StylesheetPath(index);
    const GUID guid = am.ResolveAssetGuid(path, GameEngine::kAssetSourceAliasEditor);
    if (guid.IsNull())
    {
        Logger::Log::Warning("UIDemoPanel: stylesheet '{}' did not resolve from the editor "
                             "asset source; the panel renders without it",
                             path.string());
        AttachStylesheet(index + 1);
        return;
    }

    m_LoadHandle = std::make_unique<AssetLoadHandle>(am.LoadAsset(
        guid,
        [this, post = GetPostHandle(), guid, index](Result<SharedPtr<Asset>, AssetError> r)
        {
            const bool loaded = r.IsOk() && r.Value() && r.Value()->GetType() == AssetType::UIStyle;
            post.Post(
                [this, guid, index, loaded]()
                {
                    UIManager* ui = GetOwnerManager();
                    if (!ui)
                        return; // detached mid-chain; a later bind reattaches from the top
                    if (loaded)
                    {
                        auto a = EngineCore::GetInstance().GetAssetManager().GetAsset(guid);
                        if (a && a->GetType() == AssetType::UIStyle)
                        {
                            (void)ui->AttachStyleToSubtreeFromAsset(
                                this, *static_cast<UIStyleAsset*>(a.get()));
                        }
                    }
                    // Advance whether or not this sheet arrived: a missing control sheet
                    // must not stop the panel sheet, and skipping in place keeps the
                    // cascade order of the ones that do arrive.
                    AttachStylesheet(index + 1);
                });
        },
        AssetLoadPriority::High));
}

void UIDemoPanel::RefreshControlPointers()
{
    // Resolve by id so EditorApplication can still find the native dropdown and tests can
    // access the controls.
    m_Checkbox       = dynamic_cast<Checkbox*>(FindById("UIDemoCheckbox"));
    m_Toggle         = dynamic_cast<Toggle*>(FindById("UIDemoToggle"));
    m_UiDropdown     = dynamic_cast<Dropdown*>(FindById("UIDemoDropdownUi"));
    m_NativeDropdown = dynamic_cast<Dropdown*>(FindById("UIDemoDropdownNative"));
}

} // namespace GameEngine
