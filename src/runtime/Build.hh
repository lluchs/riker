#pragma once

#include <memory>
#include <ostream>
#include <set>
#include <tuple>
#include <vector>

#include "data/InputTrace.hh"
#include "data/OutputTrace.hh"
#include "interfaces/BuildObserver.hh"
#include "interfaces/TraceHandler.hh"
#include "runtime/Command.hh"
#include "runtime/Env.hh"
#include "runtime/RebuildPlan.hh"
#include "runtime/Ref.hh"
#include "tracing/Tracer.hh"

using std::make_shared;
using std::ostream;
using std::set;
using std::shared_ptr;
using std::tuple;
using std::vector;

class Version;

/**
 * A Build instance manages the execution of a build. This instance is responsible for setting up
 * the build environment, emulating or running each of the commands, and notifying any observers of
 * dependencies and changes detected during the build.
 */
class Build : public TraceHandler, public BuildObserver {
 private:
  /// Create a build runner
  Build(bool commit, RebuildPlan plan, BuildObserver& observer, TraceHandler& output) noexcept :
      _commit(commit),
      _plan(plan),
      _observer(observer),
      _output(output),
      _env(make_shared<Env>()),
      _tracer(*this) {}

 public:
  /// Create a build runner that exclusively emulates trace steps
  static Build emulate(BuildObserver& observer = _default_observer,
                       TraceHandler& output = _default_output) noexcept {
    return Build(false, RebuildPlan(), observer, output);
  }

  static Build emulate(TraceHandler& output) noexcept {
    return Build(false, RebuildPlan(), _default_observer, output);
  }

  /// Create a build runner that executes a rebuild plan
  static Build rebuild(RebuildPlan plan, TraceHandler& output = _default_output) noexcept {
    return Build(true, plan, _default_observer, output);
  }

  // Disallow Copy
  Build(const Build&) = delete;
  Build& operator=(const Build&) = delete;

  /// Get the environment used in this build
  shared_ptr<Env> getEnvironment() const noexcept { return _env; }

  /// Get the number of steps this build has executed
  size_t getStepCount() const noexcept { return _emulated_step_count + _traced_step_count; }

  /// Get the number of steps this build emulated
  size_t getEmulatedStepCount() const noexcept { return _emulated_step_count; }

  /// Get the number of steps this build traced
  size_t getTracedStepCount() const noexcept { return _traced_step_count; }

  /// Get the number of commands this build has executed
  size_t getCommandCount() const noexcept {
    return _emulated_command_count + _traced_command_count;
  }

  /// Get the number of commands this build emulated
  size_t getEmulatedCommandCount() const noexcept { return _emulated_command_count; }

  /// Get the numebr of commands this build traced
  size_t getTracedCommandCount() const noexcept { return _traced_command_count; }

  /// Print information about this build
  ostream& print(ostream& o) const noexcept;

  /********** Handle IR steps supplied from a loaded trace **********/

  /// A command is issuing a reference to a special artifact (e.g. stdin, stdout, root dir)
  virtual void specialRef(shared_ptr<Command> c,
                          SpecialRef entity,
                          shared_ptr<Ref> output) noexcept override;

  /// A command references a new anonymous pipe
  virtual void pipeRef(shared_ptr<Command> c,
                       shared_ptr<Ref> read_end,
                       shared_ptr<Ref> write_end) noexcept override;

  /// A command references a new anonymous file
  virtual void fileRef(shared_ptr<Command> c,
                       mode_t mode,
                       shared_ptr<Ref> output) noexcept override;

  /// A command references a new anonymous symlink
  virtual void symlinkRef(shared_ptr<Command> c,
                          fs::path target,
                          shared_ptr<Ref> output) noexcept override;

  /// A command references a new anonymous directory
  virtual void dirRef(shared_ptr<Command> c, mode_t mode, shared_ptr<Ref> output) noexcept override;

  /// A command makes a reference with a path
  virtual void pathRef(shared_ptr<Command> c,
                       shared_ptr<Ref> base,
                       fs::path path,
                       AccessFlags flags,
                       shared_ptr<Ref> output) noexcept override;

  /// A command retains a handle to a Ref
  virtual void usingRef(shared_ptr<Command> c, shared_ptr<Ref> ref) noexcept override;

  /// A command is finished with a specific Ref
  virtual void doneWithRef(shared_ptr<Command> c, shared_ptr<Ref> ref) noexcept override;

  /// A command depends on the outcome of comparing two different references
  virtual void compareRefs(shared_ptr<Command> command,
                           shared_ptr<Ref> ref1,
                           shared_ptr<Ref> ref2,
                           RefComparison type) noexcept override;

  /// A command expects a reference to resolve with a particular result
  virtual void expectResult(shared_ptr<Command> c,
                            Scenario scenario,
                            shared_ptr<Ref> ref,
                            int expected) noexcept override;

  /// A command accesses metadata for an artifact and expects to find a particular version
  virtual void matchMetadata(shared_ptr<Command> c,
                             Scenario scenario,
                             shared_ptr<Ref> ref,
                             shared_ptr<MetadataVersion> expected) noexcept override;

  /// A command accesses content for an artifact and expects to find a particular version
  virtual void matchContent(shared_ptr<Command> c,
                            Scenario scenario,
                            shared_ptr<Ref> ref,
                            shared_ptr<Version> expected) noexcept override;

  /// A command modifies the metadata for an artifact
  virtual void updateMetadata(shared_ptr<Command> c,
                              shared_ptr<Ref>,
                              shared_ptr<MetadataVersion> written) noexcept override;

  /// A command writes a new version to an artifact
  virtual void updateContent(shared_ptr<Command> c,
                             shared_ptr<Ref> ref,
                             shared_ptr<Version> written) noexcept override;

  /// A command adds an entry to a directory
  virtual void addEntry(shared_ptr<Command> command,
                        shared_ptr<Ref> dir,
                        fs::path name,
                        shared_ptr<Ref> target) noexcept override;

  /// A command removes an entry from a directory
  virtual void removeEntry(shared_ptr<Command> command,
                           shared_ptr<Ref> dir,
                           fs::path name,
                           shared_ptr<Ref> target) noexcept override;

  /// A command is launching a child command
  virtual void launch(shared_ptr<Command> c, shared_ptr<Command> child) noexcept override;

  /// A command is joining with a child command
  virtual void join(shared_ptr<Command> c,
                    shared_ptr<Command> child,
                    int exit_status) noexcept override;

  /// A command has exited with an exit code
  virtual void exit(shared_ptr<Command> c, int exit_status) noexcept override;

  /// Finish running an emulated build
  virtual void finish() noexcept override;

  /********** Handle IR steps delivered from the tracing layer **********/

  /// A traced command referenced a new anonymous pipe
  tuple<shared_ptr<Ref>, shared_ptr<Ref>> tracePipeRef(shared_ptr<Command> c) noexcept;

  /// A traced command referenced a new anonymous file
  shared_ptr<Ref> traceFileRef(shared_ptr<Command> c, mode_t mode) noexcept;

  /// A traced command referenced a new anonymous symlink
  shared_ptr<Ref> traceSymlinkRef(shared_ptr<Command> c, fs::path target) noexcept;

  /// A traced command referenced a new anonymous directory
  shared_ptr<Ref> traceDirRef(shared_ptr<Command> c, mode_t mode) noexcept;

  /// A traced command referenced a path
  shared_ptr<Ref> tracePathRef(shared_ptr<Command> c,
                               shared_ptr<Ref> base,
                               fs::path path,
                               AccessFlags flags) noexcept;

  /// A command is retaining a handle to a Ref (e.g. in its file descriptor table)
  void traceUsingRef(shared_ptr<Command> c, shared_ptr<Ref> ref) noexcept;

  /// A command has closed a handle to a Ref
  void traceDoneWithRef(shared_ptr<Command> c, shared_ptr<Ref> ref) noexcept;

  /// A command compares two references and expects a specific result
  void traceCompareRefs(shared_ptr<Command> c,
                        shared_ptr<Ref> ref1,
                        shared_ptr<Ref> ref2,
                        RefComparison type) noexcept;

  /// A command expects a reference to resolve with a particular result
  void traceExpectResult(shared_ptr<Command> c, shared_ptr<Ref> ref, int expected = -1) noexcept;

  /// A command accesses metadata for an artifact and expects to find a particular version
  void traceMatchMetadata(shared_ptr<Command> c, shared_ptr<Ref> ref) noexcept;

  /// A command accesses content for an artifact and expects to find a particular version
  void traceMatchContent(shared_ptr<Command> c,
                         shared_ptr<Ref> ref,
                         shared_ptr<Version> expected) noexcept;

  /// A command modifies the metadata for an artifact
  void traceUpdateMetadata(shared_ptr<Command> c, shared_ptr<Ref>) noexcept;

  /// A command writes a new version to an artifact
  void traceUpdateContent(shared_ptr<Command> c,
                          shared_ptr<Ref> ref,
                          shared_ptr<Version> written) noexcept;

  /// Handle an AddEntry IR step
  void traceAddEntry(shared_ptr<Command> command,
                     shared_ptr<Ref> dir,
                     fs::path name,
                     shared_ptr<Ref> target) noexcept;

  /// Handle a RemoveEntry IR step
  void traceRemoveEntry(shared_ptr<Command> command,
                        shared_ptr<Ref> dir,
                        fs::path name,
                        shared_ptr<Ref> target) noexcept;

  /// A command is launching a child command
  shared_ptr<Command> traceLaunch(shared_ptr<Command> c,
                                  shared_ptr<Ref> exe_ref,
                                  vector<string> args,
                                  map<int, FileDescriptor> fds,
                                  shared_ptr<Ref> cwd_ref,
                                  shared_ptr<Ref> root_ref) noexcept;

  /// A command is joining with a child command
  void traceJoin(shared_ptr<Command> c, shared_ptr<Command> child, int exit_status) noexcept;

  /// A command has exited with an exit code
  void traceExit(shared_ptr<Command> c, int exit_status) noexcept;

  /********** Observer Interface **********/

  /// Inform observers that a command has never run
  virtual void observeCommandNeverRun(shared_ptr<Command> c) noexcept override;

  /// Inform observers that a parent command launched a child command
  virtual void observeLaunch(shared_ptr<Command> parent,
                             shared_ptr<Command> child) noexcept override;

  /// Inform observers that command c modified artifact a, creating version v
  virtual void observeOutput(shared_ptr<Command> c,
                             shared_ptr<Artifact> a,
                             shared_ptr<Version> v) noexcept override;

  /// Inform observers that command c accessed version v of artifact a
  virtual void observeInput(shared_ptr<Command> c,
                            shared_ptr<Artifact> a,
                            shared_ptr<Version> v,
                            InputType t) noexcept override;

  /// Inform observers that command c did not find the expected version in artifact a
  /// Instead of version `expected`, the command found version `observed`
  virtual void observeMismatch(shared_ptr<Command> c,
                               Scenario scenario,
                               shared_ptr<Artifact> a,
                               shared_ptr<Version> observed,
                               shared_ptr<Version> expected) noexcept override;

  /// Inform observers that the version of an artifact produced during the build does not match
  /// the on-disk version.
  virtual void observeFinalMismatch(shared_ptr<Artifact> a,
                                    shared_ptr<Version> produced,
                                    shared_ptr<Version> ondisk) noexcept override;

  /// Inform observers that a reference did not resolve as expected
  virtual void observeResolutionChange(shared_ptr<Command> c,
                                       Scenario scenario,
                                       shared_ptr<Ref> ref,
                                       int expected) noexcept override;

  /// Inform observers that two references did not compare as expected
  virtual void observeRefMismatch(shared_ptr<Command> c,
                                  shared_ptr<Ref> ref1,
                                  shared_ptr<Ref> ref2,
                                  RefComparison type) noexcept override;

  /// Inform observers that a command's exit code changed
  virtual void observeExitCodeChange(shared_ptr<Command> parent,
                                     shared_ptr<Command> child,
                                     int expected,
                                     int observed) noexcept override;

 private:
  /// Is a particular command running?
  bool isRunning(shared_ptr<Command> c) const noexcept {
    return _running.find(c) != _running.end();
  }

 private:
  /// The number of IR steps emulated in this build
  size_t _emulated_step_count = 0;

  /// The number of IR steps traced in this build
  size_t _traced_step_count = 0;

  /// The count of commands emulated in this build
  size_t _emulated_command_count = 0;

  /// The count of commands traced in this build
  size_t _traced_command_count = 0;

  /// Should this build commit the environment to the filesystem when it's finished?
  bool _commit;

  /// The rebuild plan
  RebuildPlan _plan;

  /// The observers that should be notified of dependency and change information during the build
  BuildObserver& _observer;

  /// Trace steps are sent to this trace handler, typically an OutputTrace
  TraceHandler& _output;

  /// The environment in which this build executes
  shared_ptr<Env> _env;

  /// The tracer that will be used to execute any commands that must rerun
  Tracer _tracer;

  /// A map of launched commands to the root process running that command, or nullptr if it is
  /// only being emulated
  map<shared_ptr<Command>, shared_ptr<Process>> _running;

  /// A set of commands that have exited
  set<shared_ptr<Command>> _exited;

  /// The default observer is used if an observer is not provided during setup
  inline static BuildObserver _default_observer;

  /// The default output is used if a trace handler is not provided during setup
  inline static TraceHandler _default_output;
};