#include "Animation/OpStackNode.h"

namespace GameEngine
{
namespace Animation
{

std::unordered_map<std::string, OpStackNode::FactoryFn>& OpStackNode::Registry()
{
    static std::unordered_map<std::string, FactoryFn> s_Registry;
    return s_Registry;
}

void OpStackNode::RegisterOpFactory(const std::string& opName, FactoryFn factory)
{
    Registry()[opName] = std::move(factory);
}

std::unique_ptr<OpStackNode> OpStackNode::CreateFromName(const std::string& opName,
                                                         const nlohmann::json& params)
{
    auto& reg = Registry();
    auto it = reg.find(opName);
    if (it == reg.end()) return nullptr;
    return it->second(params);
}

int OpStackNode::CanonicalOrderForName(const std::string& opName)
{
    if (opName == "AttachmentPassthrough") return 0;
    if (opName == "LookAt")                return 1;
    if (opName == "BodyIntersect")         return 2; // deferred to v2
    if (opName == "FootLock")              return 3;
    return kUnknownOpOrder;
}

const std::vector<std::string>& CanonicalOpOrder()
{
    // BodyIntersect intentionally omitted from the v1 default; if a project
    // hand-edits it into the OpStack the canonical-order check still places
    // it between LookAt and FootLock.
    static const std::vector<std::string> s_Order = {
        "AttachmentPassthrough",
        "LookAt",
        "FootLock",
    };
    return s_Order;
}

} // namespace Animation
} // namespace GameEngine
