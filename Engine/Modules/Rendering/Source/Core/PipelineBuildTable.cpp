#include "PipelineBuildTable.h"

#include <algorithm>

namespace GameEngine::Rendering
{

PipelineBuildTable::RequestResult PipelineBuildTable::Request(uint64_t key, uint64_t invalidationEpoch)
{
    std::lock_guard lock(m_Mutex);
    auto it = m_Rows.find(key);
    if (it != m_Rows.end())
    {
        if (it->second.RowStatus != Status::Failed)
            return {PipelineBuildState::Pending, 0};
        if (it->second.FailedAtEpoch == invalidationEpoch)
            return {PipelineBuildState::Failed, 0};
    }
    const uint64_t ticket = m_NextTicket++;
    m_Rows.insert_or_assign(key, Row{Status::Queued, ticket, 0});
    return {PipelineBuildState::Pending, ticket};
}

bool PipelineBuildTable::ClaimQueued(uint64_t key, uint64_t ticket)
{
    std::lock_guard lock(m_Mutex);
    auto it = m_Rows.find(key);
    if (it == m_Rows.end() || it->second.Ticket != ticket || it->second.RowStatus != Status::Queued)
        return false;
    it->second.RowStatus = Status::Running;
    return true;
}

std::optional<uint64_t> PipelineBuildTable::ClaimForSynchronousBuild(uint64_t key, bool& waitedBuildFailed)
{
    waitedBuildFailed = false;
    std::unique_lock lock(m_Mutex);
    auto it = m_Rows.find(key);
    if (it != m_Rows.end() && it->second.RowStatus == Status::Running)
    {
        const uint64_t runningTicket = it->second.Ticket;
        m_BuildEnded.wait(lock,
                          [this, key, runningTicket]
                          {
                              const auto row = m_Rows.find(key);
                              return row == m_Rows.end() || row->second.Ticket != runningTicket ||
                                     row->second.RowStatus != Status::Running;
                          });
        const auto row = m_Rows.find(key);
        waitedBuildFailed = row != m_Rows.end() && row->second.Ticket == runningTicket &&
                            row->second.RowStatus == Status::Failed;
        return std::nullopt;
    }
    // No row, a Failed one (a synchronous caller retries, as it always has), or a
    // Queued one this caller takes over: the queued job then finds a new ticket
    // and does nothing.
    const uint64_t ticket = m_NextTicket++;
    m_Rows.insert_or_assign(key, Row{Status::Running, ticket, 0});
    return ticket;
}

void PipelineBuildTable::Finish(uint64_t key, uint64_t ticket, bool succeeded, uint64_t invalidationEpoch)
{
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_Rows.find(key);
        if (it == m_Rows.end() || it->second.Ticket != ticket || it->second.RowStatus != Status::Running)
            return;
        if (succeeded)
            m_Rows.erase(it);
        else
            it->second = Row{Status::Failed, ticket, invalidationEpoch};
    }
    m_BuildEnded.notify_all();
}

void PipelineBuildTable::ReleaseUnrun(uint64_t key, uint64_t ticket)
{
    std::lock_guard lock(m_Mutex);
    auto it = m_Rows.find(key);
    if (it != m_Rows.end() && it->second.Ticket == ticket && it->second.RowStatus == Status::Queued)
        m_Rows.erase(it);
}

std::optional<PipelineBuildState> PipelineBuildTable::Find(uint64_t key, uint64_t invalidationEpoch) const
{
    std::lock_guard lock(m_Mutex);
    auto it = m_Rows.find(key);
    if (it == m_Rows.end())
        return std::nullopt;
    if (it->second.RowStatus != Status::Failed)
        return PipelineBuildState::Pending;
    if (it->second.FailedAtEpoch == invalidationEpoch)
        return PipelineBuildState::Failed;
    return std::nullopt;
}

void PipelineBuildTable::Quiesce()
{
    std::unique_lock lock(m_Mutex);
    std::erase_if(m_Rows, [](const auto& row) { return row.second.RowStatus != Status::Running; });
    m_BuildEnded.wait(lock,
                      [this]
                      {
                          return std::none_of(m_Rows.begin(), m_Rows.end(), [](const auto& row)
                                              { return row.second.RowStatus == Status::Running; });
                      });
    // The builds that were running have ended; forget their failures too, so the
    // device the rows describe leaves nothing behind.
    m_Rows.clear();
}

} // namespace GameEngine::Rendering
