// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <fcntl.h>
#include <fuchsia/hardware/midi/llcpp/fidl.h>
#include <lib/fdio/unsafe.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/types.h>

#include <fbl/algorithm.h>

#define DEV_MIDI "/dev/class/midi"

namespace midi = llcpp::fuchsia::hardware::midi;

static bool open_devices(int* out_src_fd, int* out_dest_fd) {
  int src_fd = -1;
  int dest_fd = -1;

  struct dirent* de;
  DIR* dir = opendir(DEV_MIDI);
  if (!dir) {
    printf("Error opening %s\n", DEV_MIDI);
    return -1;
  }

  while ((de = readdir(dir)) != NULL && (src_fd == -1 || dest_fd == -1)) {
    char devname[128];

    snprintf(devname, sizeof(devname), "%s/%s", DEV_MIDI, de->d_name);
    int fd = open(devname, O_RDWR);
    if (fd < 0) {
      printf("Error opening %s\n", devname);
      continue;
    }

    fdio_t* fdio = fdio_unsafe_fd_to_io(fd);
    midi::Device::SyncClient client(zx::channel(fdio_unsafe_borrow_channel(fdio)));

    auto direction = client.GetDirection();
    fdio_unsafe_release(fdio);
    if (direction.status() != ZX_OK) {
      printf("fuchsia.hardware.midi.Device/GetInfo failed for %s\n", devname);
      close(fd);
      continue;
    }
    if (direction.value().direction == midi::Direction::SOURCE) {
      if (src_fd == -1) {
        src_fd = fd;
      } else {
        close(fd);
      }
    } else if (direction.value().direction == midi::Direction::SINK) {
      if (dest_fd == -1) {
        dest_fd = fd;
      } else {
        close(fd);
      }
    } else {
      close(fd);
    }
  }

  closedir(dir);
  if (src_fd == -1) {
    close(dest_fd);
    return false;
  }
  if (dest_fd == -1) {
    close(src_fd);
    return false;
  }

  *out_src_fd = src_fd;
  *out_dest_fd = dest_fd;
  return true;
}

int main(int argc, char** argv) {
  int src_fd = -1, dest_fd = -1;
  if (!open_devices(&src_fd, &dest_fd)) {
    printf("couldn't find a usable MIDI source and sink\n");
    return -1;
  }

  auto* src_fdio = fdio_unsafe_fd_to_io(src_fd);
  auto* dest_fdio = fdio_unsafe_fd_to_io(dest_fd);
  /*
      while (1) {
          uint8_t request_buffer[fidl::MaxSizeInChannel<midi::Device::ReadRequest>()] = {};
          uint8_t response_buffer[fidl::MaxSizeInChannel<midi::Device::ReadResponse>()] = {};

          fidl::DecodedMessage<midi::Device::ReadRequest>
  request(fidl::BytePart::WrapFull(request_buffer)); request.message()->count = 3; auto result =
  midi::Device::Call::Read(zx::unowned_channel(fdio_unsafe_borrow_channel(src_fdio)),
  std::move(request), fidl::BytePart::WrapEmpty(response_buffer)); midi::Device::ReadResponse*
  response = result.Unwrap(); auto read_response = response->result.response();

          auto data = read_response.data.data();
          auto length = read_response.data.count();
          if (length < 0) break;
          printf("MIDI event:");
          for (size_t i = 0; i < length; i++) {
              printf(" %02X", data[i]);
          }
          printf("\n");


  //        if (write(dest_fd, buffer, length) < 0) break;
      }
  */
  fdio_unsafe_release(src_fdio);
  fdio_unsafe_release(dest_fdio);
  close(src_fd);
  close(dest_fd);

  return 0;
}
