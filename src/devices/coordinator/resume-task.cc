// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "resume-task.h"

#include "coordinator.h"

namespace devmgr {

ResumeTask::ResumeTask(fbl::RefPtr<Device> device, uint32_t target_system_state,
                       Completion completion)
    : Task(device->coordinator->dispatcher(), std::move(completion)),
      device_(std::move(device)),
      target_system_state_(target_system_state) {}

ResumeTask::~ResumeTask() = default;

fbl::RefPtr<ResumeTask> ResumeTask::Create(fbl::RefPtr<Device> device, uint32_t target_system_state,
                                           Completion completion) {
  return fbl::MakeRefCounted<ResumeTask>(std::move(device), target_system_state,
                                         std::move(completion));
}

bool ResumeTask::AddChildResumeTasks() {
  bool found_more_dependencies = false;
  printf("AddChildResumeTasks START for %s\n", device_->name().data());
  for (auto& child : device_->children()) {
    // Use a switch statement here so that this gets reconsidered if we add
    // more states.
    switch (child.state()) {
      // If the device is dead, any existing resume task would have been forcibly completed.
      case Device::State::kDead:
      case Device::State::kActive:
        continue;
      case Device::State::kUnbinding:
      case Device::State::kSuspending:
      case Device::State::kResuming:
      case Device::State::kResumed:
      case Device::State::kSuspended:
        printf("Adding resume task for dependency for child: %s\n", child.name().data());
        AddDependency(child.RequestResumeTask(target_system_state_));
        found_more_dependencies = true;
        break;
    }
  }
  printf("AddChildResumeTasks STOP for %s. found_more_deps: %d\n", device_->name().data(),
         found_more_dependencies);
  return found_more_dependencies;
}

void ResumeTask::Run() {
  switch (device_->state()) {
    case Device::State::kDead:
    case Device::State::kActive:
      return Complete(ZX_OK);
    case Device::State::kSuspending:
    case Device::State::kUnbinding:
    case Device::State::kSuspended:
    case Device::State::kResumed:
    case Device::State::kResuming:
      break;
  }

  // The device is about to be unbound, wait for it to complete.
  // Eventually we complete when device goes to DEAD
  if (device_->state() == Device::State::kUnbinding) {
    // The remove task depends on the unbind task, so wait for that to complete.
    auto remove_task = device_->GetActiveRemove();
    ZX_ASSERT(remove_task != nullptr);
    AddDependency(remove_task);
    return;
  }

  // The device is about to be suspended, wait for it to complete.
  if (device_->state() == Device::State::kSuspending) {
    auto suspend_task = device_->GetActiveSuspend();
    ZX_ASSERT(suspend_task != nullptr);
    AddDependency(suspend_task);
    return;
  }

  auto completion = [this](zx_status_t status) {
    if (status != ZX_OK) {
      printf("MINE MINE : ResumeTask:%s :RESUME HOOK FAILED\n", device_->name().data());
      return Complete(status);
    }
    // Handle the device proxy, if it exists, before children since they might
    // depend on it.
    if (device_->proxy() != nullptr) {
      switch (device_->proxy()->state()) {
        case Device::State::kDead:
          // Proxy is dead. We cannot resume devices under. Complete with ZX_OK.
          // We should not consider this error.
          return Complete(ZX_OK);
        case Device::State::kActive:
          break;
        case Device::State::kSuspending:
        case Device::State::kUnbinding:
        case Device::State::kSuspended:
        case Device::State::kResumed:
        case Device::State::kResuming:
          printf("Adding resume task for dependency for device: %s proxy device: %s\n",
                 device_->name().data(), device_->proxy()->name().data());
          AddDependency(device_->proxy()->RequestResumeTask(target_system_state_));
          child_resume_tasks_not_issued_ = true;
          return;
      }
    }
    child_resume_tasks_not_issued_ = false;
    if (AddChildResumeTasks()) {
      return;
    }

    printf("MINE MINE : ResumeTask:%s :COMPLETE: No more children\n", device_->name().data());
    device_->set_state(Device::State::kActive);
    device_->clear_active_resume();
    return Complete(ZX_OK);
  };

  if (device_->state() == Device::State::kSuspended) {
    printf("MINE MINE : ResumeTask: %s: START: devstate: SUSPENDED\n", device_->name().data());
    if (device_->host() == nullptr) {
      // pretend this completed successfully.
      device_->set_state(Device::State::kResumed);
      printf("MINE MINE : ResumeTask: %s No HOST.\n", device_->name().data());
      child_resume_tasks_not_issued_ = true;
      completion(ZX_OK);
      return;
    } else {
      printf("MINE MINE : ResumeTask: %s SENDING RESUME\n", device_->name().data());
      zx_status_t status = device_->SendResume(target_system_state_, std::move(completion));
      if (status != ZX_OK) {
        printf("MINE MINE : ResumeTask:%s COMPLETE: SEND RESUME FAILED\n", device_->name().data());
        device_->clear_active_resume();
        return Complete(status);
      }
    }
  }

  // This means this device's resume is complete and we need to handle the children.
  if (device_->state() == Device::State::kResumed) {
    // We come back here after proxy resume is complete unless it failed We cannot resume devices
    // under, unless proxy is active. We should not consider this error, because we do not want
    // resume task to fail, only because we have a device removed.
    if (device_->proxy() != nullptr && device_->proxy()->state() != Device::State::kActive) {
      printf("MINE MINE : ResumeTask:%s proxy state: %d\n", device_->name().data(),
             device_->proxy()->state());
      device_->set_state(Device::State::kActive);
      device_->clear_active_resume();
      return Complete(ZX_OK);
    }
    if (child_resume_tasks_not_issued_) {
      child_resume_tasks_not_issued_ = false;
      if (AddChildResumeTasks()) {
        for (auto* dependency : Dependencies()) {
          printf("The dependency %s added\n",
                 reinterpret_cast<const ResumeTask*>(dependency)->device().name().data());
        }
        return;
      }
      printf("MINE MINE : ResumeTask:%s COMPLETE: ADD CHILDREN FAILED\n", device_->name().data());
      device_->set_state(Device::State::kActive);
      device_->clear_active_resume();
      return Complete(ZX_OK);
    }
    // we have completed all dependencies. We should return ZX_OK, because
    // this device has been resumed, although children are all not resumed.
    // Complete the ResumeTask.
    device_->set_state(Device::State::kActive);
    device_->clear_active_resume();
    printf("MINE MINE : ResumeTask: Completed all dependencies for %s\n", device_->name().data());
    for (auto* dependency : Dependencies()) {
      if (dependency->is_completed()) {
        printf("The dependency %s complete\n",
               reinterpret_cast<const ResumeTask*>(dependency)->device().name().data());
      }
    }
    Complete(ZX_OK);
  }
}
}  // namespace devmgr
