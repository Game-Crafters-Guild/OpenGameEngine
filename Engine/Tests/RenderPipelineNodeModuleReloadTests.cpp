// C12 module-reload protocol — render pipeline node factory attribution.
//
// A module's RegisterRenderPipelineNodes replay runs inside the loader's
// module-stamp bracket, so every factory it registers carries the module id +
// load generation. The reconcile must swap FACTORY CODE to the newest mapped
// image: a same-typed re-registration from a newer generation of the owning
// module replaces the entry in place, types the replay stopped declaring
// retire, the load-abort purge drops an aborted generation's entries, and the
// unload quiesce ledger counts anything still owned by a superseded image.

#include <gtest/gtest.h>

#include "ECS/ModuleRegistration.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <memory>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer::Pipeline;

namespace
{
// Node double whose type name records which registration constructed it, so a
// test can prove WHICH factory (old or new generation) the registry dispatched.
class TaggedNode final : public IRenderPipelineNode
{
public:
    explicit TaggedNode(std::string tag) : m_Tag(std::move(tag)) {}
    const char* GetTypeName() const override { return m_Tag.c_str(); }
    bool Initialize(std::string, std::string, std::string*) override { return true; }

private:
    std::string m_Tag;
};

RenderPipelineNodeFactory MakeFactory(std::string tag)
{
    return [tag] { return std::make_unique<TaggedNode>(tag); };
}

std::string InstantiateTag(const RenderPipelineNodeRegistry& reg, const std::string& type)
{
    const RenderPipelineNodeTypeInfo* info = reg.Find(type);
    if (!info || !info->factory)
        return {};
    const std::unique_ptr<IRenderPipelineNode> node = info->factory();
    return node ? node->GetTypeName() : std::string{};
}

struct StampScope
{
    StampScope(std::string_view moduleId, std::uint64_t generation)
    {
        ECS::SetActiveRegistrationModule(moduleId, generation);
    }
    ~StampScope() { ECS::ClearActiveRegistrationModule(); }
};
} // namespace

// The load-bearing C12 behavior: after a reload replay re-registers a type,
// instantiation dispatches into the NEW factory; the entry is re-owned by the
// new generation so the unload ledger reads clean.
TEST(RenderPipelineNodeModuleReload, ReloadedModuleFactoryServesNewGeneration)
{
    RenderPipelineNodeRegistry reg;
    {
        StampScope stamp("TestPack", 1);
        ASSERT_TRUE(reg.Register("PkgNode", MakeFactory("PkgNode/v1"), /*perView=*/false));
    }
    EXPECT_EQ(InstantiateTag(reg, "PkgNode"), "PkgNode/v1");
    EXPECT_EQ(reg.CountSupersededModuleNodes("TestPack", 1), 0u);

    // Generation 2 loads but has not replayed yet: the entry still points at
    // generation-1 code — one ledger blocker.
    EXPECT_EQ(reg.CountSupersededModuleNodes("TestPack", 2), 1u);

    {
        StampScope stamp("TestPack", 2);
        EXPECT_TRUE(reg.Register("PkgNode", MakeFactory("PkgNode/v2"), /*perView=*/true));
    }
    EXPECT_EQ(InstantiateTag(reg, "PkgNode"), "PkgNode/v2") << "replay must swap the factory in place";
    ASSERT_NE(reg.Find("PkgNode"), nullptr);
    EXPECT_TRUE(reg.Find("PkgNode")->perView) << "replacement metadata must win";
    EXPECT_EQ(reg.CountSupersededModuleNodes("TestPack", 2), 0u) << "re-owned entry must not block unload";
}

// A type the newest replay stopped declaring retires at reconcile: Find no
// longer resolves it, re-declared siblings survive at the new generation.
TEST(RenderPipelineNodeModuleReload, UndeclaredFactoryRetiresOnReconcile)
{
    RenderPipelineNodeRegistry reg;
    {
        StampScope stamp("TestPack", 1);
        ASSERT_TRUE(reg.Register("KeepNode", MakeFactory("KeepNode/v1"), false));
        ASSERT_TRUE(reg.Register("DropNode", MakeFactory("DropNode/v1"), false));
    }

    // v2 re-declares KeepNode only.
    {
        StampScope stamp("TestPack", 2);
        EXPECT_TRUE(reg.Register("KeepNode", MakeFactory("KeepNode/v2"), false));
    }
    EXPECT_EQ(reg.RetireSupersededModuleNodes("TestPack", 2), 1u);

    EXPECT_EQ(reg.Find("DropNode"), nullptr) << "removed factory must be gone after reconcile";
    EXPECT_EQ(InstantiateTag(reg, "KeepNode"), "KeepNode/v2");
    EXPECT_EQ(reg.CountSupersededModuleNodes("TestPack", 2), 0u);
    EXPECT_TRUE(reg.HasModuleNodes("TestPack"));
}

// Load-abort purge drops exactly the aborted generation's entries; another
// module's entries and engine (unstamped) entries are untouched. An entry the
// aborted load re-owned from an older generation is dropped too (drop over
// dangle).
TEST(RenderPipelineNodeModuleReload, AbortPurgeDropsExactGeneration)
{
    RenderPipelineNodeRegistry reg;
    ASSERT_TRUE(reg.Register("CoreNode", MakeFactory("CoreNode/engine"), false)); // unstamped
    {
        StampScope stamp("OtherPack", 1);
        ASSERT_TRUE(reg.Register("OtherNode", MakeFactory("OtherNode/v1"), false));
    }
    {
        StampScope stamp("TestPack", 1);
        ASSERT_TRUE(reg.Register("PkgNode", MakeFactory("PkgNode/v1"), false));
    }
    // Aborted generation-2 load: one re-owned type, one new type.
    {
        StampScope stamp("TestPack", 2);
        EXPECT_TRUE(reg.Register("PkgNode", MakeFactory("PkgNode/v2"), false));
        EXPECT_TRUE(reg.Register("PkgNew", MakeFactory("PkgNew/v2"), false));
    }
    EXPECT_EQ(reg.PurgeModuleNodes("TestPack", 2), 2u);

    EXPECT_EQ(reg.Find("PkgNode"), nullptr) << "re-owned entry dies with the aborted image (drop over dangle)";
    EXPECT_EQ(reg.Find("PkgNew"), nullptr);
    EXPECT_EQ(InstantiateTag(reg, "OtherNode"), "OtherNode/v1");
    EXPECT_EQ(InstantiateTag(reg, "CoreNode"), "CoreNode/engine");
    EXPECT_FALSE(reg.HasModuleNodes("TestPack"));
}

// A same-typed registration from a DIFFERENT module (or a module trying to
// take over an engine type) is a collision, not a replacement: first wins.
TEST(RenderPipelineNodeModuleReload, CrossModuleCollisionStillFirstWins)
{
    RenderPipelineNodeRegistry reg;
    ASSERT_TRUE(reg.Register("CoreNode", MakeFactory("CoreNode/engine"), false));
    {
        StampScope stamp("PackA", 1);
        ASSERT_TRUE(reg.Register("SharedNode", MakeFactory("SharedNode/A"), false));
    }
    {
        StampScope stamp("PackB", 1);
        EXPECT_FALSE(reg.Register("SharedNode", MakeFactory("SharedNode/B"), false));
        EXPECT_FALSE(reg.Register("CoreNode", MakeFactory("CoreNode/hijack"), false));
    }
    EXPECT_EQ(InstantiateTag(reg, "SharedNode"), "SharedNode/A");
    EXPECT_EQ(InstantiateTag(reg, "CoreNode"), "CoreNode/engine");

    // Same-generation duplicate within one load is a duplicate, not a reload.
    {
        StampScope stamp("PackA", 1);
        EXPECT_FALSE(reg.Register("SharedNode", MakeFactory("SharedNode/dup"), false));
    }
    EXPECT_EQ(InstantiateTag(reg, "SharedNode"), "SharedNode/A");
}

// Engine (unstamped) entries are invisible to every module-scoped operation.
TEST(RenderPipelineNodeModuleReload, UnstampedEntriesUntouchedByModuleOps)
{
    RenderPipelineNodeRegistry reg;
    ASSERT_TRUE(reg.Register("CoreNode", MakeFactory("CoreNode/engine"), false));

    EXPECT_EQ(reg.RetireSupersededModuleNodes("TestPack", 5), 0u);
    EXPECT_EQ(reg.PurgeModuleNodes("TestPack", 5), 0u);
    EXPECT_EQ(reg.CountSupersededModuleNodes("TestPack", 5), 0u);
    EXPECT_FALSE(reg.HasModuleNodes("TestPack"));
    EXPECT_EQ(InstantiateTag(reg, "CoreNode"), "CoreNode/engine");
}
