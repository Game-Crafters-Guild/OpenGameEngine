// AudioEmitterSystemTests.cpp — lock tests for AudioEmitterSystem's
// lifecycle-events consumer behavior (P3 #389 + its must-fix f0d4c05) and
// the shared SwapGenerationGuard gap recovery it migrated onto (S1 #406).
// Both migrations previously shipped proven by inspection only.
//
// Harness shape: a REAL AudioSystem initialized headlessly (miniaudio's
// default backend priority ends at the null backend, so device-less CI still
// initializes), fed a clip synthesized in memory — no disk, no asset
// registry, immune to the per-GUID failed-load caching. The observable is the alive-voice count
// (AudioSystem::GetAliveVoiceCount); emitters are shaped so voices persist
// deterministically: loop=true (never reaped), spatialized=false (2D lane,
// no WorldTransform dependency), volume=0 (tests stay silent).

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AudioAsset.h"
#include "Audio/AudioSystem.h"
#include "Components/Audio/AudioEmitter.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECSModules/Audio/Systems/AudioEmitterSystem.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

using GameEngine::AssetManager;
using GameEngine::AudioAsset;
using GameEngine::GUID;
using GameEngine::Components::AudioEmitter;
using GameEngine::Engine::Audio::AudioEmitterSystem;
namespace ECS = GameEngine::ECS;
namespace Audio = GameEngine::Audio;

namespace
{

constexpr float kDt = 0.016f;

// A tiny valid 16-bit PCM WAV (0.5 s, 440 Hz, mono, 8 kHz) so the clip
// decodes through the same miniaudio path production WAVs take.
GameEngine::Vector<GameEngine::uint8> MakeTestToneWav()
{
    constexpr std::uint32_t kSampleRate = 8000;
    constexpr std::uint32_t kFrameCount = 4000;
    constexpr std::uint16_t kChannels = 1;
    constexpr std::uint16_t kBitsPerSample = 16;
    constexpr std::uint32_t kBytesPerFrame = kChannels * (kBitsPerSample / 8);
    constexpr std::uint32_t kDataBytes = kFrameCount * kBytesPerFrame;

    GameEngine::Vector<GameEngine::uint8> wav;
    wav.reserve(44 + kDataBytes);

    auto putU16 = [&](std::uint16_t v)
    {
        wav.push_back(static_cast<GameEngine::uint8>(v & 0xFF));
        wav.push_back(static_cast<GameEngine::uint8>((v >> 8) & 0xFF));
    };
    auto putU32 = [&](std::uint32_t v)
    {
        putU16(static_cast<std::uint16_t>(v & 0xFFFF));
        putU16(static_cast<std::uint16_t>(v >> 16));
    };
    auto putTag = [&](const char* tag) { wav.insert(wav.end(), tag, tag + 4); };

    putTag("RIFF");
    putU32(36 + kDataBytes);
    putTag("WAVE");
    putTag("fmt ");
    putU32(16);
    putU16(1); // PCM
    putU16(kChannels);
    putU32(kSampleRate);
    putU32(kSampleRate * kBytesPerFrame); // byte rate
    putU16(kBytesPerFrame);               // block align
    putU16(kBitsPerSample);
    putTag("data");
    putU32(kDataBytes);

    for (std::uint32_t i = 0; i < kFrameCount; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
        const float s = std::sin(2.0f * 3.14159265f * 440.0f * t);
        putU16(static_cast<std::uint16_t>(static_cast<std::int16_t>(s * 0.25f * 32767.0f)));
    }
    return wav;
}

} // namespace

class AudioEmitterSystemTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Deliberately never Initialize()d: the audio system only touches the
        // event dispatcher (a plain member) and takes benign misses from
        // GetAsset/LoadAssetAsync — the clip below never touches disk or the
        // registry.
        m_Assets = std::make_unique<AssetManager>();
        m_Audio = std::make_unique<Audio::AudioSystem>(*m_Assets);

        Audio::AudioSystemConfig config{};
        config.maxVoices = 32;
        if (!m_Audio->Initialize(config))
            GTEST_SKIP() << "no audio backend available (miniaudio engine init failed)";

        // Warm the GUID->clip cache from a loaded in-memory asset so the
        // system's ResolveClip(guid) hits the ReadyPcm fast path.
        m_ClipGuid = GUID::Generate();
        m_Clip = std::make_shared<AudioAsset>(m_ClipGuid,
                                              std::filesystem::path("audio-ecs-test-tone.wav"));
        ASSERT_TRUE(m_Clip->LoadFromData(MakeTestToneWav()));
        ASSERT_TRUE(m_Audio->ResolveClip(*m_Clip).IsValid());

        m_World = std::make_unique<ECS::World>();
        m_World->EnableLifecycleEvents<AudioEmitter>();
        m_System = std::make_unique<AudioEmitterSystem>(m_Audio.get());
    }

    AudioEmitter MakeEmitter() const
    {
        AudioEmitter emitter{};
        emitter.clipGuid.Set(m_ClipGuid);
        emitter.spatialized = false; // 2D lane: no WorldTransform dependency
        emitter.loop = true;         // persists until stopped — the orphan shape
        emitter.playOnStart = true;
        emitter.volume = 0.0f;       // silent
        return emitter;
    }

    ECS::EntityHandle CreateEmitterEntity() { return m_World->CreateHandle(MakeEmitter()); }

    // One engine-tick frame boundary + one system step.
    void SwapAndRun()
    {
        m_World->SwapLifecycleEvents();
        m_System->Update(*m_World, kDt);
    }

    std::size_t AliveVoices() const { return m_Audio->GetAliveVoiceCount(); }

    // Destruction order (reverse of declaration): system, world, audio
    // (Shutdown detaches the asset callback while m_Assets is alive), assets.
    std::unique_ptr<AssetManager> m_Assets;
    std::unique_ptr<Audio::AudioSystem> m_Audio;
    std::unique_ptr<ECS::World> m_World;
    std::unique_ptr<AudioEmitterSystem> m_System;
    GUID m_ClipGuid;
    std::shared_ptr<AudioAsset> m_Clip;
};

// ---------------------------------------------------------------------------
// Charter 1 — GetAdded init-seed: an added AudioEmitter starts a voice per
// its component fields, across the structural shapes.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, AddedEmitterStartsVoice)
{
    ASSERT_EQ(AliveVoices(), 0u);
    (void)CreateEmitterEntity();
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

TEST_F(AudioEmitterSystemTest, DisabledEmitterStartsNoVoice)
{
    (void)m_World->CreateHandle(MakeEmitter(), ECS::ComponentDisabled<AudioEmitter>{});
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 0u);
}

TEST_F(AudioEmitterSystemTest, ClonedEmitterStartsSecondVoice)
{
    auto e = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    (void)m_World->CloneEntity(e); // the Ctrl+D shape
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 2u);
}

// Preserve-handle destroy then revive + byte-restore across windows (the
// editor undo shape): teardown fires on the Removed window, and the restore
// re-emits Added through the unified add body, starting a fresh voice.
TEST_F(AudioEmitterSystemTest, UndoReviveRestartsVoice)
{
    auto e = CreateEmitterEntity();
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(
        m_World->CaptureComponentBytes(e, ECS::GetComponentTypeId<AudioEmitter>(), bytes));
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    m_World->DestroyEntityImmediatePreserveHandle(e);
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 0u);

    ASSERT_TRUE(m_World->ReviveEntityImmediatePreserveHandle(e));
    ASSERT_TRUE(
        m_World->ApplyComponentBytesImmediate(e, ECS::GetComponentTypeId<AudioEmitter>(), bytes));
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

// ---------------------------------------------------------------------------
// Charter 2 — GetRemoved handle-keyed teardown: component removal and every
// per-entity destroy path stop the voice and erase the record.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, ComponentRemoveStopsVoice)
{
    auto e = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    m_World->RemoveComponentImmediate<AudioEmitter>(e); // entity stays alive
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 0u);
}

TEST_F(AudioEmitterSystemTest, EntityDestroyStopsVoice)
{
    auto e1 = CreateEmitterEntity();
    auto e2 = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 2u);

    m_World->DestroyEntityImmediate(e1);
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);

    m_World->DestroyEntity(e2); // deferred path
    m_World->ProcessCommands();
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 0u);
}

// ---------------------------------------------------------------------------
// Charter 2b — switching an emitter off (its own tag, or its entity) stops its
// voice through GetDisabled<AudioEmitter>; the component stays, and switching
// it back on plays it again from the start.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, SwitchedOffEmitterStopsVoiceAndPlaysAgainWhenBackOn)
{
    auto e = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    ECS::Entity(m_World.get(), e).SetEnabled<AudioEmitter>(false);
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 0u);
    EXPECT_TRUE(m_World->HasComponent<AudioEmitter>(e)) << "switching off keeps the component";

    ECS::Entity(m_World.get(), e).SetEnabled<AudioEmitter>(true);
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

TEST_F(AudioEmitterSystemTest, DisabledEntityStopsVoice)
{
    auto e = CreateEmitterEntity();
    auto survivor = CreateEmitterEntity();
    (void)survivor;
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 2u);

    m_World->SetEntityEnabledImmediate(e, false);
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

TEST_F(AudioEmitterSystemTest, PauseGapSweepStopsSwitchedOffVoice)
{
    auto e = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    // Pause: the switch-off's window is promoted and discarded unseen.
    ECS::Entity(m_World.get(), e).SetEnabled<AudioEmitter>(false);
    m_World->SwapLifecycleEvents();
    m_World->SwapLifecycleEvents();
    ASSERT_TRUE(m_World->GetDisabled<AudioEmitter>().empty());

    m_System->Update(*m_World, kDt);
    EXPECT_EQ(AliveVoices(), 0u);
}

// ---------------------------------------------------------------------------
// Charter 3 — removed-then-re-added within one event window: the Removed
// event fires but HasComponent sees the restored component, so the record
// (and its voice) survive.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, RemoveReAddSameWindowKeepsVoice)
{
    auto e = CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    m_World->RemoveComponentImmediate<AudioEmitter>(e);
    m_World->AddComponentImmediate<AudioEmitter>(e, MakeEmitter()); // component swap
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);

    // Bookkeeping must still be live: a plain remove afterwards tears down.
    m_World->RemoveComponentImmediate<AudioEmitter>(e);
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 0u);
}

TEST_F(AudioEmitterSystemTest, DestroyReviveRestoreSameWindowKeepsVoice)
{
    auto e = CreateEmitterEntity();
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(
        m_World->CaptureComponentBytes(e, ECS::GetComponentTypeId<AudioEmitter>(), bytes));
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 1u);

    // Undo revive+restore before the window closes.
    m_World->DestroyEntityImmediatePreserveHandle(e);
    ASSERT_TRUE(m_World->ReviveEntityImmediatePreserveHandle(e));
    ASSERT_TRUE(
        m_World->ApplyComponentBytesImmediate(e, ECS::GetComponentTypeId<AudioEmitter>(), bytes));
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

// ---------------------------------------------------------------------------
// Charter 4 — WorldReset generation: Clear() fires no per-entity Removed
// events; the reset branch drops all bookkeeping and stops every voice.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, WorldClearStopsAllVoices)
{
    (void)CreateEmitterEntity();
    (void)CreateEmitterEntity();
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 2u);

    m_World->Clear();
    m_System->Update(*m_World, kDt); // reset generation, not an event — no swap needed
    EXPECT_EQ(AliveVoices(), 0u);

    // The system keeps working for the next scene.
    (void)CreateEmitterEntity();
    SwapAndRun();
    EXPECT_EQ(AliveVoices(), 1u);
}

// ---------------------------------------------------------------------------
// Charter 5 — the pause hole (f0d4c05): the system is disabled while the
// engine tick keeps swapping windows; an emitter entity dies during the gap
// and its Removed event is promoted and discarded unseen. On resume the
// shared SwapGenerationGuard reports gap > 1 and the one-shot IsValid sweep
// stops the orphaned voice — leaving the survivor's voice alone.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, PauseGapSweepStopsOrphanedVoice)
{
    auto doomed = CreateEmitterEntity();
    auto survivor = CreateEmitterEntity();
    (void)survivor;
    SwapAndRun();
    ASSERT_EQ(AliveVoices(), 2u);

    // Pause: the engine tick keeps swapping, the system does not run.
    m_World->DestroyEntityImmediate(doomed);
    m_World->SwapLifecycleEvents(); // Removed(doomed) promoted to current...
    m_World->SwapLifecycleEvents(); // ...and discarded unseen — the hole

    ASSERT_TRUE(m_World->GetRemoved<AudioEmitter>().empty()); // event gone for good

    m_System->Update(*m_World, kDt); // resume: guard fires, sweep runs once
    EXPECT_EQ(AliveVoices(), 1u);
}

// ---------------------------------------------------------------------------
// Charter 6 — window idempotency: the editor multi-steps waves between
// swaps, so the consumer sees the same Added/Removed spans repeatedly and
// must not double-start or double-stop.
// ---------------------------------------------------------------------------

TEST_F(AudioEmitterSystemTest, DoubleStepWithinWindowIsIdempotent)
{
    auto e = CreateEmitterEntity();
    m_World->SwapLifecycleEvents();
    m_System->Update(*m_World, kDt);
    m_System->Update(*m_World, kDt); // same Added span again
    EXPECT_EQ(AliveVoices(), 1u);

    m_World->RemoveComponentImmediate<AudioEmitter>(e);
    m_World->SwapLifecycleEvents();
    m_System->Update(*m_World, kDt);
    m_System->Update(*m_World, kDt); // same Removed span again
    EXPECT_EQ(AliveVoices(), 0u);
}
