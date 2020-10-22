#include "Build.hh"

#include <iostream>
#include <memory>
#include <ostream>

#include "artifacts/Artifact.hh"
#include "artifacts/DirArtifact.hh"
#include "artifacts/PipeArtifact.hh"
#include "artifacts/SymlinkArtifact.hh"
#include "runtime/Env.hh"
#include "runtime/RebuildPlan.hh"
#include "runtime/Ref.hh"
#include "tracing/Process.hh"
#include "tracing/Tracer.hh"
#include "ui/TracePrinter.hh"
#include "ui/options.hh"
#include "util/wrappers.hh"
#include "versions/DirVersion.hh"
#include "versions/FileVersion.hh"
#include "versions/MetadataVersion.hh"
#include "versions/SymlinkVersion.hh"

using std::cerr;
using std::cout;
using std::endl;
using std::make_unique;
using std::ostream;
using std::shared_ptr;

/************************ Observer Implementation ************************/

// Inform observers that a command has never run
void Build::observeCommandNeverRun(shared_ptr<Command> c) noexcept {
  _observer.observeCommandNeverRun(c);
}

// Inform observers that a parent command launched a child command
void Build::observeLaunch(shared_ptr<Command> parent, shared_ptr<Command> child) noexcept {
  _observer.observeLaunch(parent, child);
}

// Inform observers that command c modified artifact a, creating version v
void Build::observeOutput(shared_ptr<Command> c,
                          shared_ptr<Artifact> a,
                          shared_ptr<Version> v) noexcept {
  _observer.observeOutput(c, a, v);
}

// Inform observers that command c  accessed version v of artifact a
void Build::observeInput(shared_ptr<Command> c,
                         shared_ptr<Artifact> a,
                         shared_ptr<Version> v,
                         InputType t) noexcept {
  // If the accessing command is running, make sure this file is available.
  // One exception is when a command accesses its own output; we can skip that case because the
  // output will eventually be marked as committed.
  if (_plan.mustRerun(c) && !v->isCommitted() && v->getCreator() != c) {
    // The command c is running, and needs uncommitted version v. We can commit it now
    ASSERT(a->canCommit(v)) << "Running command " << c << " depends on an uncommittable version "
                            << v << " of " << a;
    LOG(exec) << "Committing " << v << " to " << a << " on demand";
    a->commit(v);
  }

  _observer.observeInput(c, a, v, t);
}

// Inform observers that command c did not find the expected version in artifact a
// Instead of version `expected`, the command found version `observed`
void Build::observeMismatch(shared_ptr<Command> c,
                            Scenario scenario,
                            shared_ptr<Artifact> a,
                            shared_ptr<Version> observed,
                            shared_ptr<Version> expected) noexcept {
  _observer.observeMismatch(c, scenario, a, observed, expected);
}

// Inform observers that the version of an artifact produced during the build does not match the
// on-disk version.
void Build::observeFinalMismatch(shared_ptr<Artifact> a,
                                 shared_ptr<Version> produced,
                                 shared_ptr<Version> ondisk) noexcept {
  _observer.observeFinalMismatch(a, produced, ondisk);
}

// Inform observers that a reference did not resolve as expected
void Build::observeResolutionChange(shared_ptr<Command> c,
                                    Scenario scenario,
                                    shared_ptr<Ref> ref,
                                    int expected) noexcept {
  _observer.observeResolutionChange(c, scenario, ref, expected);
}

// Inform observers that two references did not compare as expected
void Build::observeRefMismatch(shared_ptr<Command> c,
                               shared_ptr<Ref> ref1,
                               shared_ptr<Ref> ref2,
                               RefComparison type) noexcept {
  _observer.observeRefMismatch(c, ref1, ref2, type);
}

// Inform observers that a reference did not resolve as expected
void Build::observeExitCodeChange(shared_ptr<Command> parent,
                                  shared_ptr<Command> child,
                                  int expected,
                                  int observed) noexcept {
  _observer.observeExitCodeChange(parent, child, expected, observed);
}

/************************ Handle IR steps from a loaded trace ************************/

void Build::finish() noexcept {
  // Wait for all remaining processes to exit
  _tracer.wait();

  // Compare the final state of all artifacts to the actual filesystem
  _env->getRootDir()->checkFinalState(*this, "/");

  // Commit the final environment state to the filesystem
  if (_commit) _env->getRootDir()->applyFinalState(*this, "/");

  // Inform the output trace that it is finished
  _output.finish();
}

void Build::specialRef(shared_ptr<Command> c, SpecialRef entity, shared_ptr<Ref> output) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::SpecialRefPrinter{c, entity, output};

  // Create an IR step and add it to the output trace
  _output.specialRef(c, entity, output);

  // Resolve the reference
  if (entity == SpecialRef::stdin) {
    *output = Ref(AccessFlags{.r = true}, _env->getStdin(*this, c));

  } else if (entity == SpecialRef::stdout) {
    *output = Ref(AccessFlags{.w = true}, _env->getStdout(*this, c));

  } else if (entity == SpecialRef::stderr) {
    *output = Ref(AccessFlags{.w = true}, _env->getStderr(*this, c));

  } else if (entity == SpecialRef::root) {
    *output = Ref(AccessFlags{.r = true, .x = true}, _env->getRootDir());

  } else if (entity == SpecialRef::cwd) {
    auto cwd_path = fs::current_path().relative_path();
    *output = _env->getRootDir()->resolve(*this, c, cwd_path, AccessFlags{.r = true, .x = true});

    ASSERT(output->isSuccess()) << "Failed to resolve current working directory";
    output->getArtifact()->setName(".");

  } else if (entity == SpecialRef::launch_exe) {
    auto dodo = readlink("/proc/self/exe");
    auto dodo_launch = (dodo.parent_path() / "dodo-launch").relative_path();
    *output = _env->getRootDir()->resolve(*this, c, dodo_launch, AccessFlags{.r = true, .x = true});

  } else {
    FAIL << "Unknown special reference";
  }
}

// A command references a new anonymous pipe
void Build::pipeRef(shared_ptr<Command> c,
                    shared_ptr<Ref> read_end,
                    shared_ptr<Ref> write_end) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::PipeRefPrinter{c, read_end, write_end};

  // Create an IR step and add it to the output trace
  _output.pipeRef(c, read_end, write_end);

  // Resolve the reference and save the result in output
  auto pipe = _env->getPipe(*this, c);
  *read_end = Ref(AccessFlags{.r = true}, pipe);
  *write_end = Ref(AccessFlags{.w = true}, pipe);
}

// A command references a new anonymous file
void Build::fileRef(shared_ptr<Command> c, mode_t mode, shared_ptr<Ref> output) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::FileRefPrinter{c, mode, output};

  // Create an IR step and add it to the output trace
  _output.fileRef(c, mode, output);

  // Resolve the reference and save the result in output
  *output = Ref(AccessFlags{.r = true, .w = true}, _env->createFile(*this, c, mode, false));
}

// A command references a new anonymous symlink
void Build::symlinkRef(shared_ptr<Command> c, fs::path target, shared_ptr<Ref> output) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::SymlinkRefPrinter{c, target, output};

  // Create an IR step and add it to the output trace
  _output.symlinkRef(c, target, output);

  // Resolve the reference and save the result in output
  *output =
      Ref(AccessFlags{.r = true, .w = true, .x = true}, _env->getSymlink(*this, c, target, false));
}

// A command references a new anonymous directory
void Build::dirRef(shared_ptr<Command> c, mode_t mode, shared_ptr<Ref> output) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::DirRefPrinter{c, mode, output};

  // Create an IR step and add it to the output trace
  _output.dirRef(c, mode, output);

  // Resolve the reference and save the result in output
  *output = Ref(AccessFlags{.r = true, .w = true, .x = true}, _env->getDir(*this, c, mode, false));
}

// A command makes a reference with a path
void Build::pathRef(shared_ptr<Command> c,
                    shared_ptr<Ref> base,
                    fs::path path,
                    AccessFlags flags,
                    shared_ptr<Ref> output) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::PathRefPrinter{c, base, path, flags, output};

  // Create an IR step and add it to the output trace
  _output.pathRef(c, base, path, flags, output);

  // Resolve the reference and save the result in output
  ASSERT(base->isResolved()) << "Cannot resolve a path relative to an unresolved base reference.";

  *output = base->getArtifact()->resolve(*this, c, path, flags);
}

// A command retains a handle to a given Ref
void Build::usingRef(shared_ptr<Command> c, shared_ptr<Ref> ref) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::UsingRefPrinter{c, ref};

  // Create an IR step and add it to the output trace
  _output.usingRef(c, ref);

  // Inform the ref that it was closed by c
  ref->addUser(*this, c);
}

// A command closes a handle to a given Ref
void Build::doneWithRef(shared_ptr<Command> c, shared_ptr<Ref> ref) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::DoneWithRefPrinter{c, ref};

  // Create an IR step and add it to the output trace
  _output.doneWithRef(c, ref);

  // Inform the ref that it was closed by c
  ref->removeUser(*this, c);
}

// Command c depends on the outcome of comparing two different references
void Build::compareRefs(shared_ptr<Command> c,
                        shared_ptr<Ref> ref1,
                        shared_ptr<Ref> ref2,
                        RefComparison type) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::CompareRefsPrinter{c, ref1, ref2, type};

  // Create an IR step and add it to the output trace
  _output.compareRefs(c, ref1, ref2, type);

  // Does the comparison resolve as expected?
  if (type == RefComparison::SameInstance) {
    if (ref1->getArtifact() != ref2->getArtifact()) {
      observeRefMismatch(c, ref1, ref2, type);
    }
  } else if (type == RefComparison::DifferentInstances) {
    if (ref1->getArtifact() == ref2->getArtifact()) {
      observeRefMismatch(c, ref1, ref2, type);
    }
  } else {
    FAIL << "Unknown reference comparison type";
  }
}

// Command c expects a reference to resolve with a specific result
void Build::expectResult(shared_ptr<Command> c,
                         Scenario scenario,
                         shared_ptr<Ref> ref,
                         int expected) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::ExpectResultPrinter{c, scenario, ref, expected};

  // Create an IR step and add it to the output trace
  _output.expectResult(c, scenario, ref, expected);

  // Does the resolved reference match the expected result?
  if (ref->getResultCode() != expected) {
    observeResolutionChange(c, scenario, ref, expected);
  }
}

// Command c accesses an artifact's metadata
void Build::matchMetadata(shared_ptr<Command> c,
                          Scenario scenario,
                          shared_ptr<Ref> ref,
                          shared_ptr<MetadataVersion> expected) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::MatchMetadataPrinter{c, scenario, ref, expected};

  // Create an IR step and add it to the output trace
  _output.matchMetadata(c, scenario, ref, expected);

  // We can't do anything with an unresolved reference. A change should already have been reported.
  if (!ref->isResolved()) return;

  // Perform the comparison
  ref->getArtifact()->matchMetadata(*this, c, scenario, expected);
}

// Command c accesses an artifact's content
void Build::matchContent(shared_ptr<Command> c,
                         Scenario scenario,
                         shared_ptr<Ref> ref,
                         shared_ptr<Version> expected) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::MatchContentPrinter{c, scenario, ref, expected};

  // Create an IR step and add it to the output trace
  _output.matchContent(c, scenario, ref, expected);

  // We can't do anything with an unresolved reference. A change should already have been reported.
  if (!ref->isResolved()) return;

  // Perform the comparison
  ref->getArtifact()->matchContent(*this, c, scenario, expected);
}

// Command c modifies an artifact
void Build::updateMetadata(shared_ptr<Command> c,
                           shared_ptr<Ref> ref,
                           shared_ptr<MetadataVersion> written) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::UpdateMetadataPrinter{c, ref, written};

  // Create an IR step and add it to the output trace
  _output.updateMetadata(c, ref, written);

  // We can't do anything with an unresolved reference. A change should already have been reported.
  if (!ref->isResolved()) return;

  // Make sure this version is NOT marked as committed
  written->setCommitted(false);

  // Mark the version as created by the calling command. This field is transient, so we have to
  // apply it on ever run
  written->createdBy(c);

  // Apply the write
  ref->getArtifact()->updateMetadata(*this, c, written);
}

// Command c modifies an artifact
void Build::updateContent(shared_ptr<Command> c,
                          shared_ptr<Ref> ref,
                          shared_ptr<Version> written) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::UpdateContentPrinter{c, ref, written};

  // Create an IR step and add it to the output trace
  _output.updateContent(c, ref, written);

  // We can't do anything with an unresolved reference. A change should already have been reported.
  if (!ref->isResolved()) return;

  // Make sure this version is NOT marked as committed
  written->setCommitted(false);

  // Mark the version as created by the calling command. This field is transient, so we have to
  // apply it on ever run
  written->createdBy(c);

  // Apply the write
  ref->getArtifact()->updateContent(*this, c, written);
}

/// Handle an AddEntry IR step
void Build::addEntry(shared_ptr<Command> c,
                     shared_ptr<Ref> dir,
                     fs::path name,
                     shared_ptr<Ref> target) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::AddEntryPrinter{c, dir, name, target};

  // Create an IR step and add it to the output trace
  _output.addEntry(c, dir, name, target);

  // We can't do anything with unresolved references. A change should already have been reported.
  if (!dir->isResolved() || !target->isResolved()) return;

  // Add the entry to the directory
  dir->getArtifact()->addEntry(*this, c, name, target->getArtifact());
}

/// Handle a RemoveEntry IR step
void Build::removeEntry(shared_ptr<Command> c,
                        shared_ptr<Ref> dir,
                        fs::path name,
                        shared_ptr<Ref> target) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::RemoveEntryPrinter{c, dir, name, target};

  // Create an IR step and add it to the output trace
  _output.removeEntry(c, dir, name, target);

  // We can't do anything with unresolved references. A change should already have been reported.
  if (!dir->isResolved() || !target->isResolved()) return;

  // Remove the entry from the directory
  dir->getArtifact()->removeEntry(*this, c, name, target->getArtifact());
}

// This command launches a child command
void Build::launch(shared_ptr<Command> c, shared_ptr<Command> child) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::LaunchPrinter{c, child};

  LOG(exec) << c << " launching " << child;

  // If we're emulating the launch of an unexecuted command, notify observers
  if (!child->hasExecuted()) observeCommandNeverRun(child);

  // Inform observers of the launch
  observeLaunch(c, child);

  // Add the child to the parent command's set of children
  if (c) c->addChild(child);

  // Are we going to re-execute the child?
  bool launch_command = false;

  // Should we print the child command?
  bool print_command = false;

  if (_plan.mustRerun(child)) {
    // Print the command if requested, or if this is a dry run
    if (options::print_on_run || options::dry_run) print_command = true;

    // Launch the command if this is not a dry run
    if (!options::dry_run) launch_command = true;
  }

  // Print the command if requested
  if (print_command) {
    cout << child->getShortName(options::command_length) << endl;
  }

  // If we're going to launch the command, mark it as executed now
  if (launch_command) child->setExecuted();

  // Now emit the launch IR step. This has to happen after updating the executed state of the
  // command (above) and before actually launching the command.
  _output.launch(c, child);

  // Launch the command if requested
  if (launch_command) {
    // Count the traced command
    _traced_command_count++;

    // The child command requires that its working directory exists
    child->getInitialWorkingDir()->getArtifact()->mustExist(*this, child);

    // The executable must be fully committed
    child->getExecutable()->getArtifact()->commitAll();

    // The child command also depends on the artifacts reachable through its initial FDs
    for (auto& [index, desc] : child->getInitialFDs()) {
      auto artifact = desc.getRef()->getArtifact();

      // TODO: Handle pipes eventually. Just skip them for now
      if (artifact->as<PipeArtifact>()) continue;

      if (artifact->canCommitAll()) {
        artifact->commitAll();
      } else {
        WARN << "Launching " << child << " without committing referenced artifact " << artifact;
      }
    }

    // Start the child command in the tracer
    _running[child] = _tracer.start(child);

  } else {
    // Count the emulated command
    _emulated_command_count++;
  }
}

// This command joined with a child command
void Build::join(shared_ptr<Command> c, shared_ptr<Command> child, int exit_status) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::JoinPrinter{c, child, exit_status};

  // Create an IR step and add it to the output trace
  _output.join(c, child, exit_status);

  // If the child command is running in the tracer, wait for it
  if (isRunning(child)) _tracer.wait(_running[child]);

  // Did the child command's exit status match the expected result?
  if (child->getExitStatus() != exit_status) {
    observeExitCodeChange(c, child, exit_status, child->getExitStatus());
  }
}

void Build::exit(shared_ptr<Command> c, int exit_status) noexcept {
  // If this step comes from a command we cannot emulate, skip it
  if (!_plan.canEmulate(c)) return;

  // Count an emulated step
  _emulated_step_count++;

  // Log the emulated step
  LOG(ir) << "emulated " << TracePrinter::ExitPrinter{c, exit_status};

  // Create an IR step and add it to the output trace
  _output.exit(c, exit_status);

  // Record that the command has exited
  _exited.insert(c);

  // Save the exit status for this command (TODO: remove once EXIT changes are supported for real)
  c->setExitStatus(exit_status);
}

/************************ Trace IR Steps ************************/

// A command references a new anonymous pipe
tuple<shared_ptr<Ref>, shared_ptr<Ref>> Build::tracePipeRef(shared_ptr<Command> c) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Create Refs to hold the two ends of the pipe
  auto read_end = make_shared<Ref>();
  auto write_end = make_shared<Ref>();

  // Create an IR step and add it to the output trace
  _output.pipeRef(c, read_end, write_end);

  // Resolve the reference and save the result in output
  auto pipe = _env->getPipe(*this, c);
  *read_end = Ref(AccessFlags{.r = true}, pipe);
  *write_end = Ref(AccessFlags{.w = true}, pipe);

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::PipeRefPrinter{c, read_end, write_end};

  return {read_end, write_end};
}

// A command references a new anonymous file
shared_ptr<Ref> Build::traceFileRef(shared_ptr<Command> c, mode_t mode) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Create a Ref to hold the result of the resolution
  auto output = make_shared<Ref>();

  // Create an IR step and add it to the output trace
  _output.fileRef(c, mode, output);

  // Resolve the reference and save the result in output
  *output = Ref(AccessFlags{.r = true, .w = true}, _env->createFile(*this, c, mode, true));

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::FileRefPrinter{c, mode, output};

  return output;
}

// A command references a new anonymous symlink
shared_ptr<Ref> Build::traceSymlinkRef(shared_ptr<Command> c, fs::path target) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Create a Ref to hold the result of the resolution
  auto output = make_shared<Ref>();

  // Create an IR step and add it to the output trace
  _output.symlinkRef(c, target, output);

  // Resolve the reference and save the result in output
  *output =
      Ref(AccessFlags{.r = true, .w = true, .x = true}, _env->getSymlink(*this, c, target, true));

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::SymlinkRefPrinter{c, target, output};

  return output;
}

// A command references a new anonymous directory
shared_ptr<Ref> Build::traceDirRef(shared_ptr<Command> c, mode_t mode) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Create a Ref to hold the result of the resolution
  auto output = make_shared<Ref>();

  // Create an IR step and add it to the output trace
  _output.dirRef(c, mode, output);

  // Resolve the reference and save the result in output
  *output = Ref(AccessFlags{.r = true, .w = true, .x = true}, _env->getDir(*this, c, mode, true));

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::DirRefPrinter{c, mode, output};

  return output;
}

// A command makes a reference with a path
shared_ptr<Ref> Build::tracePathRef(shared_ptr<Command> c,
                                    shared_ptr<Ref> base,
                                    fs::path path,
                                    AccessFlags flags) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Create a Ref to hold the result of the resolution
  auto output = make_shared<Ref>();

  // Create an IR step and add it to the output trace
  _output.pathRef(c, base, path, flags, output);

  // Resolve the reference and save the result in output
  ASSERT(base->isResolved()) << "Cannot resolve a path relative to an unresolved base reference.";

  *output = base->getArtifact()->resolve(*this, c, path, flags);

  // If the reference could have created a file, mark that file's versions and links as committed
  if (output->isSuccess() && flags.create) output->getArtifact()->setCommitted();

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::PathRefPrinter{c, base, path, flags, output};

  return output;
}

// A command kept a handle to a Ref
void Build::traceUsingRef(shared_ptr<Command> c, shared_ptr<Ref> ref) noexcept {
  // The command may be saving its first handle to a reference, or it could be a duplicate of an
  // existing reference. Only emit the IR step for the first open.
  if (ref->addUser(*this, c)) {
    // This is an actual IR step, so count it
    _traced_step_count++;

    // Create an IR step in the output trace
    _output.usingRef(c, ref);

    // Log the traced step
    LOG(ir) << "traced " << TracePrinter::UsingRefPrinter{c, ref};
  }
}

// A command is finished using a Ref
void Build::traceDoneWithRef(shared_ptr<Command> c, shared_ptr<Ref> ref) noexcept {
  // The command might be closing its last handle to the reference, or it could just be one of
  // several remaining handles. Use the returned refcount to catch the last close operation
  if (ref->removeUser(*this, c)) {
    // This is an actual IR step, so count it
    _traced_step_count++;

    // Create an IR step in the output trace
    _output.doneWithRef(c, ref);

    // Log the traced step
    LOG(ir) << "traced " << TracePrinter::DoneWithRefPrinter{c, ref};
  }
}

// Command c expects two references to compare with a specific result
void Build::traceCompareRefs(shared_ptr<Command> c,
                             shared_ptr<Ref> ref1,
                             shared_ptr<Ref> ref2,
                             RefComparison type) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Create an IR step and add it to the output trace
  _output.compareRefs(c, ref1, ref2, type);

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::CompareRefsPrinter{c, ref1, ref2, type};
}

// Command c expects a reference to resolve with a specific result as observed from the trace
void Build::traceExpectResult(shared_ptr<Command> c, shared_ptr<Ref> ref, int expected) noexcept {
  // Count a traced step
  _traced_step_count++;

  // If no expected result was provided, use the result from the reference itself
  if (expected == -1) expected = ref->getResultCode();

  // Create an IR step and add it to the output trace
  _output.expectResult(c, Scenario::Build, ref, expected);

  // Check the expected (i.e., observed) result against our filesystem model
  WARN_IF(ref->getResultCode() != expected)
      << "Reference resolved to " << ref->getResultCode()
      << ", which does not match syscall result " << getErrorName(expected);

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::ExpectResultPrinter{c, Scenario::Build, ref, expected};
}

// Command c accesses an artifact's metadata
void Build::traceMatchMetadata(shared_ptr<Command> c, shared_ptr<Ref> ref) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Get the artifact whose metadata is being accessed
  auto artifact = ref->getArtifact();
  ASSERT(artifact) << "Tried to access metadata through unresolved reference " << ref;

  // Get the current metadata from the artifact
  auto expected = artifact->getMetadata(*this, c, InputType::Accessed);
  ASSERT(expected) << "Unable to get metadata from " << artifact;

  // Create an IR step and add it to the output trace
  _output.matchMetadata(c, Scenario::Build, ref, expected);

  // If a different command created this version, fingerprint it for later comparison
  auto creator = expected->getCreator();
  if (creator != c && !expected->hasFingerprint()) {
    // We can only take a fingerprint with a committed path
    auto path = artifact->getPath(false);
    if (path.has_value()) {
      expected->fingerprint(*this, path.value());
    }
  }

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::MatchMetadataPrinter{c, Scenario::Build, ref, expected};
}

// Command c accesses an artifact's content
void Build::traceMatchContent(shared_ptr<Command> c,
                              shared_ptr<Ref> ref,
                              shared_ptr<Version> expected) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Get the artifact whose content is being accessed
  auto artifact = ref->getArtifact();
  ASSERT(artifact) << "Tried to access content through an unresolved reference " << ref;

  ASSERT(expected) << "Attempted to match contenet of " << artifact << " against a null version";

  // Create an IR step and add it to the output trace
  _output.matchContent(c, Scenario::Build, ref, expected);

  // If a different command created this version, fingerprint it for later comparison
  auto creator = expected->getCreator();
  if (creator != c && !expected->hasFingerprint()) {
    // We can only take a fingerprint with a committed path
    auto path = artifact->getPath(false);
    if (path.has_value()) {
      expected->fingerprint(*this, path.value());
    }
  }

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::MatchContentPrinter{c, Scenario::Build, ref, expected};
}

// Command c modifies an artifact
void Build::traceUpdateMetadata(shared_ptr<Command> c, shared_ptr<Ref> ref) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Get the artifact whose metadata is being written
  auto artifact = ref->getArtifact();
  ASSERT(artifact) << "Tried to write metadata through an unresolved reference " << ref;

  // Record the update and get the written version
  auto written = artifact->updateMetadata(*this, c);
  ASSERT(written) << "Unable to get written metadata version from " << artifact;

  // Create an IR step and add it to the output trace
  _output.updateMetadata(c, ref, written);

  // The calling command created this version
  written->createdBy(c);

  // This apply operation was traced, so the written version is committed
  written->setCommitted();

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::UpdateMetadataPrinter{c, ref, written};
}

// Command c modifies an artifact
void Build::traceUpdateContent(shared_ptr<Command> c,
                               shared_ptr<Ref> ref,
                               shared_ptr<Version> written) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Get the artifact whose content is being written
  auto artifact = ref->getArtifact();
  ASSERT(artifact) << "Tried to write content through an unresolved reference " << ref;

  // Make sure we were given a version to write
  ASSERT(written) << "Attempted to write null version to " << artifact;

  // Create an IR step and add it to the output trace
  _output.updateContent(c, ref, written);

  // This apply operation was traced, so the written version is committed
  written->setCommitted();

  // The calling command created this version
  written->createdBy(c);

  // Update the artifact's content
  artifact->updateContent(*this, c, written);

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::UpdateContentPrinter{c, ref, written};
}

// A traced command is adding an entry to a directory
void Build::traceAddEntry(shared_ptr<Command> c,
                          shared_ptr<Ref> dir,
                          fs::path name,
                          shared_ptr<Ref> target) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Get the directory artifact that is being added to
  auto dir_artifact = dir->getArtifact();
  ASSERT(dir_artifact) << "Tried to add an entry to an unresolved reference";

  // Make sure the reference to the artifact being linked is resolved
  ASSERT(target->isResolved()) << "Cannot add entry " << name << " to " << dir_artifact
                               << " using unresolved reference " << target;

  // Create an IR step and add it to the output trace
  _output.addEntry(c, dir, name, target);

  // Add the entry to the directory and mark the update as committed
  dir_artifact->addEntry(*this, c, name, target->getArtifact())->setCommitted();

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::AddEntryPrinter{c, dir, name, target};
}

// A traced command is removing an entry from a directory
void Build::traceRemoveEntry(shared_ptr<Command> c,
                             shared_ptr<Ref> dir,
                             fs::path name,
                             shared_ptr<Ref> target) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Get the directory artifact that is being removed from
  auto dir_artifact = dir->getArtifact();
  ASSERT(dir_artifact) << "Tried to add an entry to an unresolved reference";

  // Make sure the reference to the artifact being linked is resolved
  ASSERT(target->isResolved()) << "Cannot remove entry " << name << " from " << dir_artifact
                               << " using unresolved reference " << target;

  // Create an IR step and add it to the output trace
  _output.removeEntry(c, dir, name, target);

  // Remove the entry from the directory and mark the update as committed
  dir_artifact->removeEntry(*this, c, name, target->getArtifact())->setCommitted();

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::RemoveEntryPrinter{c, dir, name, target};
}

// This command launches a child command
shared_ptr<Command> Build::traceLaunch(shared_ptr<Command> c,
                                       shared_ptr<Ref> exe_ref,
                                       vector<string> args,
                                       map<int, FileDescriptor> fds,
                                       shared_ptr<Ref> cwd_ref,
                                       shared_ptr<Ref> root_ref) noexcept {
  // Count a traced step and a traced command
  _traced_step_count++;
  _traced_command_count++;

  // Look to see if the current command has a matching child command
  auto child = c->findChild(exe_ref, args, fds, cwd_ref, root_ref);

  // Did we find a matching command?
  if (child) {
    LOG(exec) << "Matched command " << child;
  } else {
    child = make_shared<Command>(exe_ref, args, fds, cwd_ref, root_ref);
    LOG(exec) << "No match for command " << child;
  }

  // The child command will be executed by this build.
  child->setExecuted();

  // Create an IR step and add it to the output trace
  _output.launch(c, child);

  // Inform observers of the launch
  observeLaunch(c, child);

  // Show the command if printing is on, or if this is a dry run
  if (options::print_on_run) {
    cout << child->getShortName(options::command_length) << endl;
  }

  // The child command requires that its working directory exists
  child->getInitialWorkingDir()->getArtifact()->mustExist(*this, child);

  // The executable must be fully committed
  child->getExecutable()->getArtifact()->commitAll();

  // The child command also depends on the artifacts reachable through its initial FDs
  for (auto& [index, desc] : child->getInitialFDs()) {
    auto artifact = desc.getRef()->getArtifact();

    // TODO: Handle pipes eventually. Just skip them for now
    if (artifact->as<PipeArtifact>()) continue;

    if (artifact->canCommitAll()) {
      artifact->commitAll();
    } else {
      WARN << "Launching " << child << " without committing referenced artifact " << artifact;
    }
  }

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::LaunchPrinter{c, child};

  // Return the child command to the caller
  return child;
}

// This command joined with a child command
void Build::traceJoin(shared_ptr<Command> c, shared_ptr<Command> child, int exit_status) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Create an IR step and add it to the output trace
  _output.join(c, child, exit_status);

  // Save the exit status in the child (TODO: Remove this once we know Build::exit works)
  child->setExitStatus(exit_status);

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::JoinPrinter{c, child, exit_status};
}

void Build::traceExit(shared_ptr<Command> c, int exit_status) noexcept {
  // Count a traced step
  _traced_step_count++;

  // Create an IR step and add it to the output trace
  _output.exit(c, exit_status);

  // Record that the command has exited
  _exited.insert(c);

  // Save the exit status for this command (TODO: remove once EXIT changes are supported for real)
  c->setExitStatus(exit_status);

  // Log the traced step
  LOG(ir) << "traced " << TracePrinter::ExitPrinter{c, exit_status};
}
