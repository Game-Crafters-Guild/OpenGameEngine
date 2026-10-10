// Per-frame record of which shader generation each pass class served for each
// material. The variant cache deliberately keeps serving an older published
// pipeline while a recompile builds its replacement, so within one frame the
// colour draws and the depth draws of a material can render different
// generations. A pass that derives its output from another pass's surface (a
// previous-frame endpoint) compares the generations here before trusting it.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace GameEngine::Engine::Renderer
{

// The pass classes that serve material variants and can disagree during a
// recompile window.
enum class ServedPassClass : uint8_t
{
    Color,
    Depth,
    Count
};

class ServedGenerationTable
{
  public:
    // Reported by Get when two draws of one (material, pass class) served
    // different generations in the same frame — itself a disagreement.
    static constexpr uint32_t kConflict = 0xFFFFFFFFu;

    // Serial, once per frame, before any record window opens: advances the
    // frame every entry is stamped against and grows the storage to cover
    // `materialIndexCount` material indices. Entries are never cleared; a
    // stale frame stamp is what makes an entry unset for the new frame.
    void BeginFrame(uint32_t materialIndexCount);

    // Wait-free from any record worker. An index past the storage is ignored:
    // the frame-begin sizing covers every index the material system has
    // handed out, so this only happens for an index minted mid-frame, whose
    // draws could not have been recorded this frame anyway.
    void Record(uint32_t materialIndex, ServedPassClass passClass, uint32_t generation);

    // The generation served this frame, kConflict if two differed, nullopt
    // if nothing was served for that (material, pass class) this frame.
    std::optional<uint32_t> Get(uint32_t materialIndex, ServedPassClass passClass) const;

    uint32_t Frame() const { return m_Frame; }

  private:
    // One 64-bit word per (material, class): the frame it was written in the
    // high half, the generation in the low half, so a record is one atomic
    // store and a read never observes a torn (frame, generation) pair.
    static uint64_t Pack(uint32_t frame, uint32_t generation)
    {
        return (static_cast<uint64_t>(frame) << 32) | generation;
    }
    static uint32_t FrameOf(uint64_t packed) { return static_cast<uint32_t>(packed >> 32); }
    static uint32_t GenerationOf(uint64_t packed) { return static_cast<uint32_t>(packed); }
    size_t SlotIndex(uint32_t materialIndex, ServedPassClass passClass) const
    {
        return static_cast<size_t>(materialIndex) * static_cast<size_t>(ServedPassClass::Count)
               + static_cast<size_t>(passClass);
    }

    std::unique_ptr<std::atomic<uint64_t>[]> m_Slots;
    uint32_t m_MaterialIndexCount = 0;
    // Starts at 0 and advances before the first frame, so the zero-initialized
    // slots read as "not served" until they are written.
    uint32_t m_Frame = 0;
};

} // namespace GameEngine::Engine::Renderer
