// Cache storage abstraction.
//
// A cache entry is a single opaque blob keyed by a hex digest, which keeps the
// backends trivial: disk writes one file, S3 writes one object. The structure
// inside the blob (object code, dependency file, replayed diagnostics) is the
// concern of core/compile.cc.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace vcache::storage {

class Storage {
 public:
  virtual ~Storage() = default;

  // Short name used in logs and statistics, e.g. "disk" or "s3".
  virtual std::string Name() const = 0;

  // Reads the blob for `key`. Returns false on miss and on any error; a
  // backend failure must look like a miss so the build still proceeds.
  virtual bool Get(const std::string& key, std::string* value) = 0;

  // Stores the blob. Returns false if it was not durably written.
  virtual bool Put(const std::string& key, const std::string& value) = 0;

  virtual bool writable() const { return true; }

  // Frees space according to the backend's own policy. No-op where the
  // backend has no local size budget.
  virtual void Trim() {}
};

// Container format for the blob. Sections are optional and order-independent,
// so older entries stay readable when new section kinds are added.
enum class SectionKind : uint8_t {
  kObject = 1,   // the compiled artifact (single-output C/C++ compile)
  kDepFile = 2,  // dependency file, with paths already canonicalised
  kStderr = 3,   // compiler diagnostics, replayed verbatim on a hit
  kMeta = 4,     // "key: value\n" lines describing how the entry was produced
  kFile = 5,     // one named output file; repeated once per file
  kDepManifest = 6,  // the file set a cached -M run depended on, with hashes
};

// One output file identified by its path relative to the output directory.
// rustc emits several artifacts per invocation (rlib, rmeta, dep-info) whose
// names depend on flags vcache would rather not have to predict, so the Rust
// path captures whatever appeared in the output directory instead.
struct BlobFile {
  std::string name;
  std::string contents;
  // rustc links cargo's build scripts and any binary crate into the output
  // directory, and cargo executes them straight away. Contents alone are not
  // enough to replay that, so the execute bit travels with the entry.
  bool executable = false;
};

struct Blob {
  std::string object;
  std::string depfile;
  std::string stderr_text;
  std::string meta;
  bool has_depfile = false;

  // For a cached dependency scan: one "<hex digest> <canonical path>" line per
  // file the scan read. A hit is only valid while every one of them still
  // hashes the same, since nothing else about the entry proves it.
  std::string dep_manifest;
  bool has_dep_manifest = false;

  std::vector<BlobFile> files;
};

std::string SerializeBlob(const Blob& blob);

// Returns false if the data is truncated, has a bad magic, or fails its
// checksum -- all of which are treated as a cache miss.
bool DeserializeBlob(const std::string& data, Blob* blob);

}  // namespace vcache::storage
