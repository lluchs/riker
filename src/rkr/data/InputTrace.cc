#include "InputTrace.hh"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cereal/archives/binary.hpp>

#include "data/DefaultTrace.hh"
#include "data/Record.hh"
#include "runtime/Command.hh"
#include "util/log.hh"

using std::make_shared;
using std::make_unique;
using std::map;
using std::nullopt;
using std::optional;
using std::shared_ptr;
using std::string;
using std::tuple;
using std::unique_ptr;
using std::vector;

InputTrace::InputTrace(string filename, vector<string> args) :
    _input(filename, std::ios::binary), _archive(_input), _args(args) {
  // Load the version header from the trace file
  size_t magic;
  size_t version;
  _archive(magic, version);

  // Check the magic number and version
  if (magic != ArchiveMagic) {
    WARN << "Saved trace does appears to be invalid. Running a full build.";
    throw cereal::Exception("Wrong magic number");

  } else if (version != ArchiveVersion) {
    WARN << "Saved trace is not the correct version. Running a full build.";
    throw cereal::Exception("Wrong version");
  }

  // Add the root command to the command map
  IRLoader::addCommand(0, make_shared<Command>());
}

unique_ptr<InputTrace> InputTrace::load(string filename, vector<string> args) noexcept {
  try {
    // Try to create an input trace and return it
    return unique_ptr<InputTrace>(new InputTrace(filename, args));

  } catch (cereal::Exception& e) {
    // If there is an exception when loading the trace the load fails
    return nullptr;
  }
}

// Run this trace
void InputTrace::sendTo(IRSink& handler) noexcept {
  // Make sure the file stream is at the start of the file
  _input.clear();
  _input.seekg(0);

  size_t magic;
  size_t version;
  _archive(magic, version);

  // Loop until we hit the end of the trace
  bool done = false;
  while (!done) {
    unique_ptr<Record> record;
    _archive(record);
    done = record->isEnd();
    record->handle(*this, handler);
  }
}
