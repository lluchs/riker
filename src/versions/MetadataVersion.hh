#pragma once

#include <filesystem>
#include <memory>
#include <ostream>

#include <sys/stat.h>
#include <sys/types.h>

#include "ui/stats.hh"
#include "util/serializer.hh"

namespace fs = std::filesystem;

class AccessFlags;
class Artifact;
class Command;

class MetadataVersion {
 public:
  /// Create a new metadata version
  MetadataVersion(uid_t uid, gid_t gid, mode_t mode) noexcept : _uid(uid), _gid(gid), _mode(mode) {
    stats::versions++;
  }

  /// Create a new metadata version from a stat struct
  MetadataVersion(const struct stat& data) noexcept :
      MetadataVersion(data.st_uid, data.st_gid, data.st_mode) {}

  /// Create a new metadata version by changing the owner and/or group in this one
  std::shared_ptr<MetadataVersion> chown(uid_t user, gid_t group) noexcept;

  /// Create a new metadata version by changing the mode bits in this one
  std::shared_ptr<MetadataVersion> chmod(mode_t mode) noexcept;

  /// Check if a given access is allowed by the mode bits in this metadata record
  bool checkAccess(std::shared_ptr<Artifact> artifact, AccessFlags flags) noexcept;

  /// Get the mode field from this metadata version
  mode_t getMode() const noexcept;

  /// Commit this version to the filesystem
  void commit(fs::path path) noexcept;

  /// Compare this version to another version
  bool matches(std::shared_ptr<MetadataVersion> other) const noexcept;

  /// Print this metadata version
  std::ostream& print(std::ostream& o) const noexcept;

  /// Print a Version
  friend std::ostream& operator<<(std::ostream& o, const MetadataVersion& v) noexcept {
    return v.print(o);
  }

  /// Print a Version*
  friend std::ostream& operator<<(std::ostream& o, const MetadataVersion* v) noexcept {
    if (v == nullptr) return o << "<null MetadataVersion>";
    return v->print(o);
  }

 private:
  /// The command that created this version
  std::weak_ptr<Command> _creator;

  /// The user id for this metadata version
  uid_t _uid;

  /// The group id for this metadata version
  gid_t _gid;

  /// The file mode bits for this metadata version
  mode_t _mode;

  friend class cereal::access;
  MetadataVersion() noexcept = default;
  SERIALIZE(_uid, _gid, _mode);
};