#include "Rendering/Core/RenderGraph/RGGraph.h"

#include <cassert>

namespace GameEngine::Rendering::RenderGraph
{

void RGGraph::BuildSubmissionPlan(const uint8_t physicalQueueOfLogical[3])
{
    m_Submissions.clear();
    // Frame-local by construction: rebuilt from the graph every frame, so no
    // placement state outlives a frame and none joins the device-rebuild
    // reprovision class.
    m_BarrierSub.assign(m_Barriers.size(), kInvalidId);
    m_CrossQueueConsidered = 0;
    m_CrossQueueDeclined = 0;
    const uint32_t n = static_cast<uint32_t>(m_ScheduledOrder.size());
    if (n == 0)
        return;

    auto physOf = [&](RGPassId p)
    { return physicalQueueOfLogical[static_cast<uint32_t>(m_Passes[p].Desc.Queue)]; };

    // 1. Partition the scheduled order into maximal same-PHYSICAL-queue runs
    //    (aliased logical queues — e.g. MoltenVK compute==graphics — collapse
    //    into one submission). Timeline signal values come from PERSISTENT
    //    per-queue counters (m_QueueTimelineNext) — frames overlap in flight, so
    //    restarting values per frame would collide with frame N-1's still-pending
    //    signals.
    auto& subOfSched = m_ScratchSubOfSched;
    subOfSched.assign(n, 0);
    for (uint32_t i = 0; i < n;)
    {
        const uint8_t phys = physOf(m_ScheduledOrder[i]);
        const uint32_t first = i;
        const uint32_t subIdx = static_cast<uint32_t>(m_Submissions.size());
        while (i < n && physOf(m_ScheduledOrder[i]) == phys)
        {
            subOfSched[i] = subIdx;
            ++i;
        }
        RGSubmission s;
        s.Queue = m_Passes[m_ScheduledOrder[first]].Desc.Queue;
        s.PhysicalQueue = phys;
        s.ScheduledFirst = first;
        s.ScheduledCount = i - first;
        s.SignalValue = ++m_QueueTimelineNext[phys];
        m_Submissions.push_back(s);
    }

    // 2. Derive waits from cross-queue producer->consumer hazards. Same-queue
    //    dependencies are ordered implicitly by queue submission order and need
    //    no semaphore. Cross-queue read-after-read needs nothing when both reads
    //    merely consume the last writer (each already waits on it) — EXCEPT when
    //    one read performs a LAYOUT TRANSITION. That read is a producer in the
    //    barrier layer: it rewrites the cell's layout and takes queue ownership
    //    (RGBarriers.cpp:~350), so a later accessor on a DIFFERENT physical queue
    //    must wait on the transition's submission or it may sample/re-transition
    //    the resource mid-transition (Finding A). Tracked via
    //    lastLayoutProducerSub, parallel to lastWriterSub.
    const uint32_t resCount = static_cast<uint32_t>(m_Resources.size());
    auto& lastWriterSub = m_ScratchSubLastWriter;
    lastWriterSub.assign(resCount, kInvalidId);
    auto& lastLayoutProducerSub = m_ScratchSubLastLayoutProducer;
    lastLayoutProducerSub.assign(resCount, kInvalidId);
    if (m_SubReadersSince.size() < resCount)
        m_SubReadersSince.resize(resCount);
    for (auto& v : m_SubReadersSince)
        v.clear(); // capacity retained across frames

    EnsurePassAccessIndex();

    // A read that transitions a texture's layout is a layout producer. Classify
    // it from the already-generated barrier IR (GenerateBarriers runs first —
    // RGFrame.cpp): a texture barrier whose layout changes and whose dst scope
    // carries no write bits is a read-transition (writers are handled by
    // lastWriterSub). One barrier batch per pass; the export sentinel batch
    // (Pass == kInvalidId) is post-frame and never an intra-frame producer —
    // its placement and edges are derived in step 4 below.
    auto& passBatchOf = m_ScratchPassBatchOf;
    passBatchOf.assign(m_Passes.size(), kInvalidId);
    for (uint32_t bi = 0; bi < m_BarrierBatches.size(); ++bi)
        if (m_BarrierBatches[bi].Pass != kInvalidId)
            passBatchOf[m_BarrierBatches[bi].Pass] = bi;

    auto readTransitionsLayout = [&](RGPassId p, RGResourceId res) -> bool
    {
        const uint32_t bi = passBatchOf[p];
        if (bi == kInvalidId)
            return false;
        const RGBarrierBatch& batch = m_BarrierBatches[bi];
        for (uint32_t k = batch.First; k < batch.First + batch.Count; ++k)
        {
            const RGBarrier& b = m_Barriers[k];
            if (b.Resource == res && b.IsTexture && b.OldLayout != b.NewLayout &&
                (b.DstAccess & RGAccessMask::WriteBits) == 0)
                return true;
        }
        return false;
    };

    auto addWait = [&](uint32_t consumerSub, uint32_t producerSub)
    {
        if (producerSub == kInvalidId || producerSub == consumerSub)
            return;
        const RGSubmission& prod = m_Submissions[producerSub];
        RGSubmission& cons = m_Submissions[consumerSub];
        if (prod.PhysicalQueue == cons.PhysicalQueue)
            return; // same physical queue: ordered by submission sequence
        const uint32_t prodQueue = prod.PhysicalQueue;
        for (uint32_t i = 0; i < cons.WaitCount; ++i)
        {
            RGSemaphoreWait& w = cons.Waits[i];
            if (w.Queue == prodQueue)
            {
                if (prod.SignalValue > w.Value)
                    w.Value = prod.SignalValue; // keep the latest value per producer queue
                return;
            }
        }
        assert(cons.WaitCount < kQueueCount && "one deduped wait per foreign physical queue");
        cons.Waits[cons.WaitCount++] = RGSemaphoreWait{prodQueue, prod.SignalValue};
    };

    // Producer-tail placement for a cross-physical layout transition: the LATEST
    // submission that has touched `res` so far, but ONLY while every one of those
    // accessors sits on `producerPhys` — otherwise kInvalidId (the barrier stays
    // on its consumer). That precondition is what makes the relocation free of
    // new semaphore operations and free of new hazards, in both directions:
    //   · all accessors on one physical queue ⇒ they are ordered against the
    //     placement by submission sequence alone (addWait no-ops same-queue
    //     pairs), so the transition can never run under an earlier reader;
    //   · the consuming pass ALREADY waits on every candidate below (RAW on the
    //     last writer, RAW-on-layout on the last layout producer, WAR over the
    //     readers since the last write), and those waits dedup per producer
    //     queue keeping the latest value — which is exactly this placement.
    // Declining is always safe: the barrier records on the consumer as before.
    auto producerTailFor = [&](RGResourceId res, uint8_t producerPhys) -> uint32_t
    {
        uint32_t latest = kInvalidId;
        auto consider = [&](uint32_t s) -> bool
        {
            if (s == kInvalidId)
                return true;
            if (m_Submissions[s].PhysicalQueue != producerPhys)
                return false;
            if (latest == kInvalidId || s > latest)
                latest = s;
            return true;
        };
        if (!consider(lastWriterSub[res]) || !consider(lastLayoutProducerSub[res]))
            return kInvalidId;
        for (uint32_t rs : m_SubReadersSince[res])
            if (!consider(rs))
                return kInvalidId;
        return latest;
    };

    for (uint32_t si = 0; si < n; ++si)
    {
        const RGPassId p = m_ScheduledOrder[si];
        const uint32_t sub = subOfSched[si];

        // 2a. Relocate this pass's CROSS-PHYSICAL layout transitions onto the
        //     producing queue's tail. A transition recorded on the consumer can
        //     only discard its source scope — the producing stage (e.g.
        //     ColorAttachmentOutput) is not a legal mask on a compute queue — so
        //     the dependency rests entirely on the semaphore and the recorded
        //     scope is fiction. Recorded on the producer instead, the true source
        //     scope is expressible verbatim and it is the DST scope that is
        //     foreign; that one is provably redundant with the timeline edge the
        //     plan already derives, since a semaphore signal/wait pair carries a
        //     full memory dependency. Read BEFORE this pass's accesses are folded
        //     in below: the candidate state must be the pre-pass state.
        //     FirstTouch barriers are excluded — they have no intra-frame
        //     producer to record against and belong to the submission-front hoist.
        const uint32_t placeBatch = passBatchOf[p];
        if (placeBatch != kInvalidId)
        {
            const RGBarrierBatch& batch = m_BarrierBatches[placeBatch];
            for (uint32_t k = batch.First; k < batch.First + batch.Count; ++k)
            {
                const RGBarrier& b = m_Barriers[k];
                if (!b.IsTexture || b.OldLayout == b.NewLayout || b.FirstTouch ||
                    b.SrcQueue >= kQueueCount)
                    continue;
                const uint8_t producerPhys = physicalQueueOfLogical[b.SrcQueue];
                if (producerPhys == m_Submissions[sub].PhysicalQueue)
                    continue; // same physical queue: the source scope is already legal
                ++m_CrossQueueConsidered;
                const uint32_t placement = producerTailFor(b.Resource, producerPhys);
                if (placement != kInvalidId)
                    m_BarrierSub[k] = placement;
                else
                    ++m_CrossQueueDeclined;
            }
        }

        for (uint32_t k = m_PassAccOff[p]; k < m_PassAccOff[p + 1]; ++k)
        {
            const RGAccessRecord& a = m_Accesses[m_PassAccIdx[k]];
            if (!IsWrite(a.Access))
            {
                addWait(sub, lastWriterSub[a.Resource]); // RAW
                // Wait on the most-recent read that TRANSITIONED this resource's
                // layout: on a different physical queue that transition must
                // retire before this reader observes (or re-transitions) the
                // layout. Same-physical producers are ordered by submission
                // sequence — addWait no-ops them (closes Finding A, RAW-on-layout).
                addWait(sub, lastLayoutProducerSub[a.Resource]);
                const bool transitionsLayout = readTransitionsLayout(p, a.Resource);
                if (transitionsLayout)
                {
                    // WAR-on-layout: this read RE-TRANSITIONS the layout, so every
                    // earlier concurrent reader on another physical queue must
                    // retire first or it samples the image mid-transition — the
                    // same drain a write performs over m_SubReadersSince below.
                    // Same-physical readers no-op in addWait; the list is not
                    // cleared (a later writer's WAR edges stay direct).
                    for (uint32_t rs : m_SubReadersSince[a.Resource])
                        addWait(sub, rs);
                }
                m_SubReadersSince[a.Resource].push_back(sub);
                // If THIS read is itself the transition, it becomes the layout
                // producer — set AFTER waiting on the prior one so a chain of
                // read-transitions across queues stays ordered.
                if (transitionsLayout)
                    lastLayoutProducerSub[a.Resource] = sub;
            }
        }
        for (uint32_t k = m_PassAccOff[p]; k < m_PassAccOff[p + 1]; ++k)
        {
            const RGAccessRecord& a = m_Accesses[m_PassAccIdx[k]];
            if (IsWrite(a.Access))
            {
                for (uint32_t rs : m_SubReadersSince[a.Resource])
                    addWait(sub, rs); // WAR
                addWait(sub, lastWriterSub[a.Resource]); // WAW
                lastWriterSub[a.Resource] = sub;
                m_SubReadersSince[a.Resource].clear();
                // The write supersedes any prior read-transition layout producer:
                // it takes ownership and later readers wait on it via RAW. A stale
                // layout producer is always among the readers just WAR-covered
                // above, so clearing it only drops redundant cross-queue waits —
                // never a needed edge.
                lastLayoutProducerSub[a.Resource] = kInvalidId;
            }
        }
    }

    // 3. Explicit ordering edges carry no accesses, so the loops above can't
    //    see them: across physical queues the schedule order alone doesn't
    //    execute in order, so each cross-physical edge needs a timeline wait.
    //    (No-op under the collapsed single-queue map; culled endpoints never
    //    enter m_ScheduledOrder and drop out via kInvalidId.)
    if (!m_OrderingEdges.empty())
    {
        auto& subOfPass = m_ScratchSubOfPass;
        subOfPass.assign(m_Passes.size(), kInvalidId);
        for (uint32_t si = 0; si < n; ++si)
            subOfPass[m_ScheduledOrder[si]] = subOfSched[si];
        for (const auto& [before, after] : m_OrderingEdges)
        {
            if (before >= subOfPass.size() || after >= subOfPass.size())
                continue;
            const uint32_t prodSub = subOfPass[before];
            const uint32_t consSub = subOfPass[after];
            if (prodSub != kInvalidId && consSub != kInvalidId)
                addWait(consSub, prodSub);
        }
    }

    // 4. The export/normalize sentinel batch is not a pass, so the loops above
    //    never see it. Place each export barrier and derive its edges here.
    //    Placement: the LATEST submission among the last submission on the
    //    barrier's own (SrcQueue) physical queue, the resource's last writer,
    //    its last layout producer, and every reader since the last writer.
    //    Taking the latest keeps every derived edge pointing BACKWARD in
    //    submission order (a cross-physical accessor gets a timeline wait,
    //    never a wait on a later submission's signal), the transition is
    //    ordered after every access to the resource, and whenever no foreign
    //    accessor trails the owner the barrier records on the queue that last
    //    touched the cells — where its source scope is expressible verbatim.
    //    A foreign placement's scopes are sanitized by the recording layer
    //    like every other cross-physical barrier (RGRecord).
    const RGBarrierBatch* exportBatch = nullptr;
    for (const RGBarrierBatch& bb : m_BarrierBatches)
        if (bb.Pass == kInvalidId)
            exportBatch = &bb;
    if (exportBatch != nullptr)
    {
        uint32_t lastSubOnPhys[kQueueCount];
        for (uint32_t q = 0; q < kQueueCount; ++q)
            lastSubOnPhys[q] = kInvalidId;
        for (uint32_t s = 0; s < m_Submissions.size(); ++s)
            lastSubOnPhys[m_Submissions[s].PhysicalQueue] = s;

        for (uint32_t k = exportBatch->First; k < exportBatch->First + exportBatch->Count; ++k)
        {
            const RGBarrier& b = m_Barriers[k];
            uint32_t placement =
                b.SrcQueue < kQueueCount ? lastSubOnPhys[physicalQueueOfLogical[b.SrcQueue]]
                                         : kInvalidId;
            auto latest = [&placement](uint32_t sub)
            {
                if (sub != kInvalidId && (placement == kInvalidId || sub > placement))
                    placement = sub;
            };
            latest(lastWriterSub[b.Resource]);
            latest(lastLayoutProducerSub[b.Resource]);
            for (uint32_t rs : m_SubReadersSince[b.Resource])
                latest(rs);
            // A used-by-live resource always has at least one accessor this
            // frame, so a candidate exists; the fallback only guards a
            // normalize barrier whose untouched cells init to a queue with no
            // submissions (the accessor candidates cover the real ordering).
            assert(placement != kInvalidId &&
                   "RenderGraph: export barrier for a resource no submission touched");
            if (placement == kInvalidId)
                placement = static_cast<uint32_t>(m_Submissions.size()) - 1;
            addWait(placement, lastWriterSub[b.Resource]);
            addWait(placement, lastLayoutProducerSub[b.Resource]);
            for (uint32_t rs : m_SubReadersSince[b.Resource])
                addWait(placement, rs);
            m_BarrierSub[k] = placement;
        }
    }
}

} // namespace GameEngine::Rendering::RenderGraph
