#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "speaker_assets/speaker_assets_session.h"

namespace easy_input::speaker_assets {

// Transport callbacks only copy fixed-size USB or Wi-Fi logical records into
// these queues. The owner task performs decoding and session work.
inline constexpr std::size_t kSpeakerAssetsRuntimeIngressCapacity = 20U;
inline constexpr std::size_t kSpeakerAssetsRuntimeLifecycleCapacity = 8U;
inline constexpr std::size_t kSpeakerAssetsRuntimeReplyCapacity = 4U;
inline constexpr std::size_t kSpeakerAssetsRuntimeActiveRoutes = 4U;
inline constexpr std::size_t
    kSpeakerAssetsRuntimeClosedRouteFilterWords = 64U;
inline constexpr std::size_t
    kSpeakerAssetsRuntimeMailboxCloseCapacity = 8U;
inline constexpr std::uint32_t
    kSpeakerAssetsRuntimePartialTimeoutMs = 5000U;
inline constexpr std::uint32_t
    kSpeakerAssetsRuntimeSessionLeaseMs = 10000U;

enum class SpeakerAssetsRuntimeEnqueueResult : std::uint8_t {
  Accepted,
  Coalesced,
  Full,
  InvalidArgument,
};

enum class SpeakerAssetsActionExecutionResult : std::uint8_t {
  // No durable completion exists yet. A cooperative runner retains its own
  // cursor and will be called again only while resource work is allowed.
  Pending,
  Completed,
  // Terminal and side-effect-free. Runtime converts this to an Unavailable
  // completion so the Session unwinds instead of wedging borrowed state.
  Rejected,
};

// Production uses SpeakerAssetsCooperativeStoreRunner (declared separately)
// to implement this with bounded, exactly permitted Flash/hash units. A
// synchronous Store adapter is deliberately not supplied here: putting a long
// erase/hash behind a low-priority task does not make it input-safe.
class SpeakerAssetsRuntimeActionExecutor {
 public:
  virtual ~SpeakerAssetsRuntimeActionExecutor() = default;

  virtual SpeakerAssetsActionExecutionResult step(
      const SpeakerAssetsActionView& action,
      SpeakerAssetsActionCompletion* completion) = 0;
};

enum class SpeakerAssetsRuntimeStepResult : std::uint8_t {
  Idle,
  LifecycleApplied,
  RouteCapacityExceeded,
  UsbFrameConsumed,
  WifiFrameConsumed,
  ReplyQueued,
  ActionQueued,
  ActionExecutionPending,
  ActionCompleted,
  PausedForInput,
  ReplyBackpressure,
  PartialExpired,
  SessionLeaseExpired,
  InvalidFrameDropped,
  StaleRouteDropped,
  ExecutorUnavailable,
  ExecutorRejected,
  SessionRejected,
};

// Opaque boot-lifetime admission identity. Route generations prevent a stale
// connection from reviving; admission_id prevents an old completion on the
// same exact route from releasing a newer logical request (ABA).
struct SpeakerAssetsLogicalRequestLease {
  SpeakerAssetsRouteToken route{};
  std::uint64_t admission_id = 0U;
};

bool speaker_assets_logical_request_lease_equal(
    const SpeakerAssetsLogicalRequestLease& first,
    const SpeakerAssetsLogicalRequestLease& second);

struct SpeakerAssetsRuntimeReply {
  std::uint32_t sequence = 0U;
  SpeakerAssetsRouteToken route{};
  SpeakerAssetsLogicalRequestLease lease{};
  // This remains a normalized logical frame. Physical encoding belongs to the
  // route-specific USB or Wi-Fi transport worker.
  SpeakerAssetsFrame frame{};
};

enum class SpeakerAssetsRuntimeMailboxKind : std::uint8_t {
  RouteOpened,
  RouteClosed,
  // Sticky fail-closed fallback when exact Close traffic exceeds its
  // independent queue. The Core revokes every resource route before it can
  // process any lower-priority Open or payload.
  AllRoutesClosed,
  UsbFrame,
  WifiFrame,
};

struct SpeakerAssetsRuntimeMailboxRecord {
  SpeakerAssetsRuntimeMailboxKind kind =
      SpeakerAssetsRuntimeMailboxKind::UsbFrame;
  SpeakerAssetsRouteToken route{};
  std::uint64_t admission_id = 0U;
  std::uint32_t received_ms = 0U;
  std::uint16_t length = 0U;
  std::array<std::uint8_t, kSpeakerAssetsWifiFrameMaxBytes>
      bytes{};
  // Internal cancellation marker. take_next() never exposes such a record.
  bool cancelled = false;
};

// Callback-facing fixed mailbox. A platform wraps every mailbox method used
// after publication (enqueue/take/release/query) in its existing very short
// critical section; clear() is init/reset-only under exclusive ownership.
// These calls only validate physical shape, scan bounded metadata and
// copy/pop one fixed record; they never decode a logical EIA frame, touch
// Session, execute SHA or call Flash.
//
// The supervisor releases the critical section immediately after take_next(),
// then imports the record into its single-owner RuntimeCore and calls step().
// Close records have an independent high-priority queue, so data pressure
// cannot prevent exact route revocation.
class SpeakerAssetsRuntimeMailbox {
 public:
  SpeakerAssetsRuntimeMailbox() = default;
  SpeakerAssetsRuntimeMailbox(
      const SpeakerAssetsRuntimeMailbox&) = delete;
  SpeakerAssetsRuntimeMailbox& operator=(
      const SpeakerAssetsRuntimeMailbox&) = delete;
  SpeakerAssetsRuntimeMailbox(
      SpeakerAssetsRuntimeMailbox&&) = delete;
  SpeakerAssetsRuntimeMailbox& operator=(
      SpeakerAssetsRuntimeMailbox&&) = delete;

  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult enqueue_route_opened(
      const SpeakerAssetsRouteToken& exact_route);
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult enqueue_route_closed(
      const SpeakerAssetsRouteToken& exact_route);
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult enqueue_usb_frame(
      const SpeakerAssetsRouteToken& exact_route,
      const std::uint8_t* bytes,
      std::size_t length,
      std::uint32_t received_ms);
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult enqueue_wifi_frame(
      const SpeakerAssetsRouteToken& exact_route,
      const std::uint8_t* bytes,
      std::size_t length,
      std::uint32_t received_ms);
  // Destructively copies one close-first record. The caller must hold only the
  // mailbox's short producer/consumer critical section around this call.
  bool take_next(SpeakerAssetsRuntimeMailboxRecord* record);
  void clear();

  // V1 admits at most one logical request globally. A USB or Wi-Fi frame
  // atomically claims a non-reused admission lease inside the callback
  // critical section; every other request receives Full.
  // The supervisor compare-and-releases the exact lease only after its reply
  // was sent or step() returned that lease for a no-reply terminal
  // drop/physical timeout. Release purges queued tail records atomically.
  // Exact Close remains the route-only force-revoke operation.
  bool release_logical_request(
      const SpeakerAssetsLogicalRequestLease& lease);
  bool logical_request_lease(
      SpeakerAssetsLogicalRequestLease* lease) const;
  bool logical_request_reserved_for(
      const SpeakerAssetsRouteToken& exact_route) const;

  // These observations touch producer/consumer state and therefore require
  // the same short mailbox critical section as enqueue_*()/take_next().
  std::size_t data_size() const;
  std::size_t close_size() const;
  std::size_t raw_data_credit() const;

 private:
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult enqueue_data(
      SpeakerAssetsRuntimeMailboxKind kind,
      const SpeakerAssetsRouteToken& exact_route,
      const std::uint8_t* bytes,
      std::size_t length,
      std::uint32_t received_ms);
  bool close_pending(
      const SpeakerAssetsRouteToken& exact_route) const;
  void purge_pending_route(
      const SpeakerAssetsRouteToken& exact_route);
  void purge_pending_admission(std::uint64_t admission_id);
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult
  retain_fail_closed_tombstone(
      const SpeakerAssetsRouteToken& exact_route);
  void fail_close_pending_data();
  void clear_logical_request_reservation();
  void revoke_logical_request_for_route(
      const SpeakerAssetsRouteToken& exact_route);
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult
  reserve_logical_frame_request(
      const SpeakerAssetsRouteToken& exact_route,
      bool* newly_reserved);
  std::array<SpeakerAssetsRuntimeMailboxRecord,
             kSpeakerAssetsRuntimeIngressCapacity>
      data_{};
  std::size_t data_head_ = 0U;
  std::size_t data_size_ = 0U;
  std::array<SpeakerAssetsRouteToken,
             kSpeakerAssetsRuntimeMailboxCloseCapacity>
      closes_{};
  std::size_t close_head_ = 0U;
  std::size_t close_size_ = 0U;
  SpeakerAssetsRouteToken reserved_route_{};
  std::uint64_t reserved_admission_id_ = 0U;
  std::uint64_t next_admission_id_ = 1U;
  bool logical_request_reserved_ = false;
  bool logical_frame_request_queued_ = false;
  bool revoke_all_pending_ = false;
  // If even the post-sentinel tombstone capacity is exhausted, resource sync
  // remains unavailable until an exclusive clear/reset instead of forgetting
  // a one-shot Close and reviving an old lifetime.
  bool resource_sync_fail_closed_ = false;
  SpeakerAssetsRouteToken revoke_all_overflow_route_{};
};

// Allocation-free, platform-independent resource supervisor core.
//
// Threading contract:
// - Every method on this class belongs to one supervisor owner task.
// - TinyUSB/TCP carrier callbacks use SpeakerAssetsRuntimeMailbox, never Core.
// - The owner imports a mailbox record only after releasing the mailbox lock.
// Therefore decode/session/cooperative Flash work never runs under a callback
// critical section and the Core itself needs no lock.
class SpeakerAssetsRuntimeCore {
 public:
  explicit SpeakerAssetsRuntimeCore(
      std::uint32_t cookie_seed = 1U,
      std::uint32_t action_token_seed = 1U,
      std::uint32_t partial_timeout_ms =
          kSpeakerAssetsRuntimePartialTimeoutMs,
      std::uint32_t session_lease_ms =
          kSpeakerAssetsRuntimeSessionLeaseMs);
  SpeakerAssetsRuntimeCore(const SpeakerAssetsRuntimeCore&) = delete;
  SpeakerAssetsRuntimeCore& operator=(
      const SpeakerAssetsRuntimeCore&) = delete;
  SpeakerAssetsRuntimeCore(SpeakerAssetsRuntimeCore&&) = delete;
  SpeakerAssetsRuntimeCore& operator=(
      SpeakerAssetsRuntimeCore&&) = delete;

#if defined(EASY_INPUT_SPEAKER_ASSETS_RUNTIME_TEST_ACCESS)
 public:
#else
 private:
#endif
  // Direct Core injection exists only for allocation-free Host tests. Product
  // code must enter through the callback mailbox and import_mailbox_record(),
  // which rejects every data record without a non-zero admission lease.
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult enqueue_route_opened(
      const SpeakerAssetsRouteToken& exact_route);
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult enqueue_route_closed(
      const SpeakerAssetsRouteToken& exact_route);
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult enqueue_usb_frame(
      const SpeakerAssetsRouteToken& exact_route,
      const std::uint8_t* bytes,
      std::size_t length,
      std::uint32_t received_ms);
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult enqueue_wifi_frame(
      const SpeakerAssetsRouteToken& exact_route,
      const std::uint8_t* bytes,
      std::size_t length,
      std::uint32_t received_ms);
 public:
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult import_mailbox_record(
      const SpeakerAssetsRuntimeMailboxRecord& record);

  struct StepOutcome {
#if defined(EASY_INPUT_SPEAKER_ASSETS_RUNTIME_TEST_ACCESS)
   public:
#else
   private:
#endif
    SpeakerAssetsRuntimeStepResult result =
        SpeakerAssetsRuntimeStepResult::Idle;
    // Non-zero only for a no-reply terminal drop or physical timeout. Replies
    // carry their own lease and release only after transport send acceptance.
    SpeakerAssetsLogicalRequestLease release_now{};
    friend class SpeakerAssetsRuntimeCore;

   public:
    // Product code cannot read result alone. It must retain the outcome and
    // request both values together, then compare-and-release any non-zero
    // lease under the mailbox's short critical section.
    [[nodiscard]] bool inspect(
        SpeakerAssetsRuntimeStepResult* result_output,
        SpeakerAssetsLogicalRequestLease* release_output) const {
      if (result_output == nullptr || release_output == nullptr) {
        return false;
      }
      *result_output = result;
      *release_output = release_now;
      return true;
    }

#if defined(EASY_INPUT_SPEAKER_ASSETS_RUNTIME_TEST_ACCESS)
    // Result-only comparison is deliberately absent from the product API.
    // Host tests may use this convenience while production supervisors must
    // retain the outcome and call inspect() on every result.
    friend constexpr bool operator==(
        const StepOutcome& outcome,
        SpeakerAssetsRuntimeStepResult expected) {
      return outcome.result == expected;
    }

    friend constexpr bool operator==(
        SpeakerAssetsRuntimeStepResult expected,
        const StepOutcome& outcome) {
      return outcome == expected;
    }
#endif
  };

  // Performs at most one lifecycle, protocol, or executor unit. Lifecycle
  // close always wins so an old route is revoked before any queued payload or
  // orphaned completion can reply. There is intentionally no result-only
  // production entry point: the supervisor must inspect both fields together
  // after every step and return any exact release lease to the mailbox.
  [[nodiscard]] StepOutcome step(
      std::uint32_t now_ms,
      bool resource_steps_allowed,
      SpeakerAssetsRuntimeActionExecutor* executor);

  bool front_reply(SpeakerAssetsRuntimeReply* reply) const;
  // Returns the oldest reply for one exact route.
  bool front_reply_for_route(
      const SpeakerAssetsRouteToken& exact_route,
      SpeakerAssetsRuntimeReply* reply) const;
  bool pop_reply_if_sequence(std::uint32_t sequence);
  // Removes a previously selected reply even when it is not the global head.
  // Transport workers call this only after their exact send was accepted.
  bool remove_reply_if_sequence(std::uint32_t sequence);

  std::size_t ingress_size() const;
  std::size_t lifecycle_size() const;
  std::size_t reply_size() const;
  std::size_t active_route_count() const;
  std::size_t raw_ingress_credit() const;
  // V1 is stop-and-wait. This is per-route eligibility, not a consumable
  // grant: the callback mailbox atomically reserves the single global logical
  // request when a USB or Wi-Fi frame is accepted. Eligibility is
  // zero whenever input/recording has priority, a Store action owns borrowed
  // session memory or replies cannot be retained without loss.
  std::size_t advertised_ingress_credit(
      const SpeakerAssetsRouteToken& exact_route,
      bool resource_steps_allowed) const;
  bool action_pending() const;
  SpeakerAssetsSessionPhase session_phase() const;

 private:
  enum class IngressKind : std::uint8_t {
    UsbFrame,
    WifiFrame,
  };

  struct IngressRecord {
    IngressKind kind = IngressKind::UsbFrame;
    SpeakerAssetsRouteToken route{};
    std::uint64_t admission_id = 0U;
    std::uint32_t received_ms = 0U;
    std::uint16_t length = 0U;
    std::array<std::uint8_t, kSpeakerAssetsWifiFrameMaxBytes>
        bytes{};
  };

  enum class LifecycleKind : std::uint8_t {
    Opened,
    Closed,
  };

  struct LifecycleRecord {
    LifecycleKind kind = LifecycleKind::Opened;
    SpeakerAssetsRouteToken route{};
  };

  struct ActiveRoute {
    bool active = false;
    SpeakerAssetsRouteToken route{};
  };

  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult enqueue_lifecycle(
      LifecycleKind kind,
      const SpeakerAssetsRouteToken& exact_route);
  [[nodiscard]] SpeakerAssetsRuntimeEnqueueResult enqueue_ingress(
      IngressKind kind,
      const SpeakerAssetsRouteToken& exact_route,
      const std::uint8_t* bytes,
      std::size_t length,
      std::uint32_t received_ms,
      std::uint64_t admission_id);

  bool route_is_active(
      const SpeakerAssetsRouteToken& exact_route) const;
  bool activate_route(
      const SpeakerAssetsRouteToken& exact_route);
  bool route_generation_is_closed(
      const SpeakerAssetsRouteToken& exact_route) const;
  void remember_closed_route(
      const SpeakerAssetsRouteToken& exact_route);
  void revoke_all_routes(
      const SpeakerAssetsRouteToken& overflow_route);
  bool route_has_ingress(
      const SpeakerAssetsRouteToken& exact_route) const;
  bool route_has_reply(
      const SpeakerAssetsRouteToken& exact_route) const;
  void close_route(
      const SpeakerAssetsRouteToken& exact_route);
  void purge_ingress_for_route(
      const SpeakerAssetsRouteToken& exact_route);
  void purge_ingress_for_admission(std::uint64_t admission_id);
  void purge_replies_for_route(
      const SpeakerAssetsRouteToken& exact_route);
  [[nodiscard]] SpeakerAssetsRuntimeStepResult process_lifecycle();
  [[nodiscard]] SpeakerAssetsRuntimeStepResult process_ingress(
      const IngressRecord& record,
      bool resource_steps_allowed,
      std::uint32_t now_ms);
  [[nodiscard]] SpeakerAssetsRuntimeStepResult process_usb_frame(
      const IngressRecord& record,
      bool resource_steps_allowed,
      std::uint32_t now_ms);
  [[nodiscard]] SpeakerAssetsRuntimeStepResult process_wifi_frame(
      const IngressRecord& record,
      bool resource_steps_allowed,
      std::uint32_t now_ms);
  [[nodiscard]] SpeakerAssetsRuntimeStepResult consume_logical_frame(
      const SpeakerAssetsRouteToken& exact_route,
      const SpeakerAssetsFrame& frame,
      std::uint32_t activity_ms,
      SpeakerAssetsRuntimeStepResult no_emission_result,
      bool resource_steps_allowed,
      std::uint64_t admission_id);
  [[nodiscard]] SpeakerAssetsRuntimeStepResult process_pending_action(
      std::uint32_t now_ms,
      bool resource_steps_allowed,
      SpeakerAssetsRuntimeActionExecutor* executor);
  [[nodiscard]] SpeakerAssetsRuntimeStepResult step_once(
      std::uint32_t now_ms,
      bool resource_steps_allowed,
      SpeakerAssetsRuntimeActionExecutor* executor);
  [[nodiscard]] SpeakerAssetsRuntimeStepResult expire_partials(
      std::uint32_t now_ms);
  [[nodiscard]] SpeakerAssetsRuntimeStepResult expire_session_lease(
      std::uint32_t now_ms);
  bool expire_session_partial_after_logical_frame(
      std::uint32_t now_ms);

  bool push_reply(const SpeakerAssetsRouteToken& route,
                  const SpeakerAssetsFrame& frame,
                  std::uint64_t admission_id);
  void retire_admission_without_reply(
      const SpeakerAssetsRouteToken& route,
      std::uint64_t admission_id);
  void purge_admission_tail(std::uint64_t admission_id);
  void update_session_partial_activity(
      const SpeakerAssetsRouteToken& consumed_route,
      std::uint32_t received_ms,
      bool accepted_partial_activity);
  void refresh_session_lease(
      const SpeakerAssetsRouteToken& consumed_route,
      std::uint32_t session_cookie,
      std::uint32_t activity_ms);
  void clear_session_lease();
  std::array<IngressRecord, kSpeakerAssetsRuntimeIngressCapacity>
      ingress_{};
  std::size_t ingress_head_ = 0U;
  std::size_t ingress_size_ = 0U;

  std::array<LifecycleRecord, kSpeakerAssetsRuntimeLifecycleCapacity>
      lifecycle_{};
  std::size_t lifecycle_head_ = 0U;
  std::size_t lifecycle_size_ = 0U;

  std::array<SpeakerAssetsRuntimeReply,
             kSpeakerAssetsRuntimeReplyCapacity>
      replies_{};
  std::size_t reply_head_ = 0U;
  std::size_t reply_size_ = 0U;
  std::uint32_t next_reply_sequence_ = 1U;

  std::array<ActiveRoute, kSpeakerAssetsRuntimeActiveRoutes>
      active_routes_{};
  // Two-bit Bloom-style tombstone filter. It never forgets a closed exact
  // lifetime during one boot, so old Open events cannot revive after handle
  // churn. A collision can only reject resource sync fail-closed; it cannot
  // affect keyboard, microphone or HID routing.
  std::array<std::uint64_t,
             kSpeakerAssetsRuntimeClosedRouteFilterWords>
      closed_route_filter_{};
  SpeakerAssetsSession session_;
  SpeakerAssetsActionView pending_action_{};
  SpeakerAssetsLogicalRequestLease pending_action_lease_{};
  bool pending_action_active_ = false;
  SpeakerAssetsLogicalRequestLease release_now_{};

  SpeakerAssetsRouteToken session_partial_route_{};
  std::uint32_t session_partial_last_activity_ms_ = 0U;
  bool session_partial_active_ = false;
  std::uint32_t partial_timeout_ms_ =
      kSpeakerAssetsRuntimePartialTimeoutMs;
  SpeakerAssetsRouteToken session_lease_route_{};
  std::uint32_t session_lease_last_activity_ms_ = 0U;
  bool session_lease_active_ = false;
  std::uint32_t session_lease_ms_ =
      kSpeakerAssetsRuntimeSessionLeaseMs;
};

}  // namespace easy_input::speaker_assets
