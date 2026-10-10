#pragma once

namespace GenerationalVector {

template<typename T>
template<typename... Args>
Handle GenerationalVector<T>::Create(Args&&... args) {
    Handle handle = m_Manager.Create();
    EnsureCapacity(handle.Index());

    // Construct the element in-place using aligned storage
    new (GetStorage(handle.Index())) T(std::forward<Args>(args)...);

    return handle;
}

template<typename T>
Handle GenerationalVector<T>::Create(T&& data) {
    Handle handle = m_Manager.Create();
    EnsureCapacity(handle.Index());

    // Move construct the element
    new (GetStorage(handle.Index())) T(std::move(data));

    return handle;
}

template<typename T>
Handle GenerationalVector<T>::Create(const T& data) {
    Handle handle = m_Manager.Create();
    EnsureCapacity(handle.Index());

    // Copy construct the element
    new (GetStorage(handle.Index())) T(data);

    return handle;
}

template<typename T>
void GenerationalVector<T>::Destroy(Handle handle) {
    assert(IsValid(handle) && "Attempting to destroy invalid handle");

    // Destroy the element
    GetStorage(handle.Index())->~T();

    // Mark the handle as destroyed
    m_Manager.Destroy(handle);
}

template<typename T>
T& GenerationalVector<T>::operator[](Handle handle) {
    assert(IsValid(handle) && "Invalid handle access");
    return *GetStorage(handle.Index());
}

template<typename T>
const T& GenerationalVector<T>::operator[](Handle handle) const {
    assert(IsValid(handle) && "Invalid handle access");
    return *GetStorage(handle.Index());
}

template<typename T>
T* GenerationalVector<T>::Get(Handle handle) {
    if (!IsValid(handle)) {
        return nullptr;
    }
    return GetStorage(handle.Index());
}

template<typename T>
const T* GenerationalVector<T>::Get(Handle handle) const {
    if (!IsValid(handle)) {
        return nullptr;
    }
    return GetStorage(handle.Index());
}

template<typename T>
bool GenerationalVector<T>::IsValid(Handle handle) const {
    return m_Manager.IsAlive(handle);
}

template<typename T>
void GenerationalVector<T>::Reserve(size_t capacity) {
    m_Manager.Reserve(capacity);
    m_Data.reserve(capacity);
}

template<typename T>
void GenerationalVector<T>::Clear() {
    // Destroy all elements
    ForEach([this](Handle handle, [[maybe_unused]] T& element) {
        GetStorage(handle.Index())->~T();
    });

    m_Manager.Clear();
    m_Data.clear();
}

template<typename T>
template<typename Func>
void GenerationalVector<T>::ForEach(Func&& func) {
    const auto size = static_cast<Handle::IndexType>(m_Manager.Size());
    for (Handle::IndexType i = 0; i < size; ++i) {
        // The manager owns the slot's current generation; a recycled slot is
        // alive at generation >= 2, so the handle must be read, not guessed.
        const Handle handle = m_Manager.HandleAt(i);
        if (handle.IsValid()) {
            func(handle, *GetStorage(i));
        }
    }
}

template<typename T>
template<typename Func>
void GenerationalVector<T>::ForEach(Func&& func) const {
    const auto size = static_cast<Handle::IndexType>(m_Manager.Size());
    for (Handle::IndexType i = 0; i < size; ++i) {
        const Handle handle = m_Manager.HandleAt(i);
        if (handle.IsValid()) {
            func(handle, *GetStorage(i));
        }
    }
}

template<typename T>
void GenerationalVector<T>::EnsureCapacity(Handle::IndexType index) {
    if (index < m_Data.size()) {
        return;
    }

    // Bytewise relocation is exactly what trivial copyability licenses, so the
    // plain resize — vector's memcpy on growth — stays the fast path for the
    // handle-sized payloads the renderer stores here. if/else rather than an
    // early return: the discarded branch is never instantiated, so the trivial
    // case does not carry an unreachable relocation loop.
    if constexpr (std::is_trivially_copyable_v<T>) {
        m_Data.resize(index + 1);
    } else {
        if (index < m_Data.capacity()) {
            m_Data.resize(index + 1); // Within capacity: the buffer does not move.
            return;
        }

        // Growing the buffer relocates it, and std::vector would relocate the raw
        // storage blocks BYTEWISE — undefined for a live non-trivially-copyable T
        // (MSVC debug iterators alone keep container back-pointers that would then
        // dangle into the freed buffer). Relocate the live elements by
        // move-construction instead. Handles survive; raw element pointers are
        // invalidated, as the class contract states.
        static_assert(std::is_nothrow_move_constructible_v<T> ||
                          std::is_nothrow_copy_constructible_v<T>,
                      "GenerationalVector relocation must not throw: a throwing "
                      "relocation mid-growth leaks the constructed prefix and desyncs "
                      "the manager from storage.");
        decltype(m_Data) grown;
        grown.reserve(std::max(m_Data.capacity() * 2, static_cast<size_t>(index) + 1));
        grown.resize(static_cast<size_t>(index) + 1);
        // Bounded by the OLD size: the slot this call is growing for is alive in
        // the manager but holds no constructed element yet.
        const auto oldCount = static_cast<Handle::IndexType>(m_Data.size());
        for (Handle::IndexType i = 0; i < oldCount; ++i) {
            if (!m_Manager.HandleAt(i).IsValid()) {
                continue;
            }
            T* element = GetStorage(i);
            new (reinterpret_cast<T*>(&grown[i])) T(std::move_if_noexcept(*element));
            element->~T();
        }
        m_Data.swap(grown);
    }
}

} // namespace GenerationalVector
