#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

#include "keyboard/gatt_status_snapshot.h"

namespace {

using Cache = ai_keyboard::GattStatusSnapshotCache<2, 512>;

std::string read(Cache* cache, std::uint16_t conn_handle, std::uint16_t offset) {
  std::array<char, 512> value{};
  std::size_t len = 0;
  assert(cache->copy_for_remote_read(
      conn_handle, offset, value.data(), value.size(), &len));
  return {value.data(), len};
}

void read_blob_offsets_keep_the_offset_zero_generation() {
  Cache cache;
  assert(cache.publish("{\"generation\":1}", 16));
  assert(read(&cache, 7, 0) == "{\"generation\":1}");

  assert(cache.publish("{\"generation\":2}", 16));
  assert(read(&cache, 7, 8) == "{\"generation\":1}");
  assert(read(&cache, 8, 0) == "{\"generation\":2}");
  assert(read(&cache, 7, 16) == "{\"generation\":1}");

  assert(read(&cache, 7, 0) == "{\"generation\":2}");
}

void nonzero_offset_requires_an_existing_connection_snapshot() {
  Cache cache;
  std::array<char, 512> value{};
  std::size_t len = 0;
  assert(!cache.copy_for_remote_read(7, 1, value.data(), value.size(), &len));

  assert(read(&cache, 7, 0) == "ready");
  cache.forget(7);
  assert(!cache.copy_for_remote_read(7, 1, value.data(), value.size(), &len));
}

void local_reads_do_not_replace_remote_snapshots() {
  Cache cache;
  assert(cache.publish("old", 3));
  assert(read(&cache, 7, 0) == "old");
  assert(cache.publish("new", 3));

  std::array<char, 512> value{};
  std::size_t len = 0;
  assert(cache.copy_published(value.data(), value.size(), &len));
  assert(std::string(value.data(), len) == "new");
  assert(read(&cache, 7, 1) == "old");
}

void wire_budget_is_enforced() {
  Cache cache;
  std::array<char, 512> maximum{};
  maximum.fill('x');
  assert(cache.publish(maximum.data(), maximum.size()));
  assert(read(&cache, 7, 0).size() == maximum.size());

  std::array<char, 513> too_large{};
  assert(!cache.publish(too_large.data(), too_large.size()));
  assert(read(&cache, 8, 0).size() == maximum.size());
}

void full_reader_set_keeps_each_active_connection_stable() {
  Cache cache;
  assert(cache.publish("one", 3));
  assert(read(&cache, 7, 0) == "one");
  assert(cache.publish("two", 3));
  assert(read(&cache, 8, 0) == "two");
  assert(cache.publish("three", 5));

  assert(read(&cache, 7, 1) == "one");
  assert(read(&cache, 8, 1) == "two");
}

}  // namespace

int main() {
  read_blob_offsets_keep_the_offset_zero_generation();
  nonzero_offset_requires_an_existing_connection_snapshot();
  local_reads_do_not_replace_remote_snapshots();
  wire_budget_is_enforced();
  full_reader_set_keeps_each_active_connection_stable();
  return 0;
}
