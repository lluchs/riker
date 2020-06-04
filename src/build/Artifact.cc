#include "Artifact.hh"

#include <memory>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "build/Build.hh"
#include "build/Env.hh"
#include "data/Command.hh"
#include "data/IR.hh"
#include "data/Version.hh"
#include "ui/options.hh"

using std::dynamic_pointer_cast;
using std::make_shared;
using std::nullopt;
using std::shared_ptr;

shared_ptr<Artifact> Artifact::existing(Build* build, shared_ptr<Reference> ref) {
  // Create an artifact
  shared_ptr<Artifact> a(new Artifact(build, ref));
  a->createInitialVersion(nullptr);
  return a;
}

shared_ptr<Artifact> Artifact::created(Build* build, shared_ptr<Reference> ref,
                                       shared_ptr<Command> c) {
  shared_ptr<Artifact> a(new Artifact(build, ref));
  a->createInitialVersion(c);

  // Manufacture the expected stat data for this created artifact
  // TODO: get euid and egid from the process
  struct stat statbuf = {.st_uid = geteuid(), .st_gid = getegid()};

  // Fill in the mode field
  if (auto p = dynamic_pointer_cast<Pipe>(ref)) {
    statbuf.st_mode = S_IFIFO | 0777;
  } else if (auto a = dynamic_pointer_cast<Access>(ref)) {
    // Get the current umask
    // TODO: This should come from the running process, not dodo
    auto mask = umask(0);
    umask(mask);

    statbuf.st_mode = S_IFREG | (a->getFlags().mode & ~mask);
  }

  // Shoehorn this manufactured stat buffer into the initial version
  a->_versions.back()->setMetadata(statbuf);

  build->observeMetadataOutput(c, a, a->_versions.back());
  build->observeContentOutput(c, a, a->_versions.back());

  return a;
}

void Artifact::createInitialVersion(shared_ptr<Command> creator) {
  auto v = make_shared<Version>();
  _versions.push_back(v);
  v->identify(this);
  _metadata_version = v;
  _metadata_creator = creator;
  _content_version = v;
  _content_creator = creator;
}

// Get the path to this artifact if it has one
optional<string> Artifact::getPath() const {
  if (auto a = dynamic_pointer_cast<Access>(_ref)) {
    return a->getPath();
  } else {
    return nullopt;
  }
}

/////////////////////// Tracing Methods ///////////////////////

// Command c accesses this artifact's metadata
// Return the version it observes, or nullptr if no check is necessary
shared_ptr<Version> Artifact::accessMetadata(shared_ptr<Command> c) {
  // Inform the environment of this input
  _build->observeMetadataInput(c, shared_from_this(), _metadata_version);

  // All done
  return _metadata_version;
}

// Command c accesses this artifact's contents
// Return the version it observes, or nullptr if no check is necessary
shared_ptr<Version> Artifact::accessContents(shared_ptr<Command> c) {
  // Inform the environment of this input
  _build->observeContentInput(c, shared_from_this(), _content_version);

  // All done
  return _content_version;
}

// Command c sets the metadata for this artifact.
// Return the version created by this operation, or nullptr if no new version is necessary.
shared_ptr<Version> Artifact::setMetadata(shared_ptr<Command> c) {
  // Create the new version
  auto v = make_shared<Version>();
  v->identify(this);
  _versions.push_back(v);
  _metadata_version = v;
  _metadata_creator = c;

  // Inform the environment of this output
  _build->observeMetadataOutput(c, shared_from_this(), _metadata_version);

  // Return the newly-tagged version
  return v;
}

// Command c sets the contents of this artifact.
// Return the version created by this operation, or nullptr if no new version is necessary.
shared_ptr<Version> Artifact::setContents(shared_ptr<Command> c) {
  // Create the new version
  auto v = make_shared<Version>();
  v->identify(this);
  _versions.push_back(v);
  _content_version = v;
  _content_creator = c;

  // Inform the environment of this output
  _build->observeContentOutput(c, shared_from_this(), _content_version);

  // Return the newly-tagged version
  return v;
}

/////////////////////// Emulation Methods ///////////////////////

// Command c checks whether this artifact's metadata matches an expected version
void Artifact::checkMetadata(shared_ptr<Command> c, shared_ptr<Version> v) {
  // Inform the environment of this input
  _build->observeMetadataInput(c, shared_from_this(), _metadata_version);

  // Compare versions
  if (!_metadata_version->metadataMatch(v)) {
    _build->observeMetadataMismatch(c, shared_from_this(), _metadata_version, v);
  }
}

// Command c checks whether this artifact's content matches an expected version
void Artifact::checkContents(shared_ptr<Command> c, shared_ptr<Version> v) {
  // Inform the environment of this input
  _build->observeContentInput(c, shared_from_this(), _content_version);

  // Compare versions
  if (!_content_version->contentsMatch(v)) {
    _build->observeContentMismatch(c, shared_from_this(), _content_version, v);
  }
}

// Command c sets the metadata for this artifact to an existing version. Used during emulation.
void Artifact::setMetadata(shared_ptr<Command> c, shared_ptr<Version> v) {
  // Add the new version
  _versions.push_back(v);
  v->identify(this);
  _metadata_version = v;
  _metadata_creator = c;

  // Inform the environment of this output
  _build->observeMetadataOutput(c, shared_from_this(), _metadata_version);
}

// Command c sets the contents of this artifact to an existing version. Used during emulation.
void Artifact::setContents(shared_ptr<Command> c, shared_ptr<Version> v) {
  // Add the new version
  _versions.push_back(v);
  v->identify(this);
  _content_version = v;
  _content_creator = c;

  // Inform the environment of this output
  _build->observeContentOutput(c, shared_from_this(), _content_version);
}

// Save metadata for the latest version of this artifact
void Artifact::saveMetadata(shared_ptr<Reference> ref) {
  _metadata_version->saveMetadata(ref);
}

// Save a fingerprint of the contents of the latest version of this artifact
void Artifact::saveFingerprint(shared_ptr<Reference> ref) {
  _content_version->saveFingerprint(ref);
}

// Check if this artifact can be restored to the filesystem
bool Artifact::isSaved() const {
  return _content_version->isSaved();
}

// Check this artifact's contents and metadata against the filesystem state
void Artifact::checkFinalState(shared_ptr<Reference> ref) {
  // Create a version that represents the on-disk contents reached through this reference
  auto v = make_shared<Version>();
  v->saveMetadata(ref);
  v->saveFingerprint(ref);

  if (!_metadata_version->metadataMatch(v)) {
    _build->observeFinalMetadataMismatch(shared_from_this(), _metadata_version, v);
  }

  if (!_content_version->contentsMatch(v)) {
    _build->observeFinalContentMismatch(shared_from_this(), _content_version, v);
  }
}