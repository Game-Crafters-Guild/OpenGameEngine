#include "JobSystem/Task.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <utility>

namespace JobSystem {

Task::Task(WorkStealingThreadPool* jobSystem)
    : TaskBase(jobSystem ? jobSystem->GenerateTaskId() : TaskId{0},
               kFlagNeedsTaskData | kFlagDependencyManaged),
      m_JobSystem(jobSystem) {
}

Task::Task(WorkStealingThreadPool* jobSystem, TaskId taskId)
    : TaskBase(taskId, kFlagNeedsTaskData | kFlagDependencyManaged), m_JobSystem(jobSystem) {
}

TaskHandle Task::GetTaskHandle() const {
    if (m_JobSystem) {
        return TaskHandle(GetTaskId(), m_JobSystem);
    }
    return TaskHandle(); // Invalid handle
}

void Task::PublishException(std::exception_ptr exception) {
    GetTaskHandle().SetException(std::move(exception));
}

void Task::AddDependency(TaskId dependencyId) {
    m_LocalDependencies.push_back(dependencyId);
}

void Task::AddDependency(const TaskHandle& dependency) {
    if (dependency.IsValid()) {
        AddDependency(dependency.GetId());
    }
}

void Task::AddDependency(const Vector<TaskId>& dependencies) {
    m_LocalDependencies.insert(m_LocalDependencies.end(), dependencies.begin(), dependencies.end());
}

void Task::AddDependency(const Vector<TaskHandle>& dependencies) {
    for (const auto& dep : dependencies) {
        if (dep.IsValid()) {
            AddDependency(dep.GetId());
        }
    }
}

} // namespace JobSystem
