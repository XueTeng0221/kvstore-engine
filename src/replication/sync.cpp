#include "kvstore/replication/sync.hpp"

namespace kvstore {

PrimarySyncSession::PrimarySyncSession(IEngine& engine, ReplicationBacklog& backlog,
                                       std::filesystem::path snapshot_directory)
    : engine_(engine),
      backlog_(backlog),
      snapshot_directory_(std::move(snapshot_directory)),
      last_heartbeat_(std::chrono::steady_clock::now()) {}

Result<RecoveryPoint> PrimarySyncSession::BeginFullSync() {
  std::scoped_lock lock(mutex_);
  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto before = backlog_.newest_point();
    const auto status =
        Snapshot::Save(snapshot_directory_, engine_, before.first, before.second, false, true);
    if (!status.ok()) return status;
    const auto after = backlog_.newest_point();
    if (before != after) continue;
    snapshot_point_ = {after.first, after.second};
    return snapshot_point_;
  }
  return Status{StatusCode::kBusy, "writes prevented a stable replication snapshot"};
}

Result<std::vector<WriteEvent>> PrimarySyncSession::CatchUp(std::uint64_t next_offset) const {
  if (next_offset == 0) return Status{StatusCode::kInvalidArgument, "invalid catch-up offset"};
  auto events = backlog_.ReadFrom(next_offset);
  if (!events.ok()) return events.status();
  for (auto& event : events.value()) {
    event.source = WriteSource::kIncrementalSync;
    event.checksum = event.ComputeChecksum();
  }
  return std::move(events).value();
}

Status PrimarySyncSession::Ack(ReplicationAck ack) {
  std::scoped_lock lock(mutex_);
  if (ack.offset == 0 && ack.event_id != 0)
    return {StatusCode::kInvalidArgument, "replication ACK has an invalid zero offset"};
  if (ack.offset < acknowledged_.offset ||
      (ack.offset == acknowledged_.offset && ack.event_id < acknowledged_.event_id))
    return {StatusCode::kInvalidArgument, "replication ACK moved backwards"};
  if (ack.offset > backlog_.newest_offset() ||
      (ack.offset == backlog_.newest_offset() && ack.event_id > backlog_.newest_event_id()))
    return {StatusCode::kInvalidArgument, "replication ACK is ahead of primary"};
  if (ack.offset != 0) {
    const auto events = backlog_.ReadFrom(ack.offset);
    if (!events.ok() || events.value().empty() || events.value().front().event_id != ack.event_id)
      return {StatusCode::kInvalidArgument, "replication ACK does not match an event"};
  }
  acknowledged_ = ack;
  return Status::Ok();
}

Status PrimarySyncSession::Heartbeat(std::chrono::steady_clock::time_point now) {
  std::scoped_lock lock(mutex_);
  if (now < last_heartbeat_)
    return {StatusCode::kInvalidArgument, "heartbeat clock moved backwards"};
  last_heartbeat_ = now;
  return Status::Ok();
}

ReplicationAck PrimarySyncSession::acknowledged() const {
  std::scoped_lock lock(mutex_);
  return acknowledged_;
}

RecoveryPoint PrimarySyncSession::snapshot_point() const {
  std::scoped_lock lock(mutex_);
  return snapshot_point_;
}

Status ReplicaSyncApplier::InstallFullSync(const std::vector<Entry>& entries, RecoveryPoint point) {
  std::scoped_lock lock(mutex_);
  if (point.offset < applied_.offset ||
      (point.offset == applied_.offset && point.event_id < applied_.event_id))
    return {StatusCode::kInvalidArgument, "full sync point moved backwards"};
  const auto status = engine_.Import(entries);
  if (!status.ok()) return status;
  applied_ = {point.offset, point.event_id};
  applied_checksum_ = 0;
  return Status::Ok();
}

Status ReplicaSyncApplier::Apply(const WriteEvent& event) {
  if (event.offset != applied_.offset + 1U)
    return {StatusCode::kBusy, "incremental event offset gap"};
  if (event.ComputeChecksum() != event.checksum)
    return {StatusCode::kCorruption, "incremental event checksum mismatch"};
  if (event.source != WriteSource::kIncrementalSync)
    return {StatusCode::kInvalidArgument, "incremental event source is invalid"};
  if (event.event_id != applied_.event_id + 1U)
    return {StatusCode::kCorruption, "incremental event id gap"};
  Status status;
  switch (event.type) {
    case CommandType::kSet:
      status = engine_.Upsert(event.key, event.value).ok()
                   ? Status::Ok()
                   : Status{StatusCode::kInternal, "incremental upsert failed"};
      break;
    case CommandType::kMod:
      status = engine_.Modify(event.key, event.value);
      break;
    case CommandType::kDel:
      status = engine_.Delete(event.key);
      if (status.code() == StatusCode::kNotFound) status = Status::Ok();
      break;
    case CommandType::kIncr:
    case CommandType::kIncrBy:
    case CommandType::kDecr:
    case CommandType::kDecrBy: {
      std::int64_t delta = 0;
      try {
        delta = std::stoll(event.value);
      } catch (...) {
        return {StatusCode::kInvalidArgument, "incremental integer is invalid"};
      }
      status = engine_.Increment(event.key, delta).ok()
                   ? Status::Ok()
                   : Status{StatusCode::kInternal, "incremental increment failed"};
      break;
    }
    default:
      return {StatusCode::kInvalidArgument, "incremental event is not a write"};
  }
  if (!status.ok()) return status;
  applied_ = {event.offset, event.event_id};
  applied_checksum_ = event.checksum;
  return Status::Ok();
}

Status ReplicaSyncApplier::ApplyIncremental(const std::vector<WriteEvent>& events) {
  std::scoped_lock lock(mutex_);
  const auto before = engine_.Export();
  if (!before.ok()) return before.status();
  const auto initial_applied = applied_;
  const auto initial_checksum = applied_checksum_;
  const auto rollback = [&] {
    const auto status = engine_.Import(before.value());
    applied_ = initial_applied;
    applied_checksum_ = initial_checksum;
    return status;
  };
  for (const auto& event : events) {
    if (event.offset == applied_.offset && event.event_id == applied_.event_id) {
      if (event.checksum != applied_checksum_) {
        const auto rollback_status = rollback();
        if (!rollback_status.ok()) return rollback_status;
        return {StatusCode::kCorruption, "incremental duplicate event fork detected"};
      }
      continue;
    }
    if (event.offset <= applied_.offset) {
      const auto rollback_status = rollback();
      if (!rollback_status.ok()) return rollback_status;
      return {StatusCode::kCorruption, "incremental event fork detected"};
    }
    const auto status = Apply(event);
    if (!status.ok()) {
      const auto rollback_status = rollback();
      if (!rollback_status.ok()) return rollback_status;
      return status;
    }
  }
  return Status::Ok();
}

Status ReplicaSyncApplier::Ack(ReplicationAck ack) {
  std::scoped_lock lock(mutex_);
  if (ack.offset != applied_.offset || ack.event_id != applied_.event_id)
    return {StatusCode::kInvalidArgument, "replica ACK does not match applied point"};
  return Status::Ok();
}

ReplicationAck ReplicaSyncApplier::applied() const {
  std::scoped_lock lock(mutex_);
  return applied_;
}

}  // namespace kvstore
