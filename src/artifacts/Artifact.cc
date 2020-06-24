#include "Artifact.hh"

#include <memory>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "build/AccessTypes.hh"
#include "build/Build.hh"
#include "build/Env.hh"
#include "core/Command.hh"
#include "core/IR.hh"
#include "ui/options.hh"
#include "versions/MetadataVersion.hh"
#include "versions/Version.hh"

using std::make_shared;
using std::nullopt;
using std::shared_ptr;

Artifact::Artifact(Env& env, bool committed, shared_ptr<MetadataVersion> v) noexcept : _env(env) {
  appendVersion(v);
  _metadata_version = v;
  _metadata_committed = committed;
}

void Artifact::needsCurrentVersions(shared_ptr<Command> c) noexcept {
  _env.getBuild().observeInput(c, shared_from_this(), _metadata_version, InputType::Inherited);
}

// Check if an access is allowed by the metadata for this artifact
bool Artifact::checkAccess(shared_ptr<Command> c, AccessFlags flags) noexcept {
  // We really should report this edge here, since some other command may have set the latest
  // metadata version. Disabling this for now because it makes the graph unreadable.

  _env.getBuild().observeInput(c, shared_from_this(), _metadata_version, InputType::PathResolution);

  return _metadata_version->checkAccess(flags);
}

// Do we have saved metadata for this artifact?
bool Artifact::isSaved() const noexcept {
  return _metadata_version->isSaved();
}

// Check if the latest metadata version is committed
bool Artifact::isCommitted() const noexcept {
  return _metadata_committed;
}

// Commit the latest metadata version
void Artifact::commit(shared_ptr<Reference> ref) noexcept {
  if (!_metadata_committed) {
    ASSERT(_metadata_version->isSaved()) << "Attempted to commit unsaved version";

    _metadata_version->commit(ref);
    _metadata_committed = true;
  }
}

// Check the final state of this artifact, and save its fingerprint if necessary
void Artifact::finalize(shared_ptr<Reference> ref) noexcept {
  // Is the metadata for this artifact committed?
  if (_metadata_committed) {
    // Yes. We know the on-disk state already matches the latest version.
    // Make sure we have a fingerprint for the metadata version
    _metadata_version->fingerprint(ref);

  } else {
    // No. Check the on-disk version against the expected version
    auto v = make_shared<MetadataVersion>();
    v->fingerprint(ref);

    // Is there a difference between the tracked version and what's on the filesystem?
    if (!_metadata_version->matches(v)) {
      // Yes. Report the mismatch
      _env.getBuild().observeFinalMismatch(shared_from_this(), _metadata_version, v);
    } else {
      // No. We can treat this artifact as if we committed it
      _metadata_committed = true;
    }
  }
}

/// Get the current metadata version for this artifact
shared_ptr<MetadataVersion> Artifact::getMetadata(shared_ptr<Command> c, InputType t) noexcept {
  // Mark the metadata as accessed
  _metadata_version->accessed();

  // Notify the build of the input
  _env.getBuild().observeInput(c, shared_from_this(), _metadata_version, t);

  // Return the metadata version
  return _metadata_version;
}

/// Check to see if this artifact's metadata matches a known version
void Artifact::match(shared_ptr<Command> c, shared_ptr<MetadataVersion> expected) noexcept {
  // Get the current metadata
  auto observed = getMetadata(c, InputType::Accessed);

  // Compare versions
  if (!observed->matches(expected)) {
    // Report the mismatch
    _env.getBuild().observeMismatch(c, shared_from_this(), observed, expected);
  }
}

/// Apply a new metadata version to this artifact
void Artifact::apply(shared_ptr<Command> c,
                     shared_ptr<MetadataVersion> writing,
                     bool committed) noexcept {
  // Update the metadata version for this artifact
  appendVersion(writing);
  _metadata_version = writing;

  // Keep track of whether metadata is committed or not
  _metadata_committed = committed;

  // Report the output to the build
  _env.getBuild().observeOutput(c, shared_from_this(), _metadata_version);
}

void Artifact::appendVersion(shared_ptr<Version> v) noexcept {
  _versions.push_back(v);
}
