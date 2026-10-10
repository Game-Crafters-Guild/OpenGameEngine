#pragma once

#include <type_traits>
#include <utility>

namespace GameEngine::Scene
{

// Non-owning function reference. Replaces std::function in TLAS traversal
// hot paths to avoid per-leaf allocation and the type-erasure inlining
// barrier. Lifetime contract: the callable must outlive the FunctionRef.
//
// Usage:
//   void Visit(FunctionRef<bool(int)> cb) { if (cb(42)) ...; }
//   auto lambda = [](int x) { return x > 0; };
//   Visit(lambda);  // binds &lambda; lambda must outlive Visit's scope
template <typename Sig>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...)>
{
public:
    FunctionRef() noexcept = default;

    template <typename F,
              std::enable_if_t<!std::is_same_v<std::decay_t<F>, FunctionRef> &&
                               std::is_invocable_r_v<R, F&, Args...>, int> = 0>
    FunctionRef(F&& f) noexcept
        : m_User(reinterpret_cast<void*>(const_cast<std::remove_reference_t<F>*>(std::addressof(f))))
        , m_Trampoline(&Trampoline<std::remove_reference_t<F>>)
    {
    }

    R operator()(Args... args) const
    {
        return m_Trampoline(m_User, std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept { return m_Trampoline != nullptr; }

private:
    template <typename F>
    static R Trampoline(void* user, Args... args)
    {
        return (*reinterpret_cast<F*>(user))(std::forward<Args>(args)...);
    }

    void* m_User       = nullptr;
    R   (*m_Trampoline)(void*, Args...) = nullptr;
};

}
