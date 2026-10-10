#include "UI/ITextMeasurable.h"
#include "TextShapeCache.h"

using namespace GameEngine;

ITextMeasurable::ITextMeasurable() = default;
ITextMeasurable::~ITextMeasurable() = default;

ITextMeasurable::TextShapeCache& ITextMeasurable::GetOrCreateTextShapeCache() const
{
    if (!m_TextShapeCache)
        m_TextShapeCache = std::make_unique<TextShapeCache>();
    return *m_TextShapeCache;
}

void ITextMeasurable::ResetTextShapeCache() const
{
    m_TextShapeCache.reset();
}

bool ITextMeasurable::WasLastRunEllipsized() const
{
    return m_TextShapeCache &&
           m_TextShapeCache->LastRunEllipsized.load(std::memory_order_relaxed);
}
