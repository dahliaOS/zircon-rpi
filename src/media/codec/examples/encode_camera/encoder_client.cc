// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/codec/examples/encode_camera/encoder_client.h"

#include <lib/async/cpp/task.h>

#include <algorithm>
#include <iostream>
#include <random>

namespace {
constexpr uint64_t kInputBufferLifetimeOrdinal = 1;
constexpr uint32_t kMinOutputBufferSize = 100 * 4096;
constexpr uint32_t kMinOutputPacketsForClient = 1;
constexpr uint32_t kMinOutputBufferCount = 1;
}  // namespace

static void FatalError(std::string message) {
  std::cerr << message << std::endl;
  abort();
}

// Sets the error handler on the provided interface to log an error and abort the process.
template <class T>
static void SetAbortOnError(fidl::InterfacePtr<T>& p, std::string message) {
  p.set_error_handler([message](zx_status_t status) { FatalError(message); });
}

fit::result<std::unique_ptr<EncoderClient>, zx_status_t> EncoderClient::Create(
    fuchsia::mediacodec::CodecFactoryHandle codec_factory,
    fuchsia::sysmem::AllocatorHandle allocator, uint32_t bitrate, uint32_t gop_size) {
  auto encoder = std::unique_ptr<EncoderClient>(new EncoderClient(bitrate, gop_size));
  zx_status_t status = encoder->codec_factory_.Bind(std::move(codec_factory));
  if (status != ZX_OK) {
    return fit::error(status);
  }

  status = encoder->sysmem_.Bind(std::move(allocator));
  if (status != ZX_OK) {
    return fit::error(status);
  }

  return fit::ok(std::move(encoder));
}

EncoderClient::EncoderClient(uint32_t bitrate, uint32_t gop_size)
    : bitrate_(bitrate), gop_size_(gop_size) {
  SetAbortOnError(codec_factory_, "fuchsia.mediacodec.CodecFactory disconnected.");
  SetAbortOnError(sysmem_, "fuchsia.sysmem.Allocator disconnected.");
  SetAbortOnError(codec_, "fuchsia.media.StreamProcessor disconnected.");
  SetAbortOnError(input_buffer_collection_, "fuchsia.sysmem.BufferCollection input disconnected.");
  SetAbortOnError(output_buffer_collection_,
                  "fuchsia.sysmem.BufferCollection output disconnected.");

  codec_.events().OnStreamFailed = fit::bind_member(this, &EncoderClient::OnStreamFailed);
  codec_.events().OnInputConstraints = fit::bind_member(this, &EncoderClient::OnInputConstraints);
  codec_.events().OnFreeInputPacket = fit::bind_member(this, &EncoderClient::OnFreeInputPacket);
  codec_.events().OnOutputConstraints = fit::bind_member(this, &EncoderClient::OnOutputConstraints);
  codec_.events().OnOutputFormat = fit::bind_member(this, &EncoderClient::OnOutputFormat);
  codec_.events().OnOutputPacket = fit::bind_member(this, &EncoderClient::OnOutputPacket);
  codec_.events().OnOutputEndOfStream = fit::bind_member(this, &EncoderClient::OnOutputEndOfStream);
}

EncoderClient::~EncoderClient() {}

zx_status_t EncoderClient::Start(fuchsia::sysmem::BufferCollectionTokenHandle token,
                                 fuchsia::sysmem::ImageFormat_2 image_format, uint32_t framerate) {
  if (image_format.pixel_format.type != fuchsia::sysmem::PixelFormatType::NV12) {
    std::cout << "Unsupported pixel format" << std::endl;
    return ZX_ERR_INVALID_ARGS;
  }

  std::cout << "Starting encoder at frame rate " << framerate << std::endl;

  fuchsia::media::VideoUncompressedFormat uncompressed;
  uncompressed.image_format = image_format;

  fuchsia::media::VideoFormat video_format;
  video_format.set_uncompressed(uncompressed);

  fuchsia::media::DomainFormat domain;
  domain.set_video(std::move(video_format));

  fuchsia::media::H264EncoderSettings h264_settings;
  h264_settings.set_bit_rate(bitrate_);
  h264_settings.set_frame_rate(framerate);
  h264_settings.set_gop_size(gop_size_);

  fuchsia::media::EncoderSettings encoder_settings;
  encoder_settings.set_h264(std::move(h264_settings));

  fuchsia::media::FormatDetails input_details;
  const char* mime_type = "video/h264";
  input_details.set_format_details_version_ordinal(0)
      .set_mime_type(mime_type)
      .set_encoder_settings(std::move(encoder_settings))
      .set_domain(std::move(domain));

  fuchsia::mediacodec::CreateEncoder_Params encoder_params;
  encoder_params.set_input_details(std::move(input_details));

  auto codec_request = codec_.NewRequest();
  codec_factory_->CreateEncoder(std::move(encoder_params), std::move(codec_request));

  input_buffers_token_ = std::move(token);

  return ZX_OK;
}

void EncoderClient::BindAndSyncBufferCollection(
    fuchsia::sysmem::BufferCollectionPtr& buffer_collection,
    fuchsia::sysmem::BufferCollectionTokenHandle token,
    fuchsia::sysmem::BufferCollectionTokenHandle duplicated_token,
    BoundBufferCollectionCallback callback) {
  auto buffer_collection_request = buffer_collection.NewRequest();
  sysmem_->BindSharedCollection(std::move(token), std::move(buffer_collection_request));

  // After Sync() completes its round trip, we know that sysmem knows about
  // duplicated_token (causally), which is important because we'll shortly
  // send duplicated_token to the codec which will use duplicated_token via
  // a different sysmem channel.
  buffer_collection->Sync(
      [duplicated_token = std::move(duplicated_token), callback = std::move(callback)]() mutable {
        callback(std::move(duplicated_token));
      });
}

// Duplicate passed in token to buffer collection, then bind and sync on it, passing back the
// logical buffer collection and the duplicated token to pass to the next client (the encoder)
void EncoderClient::BindAndSyncBufferCollectionToken(
    fuchsia::sysmem::BufferCollectionPtr& buffer_collection,
    fuchsia::sysmem::BufferCollectionTokenHandle token, BoundBufferCollectionCallback callback) {
  fuchsia::sysmem::BufferCollectionTokenHandle codec_sysmem_token;

  // Bind the passed in client token
  fuchsia::sysmem::BufferCollectionTokenPtr client_token = token.Bind();

  client_token->Duplicate(std::numeric_limits<uint32_t>::max(), codec_sysmem_token.NewRequest());

  // client_token gets converted into a buffer_collection.
  //
  // Start client_token connection and start converting it into a
  // BufferCollection, so we can Sync() the previous Duplicate().
  token = client_token.Unbind();
  BindAndSyncBufferCollection(buffer_collection, std::move(token), std::move(codec_sysmem_token),
                              std::move(callback));
}

void EncoderClient::CreateAndSyncBufferCollection(
    fuchsia::sysmem::BufferCollectionPtr& buffer_collection,
    BoundBufferCollectionCallback callback) {
  fuchsia::sysmem::BufferCollectionTokenHandle codec_sysmem_token;

  // Create client_token which will get converted into out_buffer_collection.
  fuchsia::sysmem::BufferCollectionTokenPtr client_token;
  fidl::InterfaceRequest<fuchsia::sysmem::BufferCollectionToken> client_token_request =
      client_token.NewRequest();

  client_token->Duplicate(std::numeric_limits<uint32_t>::max(), codec_sysmem_token.NewRequest());

  // client_token gets converted into a buffer_collection.
  //
  // Start client_token connection and start converting it into a
  // BufferCollection, so we can Sync() the previous Duplicate().
  sysmem_->AllocateSharedCollection(std::move(client_token_request));

  auto token = client_token.Unbind();
  BindAndSyncBufferCollection(buffer_collection, std::move(token), std::move(codec_sysmem_token),
                              std::move(callback));
}

void EncoderClient::OnInputConstraints(fuchsia::media::StreamBufferConstraints input_constraints) {
  input_constraints_.emplace(std::move(input_constraints));
  BindAndSyncBufferCollectionToken(
      input_buffer_collection_, std::move(input_buffers_token_),
      [this](fuchsia::sysmem::BufferCollectionTokenHandle codec_sysmem_token) mutable {
        constexpr uint32_t kMinInputPacketsForClient = 1;

        uint32_t packet_count_for_client =
            std::max(kMinInputPacketsForClient, input_constraints_->packet_count_for_client_min());
        uint32_t packet_count_for_server = input_constraints_->packet_count_for_server_min();
        ConfigurePortBufferCollection(
            input_buffer_collection_, std::move(codec_sysmem_token), false,
            kInputBufferLifetimeOrdinal, input_constraints_->buffer_constraints_version_ordinal(),
            packet_count_for_server, packet_count_for_client,
            [this](auto result) { OnInputBuffersReady(std::move(result)); });
      });
}

void EncoderClient::OnInputBuffersReady(
    fit::result<std::pair<fuchsia::sysmem::BufferCollectionInfo_2, uint32_t>, zx_status_t> result) {
  if (result.is_error()) {
    FatalError("failed to get input buffers");
    return;
  }

  auto ready = result.take_value();
  auto buffer_collection_info = std::move(ready.first);
  input_packet_count_ = ready.second;

  all_input_buffers_.reserve(buffer_collection_info.buffer_count);
  for (uint32_t i = 0; i < buffer_collection_info.buffer_count; i++) {
    std::unique_ptr<CodecBuffer> local_buffer = CodecBuffer::CreateFromVmo(
        i, std::move(buffer_collection_info.buffers[i].vmo),
        buffer_collection_info.buffers[i].vmo_usable_start,
        buffer_collection_info.settings.buffer_settings.size_bytes, true,
        buffer_collection_info.settings.buffer_settings.is_physically_contiguous);
    if (!local_buffer) {
      FatalError("CodecBuffer::CreateFromVmo() failed");
    }
    ZX_ASSERT(all_input_buffers_.size() == i);
    all_input_buffers_.push_back(std::move(local_buffer));
  }
}

void EncoderClient::OnFreeInputPacket(fuchsia::media::PacketHeader free_input_packet) {
  if (!free_input_packet.has_packet_index()) {
    FatalError("OnFreeInputPacket(): Packet has no index.");
  }

  // drop packet release fence
  input_packets_queued_.erase(free_input_packet.packet_index());
}

void EncoderClient::QueueInputPacket(uint32_t buffer_index, zx::eventpair release_fence) {
  fuchsia::media::Packet packet;

  packet.set_stream_lifetime_ordinal(kInputBufferLifetimeOrdinal);
  packet.mutable_header()->set_buffer_lifetime_ordinal(kInputBufferLifetimeOrdinal);
  packet.mutable_header()->set_packet_index(buffer_index);
  packet.set_start_offset(0);
  packet.set_valid_length_bytes(all_input_buffers_[buffer_index]->size_bytes());
  packet.set_buffer_index(buffer_index);

  input_packets_queued_.insert({buffer_index, std::move(release_fence)});

  codec_->QueueInputPacket(std::move(packet));
}

void EncoderClient::ConfigurePortBufferCollection(
    fuchsia::sysmem::BufferCollectionPtr& buffer_collection,
    fuchsia::sysmem::BufferCollectionTokenHandle token, bool is_output,
    uint64_t new_buffer_lifetime_ordinal, uint64_t buffer_constraints_version_ordinal,
    uint32_t packet_count_for_server, uint32_t packet_count_for_client,
    ConfigurePortBufferCollectionCallback callback) {
  uint32_t packet_count = packet_count_for_server + packet_count_for_client;

  fuchsia::media::StreamBufferPartialSettings settings;
  settings.set_buffer_lifetime_ordinal(new_buffer_lifetime_ordinal);
  settings.set_buffer_constraints_version_ordinal(buffer_constraints_version_ordinal);
  settings.set_single_buffer_mode(false);
  settings.set_packet_count_for_server(packet_count_for_server);
  settings.set_packet_count_for_client(packet_count_for_client);

  settings.set_sysmem_token(std::move(token));

  fuchsia::sysmem::BufferCollectionConstraints constraints;
  constraints.usage.cpu = fuchsia::sysmem::cpuUsageReadOften | fuchsia::sysmem::cpuUsageWriteOften;
  constraints.min_buffer_count_for_camping = packet_count_for_client;

  if (is_output) {
    constraints.has_buffer_memory_constraints = true;
    constraints.buffer_memory_constraints.min_size_bytes = kMinOutputBufferSize;
    constraints.min_buffer_count = kMinOutputBufferCount;
  }

  if (is_output) {
    codec_->SetOutputBufferPartialSettings(std::move(settings));
  } else {
    codec_->SetInputBufferPartialSettings(std::move(settings));
  }

  buffer_collection->SetConstraints(true, std::move(constraints));

  buffer_collection->WaitForBuffersAllocated(
      [packet_count, callback = std::move(callback)](
          zx_status_t allocate_status,
          fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection_info) mutable {
        if (allocate_status != ZX_OK) {
          callback(fit::error(allocate_status));
          return;
        }

        auto negotiated_packet_count = std::max(packet_count, buffer_collection_info.buffer_count);
        callback(fit::ok(std::pair(std::move(buffer_collection_info), negotiated_packet_count)));
      });
}

void EncoderClient::OnOutputConstraints(
    fuchsia::media::StreamOutputConstraints output_constraints) {
  if (!output_constraints.has_stream_lifetime_ordinal()) {
    FatalError("StreamOutputConstraints missing stream_lifetime_ordinal");
  }
  last_output_constraints_.emplace(std::move(output_constraints));

  // Free the old output buffers, if any.
  all_output_buffers_.clear();

  auto new_output_buffer_lifetime_ordinal = next_output_buffer_lifetime_ordinal_;
  next_output_buffer_lifetime_ordinal_ += 2;

  CreateAndSyncBufferCollection(
      output_buffer_collection_,
      [this, new_output_buffer_lifetime_ordinal](
          fuchsia::sysmem::BufferCollectionTokenHandle codec_sysmem_token) {
        //
        // Tell the server about output settings.
        //
        const fuchsia::media::StreamBufferConstraints& buffer_constraints =
            last_output_constraints_->buffer_constraints();
        ZX_ASSERT(buffer_constraints.has_packet_count_for_server_min());
        ZX_ASSERT(buffer_constraints.has_packet_count_for_server_recommended());
        // Use min; if decode gets stuck using min, we want to notice that.
        uint32_t packet_count_for_server = buffer_constraints.packet_count_for_server_min();
        uint32_t packet_count_for_client =
            std::max(kMinOutputPacketsForClient, buffer_constraints.packet_count_for_client_min());
        if (packet_count_for_client > buffer_constraints.packet_count_for_client_max()) {
          FatalError(
              "server can't accomodate "
              "kMinExtraOutputPacketsForClient - not "
              "using server - exiting");
        }

        current_output_buffer_lifetime_ordinal_ = new_output_buffer_lifetime_ordinal;

        ConfigurePortBufferCollection(
            output_buffer_collection_, std::move(codec_sysmem_token), true,
            new_output_buffer_lifetime_ordinal,
            buffer_constraints.buffer_constraints_version_ordinal(), packet_count_for_server,
            packet_count_for_client,
            [this](auto result) { OnOutputBuffersReady(std::move(result)); });
      });
}

void EncoderClient::OnOutputBuffersReady(
    fit::result<std::pair<fuchsia::sysmem::BufferCollectionInfo_2, uint32_t>, zx_status_t> result) {
  if (result.is_error()) {
    FatalError("Failed to get output buffers");
    return;
  }

  auto ready = result.take_value();
  auto buffer_collection_info = std::move(ready.first);
  output_packet_count_ = ready.second;

  all_output_buffers_.reserve(output_packet_count_);
  for (uint32_t i = 0; i < output_packet_count_; i++) {
    std::unique_ptr<CodecBuffer> buffer = CodecBuffer::CreateFromVmo(
        i, std::move(buffer_collection_info.buffers[i].vmo),
        buffer_collection_info.buffers[i].vmo_usable_start,
        buffer_collection_info.settings.buffer_settings.size_bytes, true,
        buffer_collection_info.settings.buffer_settings.is_physically_contiguous);
    if (!buffer) {
      FatalError("CodecBuffer::Allocate() failed (output)");
    }
    ZX_ASSERT(all_output_buffers_.size() == i);
    all_output_buffers_.push_back(std::move(buffer));
  }

  codec_->CompleteOutputBufferPartialSettings(current_output_buffer_lifetime_ordinal_);
}

void EncoderClient::OnOutputFormat(fuchsia::media::StreamOutputFormat output_format) {}

void EncoderClient::OnOutputPacket(fuchsia::media::Packet output_packet, bool error_detected_before,
                                   bool error_detected_during) {
  output_packet_handler_(all_output_buffers_[output_packet.buffer_index()]->base(),
                         output_packet.valid_length_bytes());

  codec_->RecycleOutputPacket(fidl::Clone(output_packet.header()));
}

void EncoderClient::OnOutputEndOfStream(uint64_t stream_lifetime_ordinal,
                                        bool error_detected_before) {
  std::cout << "end of stream" << std::endl;
}

void EncoderClient::OnStreamFailed(uint64_t stream_lifetime_ordinal,
                                   fuchsia::media::StreamError error) {
  std::cout << "stream_lifetime_ordinal: " << stream_lifetime_ordinal << " error: " << std::hex
            << static_cast<uint32_t>(error);
  FatalError("OnStreamFailed");
}
