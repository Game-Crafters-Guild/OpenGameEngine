#pragma once

#include "UI/UIElement.h"

#include <string>

namespace GameEngine {

/** Port affordance on a GraphPortedNode. Position comes from CSS, never px Overrides. */
class GraphPort : public UIElement {
public:
    GraphPort();
    ~GraphPort() override = default;

    void SetPortId(std::string id) { m_PortId = std::move(id); }
    const std::string& GetPortId() const { return m_PortId; }
    void SetDisplayName(const std::string& name);
    void Reset();

private:
    std::string m_PortId;
};

} // namespace GameEngine
