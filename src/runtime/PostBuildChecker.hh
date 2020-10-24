#include <memory>

#include "interfaces/TraceHandler.hh"
#include "runtime/Command.hh"
#include "runtime/Ref.hh"

using std::shared_ptr;

/**
 * This class processes a build trace that has already been completed, and adds new predicates to
 * check against the state left at the end of a build.
 */
class PostBuildChecker : public TraceHandler {
 public:
  PostBuildChecker(TraceHandler& output) : _output(output) {}

  /// Called when the trace is finished
  virtual void finish() noexcept override { _output.finish(); }

  /// Handle a SpecialRef IR step
  virtual void specialRef(shared_ptr<Command> command,
                          SpecialRef entity,
                          Command::RefID output) noexcept override {
    _output.specialRef(command, entity, output);
  }

  /// Handle a PipeRef IR step
  virtual void pipeRef(shared_ptr<Command> command,
                       Command::RefID read_end,
                       Command::RefID write_end) noexcept override {
    _output.pipeRef(command, read_end, write_end);
  }

  /// Handle a FileRef IR step
  virtual void fileRef(shared_ptr<Command> command,
                       mode_t mode,
                       Command::RefID output) noexcept override {
    _output.fileRef(command, mode, output);
  }

  /// Handle a SymlinkRef IR step
  virtual void symlinkRef(shared_ptr<Command> command,
                          fs::path target,
                          Command::RefID output) noexcept override {
    _output.symlinkRef(command, target, output);
  }

  /// Handle a DirRef IR step
  virtual void dirRef(shared_ptr<Command> command,
                      mode_t mode,
                      Command::RefID output) noexcept override {
    _output.dirRef(command, mode, output);
  }

  /// Handle a PathRef IR step
  virtual void pathRef(shared_ptr<Command> command,
                       Command::RefID base,
                       fs::path path,
                       AccessFlags flags,
                       Command::RefID output) noexcept override {
    _output.pathRef(command, base, path, flags, output);
  }

  /// Handle a UsingRef IR step
  virtual void usingRef(shared_ptr<Command> command, Command::RefID ref) noexcept override {
    _output.usingRef(command, ref);
  }

  /// Handle a DoneWithRef IR step
  virtual void doneWithRef(shared_ptr<Command> command, Command::RefID ref) noexcept override {
    _output.doneWithRef(command, ref);
  }

  /// Handle a CompareRefs IR step
  virtual void compareRefs(shared_ptr<Command> command,
                           Command::RefID ref1,
                           Command::RefID ref2,
                           RefComparison type) noexcept override {
    // TODO: Update comparison predicate?
    _output.compareRefs(command, ref1, ref2, type);
  }

  /// Handle an ExpectResult IR step
  virtual void expectResult(shared_ptr<Command> command,
                            Scenario scenario,
                            Command::RefID ref,
                            int expected) noexcept override {
    if (scenario == Scenario::Build) {
      _output.expectResult(command, Scenario::Build, ref, expected);
      _output.expectResult(command, Scenario::PostBuild, ref,
                           command->getRef(ref)->getResultCode());
    }
  }

  /// Handle a MatchMetadata IR step
  virtual void matchMetadata(shared_ptr<Command> command,
                             Scenario scenario,
                             Command::RefID ref,
                             shared_ptr<MetadataVersion> expected) noexcept override {
    if (scenario == Scenario::Build) {
      // Emit the predicate from the original build phase
      _output.matchMetadata(command, Scenario::Build, ref, expected);

      // Now also emit a predicate to check for the post-build state
      if (command->getRef(ref)->isResolved()) {
        _output.matchMetadata(command, Scenario::PostBuild, ref,
                              command->getRef(ref)->getArtifact()->peekMetadata());
      } else {
        // Do we need to make sure the reference is not resolved? Hasn't that already been done?
      }
    }
  }

  /// Handle a MatchContent IR step
  virtual void matchContent(shared_ptr<Command> command,
                            Scenario scenario,
                            Command::RefID ref,
                            shared_ptr<Version> expected) noexcept override {
    if (scenario == Scenario::Build) {
      // Emit the predicate from the original build phase
      _output.matchContent(command, Scenario::Build, ref, expected);

      // Now also emit a predicate to check for the post-build state
      if (command->getRef(ref)->isResolved()) {
        _output.matchContent(command, Scenario::PostBuild, ref,
                             command->getRef(ref)->getArtifact()->peekContent());
      } else {
        // Do we need to make sure the reference is not resolved? Hasn't that already been done?
      }
    }
  }

  /// Handle an UpdateMetadata IR step
  virtual void updateMetadata(shared_ptr<Command> command,
                              Command::RefID ref,
                              shared_ptr<MetadataVersion> version) noexcept override {
    _output.updateMetadata(command, ref, version);
  }

  /// Handle an UpdateContent IR step
  virtual void updateContent(shared_ptr<Command> command,
                             Command::RefID ref,
                             shared_ptr<Version> version) noexcept override {
    _output.updateContent(command, ref, version);
  }

  /// Handle an AddEntry IR step
  virtual void addEntry(shared_ptr<Command> command,
                        Command::RefID dir,
                        fs::path name,
                        Command::RefID target) noexcept override {
    _output.addEntry(command, dir, name, target);
  }

  /// Handle a RemoveEntry IR step
  virtual void removeEntry(shared_ptr<Command> command,
                           Command::RefID dir,
                           fs::path name,
                           Command::RefID target) noexcept override {
    _output.removeEntry(command, dir, name, target);
  }

  /// Handle a Launch IR step
  virtual void launch(shared_ptr<Command> command,
                      shared_ptr<Command> child,
                      list<tuple<Command::RefID, Command::RefID>> refs) noexcept override {
    _output.launch(command, child, refs);
  }

  /// Handle a Join IR step
  virtual void join(shared_ptr<Command> command,
                    shared_ptr<Command> child,
                    int exit_status) noexcept override {
    _output.join(command, child, exit_status);
  }

  /// Handle an Exit IR step
  virtual void exit(shared_ptr<Command> command, int exit_status) noexcept override {
    _output.exit(command, exit_status);
  }

 private:
  TraceHandler& _output;
};
