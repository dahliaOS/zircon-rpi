/// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_ULIB_CHUNKED_COMPRESSION_LOGGING_H_
#define ZIRCON_SYSTEM_ULIB_CHUNKED_COMPRESSION_LOGGING_H_

#ifdef __Fuchsia__

#include <lib/syslog/global.h>

#else

#include <stdio.h>

#define FX_LOG(severity, tag, message) FX_LOG_##severity(tag, message)
#define FX_LOGF(severity, tag, fmt, ...) FX_LOGF_##severity(tag, fmt, __VA_ARGS__)

#define FX_LOG_ERROR(tag, message) fprintf(stderr, "[%s] %s\n", tag, message)
#define FX_LOGF_ERROR(tag, fmt, ...) fprintf(stderr, "[%s] " fmt "\n", tag, __VA_ARGS__)
#define FX_LOG_INFO(tag, message) fprintf(stdout, "[%s] %s\n", tag, message)
#define FX_LOGF_INFO(tag, fmt, ...) fprintf(stdout, "[%s] " fmt "\n", tag, __VA_ARGS__)

#endif

namespace chunked_compression {

constexpr const char kLogTag[] = "compress";

}  // namespace chunked_compression

#endif  // ZIRCON_SYSTEM_ULIB_CHUNKED_COMPRESSION_LOGGING_H_
