#include "Animation/AnimationController.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Animation;

TEST(AnimationController, LoadsStatesParametersAndTransitions)
{
    const std::string text = R"json({
      "schemaVersion": 1,
      "assetType": "AnimationController",
      "entryState": "Idle",
      "parameters": [
        {"name": "Speed", "type": "Float", "defaultFloat": 0.0},
        {"name": "Grounded", "type": "Bool", "defaultBool": true}
      ],
      "states": [
        {"name": "Idle", "motion": {"assetGuid": "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", "type": "Animation", "speed": 1.0, "loop": true}},
        {"name": "Move", "blendTree": {"type": "Blend1D", "parameterX": "Speed", "children": [
          {"thresholdX": 0.0, "motion": {"assetGuid": "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb", "type": "Animation"}},
          {"thresholdX": 1.0, "motion": {"assetGuid": "cccccccc-cccc-cccc-cccc-cccccccccccc", "type": "Animation"}}
        ]}}
      ],
      "transitions": [
        {"fromState": "Idle", "toState": "Move", "durationSeconds": 0.15, "conditions": [
          {"parameter": "Speed", "comparison": "Greater", "floatValue": 0.1}
        ]}
      ]
    })json";

    AnimationController controller(GUID(), std::filesystem::path("test://controller.animcontroller"));
    Vector<uint8> data(text.begin(), text.end());
    ASSERT_TRUE(controller.LoadFromData(data));

    EXPECT_EQ(controller.EntryState(), "Idle");
    ASSERT_EQ(controller.Parameters().size(), 2u);
    EXPECT_EQ(controller.Parameters()[1].Type, AnimationParameterType::Bool);
    ASSERT_NE(controller.FindState("Move"), nullptr);
    EXPECT_EQ(controller.FindState("Move")->BlendTree.Type, BlendTreeType::Blend1D);
    EXPECT_EQ(controller.FindState("Move")->BlendTree.Children.size(), 2u);
    ASSERT_EQ(controller.Transitions().size(), 1u);
    EXPECT_EQ(controller.Transitions()[0].Conditions[0].Comparison, AnimationTransitionComparison::Greater);
}

TEST(AnimationController, LoadsEmptyStubWithoutFakeIdle)
{
    const std::string text = R"json({
      "schemaVersion": 1,
      "assetType": "AnimationController",
      "entryState": "",
      "parameters": [],
      "states": [],
      "transitions": []
    })json";

    AnimationController controller(GUID(), std::filesystem::path("test://empty.animcontroller"));
    Vector<uint8> data(text.begin(), text.end());
    ASSERT_TRUE(controller.LoadFromData(data));
    EXPECT_TRUE(controller.EntryState().empty());
    EXPECT_TRUE(controller.Parameters().empty());
    EXPECT_TRUE(controller.States().empty());
    EXPECT_TRUE(controller.Transitions().empty());
}
