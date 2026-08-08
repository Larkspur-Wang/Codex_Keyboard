#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ai_keyboard {

// NimBLE invokes a characteristic access callback again for each Read Blob
// offset. Keep one immutable value per remote connection so an asynchronous
// status refresh cannot splice two JSON generations into a single long read.
//
// Thread safety is intentionally external: firmware protects this cache with
// its existing status spinlock, while host tests can use it directly.
template <std::size_t MaxReaders, std::size_t MaxValueLen>
class GattStatusSnapshotCache {
 public:
  static_assert(MaxReaders > 0);
  static_assert(MaxValueLen > 0);

  GattStatusSnapshotCache() {
    static constexpr char kInitialValue[] = "ready";
    (void)publish(kInitialValue, sizeof(kInitialValue) - 1);
  }

  bool publish(const char* value, std::size_t len) {
    if ((value == nullptr && len != 0) || len > MaxValueLen) {
      return false;
    }
    if (len != 0) {
      std::copy_n(value, len, published_value_.data());
    }
    published_value_[len] = '\0';
    published_len_ = len;
    return true;
  }

  bool copy_published(char* out,
                      std::size_t out_capacity,
                      std::size_t* out_len) const {
    return copy_value(published_value_, published_len_, out, out_capacity, out_len);
  }

  bool copy_for_remote_read(std::uint16_t conn_handle,
                            std::uint16_t offset,
                            char* out,
                            std::size_t out_capacity,
                            std::size_t* out_len) {
    Snapshot* snapshot = find(conn_handle);
    if (offset == 0) {
      if (snapshot == nullptr) {
        snapshot = allocate(conn_handle);
      }
      snapshot->value = published_value_;
      snapshot->len = published_len_;
      snapshot->last_start_sequence = next_sequence();
    } else if (snapshot == nullptr) {
      return false;
    }

    return copy_value(snapshot->value, snapshot->len, out, out_capacity, out_len);
  }

  void forget(std::uint16_t conn_handle) {
    if (auto* snapshot = find(conn_handle); snapshot != nullptr) {
      *snapshot = {};
    }
  }

  void clear_snapshots() {
    snapshots_ = {};
  }

 private:
  struct Snapshot {
    std::array<char, MaxValueLen + 1> value{};
    std::size_t len = 0;
    std::uint32_t last_start_sequence = 0;
    std::uint16_t conn_handle = 0;
    bool valid = false;
  };

  static bool copy_value(const std::array<char, MaxValueLen + 1>& value,
                         std::size_t len,
                         char* out,
                         std::size_t out_capacity,
                         std::size_t* out_len) {
    if (out == nullptr || out_len == nullptr || len > out_capacity) {
      return false;
    }
    if (len != 0) {
      std::copy_n(value.data(), len, out);
    }
    *out_len = len;
    return true;
  }

  Snapshot* find(std::uint16_t conn_handle) {
    for (auto& snapshot : snapshots_) {
      if (snapshot.valid && snapshot.conn_handle == conn_handle) {
        return &snapshot;
      }
    }
    return nullptr;
  }

  Snapshot* allocate(std::uint16_t conn_handle) {
    Snapshot* selected = &snapshots_.front();
    for (auto& snapshot : snapshots_) {
      if (!snapshot.valid) {
        selected = &snapshot;
        break;
      }
      if (snapshot.last_start_sequence < selected->last_start_sequence) {
        selected = &snapshot;
      }
    }
    *selected = {};
    selected->valid = true;
    selected->conn_handle = conn_handle;
    return selected;
  }

  std::uint32_t next_sequence() {
    ++start_sequence_;
    if (start_sequence_ == 0) {
      // Zero denotes an unused slot. Renormalize the active slots on the
      // practically unreachable 32-bit wrap instead of making one look free.
      std::uint32_t sequence = 1;
      for (auto& snapshot : snapshots_) {
        if (snapshot.valid) {
          snapshot.last_start_sequence = sequence++;
        }
      }
      start_sequence_ = sequence;
    }
    return start_sequence_;
  }

  std::array<char, MaxValueLen + 1> published_value_{};
  std::size_t published_len_ = 0;
  std::array<Snapshot, MaxReaders> snapshots_{};
  std::uint32_t start_sequence_ = 0;
};

}  // namespace ai_keyboard
