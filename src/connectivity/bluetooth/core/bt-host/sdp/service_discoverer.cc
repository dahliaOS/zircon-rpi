// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "service_discoverer.h"

#include <lib/async/default.h>

namespace bt {
namespace sdp {

ServiceDiscoverer::ServiceDiscoverer() : next_id_(1) {}

ServiceDiscoverer::SearchId ServiceDiscoverer::AddSearch(
    const UUID& uuid, std::unordered_set<AttributeId> attributes,
    ResultCallback callback) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  Search s;
  s.uuid = uuid;
  s.attributes = std::move(attributes);
  s.callback = std::move(callback);
  ZX_DEBUG_ASSERT(next_id_ <
                  std::numeric_limits<ServiceDiscoverer::SearchId>::max());
  ServiceDiscoverer::SearchId id = next_id_++;
  auto [it, placed] = searches_.emplace(id, std::move(s));
  ZX_DEBUG_ASSERT_MSG(placed, "Should always be able to place new search");
  return id;
}

bool ServiceDiscoverer::RemoveSearch(SearchId id) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  auto it = sessions_.begin();
  while (it != sessions_.end()) {
    auto &session = it->second;
    std::remove(session.queue.begin(), session.queue.end(), id);
    if (session.queue.empty()) {
      it = sessions_.erase(it);
    } else {
      it++;
    }
  }
  return searches_.erase(id);
}

bool ServiceDiscoverer::StartServiceDiscovery(PeerId peer_id,
                                              std::unique_ptr<Client> client) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  // If discovery is already happening on this peer, we can't start it again.
  if (sessions_.count(peer_id)) {
    bt_log(TRACE, "sdp", "Discovery for %s: in progress", bt_str(peer_id));
    return false;
  }
  // If there aren't any searches to do, we're done.
  if (searches_.empty()) {
    bt_log(TRACE, "sdp", "Discovery for %s: no searches", bt_str(peer_id));
    return true;
  }
  bt_log(TRACE, "sdp", "Discovery for %s: %zu searches", bt_str(peer_id), search_count());
  DiscoverySession session;
  session.client = std::move(client);
  for (auto& it : searches_) {
    session.queue.push_back(it.first);
  }
  sessions_.emplace(peer_id, std::move(session));
  ContinueSession(peer_id);
  return true;
}

size_t ServiceDiscoverer::search_count() const { return searches_.size(); }

void ServiceDiscoverer::ContinueSession(PeerId peer_id) {
  auto it = sessions_.find(peer_id);
  if (it == sessions_.end()) {
    bt_log(INFO, "sdp", "No session for %s to continue search", bt_str(peer_id));
    return;
  }
  auto& session = it->second;
  if (session.queue.empty()) {
    // This peer search is over.
    bt_log(TRACE, "sdp", "Discoverer completed for %s", bt_str(peer_id));
    sessions_.erase(it);
  } else {
    SearchId next = session.queue.front();
    session.queue.pop_front();
    auto search_it = searches_.find(next);
    ZX_DEBUG_ASSERT(search_it != searches_.end());
    Client::SearchResultCallback result_cb = [this, peer_id, search_id = next](
            auto status, const std::map<AttributeId, DataElement>& attributes) {
          auto it = searches_.find(search_id);
          if (it == searches_.end() || !status) {
            ContinueSession(peer_id);
            return false;
          }
          it->second.callback(peer_id, attributes);
          return true;
        };
    auto& search = search_it->second;
    session.client->ServiceSearchAttributes(
        {search.uuid}, search.attributes, std::move(result_cb),
        async_get_default_dispatcher());
  }
}

}  // namespace sdp
}  // namespace bt
