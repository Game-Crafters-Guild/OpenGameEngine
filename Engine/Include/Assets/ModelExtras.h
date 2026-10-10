#pragma once

#include "Types/StringHash.h"
#include "Types/Types.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string_view>

namespace GameEngine {

/**
 * @brief The kinds of object in a model's source file whose extras the model keeps.
 *
 * The values are the scripting ABI's GE_ModelObjectKind and the order in which an import charges
 * blocks to the per-model bound.
 */
enum class ModelObjectKind : uint32 {
    Scene = 0,
    Node = 1,
    Mesh = 2,
    Material = 3,
    Animation = 4,
};

/// The number of ModelObjectKind values.
inline constexpr size_t kModelObjectKindCount = 5;

/**
 * @brief The raw extras JSON text of a model's objects, kept at import and read by object kind and name.
 *
 * The engine parses none of it: games read it through ModelApi.GetExtras. The text and the names are
 * attacker-controlled input, so the table is bounded: no block over kObjectBoundBytes, and at most
 * kModelBoundBytes of blocks and their names in all. Once a block has been refused for the model bound every
 * later block is refused too, so what a model keeps is the blocks retained before the first that did not fit.
 * One block per kind and name: the first object retained under a name keeps it.
 */
class ModelExtras {
public:
    /// The largest block one object may keep: 256 KiB of extras text.
    static constexpr size_t kObjectBoundBytes = 256u * 1024u;
    /// The most bytes of blocks and their objects' names one model may keep: 1 MiB.
    static constexpr size_t kModelBoundBytes = 1024u * 1024u;

    /// What Retain did with a block.
    enum class RetainResult {
        Retained,
        OverObjectBound, ///< The block is larger than kObjectBoundBytes.
        OverModelBound,  ///< The block and its name did not fit in kModelBoundBytes, or an earlier block did not.
        NameTaken,       ///< An earlier object of the kind keeps a block under the name.
    };

    /**
     * @brief Keeps `text` as the extras of the object of `kind` named `name`, within the bounds.
     *
     * An unnamed object is named by the empty string. A kept block charges its size and its name's to the
     * model bound; a refused block is not kept and charges nothing.
     */
    RetainResult Retain(ModelObjectKind kind, std::string_view name, std::string_view text);

    /**
     * @brief The block kept for the object of `kind` named `name`; empty when none was kept.
     *
     * The view is valid until this table is next modified, moved from or destroyed.
     */
    std::string_view Find(ModelObjectKind kind, std::string_view name) const;

private:
    std::array<FastHashMap<String, String, StringHash, std::equal_to<>>, kModelObjectKindCount> m_BlocksByKind;
    size_t m_RetainedBytes = 0;
    bool m_ModelBoundReached = false;
};

} // namespace GameEngine
