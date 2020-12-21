#pragma once

#include <memory>
#include <string>

#include "artifacts/Artifact.hh"

using std::shared_ptr;
using std::string;

class Command;
class FileVersion;
class Ref;
class Version;

class FileArtifact : public Artifact {
 public:
  FileArtifact(shared_ptr<Env> env,
               shared_ptr<MetadataVersion> mv,
               shared_ptr<FileVersion> cv) noexcept;

  /************ Core Artifact Operations ************/

  /// Get the name of this artifact type
  virtual string getTypeName() const noexcept override { return "File"; }

  /// Can a specific version of this artifact be committed?
  virtual bool canCommit(shared_ptr<Version> v) const noexcept override;

  /// Commit a specific version of this artifact to the filesystem
  virtual void commit(shared_ptr<Version> v) noexcept override;

  /// Can this artifact be fully committed?
  virtual bool canCommitAll() const noexcept override;

  /// Commit all final versions of this artifact to the filesystem
  virtual void commitAll() noexcept override;

  /// Command c requires that this artifact exists in its current state. Create dependency edges.
  virtual void mustExist(const shared_ptr<Command>& c) noexcept override;

  /// Compare all final versions of this artifact to the filesystem state
  virtual void checkFinalState(fs::path path) noexcept override;

  /// Commit any pending versions and save fingerprints for this artifact
  virtual void applyFinalState(fs::path path) noexcept override;

  /// Mark all versions of this artifact as committed
  virtual void setCommitted() noexcept override;

  /************ Traced Operations ************/

  /// A traced command is about to (possibly) read from this artifact
  virtual void beforeRead(Build& build,
                          const shared_ptr<Command>& c,
                          Ref::ID ref) noexcept override;

  /// A traced command just read from this artifact
  virtual void afterRead(Build& build, const shared_ptr<Command>& c, Ref::ID ref) noexcept override;

  /// A traced command is about to (possibly) write to this artifact
  virtual void beforeWrite(Build& build,
                           const shared_ptr<Command>& c,
                           Ref::ID ref) noexcept override;

  /// A trace command just wrote to this artifact
  virtual void afterWrite(Build& build,
                          const shared_ptr<Command>& c,
                          Ref::ID ref) noexcept override;

  /// A traced command is about to (possibly) truncate this artifact to length zero
  virtual void beforeTruncate(Build& build,
                              const shared_ptr<Command>& c,
                              Ref::ID ref) noexcept override;

  /// A trace command just truncated this artifact to length zero
  virtual void afterTruncate(Build& build,
                             const shared_ptr<Command>& c,
                             Ref::ID ref) noexcept override;

  /************ Content Operations ************/

  /// Get this artifact's current content without creating any dependencies
  virtual shared_ptr<Version> peekContent() noexcept override;

  /// Check to see if this artifact's content matches a known version
  virtual void matchContent(const shared_ptr<Command>& c,
                            Scenario scenario,
                            shared_ptr<Version> expected) noexcept override;

  /// Apply a new content version to this artifact
  virtual void updateContent(const shared_ptr<Command>& c,
                             shared_ptr<Version> writing) noexcept override;

 private:
  /// The latest content version
  shared_ptr<FileVersion> _content_version;
};