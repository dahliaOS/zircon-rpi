// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <fcntl.h>
#include <fuchsia/device/c/fidl.h>
#include <fuchsia/device/llcpp/fidl.h>
#include <fuchsia/device/manager/c/fidl.h>
#include <fuchsia/device/manager/llcpp/fidl.h>
#include <fuchsia/io/llcpp/fidl.h>
#include <lib/fdio/io.h>
#include <lib/fdio/vfs.h>
#include <lib/fidl/coding.h>
#include <lib/fidl/txn_header.h>
#include <lib/sync/completion.h>
#include <lib/zircon-internal/debug.h>
#include <lib/zx/channel.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <zircon/device/vfs.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <array>
#include <new>
#include <utility>

#include <ddk/device.h>
#include <ddk/driver.h>
#include <fbl/auto_lock.h>
#include <fs/connection.h>
#include <fs/handler.h>

#include "devhost.h"
#include "zx-device.h"

namespace devmgr {

#define ZXDEBUG 0

#define CAN_WRITE(conn) (conn->flags & ZX_FS_RIGHT_WRITABLE)
#define CAN_READ(conn) (conn->flags & ZX_FS_RIGHT_READABLE)

void describe_error(zx::channel h, zx_status_t status) {
  fuchsia_io_NodeOnOpenEvent msg;
  memset(&msg, 0, sizeof(msg));
  fidl_init_txn_header(&msg.hdr, 0, fuchsia_io_NodeOnOpenOrdinal);
  msg.s = status;
  h.write(0, &msg, sizeof(msg), nullptr, 0);
}

static zx_status_t create_description(const fbl::RefPtr<zx_device_t>& dev, fs::OnOpenMsg* msg,
                                      zx::eventpair* handle) {
  memset(msg, 0, sizeof(*msg));
  fidl_init_txn_header(&msg->primary.hdr, 0, fuchsia_io_NodeOnOpenOrdinal);
  msg->extra.tag = fuchsia_io_NodeInfoTag_device;
  msg->primary.s = ZX_OK;
  msg->primary.info = (fuchsia_io_NodeInfo*)FIDL_ALLOC_PRESENT;
  handle->reset();
  zx_handle_t* event = &msg->extra.device.event;
  if (dev->event.is_valid()) {
    zx_status_t r;
    if ((r = dev->event.duplicate(ZX_RIGHTS_BASIC, handle)) != ZX_OK) {
      msg->primary.s = r;
      return r;
    }
    *event = FIDL_HANDLE_PRESENT;
  } else {
    *event = FIDL_HANDLE_ABSENT;
  }

  return ZX_OK;
}

zx_status_t devhost_device_connect(const fbl::RefPtr<zx_device_t>& dev, uint32_t flags,
                                   zx::channel rh) {
  zx_status_t r;
  // detect response directives and discard all other
  // protocol flags
  bool describe = flags & ZX_FS_FLAG_DESCRIBE;
  flags &= (~ZX_FS_FLAG_DESCRIBE);

  auto newconn = std::make_unique<DevfsConnection>();
  if (!newconn) {
    r = ZX_ERR_NO_MEMORY;
    if (describe) {
      describe_error(std::move(rh), r);
    }
    return r;
  }

  newconn->flags = flags;

  fbl::RefPtr<zx_device_t> new_dev;
  r = device_open(dev, &new_dev, flags);
  if (r != ZX_OK) {
    goto fail;
  }
  newconn->dev = new_dev;

  if (describe) {
    fs::OnOpenMsg info;
    zx::eventpair handle;
    if ((r = create_description(new_dev, &info, &handle)) != ZX_OK) {
      goto fail_open;
    }
    uint32_t hcount = (handle.is_valid()) ? 1 : 0;
    zx_handle_t raw_handles[] = {
        handle.release(),
    };
    r = rh.write(0, &info, sizeof(info), raw_handles, hcount);
    if (r != ZX_OK) {
      goto fail_open;
    }
  }

  // If we can't add the new conn and handle to the dispatcher our only option
  // is to give up and tear down.  In practice, this should never happen.
  if ((r = devhost_start_connection(std::move(newconn), std::move(rh))) != ZX_OK) {
    // TODO(teisenbe/kulakowski): Should this be goto fail_open?
    goto fail;
  }
  return ZX_OK;

fail_open:
  device_close(std::move(new_dev), flags);
fail:
  if (describe) {
    describe_error(std::move(rh), r);
  }
  return r;
}

#define DO_READ 0
#define DO_WRITE 1

static ssize_t do_sync_io(const fbl::RefPtr<zx_device_t>& dev, uint32_t opcode, void* buf,
                          size_t count, zx_off_t off) {
  size_t actual;
  zx_status_t r;
  if (opcode == DO_READ) {
    r = dev->ReadOp(buf, count, off, &actual);
  } else {
    r = dev->WriteOp(buf, count, off, &actual);
  }
  if (r < 0) {
    return r;
  } else {
    return actual;
  }
}

static zx_status_t fidl_node_clone(void* ctx, uint32_t flags, zx_handle_t object) {
  auto conn = static_cast<DevfsConnection*>(ctx);
  zx::channel c(object);
  flags = conn->flags | (flags & ZX_FS_FLAG_DESCRIBE);
  devhost_device_connect(conn->dev, flags, std::move(c));
  return ZX_OK;
}

static zx_status_t fidl_node_close(void* ctx, fidl_txn_t* txn) {
  auto conn = static_cast<DevfsConnection*>(ctx);
  // Call device_close to let the driver execute its close hook.  This may
  // be the last reference to the device, causing it to be destroyed.
  device_close(std::move(conn->dev), conn->flags);

  fuchsia_io_NodeClose_reply(txn, ZX_OK);
  return ERR_DISPATCHER_DONE;
}

static zx_status_t fidl_node_describe(void* ctx, fidl_txn_t* txn) {
  auto conn = static_cast<DevfsConnection*>(ctx);
  const auto& dev = conn->dev;
  fuchsia_io_NodeInfo info;
  memset(&info, 0, sizeof(info));
  info.tag = fuchsia_io_NodeInfoTag_device;
  if (dev->event != ZX_HANDLE_INVALID) {
    zx::eventpair event;
    zx_status_t status = dev->event.duplicate(ZX_RIGHTS_BASIC, &event);
    if (status != ZX_OK) {
      return status;
    }
    zx_handle_t* event_handle = &info.device.event;
    *event_handle = event.release();
  }
  return fuchsia_io_NodeDescribe_reply(txn, &info);
}

static zx_status_t fidl_directory_open(void* ctx, uint32_t flags, uint32_t mode,
                                       const char* path_data, size_t path_size,
                                       zx_handle_t object) {
  zx_handle_close(object);
  return ZX_ERR_NOT_SUPPORTED;
}

static zx_status_t fidl_directory_unlink(void* ctx, const char* path_data, size_t path_size,
                                         fidl_txn_t* txn) {
  return fuchsia_io_DirectoryUnlink_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_directory_readdirents(void* ctx, uint64_t max_out, fidl_txn_t* txn) {
  return fuchsia_io_DirectoryReadDirents_reply(txn, ZX_ERR_NOT_SUPPORTED, nullptr, 0);
}

static zx_status_t fidl_directory_rewind(void* ctx, fidl_txn_t* txn) {
  return fuchsia_io_DirectoryRewind_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_directory_gettoken(void* ctx, fidl_txn_t* txn) {
  return fuchsia_io_DirectoryGetToken_reply(txn, ZX_ERR_NOT_SUPPORTED, ZX_HANDLE_INVALID);
}

static zx_status_t fidl_directory_rename(void* ctx, const char* src_data, size_t src_size,
                                         zx_handle_t dst_parent_token, const char* dst_data,
                                         size_t dst_size, fidl_txn_t* txn) {
  zx_handle_close(dst_parent_token);
  return fuchsia_io_DirectoryRename_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_directory_link(void* ctx, const char* src_data, size_t src_size,
                                       zx_handle_t dst_parent_token, const char* dst_data,
                                       size_t dst_size, fidl_txn_t* txn) {
  zx_handle_close(dst_parent_token);
  return fuchsia_io_DirectoryLink_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_directory_watch(void* ctx, uint32_t mask, uint32_t options,
                                        zx_handle_t raw_watcher, fidl_txn_t* txn) {
  auto conn = static_cast<DevfsConnection*>(ctx);
  auto dev = conn->dev;
  zx::channel watcher(raw_watcher);

  const zx::channel& rpc = *dev->coordinator_rpc;
  if (!rpc.is_valid()) {
    return fuchsia_io_DirectoryWatch_reply(txn, ZX_ERR_INTERNAL);
  }

  auto response = fuchsia::device::manager::Coordinator::Call::DirectoryWatch(
      zx::unowned_channel(rpc.get()), mask, options, std::move(watcher));
  zx_status_t status = response.status();
  zx_status_t call_status = ZX_OK;
  if (status == ZX_OK && response.Unwrap()->result.is_err()) {
    call_status = response.Unwrap()->result.err();
  }

  return fuchsia_io_DirectoryWatch_reply(txn, status != ZX_OK ? status : call_status);
}

static const fuchsia_io_Directory_ops_t kDirectoryOps = []() {
  fuchsia_io_Directory_ops_t ops;
  ops.Open = fidl_directory_open;
  ops.Unlink = fidl_directory_unlink;
  ops.ReadDirents = fidl_directory_readdirents;
  ops.Rewind = fidl_directory_rewind;
  ops.GetToken = fidl_directory_gettoken;
  ops.Rename = fidl_directory_rename;
  ops.Link = fidl_directory_link;
  ops.Watch = fidl_directory_watch;
  return ops;
}();

static zx_status_t fidl_directory_admin_mount(void* ctx, zx_handle_t h, fidl_txn_t* txn) {
  zx_handle_close(h);
  return fuchsia_io_DirectoryAdminMount_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_directory_admin_mount_and_create(void* ctx, zx_handle_t h, const char* name,
                                                         size_t len, uint32_t flags,
                                                         fidl_txn_t* txn) {
  zx_handle_close(h);
  return fuchsia_io_DirectoryAdminMountAndCreate_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_directory_admin_unmount(void* ctx, fidl_txn_t* txn) {
  return fuchsia_io_DirectoryAdminUnmount_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_directory_admin_unmount_node(void* ctx, fidl_txn_t* txn) {
  return fuchsia_io_DirectoryAdminUnmountNode_reply(txn, ZX_ERR_NOT_SUPPORTED, ZX_HANDLE_INVALID);
}

static zx_status_t fidl_directory_admin_query_filesystem(void* ctx, fidl_txn_t* txn) {
  fuchsia_io_FilesystemInfo info;
  memset(&info, 0, sizeof(info));
  const char* devhost_name = "devfs:host";
  strlcpy((char*)info.name, devhost_name, fuchsia_io_MAX_FS_NAME_BUFFER);
  return fuchsia_io_DirectoryAdminQueryFilesystem_reply(txn, ZX_OK, &info);
}

static zx_status_t fidl_directory_admin_get_device_path(void* ctx, fidl_txn_t* txn) {
  return fuchsia_io_DirectoryAdminGetDevicePath_reply(txn, ZX_ERR_NOT_SUPPORTED, NULL, 0);
}

static const fuchsia_io_DirectoryAdmin_ops_t kDirectoryAdminOps = []() {
  fuchsia_io_DirectoryAdmin_ops_t ops;
  ops.Mount = fidl_directory_admin_mount;
  ops.MountAndCreate = fidl_directory_admin_mount_and_create;
  ops.Unmount = fidl_directory_admin_unmount;
  ops.UnmountNode = fidl_directory_admin_unmount_node;
  ops.QueryFilesystem = fidl_directory_admin_query_filesystem;
  ops.GetDevicePath = fidl_directory_admin_get_device_path;
  return ops;
}();

static zx_status_t fidl_file_read(void* ctx, uint64_t count, fidl_txn_t* txn) {
  auto conn = static_cast<DevfsConnection*>(ctx);
  const auto& dev = conn->dev;
  if (!CAN_READ(conn)) {
    return fuchsia_io_FileRead_reply(txn, ZX_ERR_ACCESS_DENIED, nullptr, 0);
  } else if (count > ZXFIDL_MAX_MSG_BYTES) {
    return fuchsia_io_FileRead_reply(txn, ZX_ERR_INVALID_ARGS, nullptr, 0);
  }

  uint8_t data[count];
  size_t actual = 0;
  zx_status_t status = ZX_OK;
  ssize_t r = do_sync_io(dev, DO_READ, data, count, conn->io_off);
  if (r >= 0) {
    conn->io_off += r;
    actual = r;
  } else {
    status = static_cast<zx_status_t>(r);
  }
  return fuchsia_io_FileRead_reply(txn, status, data, actual);
}

static zx_status_t fidl_file_readat(void* ctx, uint64_t count, uint64_t offset, fidl_txn_t* txn) {
  auto conn = static_cast<DevfsConnection*>(ctx);
  if (!CAN_READ(conn)) {
    return fuchsia_io_FileReadAt_reply(txn, ZX_ERR_ACCESS_DENIED, nullptr, 0);
  } else if (count > ZXFIDL_MAX_MSG_BYTES) {
    return fuchsia_io_FileReadAt_reply(txn, ZX_ERR_INVALID_ARGS, nullptr, 0);
  }

  uint8_t data[count];
  size_t actual = 0;
  zx_status_t status = ZX_OK;
  ssize_t r = do_sync_io(conn->dev, DO_READ, data, count, offset);
  if (r >= 0) {
    actual = r;
  } else {
    status = static_cast<zx_status_t>(r);
  }
  return fuchsia_io_FileReadAt_reply(txn, status, data, actual);
}

static zx_status_t fidl_file_write(void* ctx, const uint8_t* data, size_t count, fidl_txn_t* txn) {
  auto conn = static_cast<DevfsConnection*>(ctx);
  if (!CAN_WRITE(conn)) {
    return fuchsia_io_FileWrite_reply(txn, ZX_ERR_ACCESS_DENIED, 0);
  }
  size_t actual = 0;
  zx_status_t status = ZX_OK;
  ssize_t r = do_sync_io(conn->dev, DO_WRITE, (uint8_t*)data, count, conn->io_off);
  if (r >= 0) {
    conn->io_off += r;
    actual = r;
  } else {
    status = static_cast<zx_status_t>(r);
  }
  return fuchsia_io_FileWrite_reply(txn, status, actual);
}

static zx_status_t fidl_file_writeat(void* ctx, const uint8_t* data, size_t count, uint64_t offset,
                                     fidl_txn_t* txn) {
  auto conn = static_cast<DevfsConnection*>(ctx);
  if (!CAN_WRITE(conn)) {
    return fuchsia_io_FileWriteAt_reply(txn, ZX_ERR_ACCESS_DENIED, 0);
  }

  size_t actual = 0;
  zx_status_t status = ZX_OK;
  ssize_t r = do_sync_io(conn->dev, DO_WRITE, (uint8_t*)data, count, offset);
  if (r >= 0) {
    actual = r;
  } else {
    status = static_cast<zx_status_t>(r);
  }
  return fuchsia_io_FileWriteAt_reply(txn, status, actual);
}

static zx_status_t fidl_file_seek(void* ctx, int64_t offset, fuchsia_io_SeekOrigin start,
                                  fidl_txn_t* txn) {
  auto conn = static_cast<DevfsConnection*>(ctx);
  auto bad_args = [&]() { return fuchsia_io_FileSeek_reply(txn, ZX_ERR_INVALID_ARGS, 0); };
  size_t end = conn->dev->GetSizeOp();
  size_t n;
  switch (start) {
    case fuchsia_io_SeekOrigin_START:
      if ((offset < 0) || ((size_t)offset > end)) {
        return bad_args();
      }
      n = offset;
      break;
    case fuchsia_io_SeekOrigin_CURRENT:
      // TODO: track seekability with flag, don't update off
      // at all on read/write if not seekable
      n = conn->io_off + offset;
      if (offset < 0) {
        // if negative seek
        if (n > conn->io_off) {
          // wrapped around
          return bad_args();
        }
      } else {
        // positive seek
        if (n < conn->io_off) {
          // wrapped around
          return bad_args();
        }
      }
      break;
    case fuchsia_io_SeekOrigin_END:
      n = end + offset;
      if (offset <= 0) {
        // if negative or exact-end seek
        if (n > end) {
          // wrapped around
          return bad_args();
        }
      } else {
        if (n < end) {
          // wrapped around
          return bad_args();
        }
      }
      break;
    default:
      return bad_args();
  }
  if (n > end) {
    // devices may not seek past the end
    return bad_args();
  }
  conn->io_off = n;
  return fuchsia_io_FileSeek_reply(txn, ZX_OK, conn->io_off);
}

static zx_status_t fidl_file_truncate(void* ctx, uint64_t length, fidl_txn_t* txn) {
  return fuchsia_io_FileTruncate_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_file_getflags(void* ctx, fidl_txn_t* txn) {
  return fuchsia_io_FileGetFlags_reply(txn, ZX_ERR_NOT_SUPPORTED, 0);
}

static zx_status_t fidl_file_setflags(void* ctx, uint32_t flags, fidl_txn_t* txn) {
  return fuchsia_io_FileSetFlags_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_file_getbuffer(void* ctx, uint32_t flags, fidl_txn_t* txn) {
  return fuchsia_io_FileGetBuffer_reply(txn, ZX_ERR_NOT_SUPPORTED, nullptr);
}

static const fuchsia_io_File_ops_t kFileOps = []() {
  fuchsia_io_File_ops_t ops;
  ops.Read = fidl_file_read;
  ops.ReadAt = fidl_file_readat;
  ops.Write = fidl_file_write;
  ops.WriteAt = fidl_file_writeat;
  ops.Seek = fidl_file_seek;
  ops.Truncate = fidl_file_truncate;
  ops.GetFlags = fidl_file_getflags;
  ops.SetFlags = fidl_file_setflags;
  ops.GetBuffer = fidl_file_getbuffer;
  return ops;
}();

static zx_status_t fidl_node_sync(void* ctx, fidl_txn_t* txn) {
  // TODO(ZX-3294): We may want to support sync through the block
  // protocol, but in the interim, it is unsupported.
  return fuchsia_io_NodeSync_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_node_getattr(void* ctx, fidl_txn_t* txn) {
  auto conn = static_cast<DevfsConnection*>(ctx);
  fuchsia_io_NodeAttributes attributes;
  memset(&attributes, 0, sizeof(attributes));
  attributes.mode = V_TYPE_CDEV | V_IRUSR | V_IWUSR;
  attributes.content_size = conn->dev->GetSizeOp();
  attributes.link_count = 1;
  return fuchsia_io_NodeGetAttr_reply(txn, ZX_OK, &attributes);
}

static zx_status_t fidl_node_setattr(void* ctx, uint32_t flags,
                                     const fuchsia_io_NodeAttributes* attributes, fidl_txn_t* txn) {
  return fuchsia_io_NodeSetAttr_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_node_getflags(void* ctx, fidl_txn_t* txn) {
  return fuchsia_io_NodeSetAttr_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static zx_status_t fidl_node_setflags(void* ctx, uint32_t flags, fidl_txn_t* txn) {
  return fuchsia_io_NodeSetAttr_reply(txn, ZX_ERR_NOT_SUPPORTED);
}

static const fuchsia_io_Node_ops_t kNodeOps = {
    .Clone = fidl_node_clone,
    .Close = fidl_node_close,
    .Describe = fidl_node_describe,
    .Sync = fidl_node_sync,
    .GetAttr = fidl_node_getattr,
    .SetAttr = fidl_node_setattr,
    .NodeGetFlags = fidl_node_getflags,
    .NodeSetFlags = fidl_node_setflags,
};

void DevfsConnection::Bind(::fidl::StringView driver, BindCompleter::Sync completer) {
  char drv_libname[fuchsia_device_MAX_DRIVER_PATH_LEN + 1];
  ZX_ASSERT(driver.size() <= sizeof(drv_libname));
  memcpy(drv_libname, driver.data(), driver.size());
  drv_libname[driver.size()] = 0;

  zx_status_t status = device_bind(this->dev, drv_libname);
  if (status != ZX_OK) {
    return completer.Reply(status);
  }

  this->dev->set_bind_conn(completer.ToAsync());
}

void DevfsConnection::ScheduleUnbind(ScheduleUnbindCompleter::Sync completer) {
  zx_status_t status = device_schedule_remove(this->dev, true /* unbind_self */);
  return completer.Reply(status);
}

void DevfsConnection::GetDriverName(GetDriverNameCompleter::Sync completer) {
  if (!this->dev->driver) {
    return completer.Reply(ZX_ERR_NOT_SUPPORTED, {});
  }
  const char* name = this->dev->driver->name();
  if (name == nullptr) {
    name = "unknown";
  }
  completer.Reply(ZX_OK, {name, strlen(name)});
}

void DevfsConnection::GetDeviceName(GetDeviceNameCompleter::Sync completer) {
  completer.Reply({this->dev->name, strlen(this->dev->name)});
}

void DevfsConnection::GetTopologicalPath(GetTopologicalPathCompleter::Sync completer) {
  char buf[fuchsia_device_MAX_DEVICE_PATH_LEN + 1];
  size_t actual;
  zx_status_t status = devhost_get_topo_path(this->dev, buf, sizeof(buf), &actual);
  if (status != ZX_OK) {
    return completer.Reply(status, {});
  }
  if (actual > 0) {
    // Remove the accounting for the null byte
    actual--;
  }
  completer.Reply(ZX_OK, {buf, actual});
}

void DevfsConnection::GetEventHandle(GetEventHandleCompleter::Sync completer) {
  zx::eventpair event;
  zx_status_t status = this->dev->event.duplicate(ZX_RIGHTS_BASIC, &event);
  static_assert(fuchsia_device_DEVICE_SIGNAL_READABLE == DEV_STATE_READABLE);
  static_assert(fuchsia_device_DEVICE_SIGNAL_WRITABLE == DEV_STATE_WRITABLE);
  static_assert(fuchsia_device_DEVICE_SIGNAL_ERROR == DEV_STATE_ERROR);
  static_assert(fuchsia_device_DEVICE_SIGNAL_HANGUP == DEV_STATE_HANGUP);
  static_assert(fuchsia_device_DEVICE_SIGNAL_OOB == DEV_STATE_OOB);
  // TODO(teisenbe): The FIDL definition erroneously describes this as an event rather than
  // eventpair.
  completer.Reply(status, zx::event(event.release()));
}

void DevfsConnection::GetDriverLogFlags(GetDriverLogFlagsCompleter::Sync completer) {
  if (!this->dev->driver) {
    return completer.Reply(ZX_ERR_UNAVAILABLE, 0);
  }
  uint32_t flags = this->dev->driver->driver_rec()->log_flags;
  completer.Reply(ZX_OK, flags);
}

void DevfsConnection::SetDriverLogFlags(uint32_t clear_flags, uint32_t set_flags,
                                        SetDriverLogFlagsCompleter::Sync completer) {
  if (!this->dev->driver) {
    return completer.Reply(ZX_ERR_UNAVAILABLE);
  }
  uint32_t flags = this->dev->driver->driver_rec()->log_flags;
  flags &= ~clear_flags;
  flags |= set_flags;
  this->dev->driver->driver_rec()->log_flags = flags;
  completer.Reply(ZX_OK);
}

void DevfsConnection::DebugSuspend(DebugSuspendCompleter::Sync completer) {
  completer.Reply(this->dev->SuspendOp(0));
}

void DevfsConnection::DebugResume(DebugResumeCompleter::Sync completer) {
  completer.Reply(this->dev->ResumeOp(0));
}

void DevfsConnection::RunCompatibilityTests(int64_t hook_wait_time,
                                            RunCompatibilityTestsCompleter::Sync completer) {
  this->dev->PushTestCompatibilityConn(completer.ToAsync());
  device_run_compatibility_tests(this->dev, hook_wait_time);
}

void DevfsConnection::GetDevicePowerCaps(GetDevicePowerCapsCompleter::Sync completer) {
  // For now, the result is always a successful response because the device itself is not added
  // without power states validation. In future, we may add more checks for validation, and the
  // error result will be put to use.
  ::llcpp::fuchsia::device::Controller_GetDevicePowerCaps_Response response{};
  auto& states = this->dev->GetPowerStates();

  ZX_DEBUG_ASSERT(states.size() == fuchsia_device_MAX_DEVICE_POWER_STATES);
  for (size_t i = 0; i < fuchsia_device_MAX_DEVICE_POWER_STATES; i++) {
    response.dpstates[i] = states[i];
  }
  completer.Reply(::llcpp::fuchsia::device::Controller_GetDevicePowerCaps_Result::WithResponse(
      std::move(response)));
}

void DevfsConnection::UpdatePowerStateMapping(
    ::fidl::Array<::llcpp::fuchsia::device::SystemPowerStateInfo,
                  ::llcpp::fuchsia::device::manager::MAX_SYSTEM_POWER_STATES>
        mapping,
    UpdatePowerStateMappingCompleter::Sync completer) {
  ::llcpp::fuchsia::device::Controller_UpdatePowerStateMapping_Response response;

  std::array<fuchsia_device_SystemPowerStateInfo, fuchsia_device_manager_MAX_SYSTEM_POWER_STATES>
      states_mapping;
  for (size_t i = 0; i < fuchsia_device_manager_MAX_SYSTEM_POWER_STATES; i++) {
    states_mapping[i] = mapping[i];
  }
  zx_status_t status = this->dev->SetSystemPowerStateMapping(states_mapping);
  if (status != ZX_OK) {
    return completer.Reply(
        ::llcpp::fuchsia::device::Controller_UpdatePowerStateMapping_Result::WithErr(
            std::move(status)));
  }
  completer.Reply(
      ::llcpp::fuchsia::device::Controller_UpdatePowerStateMapping_Result::WithResult());
}

void DevfsConnection::GetPowerStateMapping(GetPowerStateMappingCompleter::Sync completer) {
  ::llcpp::fuchsia::device::Controller_GetPowerStateMapping_Response response;

  auto& mapping = this->dev->GetSystemPowerStateMapping();
  ZX_DEBUG_ASSERT(mapping.size() == fuchsia_device_manager_MAX_SYSTEM_POWER_STATES);

  for (size_t i = 0; i < fuchsia_device_manager_MAX_SYSTEM_POWER_STATES; i++) {
    response.mapping[i] = mapping[i];
  }
  completer.Reply(::llcpp::fuchsia::device_Controller::GetPowerStateMapping_Result::WithResult(
      std::move(response)));
}

void DevfsConnection::Suspend(::llcpp::fuchsia::device::DevicePowerState requested_state,
                              SuspendCompleter::Sync completer) {
  fuchsia_device_DevicePowerState out_state;
  zx_status_t status = devhost_device_suspend_new(this->dev, requested_state, &out_state);
  completer.Reply(status, out_state);
}

void DevfsConnection::Resume(::llcpp::fuchsia::device::DevicePowerState requested_state,
                             ResumeCompleter::Sync completer) {
  ::llcpp::fuchsia::device::DevicePowerState out_state;
  zx_status_t status = devhost_device_resume_new(this->dev, requested_state, &out_state);
  if (status != ZX_OK) {
    return completer.Reply(
        ::llcpp::fuchsia::device::Controller_Resume_Result::WithErr(std::move(status)));
  }

  ::llcpp::fuchsia::device::Controller_Resume_Response response;
  response.out_state = out_state;
  completer.Reply(
      ::llcpp::fuchsia::device::Controller_Resume_Result::WithResponse(std::move(response)));
}

zx_status_t devhost_fidl_handler(fidl_msg_t* msg, fidl_txn_t* txn, void* cookie) {
  auto conn = static_cast<DevfsConnection*>(cookie);
  zx_status_t status = fuchsia_io_Node_try_dispatch(cookie, txn, msg, &kNodeOps);
  if (status != ZX_ERR_NOT_SUPPORTED) {
    return status;
  }
  status = fuchsia_io_File_try_dispatch(cookie, txn, msg, &kFileOps);
  if (status != ZX_ERR_NOT_SUPPORTED) {
    return status;
  }
  status = fuchsia_io_Directory_try_dispatch(cookie, txn, msg, &kDirectoryOps);
  if (status != ZX_ERR_NOT_SUPPORTED) {
    return status;
  }
  status = fuchsia_io_DirectoryAdmin_try_dispatch(cookie, txn, msg, &kDirectoryAdminOps);
  if (status != ZX_ERR_NOT_SUPPORTED) {
    return status;
  }
  status = ::llcpp::fuchsia::device::Controller::TryDispatch(conn, msg, txn);
  if (status != ZX_ERR_NOT_SUPPORTED) {
    return status;
  }

  return conn->dev->MessageOp(msg, txn);
}

}  // namespace devmgr
