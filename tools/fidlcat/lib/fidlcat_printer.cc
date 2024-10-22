// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/fidlcat/lib/fidlcat_printer.h"

#include "src/lib/fidl_codec/display_handle.h"
#include "tools/fidlcat/lib/event.h"
#include "tools/fidlcat/lib/syscall_decoder_dispatcher.h"

namespace fidlcat {

FidlcatPrinter::FidlcatPrinter(SyscallDisplayDispatcher* dispatcher, uint64_t process_id,
                               std::ostream& os, std::string_view line_header, int tabulations)
    : PrettyPrinter(os, dispatcher->colors(),
                    dispatcher->message_decoder_dispatcher().display_options().pretty_print,
                    line_header, dispatcher->columns(), dispatcher->with_process_info(),
                    tabulations),
      inference_(dispatcher->inference()),
      process_id_(process_id),
      display_stack_frame_(dispatcher->decode_options().stack_level != kNoStack),
      dump_messages_(dispatcher->dump_messages()) {}

void FidlcatPrinter::DisplayHandle(const zx_handle_info_t& handle) {
  const fidl_codec::semantic::HandleDescription* known_handle =
      inference_.GetHandleDescription(process_id_, handle.handle);
  if ((handle.type == ZX_OBJ_TYPE_NONE) && (known_handle != nullptr)) {
    zx_handle_info_t tmp = handle;
    tmp.type = known_handle->object_type();
    fidl_codec::DisplayHandle(tmp, *this);
  } else {
    fidl_codec::DisplayHandle(handle, *this);
  }
  if (known_handle != nullptr) {
    (*this) << '(';
    known_handle->Display(*this);
    (*this) << ')';
  }
}

void FidlcatPrinter::DisplayStatus(zx_status_t status) {
  if (status == ZX_OK) {
    (*this) << fidl_codec::Green;
  } else {
    (*this) << fidl_codec::Red;
  }
  (*this) << fidl_codec::StatusName(status) << fidl_codec::ResetColor;
}

void FidlcatPrinter::DisplayInline(
    const std::vector<std::unique_ptr<fidl_codec::StructMember>>& members,
    const std::map<const fidl_codec::StructMember*, std::unique_ptr<fidl_codec::Value>>& values) {
  (*this) << '(';
  const char* separator = "";
  for (const auto& member : members) {
    auto it = values.find(member.get());
    if (it == values.end())
      continue;
    (*this) << separator << member->name() << ":" << fidl_codec::Green << member->type()->Name()
            << fidl_codec::ResetColor << ": ";
    it->second->PrettyPrint(member->type(), *this);
    separator = ", ";
  }
  (*this) << ")";
}

void FidlcatPrinter::DisplayOutline(
    const std::vector<std::unique_ptr<fidl_codec::StructMember>>& members,
    const std::map<const fidl_codec::StructMember*, std::unique_ptr<fidl_codec::Value>>& values) {
  fidl_codec::Indent indent(*this);
  for (const auto& member : members) {
    auto it = values.find(member.get());
    if (it == values.end())
      continue;
    auto fidl_message_value = it->second->AsFidlMessageValue();
    if (fidl_message_value != nullptr) {
      it->second->PrettyPrint(member->type(), *this);
    } else {
      (*this) << member->name() << ":" << fidl_codec::Green << member->type()->Name()
              << fidl_codec::ResetColor << ": ";
      it->second->PrettyPrint(member->type(), *this);
      (*this) << '\n';
    }
  }
}

void FidlcatPrinter::DisplayStackFrame(const std::vector<Location>& stack_frame) {
  bool save_header_on_every_line = header_on_every_line();
  // We want a header on every stack frame line.
  set_header_on_every_line(true);
  for (const auto& location : stack_frame) {
    *this << fidl_codec::YellowBackground << "at " << fidl_codec::Red;
    if (!location.path().empty()) {
      *this << location.path() << fidl_codec::ResetColor << fidl_codec::YellowBackground << ':'
            << fidl_codec::Blue << location.line() << ':' << location.column()
            << fidl_codec::ResetColor;
    } else {
      *this << std::hex << location.address() << fidl_codec::ResetColor << std::dec;
    }
    if (!location.symbol().empty()) {
      *this << ' ' << location.symbol();
    }
    *this << '\n';
  }
  set_header_on_every_line(save_header_on_every_line);
}

}  // namespace fidlcat
