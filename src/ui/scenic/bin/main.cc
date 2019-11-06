// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/zx/channel.h>
#include <lib/zx/profile.h>
#include <lib/zx/thread.h>

#include <memory>

#include <trace-provider/provider.h>

#include "fuchsia/scheduler/cpp/fidl.h"
#include "lib/fdio/directory.h"
#include "src/lib/fsl/syslogger/init.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/inspect_deprecated/inspect.h"
#include "src/ui/scenic/bin/app.h"

// Returns a handle to a profile for the specified deadline parameters.
zx::profile GetProfile(zx::duration capacity, zx::duration deadline, zx::duration period) {
  // Connect to the scheduler profile service to request a new profile.
  zx::channel channel0, channel1;
  zx_status_t status;

  status = zx::channel::create(0u, &channel0, &channel1);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to create channel pair: " << status;
    return {};
  }

  status = fdio_service_connect(
      (std::string("/svc/") + fuchsia::scheduler::ProfileProvider::Name_).c_str(), channel0.get());
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to connect to profile provider: " << status;
    return {};
  }

  fuchsia::scheduler::ProfileProvider_SyncProxy provider{std::move(channel1)};

  zx_status_t fidl_status;
  zx::profile profile;
  status = provider.GetDeadlineProfile(capacity.get(), deadline.get(), period.get(), "scenic/main",
                                       &fidl_status, &profile);
  if (status != ZX_OK || fidl_status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to request profile: " << status << ", " << fidl_status;
    return {};
  }

  return profile;
}

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line))
    return 1;
  if (fsl::InitLoggerFromCommandLine(command_line, {"scenic"}) != ZX_OK)
    return 1;

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher());
  std::unique_ptr<sys::ComponentContext> app_context(sys::ComponentContext::Create());

  // Set up an inspect_deprecated::Node to inject into the App.
  auto object_dir = component::ObjectDir(component::Object::Make("objects"));
  fidl::BindingSet<fuchsia::inspect::deprecated::Inspect> inspect_bindings;
  app_context->outgoing()->GetOrCreateDirectory("objects")->AddEntry(
      fuchsia::inspect::deprecated::Inspect::Name_,
      std::make_unique<vfs::Service>(inspect_bindings.GetHandler(object_dir.object().get())));

  scenic_impl::App app(std::move(app_context), inspect_deprecated::Node(std::move(object_dir)),
                       [&loop] { loop.Quit(); });

  // TODO(40858): Migrate to the role-based API when available, instead of hard
  // coding parameters.
  {
    const zx::duration capacity = zx::msec(6);
    const zx::duration deadline = zx::msec(8);
    const zx::duration period = deadline;
    const auto profile = GetProfile(capacity, deadline, period);
    if (profile) {
      const auto status = zx::thread::self()->set_profile(profile, 0);
      if (status != ZX_OK) {
        FXL_LOG(ERROR) << "Failed to apply profile to main thread: " << status;
      }
    }
  }

  loop.Run();
  FXL_LOG(INFO) << "Quit main Scenic loop.";

  return 0;
}
