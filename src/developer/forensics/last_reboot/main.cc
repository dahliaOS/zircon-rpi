// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/fit/promise.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/sys/inspect/cpp/component.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>

#include <string>

#include "src/developer/forensics/last_reboot/main_service.h"
#include "src/lib/files/file.h"
#include "src/lib/files/path.h"

namespace {

constexpr char kTmpGracefulRebootReason[] = "/tmp/graceful_reboot_reason.txt";
constexpr char kCacheGracefulRebootReason[] = "/cache/graceful_reboot_reason.txt";

void MoveGracefulRebootReason() {
  if (files::IsFile(kTmpGracefulRebootReason)) {
    FX_LOGS(INFO)
        << "The graceful reboot reason has been moved in a previous instance of the component";
    return;
  }

  if (!files::IsFile(kCacheGracefulRebootReason)) {
    return;
  }

  std::string content;
  if (!files::ReadFileToString(kCacheGracefulRebootReason, &content)) {
    FX_LOGS(ERROR) << "Failed to read graceful reboot reason from " << kCacheGracefulRebootReason;
    return;
  }

  if (!files::WriteFile(kTmpGracefulRebootReason, content.c_str(), content.size())) {
    FX_LOGS(ERROR) << "Failed to write graceful reboot reason to " << kTmpGracefulRebootReason;
    return;
  }

  if (!files::DeletePath(kCacheGracefulRebootReason, /*recursive=*/true)) {
    FX_LOGS(ERROR) << "Failed to delete " << kCacheGracefulRebootReason;
    return;
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace ::forensics::last_reboot;

  syslog::SetTags({"feedback"});

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  auto context = sys::ComponentContext::CreateAndServeOutgoingDirectory();
  auto inspector = std::make_unique<sys::ComponentInspector>(context.get());

  MoveGracefulRebootReason();

  MainService main_service(MainService::Config{
      .dispatcher = loop.dispatcher(),
      .services = context->svc(),
      .root_node = &(inspector->root()),
      .reboot_log = RebootLog::ParseRebootLog("/boot/log/last-panic.txt", kTmpGracefulRebootReason),
      .graceful_reboot_reason_write_path = kCacheGracefulRebootReason,
  });

  // fuchsia.feedback.LastRebootInfoProvider
  context->outgoing()->AddPublicService(
      ::fidl::InterfaceRequestHandler<fuchsia::feedback::LastRebootInfoProvider>(
          [&main_service](
              ::fidl::InterfaceRequest<fuchsia::feedback::LastRebootInfoProvider> request) {
            main_service.HandleLastRebootInfoProviderRequest(std::move(request));
          }));

  main_service.WatchForImminentGracefulReboot();

  // We file the crash report with a 90s delay to increase the likelihood that Inspect data (at all
  // and specifically the data from memory_monitor) is included in the bugreport.zip generated by
  // the Feedback service. The memory_monitor Inspect data is critical to debug OOM crash reports.
  // TODO(fxb/46216, fxb/48485): remove delay.
  main_service.Report(/*crash_reporting_delay=*/zx::sec(90));

  loop.Run();

  return EXIT_SUCCESS;
}
