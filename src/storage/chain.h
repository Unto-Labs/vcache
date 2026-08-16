// SPDX-License-Identifier: GPL-3.0-or-later
// Multi-level cache chain, following sccache's layering idea: a fast local
// layer in front of a shared remote one.
//
// Reads walk the chain in order and stop at the first hit. A hit found in a
// slower layer is written back into every faster layer it passed through, so a
// remote hit becomes a local hit for the rest of the build. Writes go to every
// writable layer.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "storage/storage.h"

namespace vcache::storage {

struct GetResult {
  bool hit = false;
  std::string value;
  std::string layer;  // name of the layer that served the hit
};

class CacheChain {
 public:
  void AddLayer(std::unique_ptr<Storage> layer);

  bool empty() const { return layers_.empty(); }
  size_t size() const { return layers_.size(); }

  GetResult Get(const std::string& key);

  // Writes to every writable layer. Returns true if at least one succeeded.
  bool Put(const std::string& key, const std::string& value);

  std::vector<std::string> LayerNames() const;

  void Trim();

 private:
  std::vector<std::unique_ptr<Storage>> layers_;
};

}  // namespace vcache::storage
