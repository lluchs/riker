#include "Version.hh"

#include <memory>
#include <optional>

#include <sys/stat.h>
#include <sys/types.h>

#include "data/IR.hh"
#include "rebuild/Artifact.hh"
#include "ui/log.hh"

using std::dynamic_pointer_cast;
using std::nullopt;
using std::shared_ptr;

bool Version::isSaved() const {
  // Empty files can be recreated
  if (_metadata.has_value() && _metadata.value().st_size == 0) {
    return true;
  }

  // We're not saving any other files at this point
  return false;
}

bool Version::metadataMatch(shared_ptr<Version> other) const {
  // Get metadata from both versions
  auto& m1 = _metadata;
  auto& m2 = other->_metadata;

  // We need metadata for both versions to compare
  if (!m1.has_value() || !m2.has_value()) return false;

  // We only compare uid, gid, and mode (which covers both type and permissions)
  if (m1.value().st_uid != m2.value().st_uid) {
    LOG << "uid mismatch";
    return false;
  }

  if (m1.value().st_gid != m2.value().st_gid) {
    LOG << "gid mismatch";
    return false;
  }

  if (m1.value().st_mode != m2.value().st_mode) {
    LOG << "mode mismatch";
    return false;
  }

  // Copy the identity to/from the matched version
  identify(other);

  // That's it. Metadata must match
  return true;
}

/// Equality function for timespec structs
bool operator==(const timespec& t1, const timespec& t2) {
  return t1.tv_sec == t2.tv_sec && t1.tv_nsec == t2.tv_nsec;
}

bool Version::contentsMatch(shared_ptr<Version> other) const {
  // Get metadata from both versions
  auto& m1 = _metadata;
  auto& m2 = other->_metadata;

  // We need metadata from both versions to compare
  if (!m1.has_value() || !m2.has_value()) {
    LOG << "Fingerprint mismatch: missing metadata";
    return false;
  }

  // Compare mtimes
  if (m1.value().st_mtim == m2.value().st_mtim) {
    // The other version matched, so copy the identity between them
    identify(other);
    return true;
  } else {
    LOG << "Fingerprint mismatch: different mtimes";
    LOG << "  " << this << ": " << m1.value().st_mtim.tv_sec << ", " << m1.value().st_mtim.tv_nsec;
    LOG << "  " << other << ": " << m2.value().st_mtim.tv_sec << ", " << m2.value().st_mtim.tv_nsec;
    return false;
  }
}

void Version::saveMetadata(shared_ptr<Reference> ref) {
  auto a = dynamic_pointer_cast<Access>(ref);
  if (!a) {
    _metadata = nullopt;
    return;
  }

  struct stat statbuf;
  if (a->stat(&statbuf) == 0) {
    _metadata = statbuf;
  }
}

void Version::saveFingerprint(shared_ptr<Reference> ref) {
  saveMetadata(ref);
}

void Version::identify(const Artifact* a) const {
  auto p = a->getPath();
  _identity =
      string("[") + p.value_or("<anon>") + " v" + std::to_string(a->getVersionCount() - 1) + "]";
}

void Version::identify(shared_ptr<Version> other) const {
  if (_identity.has_value()) {
    other->_identity = _identity;
  } else if (other->_identity.has_value()) {
    _identity = other->_identity;
  }
}

ostream& operator<<(ostream& o, const Version& v) {
  return o << v._identity.value_or("[Unknown Version]");
}
