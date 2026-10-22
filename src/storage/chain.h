// SPDX-FileCopyrightText: 2026 Unto Labs
// SPDX-License-Identifier: Apache-2.0
// Multi-level cache chain, following sccache's layering idea: a fast local
// layer in front of a shared remote one.
//
// Reads walk the chain in order and stop at the first hit. A hit found in a
// slower layer is written back into every faster layer it passed through, so a
// remote hit becomes a local hit for the rest of the build. Writes go to every
// writable layer.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "storage/storage.h"

namespace vcache::storage {

struct GetResult {
  bool hit = false;
  std::string value;
  std::string layer;  // name of the layer that served the hit

  // One entry per layer that failed for a reason other than the entry being
  // absent, formatted "<layer>: <detail>". A miss contributes nothing here:
  // the point is to tell a cold cache apart from a broken one.
  std::vector<std::string> errors;
};

struct PutResult {
  bool stored = false;  // at least one writable layer took it
  std::vector<std::string> errors;
};

// A cache that answers whole-chain lookups and stores on the chain's behalf --
// in practice the daemon, which owns the layers itself. Unlike a Storage it
// reports which layer served a hit, because statistics are kept per layer.
//
// Each call returns false only when the remote could not be asked at all (the
// connection broke, the reply was malformed). A miss or a failed layer is a
// successful call with that outcome in the result.
class RemoteCache {
 public:
  virtual ~RemoteCache() = default;
  virtual std::string Name() const = 0;
  virtual bool Get(const std::string& key, GetResult* result) = 0;
  virtual bool Put(const std::string& key, const std::string& value,
                   PutResult* result) = 0;

  // As Get, but from the layers slower than `after`. Returning true with no
  // hit is an answer; false means the remote stopped answering.
  virtual bool GetAfter(const std::string& key, const std::string& after,
                        GetResult* result) = 0;
};

class CacheChain {
 public:
  void AddLayer(std::unique_ptr<Storage> layer);

  // Sends every lookup and store to `remote` instead of the local layers. If
  // the remote stops answering part-way through, `build_local` is called once
  // to populate the local layers and the chain carries on with those, so a
  // daemon that dies mid-build costs the daemon's benefits and nothing else.
  void SetRemote(std::unique_ptr<RemoteCache> remote,
                 std::function<void(CacheChain*)> build_local);
  bool remote() const { return remote_ != nullptr; }

  bool empty() const { return remote_ == nullptr && layers_.empty(); }
  size_t size() const { return layers_.size(); }

  GetResult Get(const std::string& key);

  // As Get, but in-process it asks only the local layers and backfills
  // nothing. A manifest store re-reads its key just before writing, to merge
  // what a concurrent store on this machine wrote; going to S3 for that would
  // add a round trip to every miss. A daemon chain is asked as usual: the
  // daemon answers from its own layers.
  GetResult GetLocal(const std::string& key);

  // As Get, but asks only the layers slower than `after` -- the layer an
  // earlier lookup of the same key answered from -- and backfills nothing.
  //
  // A manifest lookup uses it when no state in the copy it found still holds.
  // A faster layer's copy is not necessarily the newest: a disk cache baked
  // from an older tree keeps a manifest whose states all name headers that
  // have since changed, while S3 holds the state this tree records on every
  // build. Stopping at the first copy found misses on every one of them.
  GetResult GetAfter(const std::string& key, const std::string& after);

  // Writes only to the writable layers faster than `layer`, so a manifest
  // merged with a slower layer's copy is found first next time. A daemon
  // chain stores nothing here; the daemon owns its layers.
  PutResult PutBefore(const std::string& key, const std::string& value,
                      const std::string& layer);

  // Writes to every writable layer. Reports success if at least one took it,
  // and separately reports every layer that failed -- a store that reached
  // disk but not S3 is a success for the build and still a fault worth
  // surfacing, which a single bool cannot say.
  PutResult Put(const std::string& key, const std::string& value);

  std::vector<std::string> LayerNames() const;

  void Trim();

 private:
  // Drops the remote and builds the local layers in its place.
  void FallBackToLocal();

  std::vector<std::unique_ptr<Storage>> layers_;
  std::unique_ptr<RemoteCache> remote_;
  std::function<void(CacheChain*)> build_local_;
};

}  // namespace vcache::storage
