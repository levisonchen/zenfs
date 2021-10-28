// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <locale>
#include "rocksdb/compaction_dispatcher.h"
#if !defined(ROCKSDB_LITE) && !defined(OS_WIN)

#include "zbd_zenfs.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libzbd/zbd.h>
#include <linux/blkzoned.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <ctime>
#include <iostream>
#include <map>
#include <tuple>

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "io_zenfs.h"
#include "rocksdb/env.h"
#include "utilities/trace/bytedance_metrics_reporter.h"

#define KB (1024)
#define MB (1024 * KB)

#define ZENFS_DEBUG
#include "utils.h"

/* Number of reserved zones for op log
 * Two non-offline op log zones are needed to be able
 * to roll the log safely. One extra
 * is allocated to cover for one zone going offline.
 */
#define ZENFS_OP_LOG_ZONES (2)

/* Number of reserved zones for metadata snapshot
 */
#define ZENFS_SNAPSHOT_ZONES (2)

/* Minimum of number of zones that makes sense */
#define ZENFS_MIN_ZONES (32)

namespace ROCKSDB_NAMESPACE {

Zone::Zone(ZonedBlockDevice *zbd, struct zbd_zone *z)
    : zbd_(zbd),
      start_(zbd_zone_start(z)),
      max_capacity_(zbd_zone_capacity(z)),
      wp_(zbd_zone_wp(z)),
      open_for_write_(false) {
  lifetime_ = Env::WLTH_NOT_SET;
  used_capacity_ = 0;
  capacity_ = 0;
  bg_processing = false;
  if (!(zbd_zone_full(z) || zbd_zone_offline(z) || zbd_zone_rdonly(z)))
    capacity_ = zbd_zone_capacity(z) - (zbd_zone_wp(z) - zbd_zone_start(z));

  memset(&wr_ctx.io_ctx, 0, sizeof(wr_ctx.io_ctx));
  wr_ctx.fd = zbd_->GetWriteFD();
  wr_ctx.iocbs[0] = &wr_ctx.iocb;
  wr_ctx.inflight = 0;

  if (io_setup(1, &wr_ctx.io_ctx) < 0) {
    fprintf(stderr, "Failed to allocate io context\n");
 }
}

bool Zone::IsUsed() { return (used_capacity_ > 0) || open_for_write_; }
bool Zone::IsFull() { return (capacity_ == 0); }
bool Zone::IsEmpty() { return (wp_ == start_); }
uint64_t Zone::GetZoneNr() { return start_ / zbd_->GetZoneSize(); }
uint64_t Zone::GetCapacityLeft() { return capacity_; }

void Zone::CloseWR() {
  assert(open_for_write_);
  Sync();
  if (!Close().ok()) assert(false);
}

void Zone::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"start\":" << start_ << ",";
  json_stream << "\"capacity\":" << capacity_ << ",";
  json_stream << "\"max_capacity\":" << max_capacity_ << ",";
  json_stream << "\"wp\":" << wp_ << ",";
  json_stream << "\"lifetime\":" << lifetime_ << ",";
  json_stream << "\"used_capacity\":" << used_capacity_;
  json_stream << "}";
}

IOStatus Zone::Reset() {
  size_t zone_sz = zbd_->GetZoneSize();
  unsigned int report = 1;
  struct zbd_zone z;
  int ret;

  assert(!IsUsed());

  ret = zbd_reset_zones(zbd_->GetWriteFD(), start_, zone_sz);
  if (ret) return IOStatus::IOError("Zone reset failed\n");

  ret = zbd_report_zones(zbd_->GetReadFD(), start_, zone_sz, ZBD_RO_ALL, &z, &report);

  if (ret || (report != 1)) {
    return IOStatus::IOError("Zone report failed\n");
  }

  if (zbd_zone_offline(&z))
    capacity_ = 0;
  else
    max_capacity_ = capacity_ = zbd_zone_capacity(&z);

  wp_ = start_;
  lifetime_ = Env::WLTH_NOT_SET;

  return IOStatus::OK();
}

IOStatus Zone::Finish() {
  size_t zone_sz = zbd_->GetZoneSize();
  int fd = zbd_->GetWriteFD();
  int ret;

  assert(!open_for_write_);

  ret = zbd_finish_zones(fd, start_, zone_sz);
  if (ret) return IOStatus::IOError("Zone finish failed\n");

  capacity_ = 0;
  wp_ = start_ + zone_sz;

  return IOStatus::OK();
}

IOStatus Zone::Close() {
  size_t zone_sz = zbd_->GetZoneSize();
  int fd = zbd_->GetWriteFD();
  int ret;

  open_for_write_ = false;

  if (!(IsEmpty() || IsFull())) {
    ret = zbd_close_zones(fd, start_, zone_sz);
    if (ret) return IOStatus::IOError("Zone close failed\n");
  }

  return IOStatus::OK();
}

IOStatus Zone::Append(char *data, uint32_t size) {
  char *ptr = data;
  uint32_t left = size;
  int fd = zbd_->GetWriteFD();
  int ret;
  IOStatus s;

  if (capacity_ < size) return IOStatus::NoSpace("Not enough capacity for append");

  assert((size % zbd_->GetBlockSize()) == 0);

  /* Make sure we don't have any outstanding writes */
  s = Sync();
  if (!s.ok())
    return s;

  while (left) {
    ret = pwrite(fd, ptr, size, wp_);
    if (ret < 0) return IOStatus::IOError("Write failed");

    ptr += ret;
    wp_ += ret;
    capacity_ -= ret;
    left -= ret;
  }

  return IOStatus::OK();
}

IOStatus Zone::Sync() {
  struct io_event events[1];
  struct timespec timeout;
  int ret;
  timeout.tv_sec = 1;
  timeout.tv_nsec = 0;

  if (wr_ctx.inflight == 0)
    return IOStatus::OK();

  ret = io_getevents(wr_ctx.io_ctx, 1, 1, events, &timeout);
  if (ret != 1) {
    fprintf(stderr, "Failed to complete io - timeout ret: %d\n", ret);
    return IOStatus::IOError("Failed to complete io - timeout?");
  }

  ret = events[0].res;
  if (ret != (int)(wr_ctx.iocb.u.c.nbytes)) {
    if (ret >= 0) {
        /* TODO: we need to handle this case and keep on submittin' until we're done*/
        fprintf(stderr, "failed to complete io - short write\n");
        return IOStatus::IOError("Failed to complete io - short write");
    } else {
        return IOStatus::IOError("Failed to complete io - io error");
    }
  }

  wr_ctx.inflight = 0;

  return IOStatus::OK();
}

IOStatus Zone::Append_async(char *data, uint32_t size) {
  char *ptr = data;
  uint32_t left = size;
  int ret;
  IOStatus s;

  assert((size % zbd_->GetBlockSize()) == 0);

  /* Make sure we don't have any outstanding writes */
  s = Sync();
  if (!s.ok())
    return s;

  if (capacity_ < size)
    return IOStatus::NoSpace("Not enough capacity for append");

  io_prep_pwrite(&wr_ctx.iocb, wr_ctx.fd, data, size, wp_);

  ret = io_submit(wr_ctx.io_ctx, 1, wr_ctx.iocbs);
  if (ret < 0) {
    fprintf(stderr, "Failed to submit io\n");
    return IOStatus::IOError("Failed to submit io");
  }

  wr_ctx.inflight = size;
  ptr += size;
  wp_ += size;
  capacity_ -= size;
  left -= size;

  return IOStatus::OK();
}

ZoneExtent::ZoneExtent(uint64_t start, uint32_t length, Zone *zone)
    : start_(start), length_(length), zone_(zone) {}

Zone *ZonedBlockDevice::GetIOZone(uint64_t offset) {
  for (const auto z : io_zones_)
    if (z->start_ <= offset && offset < (z->start_ + zone_sz_)) return z;
  return nullptr;
}

std::vector<ZoneStat> ZonedBlockDevice::GetStat() {
  std::vector<ZoneStat> stat;
  for (const auto z : io_zones_) {
    ZoneStat zone_stat;
    zone_stat.total_capacity = z->max_capacity_;
    zone_stat.write_position = z->wp_;
    zone_stat.start_position = z->start_;
    stat.emplace_back(std::move(zone_stat));
  }
  return stat;
}

BackgroundWorker::BackgroundWorker(bool run_at_beginning) {
  {
    std::unique_lock<std::mutex> lk(job_mtx_);
    if (run_at_beginning) {
      Run();
    }
  }

  worker_ = std::thread(&BackgroundWorker::ProcessJobs, this);
}

BackgroundWorker::~BackgroundWorker() {
  {
    std::unique_lock<std::mutex> lk(job_mtx_);
    Terminate();
    job_cv_.notify_all();
  }

  worker_.join();
  for (auto& job : jobs_) {
    (*job)();
  }
}

void BackgroundWorker::Wait() {
  state_ = kWaiting;
}

void BackgroundWorker::Run() {
  state_ = kRunning;
}

void BackgroundWorker::Terminate() {
  state_ = kTerminated;
}

void BackgroundWorker::ProcessJobs() {
  while (true) {
    {
      std::unique_lock<std::mutex> lk(job_mtx_);
      job_cv_.wait(lk, [this](){return !jobs_.empty() || state_ == kTerminated;});
      if (state_ == kTerminated) {
        return;
      }
      job_now_.swap(jobs_.front());
      jobs_.pop_front();
    }
    (*job_now_)();
  }
}

void BackgroundWorker::SubmitJob(std::function<void()> fn) {
  std::unique_lock<std::mutex> lk(job_mtx_);
  jobs_.push_back(std::make_unique<SimpleJob>(fn));
  job_cv_.notify_one();
}

void BackgroundWorker::SubmitJob(std::unique_ptr<BackgroundJob>&& job) {
  std::unique_lock<std::mutex> lk(job_mtx_);
  jobs_.push_back(std::move(job));
  job_cv_.notify_one();
}

ZonedBlockDevice::ZonedBlockDevice(std::string bdevname, std::shared_ptr<Logger> logger)
    : ZonedBlockDevice(bdevname, logger, "", std::make_shared<ByteDanceMetricsReporterFactory>()) {}

static std::string write_latency_metric_name = "zenfs_write_latency";
static std::string read_latency_metric_name = "zenfs_read_latency";
static std::string fg_sync_latency_metric_name = "fg_zenfs_sync_latency";
static std::string bg_sync_latency_metric_name = "bg_zenfs_sync_latency";
static std::string io_alloc_wal_latency_metric_name = "zenfs_io_alloc_wal_latency";
static std::string io_alloc_non_wal_latency_metric_name = "zenfs_io_alloc_non_wal_latency";
static std::string io_alloc_wal_actual_latency_metric_name = "zenfs_io_alloc_wal_actual_latency";
static std::string io_alloc_non_wal_actual_latency_metric_name = "zenfs_io_alloc_non_wal_actual_latency";
static std::string meta_alloc_latency_metric_name = "zenfs_meta_alloc_latency";
static std::string roll_latency_metric_name = "zenfs_roll_latency";

static std::string write_qps_metric_name = "zenfs_write_qps";
static std::string read_qps_metric_name = "zenfs_read_qps";
static std::string sync_qps_metric_name = "zenfs_sync_qps";
static std::string io_alloc_qps_metric_name = "zenfs_io_alloc_qps";
static std::string meta_alloc_qps_metric_name = "zenfs_meta_alloc_qps";
static std::string roll_qps_metric_name = "zenfs_roll_qps";

static std::string write_throughput_metric_name = "zenfs_write_throughput";
static std::string roll_throughput_metric_name = "zenfs_roll_throughput";

static std::string active_zones_metric_name = "zenfs_active_zones";
static std::string open_zones_metric_name = "zenfs_open_zones";
static std::string zbd_free_space_metric_name = "zenfs_free_space";
static std::string zbd_used_space_metric_name = "zenfs_used_space";
static std::string zbd_reclaimable_space_metric_name = "zenfs_reclaimable_space";
static std::string zbd_total_extent_length_metric_name = "zenfs_total_extent_length";

ZonedBlockDevice::ZonedBlockDevice(std::string bdevname, std::shared_ptr<Logger> logger, std::string bytedance_tags,
                                   std::shared_ptr<MetricsReporterFactory> metrics_reporter_factory)
    : filename_("/dev/" + bdevname),
      logger_(logger),
      metrics_reporter_factory_(new CurriedMetricsReporterFactory(metrics_reporter_factory, logger.get(), Env::Default())),
      // A short advice for new developers: BE SURE TO STORE `bytedance_tags_`
      // somewhere,
      // and pass the stored `bytedance_tags_` to the reporters. Otherwise the
      // metrics
      // library will panic with `std::logic_error`.
      bytedance_tags_(bytedance_tags),
      write_latency_reporter_(
          *metrics_reporter_factory_->BuildHistReporter(write_latency_metric_name, bytedance_tags_)),
      read_latency_reporter_(
          *metrics_reporter_factory_->BuildHistReporter(read_latency_metric_name, bytedance_tags_)),
      fg_sync_latency_reporter_(
          *metrics_reporter_factory_->BuildHistReporter(fg_sync_latency_metric_name, bytedance_tags_)),
      bg_sync_latency_reporter_(
          *metrics_reporter_factory_->BuildHistReporter(bg_sync_latency_metric_name, bytedance_tags_)),
      meta_alloc_latency_reporter_(
          *metrics_reporter_factory_->BuildHistReporter(
              meta_alloc_latency_metric_name, bytedance_tags_)),
      io_alloc_wal_latency_reporter_(*metrics_reporter_factory_->BuildHistReporter(
              io_alloc_wal_latency_metric_name, bytedance_tags_)),
      io_alloc_non_wal_latency_reporter_(*metrics_reporter_factory_->BuildHistReporter(
              io_alloc_non_wal_latency_metric_name, bytedance_tags_)),
      io_alloc_wal_actual_latency_reporter_(*metrics_reporter_factory_->BuildHistReporter(
              io_alloc_wal_actual_latency_metric_name, bytedance_tags_)),
      io_alloc_non_wal_actual_latency_reporter_(*metrics_reporter_factory_->BuildHistReporter(
              io_alloc_non_wal_actual_latency_metric_name, bytedance_tags_)),
      roll_latency_reporter_(*metrics_reporter_factory_->BuildHistReporter(
          roll_latency_metric_name, bytedance_tags_)),
      write_qps_reporter_(*metrics_reporter_factory_->BuildCountReporter(
          write_qps_metric_name, bytedance_tags_)),
      read_qps_reporter_(*metrics_reporter_factory_->BuildCountReporter(
          read_qps_metric_name, bytedance_tags_)),
      sync_qps_reporter_(*metrics_reporter_factory_->BuildCountReporter(
          sync_qps_metric_name, bytedance_tags_)),
      meta_alloc_qps_reporter_(*metrics_reporter_factory_->BuildCountReporter(
          meta_alloc_qps_metric_name, bytedance_tags_)),
      io_alloc_qps_reporter_(*metrics_reporter_factory_->BuildCountReporter(
          io_alloc_qps_metric_name, bytedance_tags_)),
      roll_qps_reporter_(*metrics_reporter_factory_->BuildCountReporter(
          roll_qps_metric_name, bytedance_tags_)),
      write_throughput_reporter_(*metrics_reporter_factory_->BuildCountReporter(
          write_throughput_metric_name, bytedance_tags_)),
      roll_throughput_reporter_(*metrics_reporter_factory_->BuildCountReporter(
          roll_throughput_metric_name, bytedance_tags_)),
      active_zones_reporter_(*metrics_reporter_factory_->BuildHistReporter(
          active_zones_metric_name, bytedance_tags_)),
      open_zones_reporter_(*metrics_reporter_factory_->BuildHistReporter(
          open_zones_metric_name, bytedance_tags_)),
      zbd_free_space_reporter_(*metrics_reporter_factory_->BuildHistReporter(
          zbd_free_space_metric_name, bytedance_tags_)),
      zbd_used_space_reporter_(*metrics_reporter_factory_->BuildHistReporter(
          zbd_used_space_metric_name, bytedance_tags_)),
      zbd_reclaimable_space_reporter_(*metrics_reporter_factory_->BuildHistReporter(
          zbd_reclaimable_space_metric_name, bytedance_tags_)),
      zbd_total_extent_length_reporter_(*metrics_reporter_factory_->BuildHistReporter(
          zbd_total_extent_length_metric_name, bytedance_tags_)) {
    Info(logger_, "New Zoned Block Device: %s (with metrics enabled)",
       filename_.c_str());
}

std::string ZonedBlockDevice::ErrorToString(int err) {
  char *err_str = strerror(err);
  if (err_str != nullptr) return std::string(err_str);
  return "";
}

IOStatus ZonedBlockDevice::CheckScheduler() {
  std::ostringstream path;
  std::string s = filename_;
  std::fstream f;

  s.erase(0, 5);  // Remove "/dev/" from /dev/nvmeXnY
  path << "/sys/block/" << s << "/queue/scheduler";
  f.open(path.str(), std::fstream::in);
  if (!f.is_open()) {
    return IOStatus::InvalidArgument("Failed to open " + path.str());
  }

  std::string buf;
  getline(f, buf);
  if (buf.find("[mq-deadline]") == std::string::npos) {
    f.close();
    return IOStatus::InvalidArgument("Current ZBD scheduler is not mq-deadline, set it to mq-deadline.");
  }

  f.close();
  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::Open(bool readonly) {
  struct zbd_zone *zone_rep;
  unsigned int reported_zones;
  uint64_t addr_space_sz;
  zbd_info info;
  Status s;
  uint64_t i = 0;
  uint64_t m = 0;
  int ret;

  read_f_ = zbd_open(filename_.c_str(), O_RDONLY, &info);
  if (read_f_ < 0) {
    return IOStatus::InvalidArgument("Failed to open zoned block device: " + ErrorToString(errno));
  }

  read_direct_f_ = zbd_open(filename_.c_str(), O_RDONLY | O_DIRECT, &info);
  if (read_direct_f_ < 0) {
    return IOStatus::InvalidArgument("Failed to open zoned block device: " + ErrorToString(errno));
  }

  if (readonly) {
    write_f_ = -1;
  } else {
    write_f_ = zbd_open(filename_.c_str(), O_WRONLY | O_DIRECT | O_EXCL, &info);
    if (write_f_ < 0) {
      return IOStatus::InvalidArgument("Failed to open zoned block device: " + ErrorToString(errno));
    }
  }

  if (info.model != ZBD_DM_HOST_MANAGED) {
    return IOStatus::NotSupported("Not a host managed block device");
  }

  if (info.nr_zones < ZENFS_MIN_ZONES) {
    return IOStatus::NotSupported("To few zones on zoned block device (32 required)");
  }

  IOStatus ios = CheckScheduler();
  if (ios != IOStatus::OK()) return ios;

  block_sz_ = info.pblock_size;
  zone_sz_ = info.zone_size;
  nr_zones_ = info.nr_zones;

  /* We need 3 open zones for meta data writes , the rest can be used for files
   */
  max_nr_active_io_zones_ = info.max_nr_active_zones - 3;
  max_nr_open_io_zones_ = info.max_nr_active_zones - 3;

  Info(logger_, "Zone block device nr zones: %u max active: %u max open: %u \n", info.nr_zones,
       info.max_nr_active_zones, info.max_nr_open_zones);

  addr_space_sz = (uint64_t)nr_zones_ * zone_sz_;

  meta_worker_.reset(new BackgroundWorker());
  data_worker_.reset(new BackgroundWorker());
  active_zones_.resize(max_nr_active_io_zones_);
  
  ret = zbd_list_zones(read_f_, 0, addr_space_sz, ZBD_RO_ALL, &zone_rep, &reported_zones);

  if (ret || reported_zones != nr_zones_) {
    Error(logger_, "Failed to list zones, err: %d", ret);
    return IOStatus::IOError("Failed to list zones");
  }

  while (m < ZENFS_OP_LOG_ZONES && i < reported_zones) {
    struct zbd_zone *z = &zone_rep[i++];
    /* Only use sequential write required zones */
    if (zbd_zone_type(z) == ZBD_ZONE_TYPE_SWR) {
      if (!zbd_zone_offline(z)) {
        op_zones_.push_back(new Zone(this, z));
      }
      m++;
    }
  }

  m = 0;
  // initialize metadata snapshop zones
  while (m < ZENFS_SNAPSHOT_ZONES && i < reported_zones) {
    struct zbd_zone *z = &zone_rep[i++];
    /* Only use sequential write required zones */
    if (zbd_zone_type(z) == ZBD_ZONE_TYPE_SWR) {
      if (!zbd_zone_offline(z)) {
        snapshot_zones_.push_back(new Zone(this, z));
      }
      m++;
    }
  }

  active_io_zones_ = 0;
  open_io_zones_ = 0;

  for (; i < reported_zones; i++) {
    struct zbd_zone *z = &zone_rep[i];
    /* Only use sequential write required zones */
    if (zbd_zone_type(z) == ZBD_ZONE_TYPE_SWR) {
      if (!zbd_zone_offline(z)) {
        Zone *newZone = new Zone(this, z);
        io_zones_.push_back(newZone);
        if (zbd_zone_imp_open(z) || zbd_zone_exp_open(z) || zbd_zone_closed(z)) {
          active_zones_[active_io_zones_++] = newZone;
          if (zbd_zone_imp_open(z) || zbd_zone_exp_open(z)) {
            if (!readonly) {
              newZone->Close();
            }
          }
        }
      }
    }
  }

  for (i = active_io_zones_; i < max_nr_active_io_zones_; i++) {
    active_zones_[i] = nullptr;
  }

  free(zone_rep);
  start_time_ = time(NULL);

  return IOStatus::OK();
}

uint64_t ZonedBlockDevice::GetFreeSpace() {
  uint64_t free = 0;
  for (const auto z : io_zones_) {
    free += z->capacity_;
  }
  return free;
}

uint64_t ZonedBlockDevice::GetUsedSpace() {
  uint64_t used = 0;
  for (const auto z : io_zones_) {
    used += z->used_capacity_;
  }
  return used;
}

uint64_t ZonedBlockDevice::GetReclaimableSpace() {
  uint64_t reclaimable = 0;
  for (const auto z : io_zones_) {
    if (z->IsFull()) reclaimable += (z->max_capacity_ - z->used_capacity_);
  }
  return reclaimable;
}

void ZonedBlockDevice::ReportSpaceUtilization() {
  Info(logger_, "zbd free space %lu GB MkFS\n", GetFreeSpace() / (1024 * 1024 * 1024));
  zbd_free_space_reporter_.AddRecord(GetFreeSpace() / (1024 * 1024 * 1024));

  Info(logger_, "zbd used space %lu GB MkFS\n", GetUsedSpace() / (1024 * 1024 * 1024));
  zbd_used_space_reporter_.AddRecord(GetUsedSpace() / (1024 * 1024 * 1024));

  Info(logger_, "zbd reclaimable space %lu GB MkFS\n", GetUsedSpace() / (1024 * 1024 * 1024));
  zbd_reclaimable_space_reporter_.AddRecord(GetReclaimableSpace() / (1024 * 1024 * 1024));
}

void ZonedBlockDevice::LogZoneStats() {
  uint64_t used_capacity = 0;
  uint64_t reclaimable_capacity = 0;
  uint64_t reclaimables_max_capacity = 0;
  uint64_t active = 0;
  // io_zones_mtx_.lock();

  for (const auto z : io_zones_) {
    used_capacity += z->used_capacity_;

    if (z->used_capacity_) {
      reclaimable_capacity += z->max_capacity_ - z->used_capacity_;
      reclaimables_max_capacity += z->max_capacity_;
    }

    if (!(z->IsFull() || z->IsEmpty())) active++;
  }

  if (reclaimables_max_capacity == 0) reclaimables_max_capacity = 1;

  Info(logger_,
       "[Zonestats:time(s),used_cap(MB),reclaimable_cap(MB), "
       "avg_reclaimable(%%), active(#), active_zones(#), open_zones(#)] %ld "
       "%lu %lu %lu %lu %ld %ld\n",
       time(NULL) - start_time_, used_capacity / MB, reclaimable_capacity / MB,
       100 * reclaimable_capacity / reclaimables_max_capacity, active, active_io_zones_.load(), open_io_zones_.load());

  // io_zones_mtx_.unlock();
}

void ZonedBlockDevice::LogZoneUsage() {
  for (const auto z : io_zones_) {
    int64_t used = z->used_capacity_;

    if (used > 0) {
      Debug(logger_, "Zone 0x%lX used capacity: %ld bytes (%ld MB)\n", z->start_, used, used / MB);
    }
  }
}

ZonedBlockDevice::~ZonedBlockDevice() {

  meta_worker_.reset(nullptr);

  for (const auto z : op_zones_) {
    delete z;
  }

  for (const auto z : snapshot_zones_) {
    delete z;
  }

  for (const auto z : io_zones_) {
    delete z;
  }

  zbd_close(read_f_);
  zbd_close(read_direct_f_);
  zbd_close(write_f_);
}

#define LIFETIME_DIFF_NOT_GOOD (100)
#define LIFETIME_DIFF_MEH (2)

unsigned int GetLifeTimeDiff(Env::WriteLifeTimeHint zone_lifetime, Env::WriteLifeTimeHint file_lifetime) {
  assert(file_lifetime >= 0 && file_lifetime <= Env::WLTH_EXTREME);

  if ((file_lifetime == Env::WLTH_NOT_SET) || (file_lifetime == Env::WLTH_NONE)) {
    if (file_lifetime == zone_lifetime) {
      return 0;
    } else {
      return LIFETIME_DIFF_NOT_GOOD;
    }
  }

  if (zone_lifetime == file_lifetime)
    return LIFETIME_DIFF_MEH;

  if (zone_lifetime > file_lifetime) return zone_lifetime - file_lifetime;
  return LIFETIME_DIFF_NOT_GOOD;
}

Zone *ZonedBlockDevice::AllocateMetaZone() {
  LatencyHistGuard guard(&meta_alloc_latency_reporter_);
  meta_alloc_qps_reporter_.AddCount(1);

  for (const auto z : op_zones_) {
    if (z->IsEmpty()) {
      return z;
    }
  }

  return nullptr;
}

Zone *ZonedBlockDevice::AllocateSnapshotZone() {
  LatencyHistGuard guard(&meta_alloc_latency_reporter_);
  meta_alloc_qps_reporter_.AddCount(1);

  for (const auto z : snapshot_zones_) {
    if (z->IsEmpty()) {
      return z;
    }
  }

  return nullptr;
}

void ZonedBlockDevice::ResetUnusedIOZones() {
  const std::lock_guard<std::mutex> lock(zone_resources_mtx_);
  /* Reset any unused zones */
  for (const auto z : io_zones_) {
    if (!z->IsUsed() && !z->IsEmpty()) {
      if (!z->IsFull()) active_io_zones_--;
      if (!z->Reset().ok()) Warn(logger_, "Failed reseting zone");
    }
  }
  active_zones_reporter_.AddRecord(active_io_zones_);
}

void ZonedBlockDevice::BgResetDataZone(Zone* z, Zone** slot) {
  data_worker_->SubmitJob([&, z, slot]() {
    assert(*slot == z);
    auto s = z->Reset();
    if (!s.ok()) {
      Error(logger_, "Failed to reset zones, err: %s", s.ToString().c_str());
      assert(false);
    }
    active_io_zones_--;
    z->bg_processing = false;
    active_zones_mtx_.lock();
    *slot = nullptr;
    active_zones_mtx_.unlock();
    active_zones_reporter_.AddRecord(active_io_zones_);
  });
}

void ZonedBlockDevice::BgFinishDataZone(Zone *z, Zone **slot) {
  data_worker_->SubmitJob([&, z, slot]() {
    assert(*slot == z);
    auto s = z->Finish();
    if (!s.ok()) {
      Error(logger_, "Failed to finish zones, err: %s", s.ToString().c_str());
      assert(false);
    }
    active_io_zones_--;
    z->bg_processing = false;
    active_zones_mtx_.lock();
    *slot = nullptr;
    active_zones_mtx_.unlock();
    active_zones_reporter_.AddRecord(active_io_zones_);
  });
}

void ZonedBlockDevice::TriggerBgFinishAndReset() {
  int e = 0, d = 1;
  // if compare exchange failed, means there is bg finish and reset job running,
  // so we could skip this time.
  if (!bg_io_zone_recycling_.compare_exchange_strong(e, d)) return;
  // Opreational, scan for jobs.
  for (int i = 0; i < active_zones_.size(); ++i) {
    Zone* z = active_zones_[i];
    // Skip it when the slot is empty.
    if (!z) continue;
    // Skip already enqueued ones.
    if (z->bg_processing) continue;
    // Skip it when is occupied or empty or fulled, this will be handle by other threads.
    if (z->open_for_write_ || z->IsEmpty() || (z->IsFull() && z->IsUsed()))
      continue;
    if (!z->IsUsed()) {
      z->bg_processing = true;
      BgResetDataZone(z, &active_zones_[i]);
      continue;
    }
    if (z->capacity_ < (z->max_capacity_ * finish_threshold_ / 100)) {
      z->bg_processing = true;
      BgFinishDataZone(z, &active_zones_[i]);
    }
  }
  bg_io_zone_recycling_.store(0);
}

// Helper function for selecting one from active zone vector.
bool ZonedBlockDevice::GetActiveZone(int start, Env::WriteLifeTimeHint file_lifetime, Zone* full_zone, Zone** ret) {
  std::unique_lock<std::mutex> lk(active_zones_mtx_);
  bool success = false;

  // If caller's active zone is full, set bg finish for it.
  if (full_zone)
    if (!full_zone->bg_processing)
      for (int i = 0; i < active_zones_.size(); i++)
        if (active_zones_[i] == full_zone) {
          full_zone->open_for_write_ = false;
          full_zone->bg_processing = true;
          BgFinishDataZone(full_zone, &active_zones_[i]);
          break;
        }

  // Trigger bg recycling with lower priority job.
  for (int i = start; i < active_zones_.size() && !success; i++) {
    if (active_zones_[i] != nullptr) {
      // This slot is referencing a zone.
      Zone* z = active_zones_[i];
      // If this zone is under recycling, skip it.
      if (z->bg_processing) continue;
      // This zone is avaliable, check if is open for write.
      if (!z->open_for_write_) {
        // Not open for write by other, we can reuse it for this time.
        // Set this zone is occupied.
        z->open_for_write_ = true;
        // Set return true for reusing selected zone successfully.
        *ret = z;
        success = true;
      }
    } else {
      // This slot is empty, try allocate a new zone to it.
      io_zones_mtx_.lock();
      Zone* allocated_zone = nullptr;
      unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
      // Found best suitable life time hint zone.
      for (Zone* z : io_zones_) {
        // Skip zones under recycling.
        if (!z) continue;
        if ((!z->open_for_write_) && (z->used_capacity_ > 0) && !z->IsFull()) {
          unsigned int diff = GetLifeTimeDiff(z->lifetime_, file_lifetime);
          if (diff <= best_diff) {
            allocated_zone = z;
            best_diff = diff;
          }
        }
      }
      // Use an empty zone when no good match exists.
      if (best_diff >= LIFETIME_DIFF_NOT_GOOD) {
        for (Zone* z : io_zones_) {
        // Skip zones under recycling.
          if (!z) continue;
          if ((!z->open_for_write_) && z->IsEmpty()) {
            z->lifetime_ = file_lifetime;
            allocated_zone = z;
          }
        }
      }
      if (allocated_zone) {
        // Target zone found, make it active.
        allocated_zone->open_for_write_ = true;
        allocated_zone->lifetime_ = file_lifetime;
        active_zones_[i] = allocated_zone;
        active_io_zones_++;
        // Set return true for reusing selected zone successfully.
        *ret = allocated_zone;
        success = true;
      }
      // Trigger bg recycling with low priority jobs.
      if (start != 0) TriggerBgFinishAndReset();
      io_zones_mtx_.unlock();
    }
  }

  // Keep waiting in the loop.
  return success;
};

Zone *ZonedBlockDevice::AllocateZone(Env::WriteLifeTimeHint file_lifetime, bool is_wal, Zone* full_zone) {
  LatencyHistGuard guard(is_wal ? &io_alloc_wal_latency_reporter_
                                : &io_alloc_non_wal_latency_reporter_);
  // Victim zone.
  Zone* z = nullptr;

  // Keep trying until got one or no avaliable resource.
  bool try_again = false;
  do {
    if (is_wal) {
      // High priority requset : take it and go!
      wal_zone_allocating_++;
      try_again = GetActiveZone(0, file_lifetime, full_zone, &z);
      wal_zone_allocating_--;
    } else {
      // Low priority requset : wait until no contention.
      while (wal_zone_allocating_.load() != 0) {
        std::this_thread::yield();
      }
      try_again = GetActiveZone(2, file_lifetime, full_zone, &z);
    }
  } while (!try_again);

  active_zones_reporter_.AddRecord(active_io_zones_);

  return z;
}

std::string ZonedBlockDevice::GetFilename() { return filename_; }

uint32_t ZonedBlockDevice::GetBlockSize() { return block_sz_; }

template <typename T>
void ZonedBlockDevice::EncodeJsonZone(std::ostream &json_stream,
                                      const T zones) {
  bool first_element = true;
  json_stream << "[";
  for (Zone *zone : zones) {
    if (first_element) {
      first_element = false;
    } else {
      json_stream << ",";
    }
    zone->EncodeJson(json_stream);
  }

  json_stream << "]";
}

void ZonedBlockDevice::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"meta\":";
  EncodeJsonZone(json_stream, op_zones_);
  json_stream << "\"meta snapshot\":";
  EncodeJsonZone(json_stream, snapshot_zones_);
  json_stream << ",\"io\":";
  EncodeJsonZone(json_stream, io_zones_);
  json_stream << "}";
}

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && !defined(OS_WIN)
