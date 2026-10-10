#include <gtest/gtest.h>
#include "UI/Layout/YogaLayout.h"
#include "UI/ResolvedStyle.h"
#include <vector>

using namespace GameEngine;
using namespace GameEngine::UILayout;

TEST(LayoutYogaTests, DirectionRTLFlipsStartForRow)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) {
        GTEST_SKIP() << "Yoga not available";
    }

    // LTR container: expect children laid out from left to right at 0, 100
    {
        YGNodeRef root = YogaAdapter::CreateNode();
        ResolvedStyle rootStyle;
        rootStyle.Layout.FlexDirection = FlexDirection::Row;
        rootStyle.Layout.HasFlexDirection = true;
        rootStyle.Layout.Direction = Direction::LTR;
        YogaAdapter::ApplyStyle(root, rootStyle);

        // Child 0
        YGNodeRef c0 = YogaAdapter::CreateNode();
        ResolvedStyle s0; s0.Layout.Width = StyleLength::Px(100.0f); s0.Layout.Height = StyleLength::Px(10.0f);
        YogaAdapter::ApplyStyle(c0, s0);
        // Child 1
        YGNodeRef c1 = YogaAdapter::CreateNode();
        ResolvedStyle s1; s1.Layout.Width = StyleLength::Px(100.0f); s1.Layout.Height = StyleLength::Px(10.0f);
        YogaAdapter::ApplyStyle(c1, s1);

        YGNodeInsertChild(root, c0, 0);
        YGNodeInsertChild(root, c1, 1);

        YogaAdapter::CalculateLayout(root, 300.0f, 100.0f);

        float x0 = YGNodeLayoutGetLeft(c0);
        float x1 = YGNodeLayoutGetLeft(c1);
        EXPECT_NEAR(x0, 0.0f, 0.01f);
        EXPECT_NEAR(x1, 100.0f, 0.01f);

        YogaAdapter::DestroyNode(c1);
        YogaAdapter::DestroyNode(c0);
        YogaAdapter::DestroyNode(root);
    }

    // RTL container: expect children laid out from right to left at 200, 100
    {
        YGNodeRef root = YogaAdapter::CreateNode();
        ResolvedStyle rootStyle;
        rootStyle.Layout.FlexDirection = FlexDirection::Row;
        rootStyle.Layout.HasFlexDirection = true;
        rootStyle.Layout.Direction = Direction::RTL;
        YogaAdapter::ApplyStyle(root, rootStyle);

        // Child 0
        YGNodeRef c0 = YogaAdapter::CreateNode();
        ResolvedStyle s0; s0.Layout.Width = StyleLength::Px(100.0f); s0.Layout.Height = StyleLength::Px(10.0f);
        YogaAdapter::ApplyStyle(c0, s0);
        // Child 1
        YGNodeRef c1 = YogaAdapter::CreateNode();
        ResolvedStyle s1; s1.Layout.Width = StyleLength::Px(100.0f); s1.Layout.Height = StyleLength::Px(10.0f);
        YogaAdapter::ApplyStyle(c1, s1);

        YGNodeInsertChild(root, c0, 0);
        YGNodeInsertChild(root, c1, 1);

        YogaAdapter::CalculateLayout(root, 300.0f, 100.0f);

        float x0 = YGNodeLayoutGetLeft(c0);
        float x1 = YGNodeLayoutGetLeft(c1);
        EXPECT_NEAR(x0, 200.0f, 0.01f);
        EXPECT_NEAR(x1, 100.0f, 0.01f);

        YogaAdapter::DestroyNode(c1);
        YogaAdapter::DestroyNode(c0);
        YogaAdapter::DestroyNode(root);
    }
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, DirectionDoesNotAffectColumnMainAxis)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) {
        GTEST_SKIP() << "Yoga not available";
    }

    auto buildAndMeasure = [](Direction dir) {
        YGNodeRef root = YogaAdapter::CreateNode();
        ResolvedStyle rootStyle;
        rootStyle.Layout.FlexDirection = FlexDirection::Column;
        rootStyle.Layout.HasFlexDirection = true;
        rootStyle.Layout.Direction = dir;
        YogaAdapter::ApplyStyle(root, rootStyle);

        YGNodeRef c0 = YogaAdapter::CreateNode();
        ResolvedStyle s0; s0.Layout.Height = StyleLength::Px(10.0f); s0.Layout.Width = StyleLength::Px(50.0f); YogaAdapter::ApplyStyle(c0, s0);
        YGNodeRef c1 = YogaAdapter::CreateNode();
        ResolvedStyle s1; s1.Layout.Height = StyleLength::Px(10.0f); s1.Layout.Width = StyleLength::Px(50.0f); YogaAdapter::ApplyStyle(c1, s1);

        YGNodeInsertChild(root, c0, 0);
        YGNodeInsertChild(root, c1, 1);
        YogaAdapter::CalculateLayout(root, 100.0f, 100.0f);

        float y0 = YGNodeLayoutGetTop(c0);
        float y1 = YGNodeLayoutGetTop(c1);

        YogaAdapter::DestroyNode(c1);
        YogaAdapter::DestroyNode(c0);
        YogaAdapter::DestroyNode(root);
        return std::pair<float,float>(y0, y1);
    };

    auto ltr = buildAndMeasure(Direction::LTR);
    auto rtl = buildAndMeasure(Direction::RTL);

    // Column main-axis is vertical; direction affects horizontal logical properties,
    // so Y positions should be identical.
    EXPECT_NEAR(ltr.first, 0.0f, 0.01f);
    EXPECT_NEAR(ltr.second, 10.0f, 0.01f);
    EXPECT_NEAR(rtl.first, ltr.first, 0.01f);
    EXPECT_NEAR(rtl.second, ltr.second, 0.01f);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, DefaultBlockDisplayStacksVertically)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rootStyle; // defaults: Block display, no explicit flex-direction
    YogaAdapter::ApplyStyle(root, rootStyle);

    auto makeChild = [](){
        YGNodeRef n = YogaAdapter::CreateNode();
        ResolvedStyle s;
        s.Layout.Width = StyleLength::Px(50.0f);
        s.Layout.Height = StyleLength::Px(20.0f);
        YogaAdapter::ApplyStyle(n, s);
        return n;
    };

    YGNodeRef a = makeChild();
    YGNodeRef b = makeChild();
    YGNodeInsertChild(root, a, 0);
    YGNodeInsertChild(root, b, 1);

    YogaAdapter::CalculateLayout(root, 100.0f, 100.0f);

    EXPECT_NEAR(YGNodeLayoutGetTop(a), 0.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetTop(b), 20.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(a), 0.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(b), 0.0f, 0.01f);

    YogaAdapter::DestroyNode(b);
    YogaAdapter::DestroyNode(a);
    YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, DisplayFlexWithoutFlexDirectionDefaultsToRow)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rootStyle;
    rootStyle.Layout.DisplayMode = DisplayMode::Flex; // mimic CSS display:flex without flex-direction
    YogaAdapter::ApplyStyle(root, rootStyle);

    auto makeChild = [](){
        YGNodeRef n = YogaAdapter::CreateNode();
        ResolvedStyle s;
        s.Layout.Width = StyleLength::Px(100.0f);
        s.Layout.Height = StyleLength::Px(10.0f);
        YogaAdapter::ApplyStyle(n, s);
        return n;
    };

    YGNodeRef a = makeChild();
    YGNodeRef b = makeChild();
    YGNodeInsertChild(root, a, 0);
    YGNodeInsertChild(root, b, 1);

    YogaAdapter::CalculateLayout(root, 300.0f, 50.0f);

    EXPECT_NEAR(YGNodeLayoutGetLeft(a), 0.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(b), 100.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetTop(a), 0.0f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetTop(b), 0.0f, 0.01f);

    YogaAdapter::DestroyNode(b);
    YogaAdapter::DestroyNode(a);
    YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, JustifyContentVariantsLTRandRTL)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    auto measure = [](Direction dir, JustifyContent jc){
        YGNodeRef root = YogaAdapter::CreateNode();
        ResolvedStyle rs;
        rs.Layout.FlexDirection = FlexDirection::Row;
        rs.Layout.HasFlexDirection = true;
        rs.Layout.Direction = dir;
        rs.Layout.JustifyContent = jc;
        YogaAdapter::ApplyStyle(root, rs);
        YGNodeRef a = YogaAdapter::CreateNode(); ResolvedStyle sa; sa.Layout.Width = StyleLength::Px(100.0f); sa.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(a, sa);
        YGNodeRef b = YogaAdapter::CreateNode(); ResolvedStyle sb; sb.Layout.Width = StyleLength::Px(100.0f); sb.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(b, sb);
        YGNodeInsertChild(root, a, 0); YGNodeInsertChild(root, b, 1);
        YogaAdapter::CalculateLayout(root, 300, 50);
        float xA = YGNodeLayoutGetLeft(a); float xB = YGNodeLayoutGetLeft(b);
        float wA = YGNodeLayoutGetWidth(a); (void)wA;
        YogaAdapter::DestroyNode(b); YogaAdapter::DestroyNode(a); YogaAdapter::DestroyNode(root);
        return std::pair<float,float>(xA, xB);
    };

    // Center: packed in middle regardless of direction; positions set should be {50,150}
    {
        auto ltr = measure(Direction::LTR, JustifyContent::Center);
        auto rtl = measure(Direction::RTL, JustifyContent::Center);
        auto check = [](std::pair<float,float> p){ float lo = std::min(p.first, p.second); float hi = std::max(p.first, p.second); EXPECT_NEAR(lo, 50.f, 0.01f); EXPECT_NEAR(hi, 150.f, 0.01f); };
        check(ltr); check(rtl);
    }
    // FlexEnd: LTR -> {100,200}; RTL -> {0,100}
    {
        auto ltr = measure(Direction::LTR, JustifyContent::FlexEnd);
        auto rtl = measure(Direction::RTL, JustifyContent::FlexEnd);
        auto checkLTR = [](std::pair<float,float> p){ float lo = std::min(p.first, p.second); float hi = std::max(p.first, p.second); EXPECT_NEAR(lo, 100.f, 0.01f); EXPECT_NEAR(hi, 200.f, 0.01f); };
        auto checkRTL = [](std::pair<float,float> p){ float lo = std::min(p.first, p.second); float hi = std::max(p.first, p.second); EXPECT_NEAR(lo, 0.f, 0.01f); EXPECT_NEAR(hi, 100.f, 0.01f); };
        checkLTR(ltr); checkRTL(rtl);
    }
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, AlignItemsAndAlignSelf)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rs;
    rs.Layout.FlexDirection = FlexDirection::Row;
    rs.Layout.HasFlexDirection = true;
    rs.Layout.Height = StyleLength::Px(100.0f);
    rs.Layout.AlignItems = AlignItems::FlexEnd;
    YogaAdapter::ApplyStyle(root, rs);

    YGNodeRef a = YogaAdapter::CreateNode(); ResolvedStyle sa; sa.Layout.Width = StyleLength::Px(50.0f); sa.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(a, sa);
    YGNodeRef b = YogaAdapter::CreateNode(); ResolvedStyle sb; sb.Layout.Width = StyleLength::Px(50.0f); sb.Layout.Height = StyleLength::Px(10.0f); sb.Layout.AlignSelf = AlignItems::FlexStart; YogaAdapter::ApplyStyle(b, sb);

    YGNodeInsertChild(root, a, 0); YGNodeInsertChild(root, b, 1);
    YogaAdapter::CalculateLayout(root, 200, 100);

    float yA = YGNodeLayoutGetTop(a); // respects align-items: flex-end -> 90
    float yB = YGNodeLayoutGetTop(b); // overrides with align-self: flex-start -> 0

    EXPECT_NEAR(yA, 90.f, 0.01f);
    EXPECT_NEAR(yB, 0.f, 0.01f);

    YogaAdapter::DestroyNode(b); YogaAdapter::DestroyNode(a); YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, FlexGrowDistribution)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rs;
    rs.Layout.FlexDirection = FlexDirection::Row;
    rs.Layout.HasFlexDirection = true;
    YogaAdapter::ApplyStyle(root, rs);
    YGNodeRef a = YogaAdapter::CreateNode(); ResolvedStyle sa; sa.Layout.FlexGrow = 1; sa.Layout.Width = StyleLength::Px(0.0f); sa.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(a, sa);
    YGNodeRef b = YogaAdapter::CreateNode(); ResolvedStyle sb; sb.Layout.FlexGrow = 2; sb.Layout.Width = StyleLength::Px(0.0f); sb.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(b, sb);
    YGNodeInsertChild(root, a, 0); YGNodeInsertChild(root, b, 1);

    YogaAdapter::CalculateLayout(root, 300, 20);

    float wA = YGNodeLayoutGetWidth(a);
    float wB = YGNodeLayoutGetWidth(b);
    EXPECT_NEAR(wA, 100.f, 0.01f);
    EXPECT_NEAR(wB, 200.f, 0.01f);

    YogaAdapter::DestroyNode(b); YogaAdapter::DestroyNode(a); YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}


TEST(LayoutYogaTests, AbsolutePositioningDoesNotAffectFlow)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rs;
    rs.Layout.FlexDirection = FlexDirection::Row;
    rs.Layout.HasFlexDirection = true;
    YogaAdapter::ApplyStyle(root, rs);

    YGNodeRef abs = YogaAdapter::CreateNode(); ResolvedStyle sa; sa.Layout.PositionType = PositionType::Absolute; sa.Layout.PositionLeft = StyleLength::Px(20); sa.Layout.PositionTop = StyleLength::Px(30); sa.Layout.Width = StyleLength::Px(40.0f); sa.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(abs, sa);
    YGNodeRef flow = YogaAdapter::CreateNode(); ResolvedStyle sf; sf.Layout.Width = StyleLength::Px(100.0f); sf.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(flow, sf);

    YGNodeInsertChild(root, abs, 0); YGNodeInsertChild(root, flow, 1);

    YogaAdapter::CalculateLayout(root, 300, 100);

    float xAbs = YGNodeLayoutGetLeft(abs); float yAbs = YGNodeLayoutGetTop(abs);
    float xFlow = YGNodeLayoutGetLeft(flow);

    EXPECT_NEAR(xAbs, 20.f, 0.01f);
    EXPECT_NEAR(yAbs, 30.f, 0.01f);
    EXPECT_NEAR(xFlow, 0.f, 0.01f); // flow item remains at start of line

    YogaAdapter::DestroyNode(flow); YogaAdapter::DestroyNode(abs); YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, GapWithWrapCreatesInterRowAndInterItemSpacing)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rs;
    rs.Layout.FlexDirection = FlexDirection::Row;
    rs.Layout.HasFlexDirection = true;
    rs.Layout.FlexWrap = true;
    rs.Layout.RowGap = 10;
    rs.Layout.ColumnGap = 10;
    rs.Layout.AlignContent = AlignContent::FlexStart;
    YogaAdapter::ApplyStyle(root, rs);

    auto addChild = [](){ YGNodeRef n = YogaAdapter::CreateNode(); ResolvedStyle s; s.Layout.Width = StyleLength::Px(100.0f); s.Layout.Height = StyleLength::Px(20.0f); YogaAdapter::ApplyStyle(n, s); return n; };
    YGNodeRef a = addChild(); YGNodeRef b = addChild(); YGNodeRef c = addChild(); YGNodeRef d = addChild();

    YGNodeInsertChild(root, a, 0); YGNodeInsertChild(root, b, 1); YGNodeInsertChild(root, c, 2); YGNodeInsertChild(root, d, 3);

    YogaAdapter::CalculateLayout(root, 230, 200);

    // Expect two rows: (a,b) then (c,d). Column gap = 10 -> second item x = 110
    EXPECT_NEAR(YGNodeLayoutGetLeft(a), 0.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(b), 110.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(c), 0.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(d), 110.f, 0.01f);

    // Row gap = 10 -> second row y = itemHeight(20) + 10 = 30
    EXPECT_NEAR(YGNodeLayoutGetTop(c), 30.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetTop(d), 30.f, 0.01f);

    YogaAdapter::DestroyNode(d); YogaAdapter::DestroyNode(c); YogaAdapter::DestroyNode(b); YogaAdapter::DestroyNode(a); YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}



TEST(LayoutYogaTests, FlexShrinkDistribution)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rs;
    rs.Layout.FlexDirection = FlexDirection::Row;
    rs.Layout.HasFlexDirection = true;
    YogaAdapter::ApplyStyle(root, rs);
    YGNodeRef a = YogaAdapter::CreateNode(); ResolvedStyle sa; sa.Layout.Width = StyleLength::Px(100.0f); sa.Layout.Height = StyleLength::Px(10.0f); sa.Layout.FlexShrink = 1.0f; YogaAdapter::ApplyStyle(a, sa);
    YGNodeRef b = YogaAdapter::CreateNode(); ResolvedStyle sb; sb.Layout.Width = StyleLength::Px(100.0f); sb.Layout.Height = StyleLength::Px(10.0f); sb.Layout.FlexShrink = 1.0f; YogaAdapter::ApplyStyle(b, sb);
    YGNodeInsertChild(root, a, 0); YGNodeInsertChild(root, b, 1);

    YogaAdapter::CalculateLayout(root, 150, 20);

    float wA = YGNodeLayoutGetWidth(a);
    float wB = YGNodeLayoutGetWidth(b);
    EXPECT_NEAR(wA, 75.f, 0.01f);
    EXPECT_NEAR(wB, 75.f, 0.01f);

    YogaAdapter::DestroyNode(b); YogaAdapter::DestroyNode(a); YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, JustifyContentSpaceVariantsLTRandRTL)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    auto measure = [](Direction dir, JustifyContent jc){
        YGNodeRef root = YogaAdapter::CreateNode();
        ResolvedStyle rs;
        rs.Layout.FlexDirection = FlexDirection::Row;
        rs.Layout.HasFlexDirection = true;
        rs.Layout.Direction = dir;
        rs.Layout.JustifyContent = jc;
        YogaAdapter::ApplyStyle(root, rs);
        YGNodeRef a = YogaAdapter::CreateNode(); ResolvedStyle sa; sa.Layout.Width = StyleLength::Px(100.0f); sa.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(a, sa);
        YGNodeRef b = YogaAdapter::CreateNode(); ResolvedStyle sb; sb.Layout.Width = StyleLength::Px(100.0f); sb.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(b, sb);
        YGNodeInsertChild(root, a, 0); YGNodeInsertChild(root, b, 1);
        YogaAdapter::CalculateLayout(root, 300, 50);
        float xA = YGNodeLayoutGetLeft(a); float xB = YGNodeLayoutGetLeft(b);
        YogaAdapter::DestroyNode(b); YogaAdapter::DestroyNode(a); YogaAdapter::DestroyNode(root);
        return std::pair<float,float>(xA, xB);
    };

    auto checkSorted = [](std::pair<float,float> p, float lo, float hi){ float a = std::min(p.first, p.second); float b = std::max(p.first, p.second); EXPECT_NEAR(a, lo, 0.01f); EXPECT_NEAR(b, hi, 0.01f); };

    // SpaceBetween => {0,200}
    checkSorted(measure(Direction::LTR, JustifyContent::SpaceBetween), 0.f, 200.f);
    checkSorted(measure(Direction::RTL, JustifyContent::SpaceBetween), 0.f, 200.f);
    // SpaceAround => {25,175}
    checkSorted(measure(Direction::LTR, JustifyContent::SpaceAround), 25.f, 175.f);
    checkSorted(measure(Direction::RTL, JustifyContent::SpaceAround), 25.f, 175.f);
    // SpaceEvenly => {33.333,166.667}
    auto ltrEven = measure(Direction::LTR, JustifyContent::SpaceEvenly);
    auto rtlEven = measure(Direction::RTL, JustifyContent::SpaceEvenly);
    {
        float a = std::min(ltrEven.first, ltrEven.second);
        float b = std::max(ltrEven.first, ltrEven.second);
        EXPECT_NEAR(a, 33.333f, 1.0f);
        EXPECT_NEAR(b, 166.667f, 1.0f);
    }
    {
        float a = std::min(rtlEven.first, rtlEven.second);
        float b = std::max(rtlEven.first, rtlEven.second);
        EXPECT_NEAR(a, 33.333f, 1.0f);
        EXPECT_NEAR(b, 166.667f, 1.0f);
    }
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, AlignContentVariantsWithMultiLine)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    auto build = [](AlignContent ac){
        YGNodeRef root = YogaAdapter::CreateNode();
        ResolvedStyle rs;
        rs.Layout.FlexDirection = FlexDirection::Row;
        rs.Layout.HasFlexDirection = true;
        rs.Layout.FlexWrap = true;
        rs.Layout.AlignContent = ac;
        YogaAdapter::ApplyStyle(root, rs);
        auto add = [](){ YGNodeRef n = YogaAdapter::CreateNode(); ResolvedStyle s; s.Layout.Width = StyleLength::Px(100.0f); s.Layout.Height = StyleLength::Px(20.0f); YogaAdapter::ApplyStyle(n, s); return n; };
        YGNodeRef a = add(); YGNodeRef b = add(); YGNodeRef c = add(); YGNodeRef d = add();
        YGNodeInsertChild(root, a, 0); YGNodeInsertChild(root, b, 1); YGNodeInsertChild(root, c, 2); YGNodeInsertChild(root, d, 3);
        YogaAdapter::CalculateLayout(root, 210, 100); // 2 per row, 2 rows, total content height 40
        float yA = YGNodeLayoutGetTop(a); float yC = YGNodeLayoutGetTop(c);
        YogaAdapter::DestroyNode(d); YogaAdapter::DestroyNode(c); YogaAdapter::DestroyNode(b); YogaAdapter::DestroyNode(a); YogaAdapter::DestroyNode(root);
        return std::pair<float,float>(yA, yC);
    };

    // leftover = 60 (100 - 40)
    // FlexStart => rows at 0,20
    { auto p = build(AlignContent::FlexStart); EXPECT_NEAR(p.first, 0.f, 0.01f); EXPECT_NEAR(p.second, 20.f, 0.01f); }
    // Center => rows at 30,50
    { auto p = build(AlignContent::Center); EXPECT_NEAR(p.first, 30.f, 0.01f); EXPECT_NEAR(p.second, 50.f, 0.01f); }
    // FlexEnd => rows at 60,80
    { auto p = build(AlignContent::FlexEnd); EXPECT_NEAR(p.first, 60.f, 0.01f); EXPECT_NEAR(p.second, 80.f, 0.01f); }
    // SpaceBetween => rows at 0,80
    { auto p = build(AlignContent::SpaceBetween); EXPECT_NEAR(p.first, 0.f, 0.01f); EXPECT_NEAR(p.second, 80.f, 0.01f); }
    // SpaceAround => rows at 15,65
    { auto p = build(AlignContent::SpaceAround); EXPECT_NEAR(p.first, 15.f, 0.01f); EXPECT_NEAR(p.second, 65.f, 0.01f); }
    // SpaceEvenly => rows at 20,60
    { auto p = build(AlignContent::SpaceEvenly); EXPECT_NEAR(p.first, 20.f, 0.01f); EXPECT_NEAR(p.second, 60.f, 0.01f); }
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}


TEST(LayoutYogaTests, OrderSortingReordersAndIsStable)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rs;
    rs.Layout.FlexDirection = FlexDirection::Row;
    rs.Layout.HasFlexDirection = true;
    YogaAdapter::ApplyStyle(root, rs);
    // Two equal-width children
    YGNodeRef a = YogaAdapter::CreateNode(); ResolvedStyle sa; sa.Layout.Width = StyleLength::Px(100.0f); sa.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(a, sa);
    YGNodeRef b = YogaAdapter::CreateNode(); ResolvedStyle sb; sb.Layout.Width = StyleLength::Px(100.0f); sb.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(b, sb);

    // B has lower order -> should come first; equals are stable by originalIndex
    std::vector<YogaAdapter::ChildWithOrder> kids = {
        { a, 0, 0 },
        { b, -1, 1 }
    };
    YogaAdapter::InsertChildrenSortedByOrder(root, std::span<const YogaAdapter::ChildWithOrder>(kids.data(), kids.size()));

    YogaAdapter::CalculateLayout(root, 300, 50);
    EXPECT_NEAR(YGNodeLayoutGetLeft(b), 0.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(a), 100.f, 0.01f);

    YogaAdapter::DestroyNode(b); YogaAdapter::DestroyNode(a); YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, UnsettingOrderRestoresOriginalIndex)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rs;
    rs.Layout.FlexDirection = FlexDirection::Row;
    rs.Layout.HasFlexDirection = true;
    YogaAdapter::ApplyStyle(root, rs);
    // Two equal-width children inserted A then B
    YGNodeRef a = YogaAdapter::CreateNode(); ResolvedStyle sa; sa.Layout.Width = StyleLength::Px(100.0f); sa.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(a, sa);
    YGNodeRef b = YogaAdapter::CreateNode(); ResolvedStyle sb; sb.Layout.Width = StyleLength::Px(100.0f); sb.Layout.Height = StyleLength::Px(10.0f); YogaAdapter::ApplyStyle(b, sb);

    // Both orders default (unset) -> stable original order A,B
    std::vector<YogaAdapter::ChildWithOrder> kids = {
        { a, 0, 0 },
        { b, 0, 1 }
    };
    YogaAdapter::InsertChildrenSortedByOrder(root, std::span<const YogaAdapter::ChildWithOrder>(kids.data(), kids.size()));

    YogaAdapter::CalculateLayout(root, 300, 50);
    EXPECT_NEAR(YGNodeLayoutGetLeft(a), 0.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(b), 100.f, 0.01f);

    YogaAdapter::DestroyNode(b); YogaAdapter::DestroyNode(a); YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif


}

TEST(LayoutYogaTests, OrderWithWrapReordersAcrossLines)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rs;
    rs.Layout.FlexDirection = FlexDirection::Row;
    rs.Layout.HasFlexDirection = true;
    rs.Layout.FlexWrap = true;
    rs.Layout.AlignContent = AlignContent::FlexStart;
    YogaAdapter::ApplyStyle(root, rs);

    auto add = [](){ YGNodeRef n = YogaAdapter::CreateNode(); ResolvedStyle s; s.Layout.Width = StyleLength::Px(100.0f); s.Layout.Height = StyleLength::Px(20.0f); YogaAdapter::ApplyStyle(n, s); return n; };
    YGNodeRef a = add(); YGNodeRef b = add(); YGNodeRef c = add(); YGNodeRef d = add();

    // Orders: b(-1), d(0), c(1), a(2) -> two rows: (b,d) then (c,a)
    std::vector<YogaAdapter::ChildWithOrder> kids = {
        { a,  2, 0 },
        { b, -1, 1 },
        { c,  1, 2 },
        { d,  0, 3 }
    };
    YogaAdapter::InsertChildrenSortedByOrder(root, std::span<const YogaAdapter::ChildWithOrder>(kids.data(), kids.size()));

    YogaAdapter::CalculateLayout(root, 210, 200);

    EXPECT_NEAR(YGNodeLayoutGetLeft(b), 0.f, 0.01f); EXPECT_NEAR(YGNodeLayoutGetTop(b), 0.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(d),100.f, 0.01f); EXPECT_NEAR(YGNodeLayoutGetTop(d), 0.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(c), 0.f, 0.01f); EXPECT_NEAR(YGNodeLayoutGetTop(c),20.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(a),100.f, 0.01f); EXPECT_NEAR(YGNodeLayoutGetTop(a),20.f, 0.01f);

    YogaAdapter::DestroyNode(d); YogaAdapter::DestroyNode(c); YogaAdapter::DestroyNode(b); YogaAdapter::DestroyNode(a); YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, ColumnDirectionOrderAffectsVerticalPositions)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rs; rs.Layout.FlexDirection = FlexDirection::Column; YogaAdapter::ApplyStyle(root, rs);

    auto add = [](){ YGNodeRef n = YogaAdapter::CreateNode(); ResolvedStyle s; s.Layout.Height = StyleLength::Px(10.0f); s.Layout.Width = StyleLength::Px(50.0f); YogaAdapter::ApplyStyle(n, s); return n; };
    YGNodeRef a = add(); YGNodeRef b = add(); YGNodeRef c = add();

    // b(-1), a(0), c(1) -> y positions 0,10,20 respectively
    std::vector<YogaAdapter::ChildWithOrder> kids = {
        { a, 0, 0 }, { b, -1, 1 }, { c, 1, 2 }
    };
    YogaAdapter::InsertChildrenSortedByOrder(root, kids);

    YogaAdapter::CalculateLayout(root, 100, 100);

    EXPECT_NEAR(YGNodeLayoutGetTop(b), 0.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetTop(a),10.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetTop(c),20.f, 0.01f);

    YogaAdapter::DestroyNode(c); YogaAdapter::DestroyNode(b); YogaAdapter::DestroyNode(a); YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

TEST(LayoutYogaTests, NestedContainersOrderAppliedIndependently)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (!YogaAdapter::IsAvailable()) { GTEST_SKIP() << "Yoga not available"; }

    YGNodeRef root = YogaAdapter::CreateNode();
    ResolvedStyle rs;
    rs.Layout.FlexDirection = FlexDirection::Row;
    rs.Layout.HasFlexDirection = true;
    YogaAdapter::ApplyStyle(root, rs);

    // Two containers each 200px wide, with two 100px children
    auto makeContainer = [](){
        YGNodeRef p = YogaAdapter::CreateNode();
        ResolvedStyle ps;
        ps.Layout.FlexDirection = FlexDirection::Row;
        ps.Layout.HasFlexDirection = true;
        ps.Layout.Width = StyleLength::Px(200.0f);
        ps.Layout.Height = StyleLength::Px(40.0f);
        YogaAdapter::ApplyStyle(p, ps);
        return p;
    };
    auto makeChild = [](){ YGNodeRef n = YogaAdapter::CreateNode(); ResolvedStyle s; s.Layout.Width = StyleLength::Px(100.0f); s.Layout.Height = StyleLength::Px(20.0f); YogaAdapter::ApplyStyle(n, s); return n; };

    YGNodeRef p0 = makeContainer(); YGNodeRef p1 = makeContainer();
    YGNodeRef a0 = makeChild(); YGNodeRef b0 = makeChild(); // inside p0
    YGNodeRef a1 = makeChild(); YGNodeRef b1 = makeChild(); // inside p1

    // Root order: p1(-1) before p0(1)
    std::vector<YogaAdapter::ChildWithOrder> rootKids = { { p0, 1, 0 }, { p1, -1, 1 } };
    YogaAdapter::InsertChildrenSortedByOrder(root, rootKids);

    // p0 inner order: b0(-1) before a0(1)
    std::vector<YogaAdapter::ChildWithOrder> p0kids = { { a0, 1, 0 }, { b0, -1, 1 } };
    YogaAdapter::InsertChildrenSortedByOrder(p0, p0kids);

    // p1 inner default order: a1 then b1 (stable original order)
    std::vector<YogaAdapter::ChildWithOrder> p1kids = { { a1, 0, 0 }, { b1, 0, 1 } };
    YogaAdapter::InsertChildrenSortedByOrder(p1, p1kids);

    YogaAdapter::CalculateLayout(root, 400, 100);

    // Inside p0: b0 then a0
    EXPECT_NEAR(YGNodeLayoutGetLeft(b0), 0.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(a0),100.f, 0.01f);
    // Inside p1: a1 then b1
    EXPECT_NEAR(YGNodeLayoutGetLeft(a1), 0.f, 0.01f);
    EXPECT_NEAR(YGNodeLayoutGetLeft(b1),100.f, 0.01f);

    YogaAdapter::DestroyNode(b1); YogaAdapter::DestroyNode(a1);
    YogaAdapter::DestroyNode(b0); YogaAdapter::DestroyNode(a0);
    YogaAdapter::DestroyNode(p1); YogaAdapter::DestroyNode(p0); YogaAdapter::DestroyNode(root);
#else
    GTEST_SKIP() << "Yoga not available";
#endif
}

