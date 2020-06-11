#include "Command.hh"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "artifacts/Artifact.hh"
#include "build/Build.hh"
#include "core/AccessFlags.hh"
#include "core/FileDescriptor.hh"
#include "core/IR.hh"
#include "ui/options.hh"
#include "util/path.hh"
#include "versions/ContentVersion.hh"
#include "versions/MetadataVersion.hh"
#include "versions/Version.hh"

using std::cout;
using std::endl;
using std::make_shared;
using std::map;
using std::shared_ptr;
using std::string;

namespace fs = std::filesystem;

// The root command invokes "dodo launch" to run the actual build script. Construct this command.
shared_ptr<Command> Command::createRootCommand() {
  // We need to get the path to dodo. Use readlink for this.
  fs::path dodo = readlink("/proc/self/exe");
  fs::path dodo_launch = dodo.parent_path() / "dodo-launch";

  auto stdin_ref = make_shared<Pipe>();
  auto stdout_ref = make_shared<Pipe>();
  auto stderr_ref = make_shared<Pipe>();

  map<int, FileDescriptor> default_fds = {{0, FileDescriptor(stdin_ref, false)},
                                          {1, FileDescriptor(stdout_ref, true)},
                                          {2, FileDescriptor(stderr_ref, true)}};

  shared_ptr<Command> root(new Command(dodo_launch, {"dodo-launch"}, default_fds));

  return root;
}

string Command::getShortName() const {
  // By default, the short name is the executable
  auto result = _exe;

  // If we have arguments, use args[0] instead of the exe name
  if (_args.size() > 0) result = _args.front();

  // Strip path from the base name
  auto pos = result.rfind('/');
  if (pos != string::npos) {
    result = result.substr(pos + 1);
  }

  // Add arguments up to a length of 20 characters
  size_t index = 1;
  while (index < _args.size() && result.length() < 20) {
    result += " " + _args[index];
    index++;
  }

  if (result.length() >= 20) {
    result = result.substr(0, 17) + "...";
  }

  return result;
}

string Command::getFullName() const {
  string result;
  bool first = true;
  for (const string& arg : _args) {
    if (!first) result += " ";
    first = false;
    result += arg;
  }
  return result;
}

void Command::emulate(Build& build) {
  // If this command has never run, report it as changed
  if (_steps.empty()) build.observeCommandNeverRun(shared_from_this());

  for (auto step : _steps) {
    step->emulate(shared_from_this(), build);
  }
}

// This command accesses an artifact by path.
shared_ptr<Access> Command::access(string path, AccessFlags flags) {
  auto ref = make_shared<Access>(path, flags);
  _steps.push_back(ref);
  return ref;
}

// This command creates a reference to a new pipe
shared_ptr<Pipe> Command::pipe() {
  auto ref = make_shared<Pipe>();
  _steps.push_back(ref);
  return ref;
}

// This command observes a reference resolve with a particular result
void Command::referenceResult(const shared_ptr<Reference>& ref, int result) {
  _steps.push_back(make_shared<ReferenceResult>(ref, result));
}

// This command depends on the metadata of a referenced artifact
void Command::metadataMatch(const shared_ptr<Reference>& ref) {
  ASSERT(ref->isResolved()) << "Cannot check for a metadata match on an unresolved reference.";

  // Do we have to log this read?
  if (!_metadata_filter.readRequired(this, ref)) return;

  // Inform the artifact that this command accesses its metadata
  auto v = ref->getArtifact()->accessMetadata(shared_from_this(), ref);

  // If the last write was from this command we don't need a fingerprint to compare.
  if (v->getCreator() != shared_from_this()) {
    v->fingerprint(ref);
  }

  // Add the IR step
  _steps.push_back(make_shared<MetadataMatch>(ref, v));

  // Report the read
  _metadata_filter.read(this, ref);
}

// This command depends on the contents of a referenced artifact
void Command::contentsMatch(const shared_ptr<Reference>& ref) {
  ASSERT(ref->isResolved()) << "Cannot check for a content match on an unresolved reference.";

  // Do we have to log this read?
  if (!_content_filter.readRequired(this, ref)) return;

  // Inform the artifact that this command accesses its contents
  auto v = ref->getArtifact()->accessContents(shared_from_this(), ref);

  if (!v) {
    WARN << "Accessing contents of " << ref << " returned a null version";
    return;
  }

  // If the last write was from this command, we don't need a fingerprint to compare.
  if (v->getCreator() != shared_from_this()) {
    v->fingerprint(ref);
  }

  // Add the IR step
  _steps.push_back(make_shared<ContentsMatch>(ref, v));

  // Report the read
  _content_filter.read(this, ref);
}

// This command sets the metadata of a referenced artifact
void Command::setMetadata(const shared_ptr<Reference>& ref) {
  ASSERT(ref->isResolved()) << "Cannot set metadata for an unresolved reference.";

  // Do we have to log this write?
  if (!_metadata_filter.writeRequired(this, ref)) return;

  // Inform the artifact that this command sets its metadata
  auto v = ref->getArtifact()->setMetadata(shared_from_this(), ref);

  // Create the SetMetadata step and add it to the command
  _steps.push_back(make_shared<SetMetadata>(ref, v));

  // Report the write
  _metadata_filter.write(this, ref, v);
}

// This command sets the contents of a referenced artifact
void Command::setContents(const shared_ptr<Reference>& ref) {
  ASSERT(ref->isResolved()) << "Cannot set contents for an unresolved reference.";

  // Do we have to log this write?
  if (!_content_filter.writeRequired(this, ref)) return;

  // Inform the artifact that this command sets its contents
  auto v = ref->getArtifact()->setContents(shared_from_this(), ref);

  ASSERT(v) << "Setting contents of " << ref << " produced a null version";

  // Create the SetContents step and add it to the command
  _steps.push_back(make_shared<SetContents>(ref, v));

  // Report the write
  _content_filter.write(this, ref, v);
}

// This command launches a child command
shared_ptr<Command> Command::launch(string exe, vector<string> args, map<int, FileDescriptor> fds) {
  auto child = make_shared<Command>(exe, args, fds);

  if (options::print_on_run) cout << child->getFullName() << endl;

  _steps.push_back(make_shared<Launch>(child));
  _children.push_back(child);

  return child;
}

// Record the effect of a read by command c using reference ref
void Command::AccessFilter::read(Command* c, shared_ptr<Reference> ref) {
  // Command c can read through reference ref without logging until the next write
  _observed.emplace(c, ref.get());
}

// Does command c need to add a read through reference ref to its trace?
bool Command::AccessFilter::readRequired(Command* c, shared_ptr<Reference> ref) {
  // If this optimization is disabled, the read is always required
  if (!options::combine_reads) return true;

  // If this command has already read through this reference, the read is not required
  if (_observed.find({c, ref.get()}) != _observed.end()) return false;

  // Otherwise the read is required
  return true;
}

// Record the effect of a write by command c using reference ref
void Command::AccessFilter::write(Command* c, shared_ptr<Reference> ref,
                                  shared_ptr<Version> written) {
  // All future reads could be affected by this write, so they need to be logged
  _observed.clear();

  // Keep track of the last write
  _last_writer = c;
  _last_write_ref = ref.get();
  _last_written_version = written.get();

  // The writer can observe its own written value without logging
  _observed.emplace(c, ref.get());
}

// Does command c need to add a write through reference ref to its trace?
bool Command::AccessFilter::writeRequired(Command* c, shared_ptr<Reference> ref) {
  // If this optimization is disabled, the write is always required
  if (!options::combine_writes) return true;

  // If this is the first write through the filter, it must be added to the trace
  if (!_last_written_version) return true;

  // If the last version written through this filter was accessed, add a new write to the trace
  if (_last_written_version->isAccessed()) return true;

  // If a different command is writing, add a new write to the trace
  if (c != _last_writer) return true;

  // If the same command is using a different reference to write, add a new write to the trace
  if (ref.get() != _last_write_ref) return true;

  // This write is by the same command as the last write using the same reference.
  // The previously-written value has not been accessed, so we do not need to log a new write.
  return false;
}