// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <streambuf>

#include <gflags/gflags.h>
#include <rocksdb/file_system.h>
#include <util/testutil.h>

#include "fs/fs_zenfs.h"

using GFLAGS_NAMESPACE::ParseCommandLineFlags;
using GFLAGS_NAMESPACE::RegisterFlagValidator;
using GFLAGS_NAMESPACE::SetUsageMessage;

DEFINE_string(zbd, "", "Path to a zoned block device.");
DEFINE_string(aux_path, "", "Path for auxiliary file storage (log and lock files).");
DEFINE_bool(force, false, "Force file system creation.");
DEFINE_string(path, "", "File path");
DEFINE_int32(finish_threshold, 0, "Finish used zones if less than x% left");
DEFINE_string(restore_path, "", "Path to restore files");
DEFINE_string(backup_path, "", "Path to backup files");
DEFINE_int32(max_active_zones, 0, "Max active zone limit");
DEFINE_int32(max_open_zones, 0, "Max active zone limit");

namespace ROCKSDB_NAMESPACE {

ZonedBlockDevice *zbd_open(bool readonly) {
  auto logger = std::make_shared<test::NullLogger>();
  ZonedBlockDevice *zbd = new ZonedBlockDevice(FLAGS_zbd, logger);
  IOStatus open_status = zbd->Open(readonly);

  if (!open_status.ok()) {
    fprintf(stderr, "Failed to open zoned block device: %s, error: %s\n", FLAGS_zbd.c_str(),
            open_status.ToString().c_str());
    delete zbd;
    return nullptr;
  }

  return zbd;
}

Status zenfs_mount(ZonedBlockDevice *zbd, ZenFS **zenFS, bool readonly, bool formating = false) {
  Status s;
  auto logger = std::make_shared<test::NullLogger>();
  auto metrics = std::make_shared<BytedanceMetrics>(std::make_shared<ByteDanceMetricsReporterFactory>(), "", logger);
  *zenFS = new ZenFS(zbd, FileSystem::Default(), logger, metrics);
  s = (*zenFS)->Mount(readonly, formating);
  if (!s.ok()) {
    delete *zenFS;
    *zenFS = nullptr;
  }

  // Wait till all reset tasks finished
  std::unique_lock<std::mutex> lk(zbd->metazone_reset_mtx_);
  return s;
}

int zenfs_tool_mkfs() {
  Status s;
  DIR *aux_dir;

  if (FLAGS_aux_path.empty()) {
    fprintf(stderr, "You need to specify --aux_path\n");
    return 1;
  }

  aux_dir = opendir(FLAGS_aux_path.c_str());
  if (ENOENT != errno) {
    fprintf(stderr, "Error: aux path exists\n");
    closedir(aux_dir);
    return 1;
  }

  ZonedBlockDevice *zbd = zbd_open(false);
  if (zbd == nullptr) return 1;

  ZenFS *zenFS;
  s = zenfs_mount(zbd, &zenFS, false, true);
  if ((s.ok() || !s.IsNotFound()) && !FLAGS_force) {
    fprintf(stderr, "Existing filesystem found, use --force if you want to replace it.\n");
    return 1;
  }

  if (zenFS != nullptr) delete zenFS;

  zbd = zbd_open(false);

  auto logger = std::make_shared<test::NullLogger>();
  auto metrics = std::make_shared<BytedanceMetrics>(std::make_shared<ByteDanceMetricsReporterFactory>(), "", logger);
  zenFS = new ZenFS(zbd, FileSystem::Default(), logger, metrics);

  if (FLAGS_aux_path.back() != '/') FLAGS_aux_path.append("/");

  s = zenFS->MkFS(FLAGS_aux_path, FLAGS_finish_threshold, FLAGS_max_open_zones, FLAGS_max_active_zones);
  if (!s.ok()) {
    fprintf(stderr, "Failed to create file system, error: %s\n", s.ToString().c_str());
    delete zenFS;
    return 1;
  }

  fprintf(stdout, "ZenFS file system created. Free space: %lu MB\n", zbd->GetFreeSpace() / (1024 * 1024));

  delete zenFS;
  return 0;
}

void list_children(ZenFS *zenFS, std::string path) {
  IOOptions opts;
  IODebugContext dbg;
  std::vector<std::string> result;
  uint64_t size;
  IOStatus io_status = zenFS->GetChildren(path, opts, &result, &dbg);

  if (!io_status.ok()) {
    fprintf(stderr, "Error: %s %s\n", io_status.ToString().c_str(), path.c_str());
    return;
  }

  for (const auto &f : result) {
    io_status = zenFS->GetFileSize(path + f, opts, &size, &dbg);
    if (!io_status.ok()) {
      fprintf(stderr, "Failed to get size of file %s\n", f.c_str());
      return;
    }
    uint64_t mtime;
    io_status = zenFS->GetFileModificationTime(path + f, opts, &mtime, &dbg);
    if (!io_status.ok()) {
      fprintf(stderr, "Failed to get modification time of file %s, error = %s\n", f.c_str(),
              io_status.ToString().c_str());
      return;
    }
    time_t mt = (time_t)mtime;
    struct tm *fct = std::localtime(&mt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%b %d %Y %H:%M:%S", fct);
    std::string mdtime;
    mdtime.assign(buf, sizeof(buf));

    fprintf(stdout, "%12lu\t%-32s%-32s\n", size, mdtime.c_str(), f.c_str());
  }
}

void format_path(std::string &path) {
  std::size_t pos = path.find('/', 0);
  while (pos != std::string::npos) {
    while (path.compare(pos + 1, 1, "/") == 0) {
      path.erase(pos + 1, 1);
    }
    pos = path.find('/', pos + 1);
  }

  if (path.front() == '/') path.erase(0, 1);

  if (path.back() != '/') path = path + "/";
}

int zenfs_tool_list() {
  Status s;
  ZonedBlockDevice *zbd = zbd_open(true);
  if (zbd == nullptr) return 1;

  ZenFS *zenFS;
  s = zenfs_mount(zbd, &zenFS, true);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n", s.ToString().c_str());
    return 1;
  }

  format_path(FLAGS_path);
  list_children(zenFS, FLAGS_path);

  return 0;
}

int zenfs_tool_df() {
  Status s;
  ZonedBlockDevice *zbd = zbd_open(true);
  if (zbd == nullptr) return 1;

  ZenFS *zenFS;
  s = zenfs_mount(zbd, &zenFS, true);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n", s.ToString().c_str());
    return 1;
  }
  uint64_t used = zbd->GetUsedSpace();
  uint64_t free = zbd->GetFreeSpace();
  uint64_t reclaimable = zbd->GetReclaimableSpace();

  /* Avoid divide by zero */
  if (used == 0) used = 1;

  fprintf(stdout, "Free: %lu MB\nUsed: %lu MB\nReclaimable: %lu MB\nSpace amplification: %lu%%\n", free / (1024 * 1024),
          used / (1024 * 1024), reclaimable / (1024 * 1024), (100 * reclaimable) / used);

  return 0;
}

int zenfs_tool_stat() {
  Status s;
  ZonedBlockDevice *zbd = zbd_open(true);
  if (zbd == nullptr) return 1;

  ZenFS *zenFS;
  s = zenfs_mount(zbd, &zenFS, true);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n", s.ToString().c_str());
    return 1;
  }
  auto stat = zenFS->GetStat();

  for (auto &&zone : stat) {
    std::cout << "Zone total=" << zone.total_capacity << " write_position=" << zone.write_position
              << " start_position=" << zone.start_position << std::endl;
    for (auto &&file : zone.files) {
      std::cout << "  [" << file.file_id << "] " << file.filename << " " << file.size_in_zone << std::endl;
    }
  }

  return 0;
}

int zenfs_tool_lsuuid() {
  std::map<std::string, std::string>::iterator it;
  std::map<std::string, std::string> zenFileSystems = ListZenFileSystems();

  for (it = zenFileSystems.begin(); it != zenFileSystems.end(); it++)
    fprintf(stdout, "%s\t%s\n", it->first.c_str(), it->second.c_str());

  return 0;
}

static std::map<std::string, Env::WriteLifeTimeHint> wlth_map;

Env::WriteLifeTimeHint GetWriteLifeTimeHint(std::string filename) {
  if (wlth_map.find(filename) != wlth_map.end()) {
    return wlth_map[filename];
  }
  return Env::WriteLifeTimeHint::WLTH_NOT_SET;
}

int SaveWriteLifeTimeHints() {
  std::ofstream wlth_file(FLAGS_path + "/write_lifetime_hints.dat");

  if (!wlth_file.is_open()) {
    fprintf(stderr, "Failed to store time hints\n");
    return 1;
  }

  for (auto it = wlth_map.begin(); it != wlth_map.end(); it++) {
    wlth_file << it->first << "\t" << it->second << "\n";
  }

  wlth_file.close();
  return 0;
}

void ReadWriteLifeTimeHints() {
  std::ifstream wlth_file(FLAGS_path + "/write_lifetime_hints.dat");

  if (!wlth_file.is_open()) {
    fprintf(stderr, "WARNING: failed to read write life times\n");
    return;
  }

  std::string filename;
  uint32_t lth;

  while (wlth_file >> filename >> lth) {
    wlth_map.insert(std::make_pair(filename, (Env::WriteLifeTimeHint)lth));
    fprintf(stdout, "read: %s %u \n", filename.c_str(), lth);
  }

  wlth_file.close();
}

IOStatus zenfs_tool_copy_file(FileSystem *f_fs, std::string f, FileSystem *t_fs, std::string t) {
  FileOptions fopts;
  IOOptions iopts;
  IODebugContext dbg;
  IOStatus s;
  std::unique_ptr<FSSequentialFile> f_file;
  std::unique_ptr<FSWritableFile> t_file;
  size_t buffer_sz = 1024 * 1024;
  uint64_t to_copy;
  char *buffer;

  fprintf(stdout, "%s\n", f.c_str());

  s = f_fs->GetFileSize(f, iopts, &to_copy, &dbg);
  if (!s.ok()) {
    return s;
  }

  s = f_fs->NewSequentialFile(f, fopts, &f_file, &dbg);
  if (!s.ok()) {
    return s;
  }

  s = t_fs->NewWritableFile(t, fopts, &t_file, &dbg);
  if (!s.ok()) {
    return s;
  }

  t_file->SetWriteLifeTimeHint(GetWriteLifeTimeHint(t));

  buffer = (char *)malloc(buffer_sz);
  if (buffer == nullptr) {
    return IOStatus::IOError("Failed to allocate copy buffer");
  }

  while (to_copy > 0) {
    size_t chunk_sz = to_copy;
    Slice chunk_slice;

    if (chunk_sz > buffer_sz) chunk_sz = buffer_sz;

    s = f_file->Read(chunk_sz, iopts, &chunk_slice, buffer, &dbg);
    if (!s.ok()) {
      break;
    }

    s = t_file->Append(chunk_slice, iopts, &dbg);
    to_copy -= chunk_slice.size();
  }

  free(buffer);
  if (!s.ok()) {
    return s;
  }

  return t_file->Fsync(iopts, &dbg);
}

IOStatus zenfs_tool_copy_dir(FileSystem *f_fs, std::string f_dir, FileSystem *t_fs, std::string t_dir) {
  IOOptions opts;
  IODebugContext dbg;
  IOStatus s;
  std::vector<std::string> files;

  s = f_fs->GetChildren(f_dir, opts, &files, &dbg);
  if (!s.ok()) {
    return s;
  }

  for (const auto &f : files) {
    std::string filename = f_dir + f;
    bool is_dir;

    if (f == "." || f == ".." || f == "write_lifetime_hints.dat") continue;

    s = f_fs->IsDirectory(filename, opts, &is_dir, &dbg);
    if (!s.ok()) {
      return s;
    }

    std::string dest_filename;

    if (t_dir == "") {
      dest_filename = f;
    } else {
      dest_filename = t_dir + "/" + f;
    }

    if (is_dir) {
      s = t_fs->CreateDir(dest_filename, opts, &dbg);
      if (!s.ok()) {
        return s;
      }
      s = zenfs_tool_copy_dir(f_fs, filename + "/", t_fs, dest_filename);
      if (!s.ok()) {
        return s;
      }
    } else {
      s = zenfs_tool_copy_file(f_fs, filename, t_fs, dest_filename);
      if (!s.ok()) {
        return s;
      }
    }
  }

  return s;
}

int zenfs_tool_backup() {
  Status status;
  IOStatus io_status;
  ZonedBlockDevice *zbd;
  ZenFS *zenFS;

  zbd = zbd_open(false);
  if (zbd == nullptr) return 1;

  status = zenfs_mount(zbd, &zenFS, false);
  if (!status.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n", status.ToString().c_str());
    return 1;
  }

  if (!FLAGS_backup_path.empty() && FLAGS_backup_path.back() != '/') {
    std::string dest_filename = FLAGS_path + "/" + FLAGS_backup_path.substr(FLAGS_backup_path.find_last_of('/') + 1);
    io_status = zenfs_tool_copy_file(zenFS, FLAGS_backup_path, FileSystem::Default().get(), dest_filename);
  } else {
    io_status = zenfs_tool_copy_dir(zenFS, FLAGS_backup_path, FileSystem::Default().get(), FLAGS_path);
  }
  if (!io_status.ok()) {
    fprintf(stderr, "Copy failed, error: %s\n", io_status.ToString().c_str());
    return 1;
  }

  wlth_map = zenFS->GetWriteLifeTimeHints();
  return SaveWriteLifeTimeHints();
}

int zenfs_tool_restore() {
  Status status;
  IOStatus io_status;
  ZonedBlockDevice *zbd;
  ZenFS *zenFS;

  if (FLAGS_restore_path.empty()) {
    fprintf(stderr, "Error: Specify --restore_path=<db path> to restore the db\n");
    return 1;
  }

  ReadWriteLifeTimeHints();

  zbd = zbd_open(false);
  if (zbd == nullptr) return 1;

  status = zenfs_mount(zbd, &zenFS, false);
  if (!status.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n", status.ToString().c_str());
    return 1;
  }

  io_status = zenfs_tool_copy_dir(FileSystem::Default().get(), FLAGS_path, zenFS, FLAGS_restore_path);
  if (!io_status.ok()) {
    fprintf(stderr, "Copy failed, error: %s\n", io_status.ToString().c_str());
    return 1;
  }

  return 0;
}

int zenfs_tool_dump() {
  Status s;
  ZonedBlockDevice *zbd = zbd_open(true);
  if (zbd == nullptr) return 1;

  ZenFS *zenFS;
  s = zenfs_mount(zbd, &zenFS, true);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n", s.ToString().c_str());
    return 1;
  }

  std::ostream &json_stream = std::cout;
  json_stream << "{\"zones\":";
  zbd->EncodeJson(json_stream);
  json_stream << ",\"files\":";
  zenFS->EncodeJson(json_stream);
  json_stream << "}";

  return 0;
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char **argv) {
  gflags::SetUsageMessage(std::string("\nUSAGE:\n") + std::string(argv[0]) +
                          +" <command> [OPTIONS]...\nCommands: mkfs, list, ls-uuid, df, backup, restore");
  if (argc < 2) {
    fprintf(stderr, "You need to specify a command.\n");
    return 1;
  }

  std::string subcmd(argv[1]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_zbd.empty() && subcmd != "ls-uuid") {
    fprintf(stderr, "You need to specify a zoned block device using --zbd\n");
    return 1;
  }
  if (subcmd == "mkfs") {
    return ROCKSDB_NAMESPACE::zenfs_tool_mkfs();
  } else if (subcmd == "list") {
    return ROCKSDB_NAMESPACE::zenfs_tool_list();
  } else if (subcmd == "ls-uuid") {
    return ROCKSDB_NAMESPACE::zenfs_tool_lsuuid();
  } else if (subcmd == "df") {
    return ROCKSDB_NAMESPACE::zenfs_tool_df();
  } else if (subcmd == "backup") {
    return ROCKSDB_NAMESPACE::zenfs_tool_backup();
  } else if (subcmd == "restore") {
    return ROCKSDB_NAMESPACE::zenfs_tool_restore();
  } else if (subcmd == "dump") {
    return ROCKSDB_NAMESPACE::zenfs_tool_dump();
  } else if (subcmd == "stat") {
    return ROCKSDB_NAMESPACE::zenfs_tool_stat();
  } else {
    fprintf(stderr, "Subcommand not recognized: %s\n", subcmd.c_str());
    return 1;
  }

  return 0;
}
