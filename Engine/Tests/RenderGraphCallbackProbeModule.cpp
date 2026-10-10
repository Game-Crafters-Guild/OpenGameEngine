#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include <memory>

namespace
{
using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;

struct CapturedLifetime
{
    int* Destroyed;
    ~CapturedLifetime() { ++*Destroyed; }
};

class CallbackNode final : public IRenderPipelineNode
{
  public:
    CallbackNode(int* destroyed, int* executed) : m_Destroyed(destroyed), m_Executed(executed) {}
    const char* GetTypeName() const override { return "CallbackProbe"; }
    bool Initialize(std::string, std::string, std::string*) override { return true; }
    void DeclareForView(ViewDeclare& d) override
    {
        auto lifetime = std::make_shared<CapturedLifetime>(m_Destroyed);
        d.Frame.AddPass("DynamicModuleCallback", Rendering::PassPhase::kDefault, [](Rendering::RenderGraph::RGPassBuilder& pass)
                        { pass.PreventCulling(); }, [lifetime, executed = m_Executed](Rendering::RenderGraph::RGContext&)
                        { ++*executed; });
    }

  private:
    int* m_Destroyed;
    int* m_Executed;
};
} // namespace

#if defined(_WIN32)
#define CALLBACK_EXPORT __declspec(dllexport)
#else
#define CALLBACK_EXPORT __attribute__((visibility("default")))
#endif
extern "C" CALLBACK_EXPORT bool RegisterCallbackProbe(RenderServices* services, int* destroyed, int* executed)
{
    return services->Spine().RegisterPipelineNodeType("CallbackProbe", [destroyed, executed]
                                                      { return std::make_unique<CallbackNode>(destroyed, executed); }, true);
}
