#include "Assets/ExpectedAssetWrite.h"

#include "Assets/AssetManager.h"

namespace GameEngine
{

ExpectedAssetWrite::ExpectedAssetWrite(AssetManager& assets, ExpectedWriteLedger::Registration id)
    : m_Assets(assets), m_Id(id)
{
}

ExpectedAssetWrite::~ExpectedAssetWrite()
{
    m_Assets.RetireExpectedWrite(m_Id);
}

void ExpectedAssetWrite::Report(const std::filesystem::path& written)
{
    m_Assets.ReportExpectedWrite(written);
}

} // namespace GameEngine
