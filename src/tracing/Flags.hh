#pragma once

#include <ostream>

#include <fcntl.h>
#include <fmt/core.h>
#include <sys/stat.h>
#include <sys/types.h>

using std::ostream;

/****** Pretty Printers for Syscall Arguments ******/

/// A wrapper for file descriptors used as the base in the at* system calls
class at_fd {
 public:
  /// Create an at_fd from a file descriptor
  explicit at_fd(int fd) noexcept : _fd(fd) {}

  /// Create an at_fd that indicates the current working directory
  static at_fd cwd() noexcept { return at_fd(AT_FDCWD); }

  /// Does this fd indiciate the current working directory?
  bool isCWD() const noexcept { return _fd == AT_FDCWD; }

  /// Get the file descriptor number
  int getFD() const noexcept { return _fd; }

  friend ostream& operator<<(ostream& o, const at_fd& v) noexcept {
    if (v._fd == AT_FDCWD) {
      return o << "AT_FDCWD";
    } else {
      return o << v._fd;
    }
  }

 private:
  int _fd;
};

class mode_flags {
 public:
  mode_flags(mode_t mode) : _mode(mode) {}

  /// Check if the flags include specific option
  template <int flag>
  bool has() const noexcept {
    return (_mode & flag) == flag;
  }

  bool isSocket() const noexcept { return (_mode & S_IFMT) == S_IFSOCK; }
  bool isSymlink() const noexcept { return (_mode & S_IFMT) == S_IFLNK; }
  bool isRegularFile() const noexcept { return (_mode & S_IFMT) == S_IFREG; }
  bool isBlockDevice() const noexcept { return (_mode & S_IFMT) == S_IFBLK; }
  bool isCharDevice() const noexcept { return (_mode & S_IFMT) == S_IFCHR; }
  bool isFIFO() const noexcept { return (_mode & S_IFMT) == S_IFIFO; }

  bool userRead() const noexcept { return has<S_IRUSR>(); }
  bool userWrite() const noexcept { return has<S_IWUSR>(); }
  bool userExecute() const noexcept { return has<S_IXUSR>(); }
  bool groupRead() const noexcept { return has<S_IRGRP>(); }
  bool groupWrite() const noexcept { return has<S_IWGRP>(); }
  bool groupExecute() const noexcept { return has<S_IXGRP>(); }
  bool otherRead() const noexcept { return has<S_IROTH>(); }
  bool otherWrite() const noexcept { return has<S_IWOTH>(); }
  bool otherExecute() const noexcept { return has<S_IXOTH>(); }
  bool setUID() const noexcept { return has<S_ISUID>(); }
  bool setGID() const noexcept { return has<S_ISGID>(); }
  bool sticky() const noexcept { return has<S_ISVTX>(); }

  mode_t getMode() const noexcept { return _mode; }

  friend ostream& operator<<(ostream& o, const mode_flags& p) noexcept {
    if (p._mode == 0) return o << 0;

    o << (p.userRead() ? 'r' : '-');
    o << (p.userWrite() ? 'w' : '-');
    o << (p.userExecute() ? (p.setUID() ? 's' : 'x') : (p.setUID() ? 'S' : '-'));
    o << (p.groupRead() ? 'r' : '-');
    o << (p.groupWrite() ? 'w' : '-');
    o << (p.groupExecute() ? (p.setGID() ? 's' : 'x') : (p.setGID() ? 'S' : '-'));
    o << (p.otherRead() ? 'r' : '-');
    o << (p.otherWrite() ? 'w' : '-');
    o << (p.otherExecute() ? (p.sticky() ? 't' : 'x') : (p.sticky() ? 'T' : '-'));

    o << fmt::format(" ({:o})", p._mode);
    return o;
  }

 private:
  mode_t _mode;
};

/// A wrapper for O_* flags provided to system calls
class o_flags {
 public:
  /// Create a default o_flags value
  o_flags() noexcept : _flags(0) {}

  /// Create a wrapper for O_* flags from an integer value
  explicit o_flags(int flags) noexcept : _flags(flags) {}

  /// Do the flags include a request for read access?
  bool readable() const noexcept { return has<O_RDWR>() || (has<O_RDONLY>() && !has<O_WRONLY>()); }

  /// Do the flags include a request for write access?
  bool writable() const noexcept { return has<O_RDWR>() || has<O_WRONLY>(); }

  bool append() const noexcept { return has<O_APPEND>(); }
  bool cloexec() const noexcept { return has<O_CLOEXEC>(); }
  bool creat() const noexcept { return has<O_CREAT>(); }
  bool directory() const noexcept { return has<O_DIRECTORY>(); }
  bool excl() const noexcept { return has<O_EXCL>(); }
  bool nofollow() const noexcept { return has<O_NOFOLLOW>(); }
  bool tmpfile() const noexcept { return has<O_TMPFILE>(); }
  bool trunc() const noexcept { return has<O_TRUNC>(); }

  friend ostream& operator<<(ostream& o, const o_flags& p) noexcept {
    bool noflag = true;

    // decode O_RDWR, O_RDONLY, O_WRONLY. Check O_RDWR first in case O_RDWR == O_RDONLY | O_WRONLY
    if ((p._flags & O_RDWR) == O_RDWR) {
      o << "O_RDWR";
      noflag = false;
    } else if ((p._flags & O_WRONLY) == O_WRONLY) {
      o << "O_WRONLY";
      noflag = false;
    } else if ((p._flags & O_RDONLY) == O_RDONLY) {
      o << "O_RDONLY";
      noflag = false;
    }

    // pretty printer
    auto dec = [&](int flag, const char* fstr) {
      if ((p._flags & flag) == flag) {
        if (!noflag) o << "|";
        o << fstr;
        noflag = false;
      }
    };

    // decode the rest
    dec(O_APPEND, "O_APPEND");
    dec(O_CLOEXEC, "O_CLOEXEC");
    dec(O_CREAT, "O_CREAT");
    dec(O_DIRECT, "O_DIRECT");
    dec(O_DIRECTORY, "O_DIRECTORY");
    dec(O_EXCL, "O_EXCL");
    dec(O_NOCTTY, "O_NOCTTY");
    dec(O_NOFOLLOW, "O_NOFOLLOW");
    dec(O_NONBLOCK, "O_NONBLOCK");
    dec(O_TMPFILE, "O_TMPFILE");
    dec(O_TRUNC, "O_TRUNC");

    // append flags in octal
    o << fmt::format(" ({:o})", p._flags);

    return o;
  }

 private:
  /// Check if the flags include specific option
  template <int flag>
  bool has() const noexcept {
    return (_flags & flag) == flag;
  }

 private:
  int _flags;
};

class at_flags {
 public:
  at_flags() noexcept : _flags(0) {}

  at_flags(int flags) noexcept : _flags(flags) {}

  bool eaccess() const noexcept { return has<AT_EACCESS>(); }
  bool empty_path() const noexcept { return has<AT_EMPTY_PATH>(); }
  bool symlink_follow() const noexcept { return has<AT_SYMLINK_FOLLOW>(); }
  bool symlink_nofollow() const noexcept { return has<AT_SYMLINK_NOFOLLOW>(); }
  bool removedir() const noexcept { return has<AT_REMOVEDIR>(); }

  friend ostream& operator<<(ostream& o, const at_flags& p) noexcept {
    if (p._flags == 0) return o << 0;

    bool noflag = true;

    // pretty printer
    auto dec = [&](int flag, const char* fstr) {
      if ((p._flags & flag) == flag) {
        if (!noflag) o << "|";
        o << fstr;
        noflag = false;
      }
    };

    // Decode the flags
    dec(AT_EACCESS, "AT_EACCESS");
    dec(AT_EMPTY_PATH, "AT_EMPTY_PATH");
    dec(AT_SYMLINK_FOLLOW, "AT_SYMLINK_FOLLOW");
    dec(AT_SYMLINK_NOFOLLOW, "AT_SYMLINK_NOFOLLOW");
    dec(AT_REMOVEDIR, "AT_REMOVEDIR");

    return o << fmt::format(" ({:o})", p._flags);
  }

 private:
  // Check if the flags include specific option
  template <int flag>
  bool has() const noexcept {
    return (_flags & flag) == flag;
  }

 private:
  int _flags;
};

class rename_flags {
 public:
  rename_flags() noexcept : _flags(0) {}

  rename_flags(int flags) noexcept : _flags(flags) {}

  bool exchange() const noexcept { return has<RENAME_EXCHANGE>(); }
  bool noreplace() const noexcept { return has<RENAME_NOREPLACE>(); }
  bool whiteout() const noexcept { return has<RENAME_WHITEOUT>(); }

  friend ostream& operator<<(ostream& o, const rename_flags& p) noexcept {
    bool noflag = true;

    // pretty printer
    auto dec = [&](int flag, const char* fstr) {
      if ((p._flags & flag) == flag) {
        if (!noflag) o << "|";
        o << fstr;
        noflag = false;
      }
    };

    // Decode the flags
    dec(RENAME_EXCHANGE, "RENAME_EXCHANGE");
    dec(RENAME_NOREPLACE, "RENAME_NOREPLACE");
    dec(RENAME_WHITEOUT, "RENAME_WHITEOUT");

    return o << fmt::format(" ({:o})", p._flags);
  }

 private:
  // Check if the flags include specific option
  template <int flag>
  bool has() const noexcept {
    return (_flags & flag) == flag;
  }

 private:
  int _flags;
};
