#include "SymlinkArtifact.hh"

#include <memory>

#include "build/Build.hh"
#include "versions/SymlinkVersion.hh"

using std::shared_ptr;

SymlinkArtifact::SymlinkArtifact(Env& env,
                                 bool committed,
                                 shared_ptr<MetadataVersion> mv,
                                 shared_ptr<SymlinkVersion> sv) noexcept :
    Artifact(env, committed, mv) {
  appendVersion(sv);
  _symlink_version = sv;
  _symlink_committed = committed;
}

// Record a dependency on the current versions of this artifact
void SymlinkArtifact::needsCurrentVersions(shared_ptr<Command> c) noexcept {
  _env.getBuild().observeInput(c, shared_from_this(), _symlink_version, InputType::Inherited);
  Artifact::needsCurrentVersions(c);
}

// Get the current symlink version of this artifact
shared_ptr<SymlinkVersion> SymlinkArtifact::getSymlink(shared_ptr<Command> c,
                                                       InputType t) noexcept {
  // Mark the metadata as accessed
  _symlink_version->accessed();

  // Notify the build of the input
  _env.getBuild().observeInput(c, shared_from_this(), _symlink_version, t);

  // Return the metadata version
  return _symlink_version;
}

// Check to see if this artifact's symlink destination matches a known version
void SymlinkArtifact::match(shared_ptr<Command> c, shared_ptr<SymlinkVersion> expected) noexcept {
  // Get the current metadata
  auto observed = getSymlink(c, InputType::Accessed);

  // Compare versions
  if (!observed->matches(expected)) {
    // Report the mismatch
    _env.getBuild().observeMismatch(c, shared_from_this(), observed, expected);
  }
}

void SymlinkArtifact::finalize(shared_ptr<Reference> ref) noexcept {
  // TODO: Check the on-disk symlink here

  // Check metadata in the top-level artifact
  Artifact::finalize(ref);
}
