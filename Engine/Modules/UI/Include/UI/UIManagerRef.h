#pragma once

#include <memory>

namespace GameEngine
{
class UIManager;

/// A non-owning reference to a UIManager, held by something that can outlive it: a control's
/// registration with the manager it was attached to, a panel's last dispatch owner, a settings
/// page's list of managers it styles.
///
/// Get() returns the manager through teardown detach notifications, then nullptr once its
/// lifetime token is invalidated before the owned tree is destroyed. The token, not the address,
/// identifies the manager, so a
/// later UIManager allocated at the same address is not mistaken for the one referenced.
/// UI thread only, like the manager itself.
class UIManagerRef
{
  public:
    UIManagerRef() = default;
    explicit UIManagerRef(UIManager* manager);

    UIManager* Get() const;

  private:
    UIManager* m_Manager = nullptr;
    std::weak_ptr<const void> m_Lifetime;
};

} // namespace GameEngine
