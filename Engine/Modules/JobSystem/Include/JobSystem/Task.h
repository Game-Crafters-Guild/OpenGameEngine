#pragma once

#include "JobSystem/TaskHandle.h"
#include "JobSystem/Types.h"

#include <exception>

namespace JobSystem
{

/**
 * @brief Base of every envelope that flows through the pool's queues.
 *
 * Carries exactly what the dispatch loop reads: one virtual (Execute), the
 * task id, and three capability bits fixed at construction. A bare
 * fire-and-forget envelope is a TaskBase (24 bytes on 64-bit targets) plus
 * its closure, so every in-tree closure fits a 128-byte slab (the size table
 * in TaskSlabPool.h). The dependency-aware graph and handle path derives from
 * Task below.
 */
class TaskBase
{
  public:
    virtual ~TaskBase() = default;
    virtual void Execute() = 0;

    /** @brief Unique task identifier (0 for bare fire-and-forget envelopes). */
    TaskId GetTaskId() const { return m_TaskId; }

    /** @brief True when the task is registered in the dependency graph. */
    bool RequiresDependencyManagement() const
    {
        return (m_Flags & kFlagDependencyManaged) != 0;
    }

    /** @brief True when the task participates in TaskHandle/TaskData tracking. */
    bool NeedsTaskData() const { return (m_Flags & kFlagNeedsTaskData) != 0; }

    /**
     * @brief True only for Detail::HandleTask envelopes (the Submit(F&&)
     * path), which carry their SharedPtr<TaskData> inline. Only HandleTask's
     * constructor sets the flag, so a true value licenses the
     * static_cast<Detail::HandleTask*> in ExecuteTaskOptimized and the
     * shutdown drain.
     */
    bool HasInlineTaskData() const { return (m_Flags & kFlagInlineTaskData) != 0; }

  protected:
    enum TaskFlags : uint8
    {
        kFlagNone = 0,
        kFlagNeedsTaskData = 1 << 0,
        kFlagDependencyManaged = 1 << 1,
        kFlagInlineTaskData = 1 << 2,
    };

    TaskBase(TaskId taskId, uint8 flags) : m_TaskId(taskId), m_Flags(flags) {}

  private:
    TaskId m_TaskId;
    uint8 m_Flags;
};

/**
 * @brief Dependency-aware task base (the graph and handle path).
 *
 * Every Task is dependency-managed. Dependencies are declared during
 * creation and stored on the task until WorkStealingThreadPool::Submit
 * (UniquePtr<Task>) registers them in the dependency graph.
 */
class Task : public TaskBase
{
  public:
    ~Task() override = default;
    virtual bool IsCancelled() const { return false; }

    /**
     * @brief The TaskHandle for this task; invalid when the task has no pool.
     */
    TaskHandle GetTaskHandle() const;

    /**
     * @brief Add a dependency before submission.
     * @param dependencyId Task that must be terminal before this task runs
     */
    void AddDependency(TaskId dependencyId);

    /**
     * @brief Add a dependency before submission; an invalid handle adds none.
     * @param dependency Task that must be terminal before this task runs
     */
    void AddDependency(const TaskHandle& dependency);

    /**
     * @brief Add several dependencies before submission.
     * @param dependencies Tasks that must be terminal before this task runs
     */
    void AddDependency(const Vector<TaskId>& dependencies);

    /**
     * @brief Add several dependencies before submission; invalid handles add none.
     * @param dependencies Tasks that must be terminal before this task runs
     */
    void AddDependency(const Vector<TaskHandle>& dependencies);

    /**
     * @brief The dependencies declared so far (registered at submission).
     */
    const Vector<TaskId>& GetDependencies() const { return m_LocalDependencies; }

  protected:
    /**
     * @brief Publish this task's result payload, readable through
     * TaskHandle::TryGetResult<T> once the task completes.
     *
     * Call from Execute() only: the executing task won the cancel
     * arbitration CAS (Pending -> Running), so it is the payload's single
     * writer while it runs. TaskHandle has no public mutators, because
     * force-completing a handle races that arbitration (see TaskHandle's
     * private mutator block).
     */
    template <typename T>
    void PublishResult(const T& result);

    /**
     * @brief Record an exception for caller inspection (TaskData::Exception;
     * the status does not change). Call from Execute() only, under the same
     * single-writer rule as PublishResult.
     */
    void PublishException(std::exception_ptr exception);

    /**
     * @brief Construct with a fresh TaskId from `jobSystem`.
     * @param jobSystem The pool the task is submitted to; null yields TaskId 0
     */
    explicit Task(WorkStealingThreadPool* jobSystem);

    /**
     * @brief Construct with a given TaskId.
     * @param jobSystem The pool the task is submitted to
     * @param taskId The id to use
     */
    Task(WorkStealingThreadPool* jobSystem, TaskId taskId);

    /**
     * @brief The pool this task is submitted to, for submitting further tasks.
     */
    WorkStealingThreadPool* GetJobSystem() const { return m_JobSystem; }

    WorkStealingThreadPool* m_JobSystem; // the pool given at construction

  private:
    Vector<TaskId> m_LocalDependencies; // registered in the graph at submission

    friend class WorkStealingThreadPool;
};

template <typename T>
void Task::PublishResult(const T& result)
{
    GetTaskHandle().SetResult(result);
}

} // namespace JobSystem
