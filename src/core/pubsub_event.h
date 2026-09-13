// pubsub_event.h -- heap-owned messages carried between IO threads.
//
// This is transport data only.  Subscriber indexes and connection state stay in IoLoop, so this
// header does not give executors (or shards) a path to either.  Events name a connection by its
// process-unique id and owning IO; no cross-thread Client pointer is retained.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tomo {

enum class PubSubEventKind : uint8_t {
    ModifyRequest,
    ModifyResult,
    PublishRequest,
    NotificationEnqueue,
    NotificationRequest,
    NotificationContinue,
    DeliveryBatch,
    ChannelsRequest,
    ChannelsResult,
    NumsubRequest,
    NumsubResult,
    NumpatRequest,
    NumpatResult,
    ResetRequest,
    ResetResult,
    CleanupRequest,
    CleanupResult,
    AclPermissionsChanged,
    AclUserDeleted,
    ClientListRequest,
    ClientListResult,
    ClientKillRequest,
    ClientKillResult,
    // Lane F. All four are produced only while the CLIENT/MONITOR armed word is non-zero.
    MonitorFeed,          // one pre-formatted feed line, encoded once and shared by every owner
    TrackingInvalidate,   // keys whose value changed; each owner filters against its own table
    TrackingFlush,        // FLUSHALL/FLUSHDB: deliver the null invalidation to every tracker
    TrackingDeliver,      // redirect hop: owner of the redirect target emits the frame
    ClientUnblockRequest,
    ClientUnblockResult,
};

enum ClientFilterMask : uint32_t {
    ClientFilterId = 1u << 0,
    ClientFilterAddr = 1u << 1,
    ClientFilterLaddr = 1u << 2,
    ClientFilterType = 1u << 3,
    ClientFilterUser = 1u << 4,
    ClientFilterMaxAge = 1u << 5,
    ClientFilterIdList = 1u << 6,
};

enum class ClientTypeFilter : int8_t {
    Any = -1,
    Normal = 0,
    Master = 1,
    Replica = 2,
    Pubsub = 3,
};

struct PubSubEventItem {
    uint32_t index = 0;
    bool changed = false;
    uint64_t count = 0;
    std::string value;
};

// One publish, encoded ONCE at the channel home and shared by every owning IO thread that has a
// subscriber for it.  Refcounted because the owners retire independently: the last one frees it.
//
// `frame` holds the RESP2 delivery exactly as it goes on the wire.  The RESP3 push frame is
// byte-identical except for the leading '*' -> '>' (both element counts are single digits), so one
// encoding serves both protocols.  `body_off` is the offset of the `$<len>\r\n<channel>...` tail,
// which `pmessage` reuses verbatim behind its own 4-element header.
struct PubSubBlob {
    std::atomic<uint32_t> refs{0};
    bool shard = false;
    uint32_t body_off = 0;
    std::string frame;
};

// One (blob, destination connection) pair queued for an owning IO thread.  Connections are named
// by process-unique id, never by a cross-thread Client*.  A non-empty `pattern` selects the
// `pmessage` shape.  Each item holds ONE reference on `blob`.
struct PubSubDelivery {
    PubSubBlob* blob = nullptr;
    uint64_t conn = 0;
    std::string pattern;
};

// A finished PUBLISH/SPUBLISH, batched back to the publisher's own IO thread.
struct PubSubResult {
    uint64_t conn_id = 0;
    uint64_t op_id = 0;
    uint64_t count = 0;
};

struct PubSubNotificationItem {
    std::string channel;
    std::string message;
};

struct PubSubNotificationChain {
    uint32_t coordinator_io = 0;
    uint32_t index = 0;
    std::vector<PubSubNotificationItem> items;
};

struct PubSubEvent {
    PubSubEventKind kind = PubSubEventKind::DeliveryBatch;
    uint32_t origin_io = 0;
    uint32_t target_io = 0;
    uint64_t conn_id = 0;
    uint64_t op_id = 0;
    bool pattern = false;
    bool shard = false;
    bool subscribe = false;
    uint64_t count = 0;
    uint32_t acl_user_index = 0;
    const void* acl_permissions = nullptr;  // immutable AclPerm; intentionally transport-opaque
    uint32_t client_filter_mask = 0;
    ClientTypeFilter client_type = ClientTypeFilter::Any;
    uint64_t client_id = 0;
    uint64_t client_max_age = 0;
    uint64_t caller_id = 0;
    bool client_skipme = false;
    bool client_old_form = false;
    std::string client_addr;
    std::string client_laddr;
    std::string client_user;
    std::string channel;
    std::string message;
    std::string pattern_text;
    std::vector<PubSubEventItem> items;
    // DeliveryBatch payload: every publish this home resolved for ONE destination IO in one pass,
    // plus every publish reply owed to that same IO.  Both stay in publish order.
    std::vector<PubSubDelivery> deliveries;
    std::vector<PubSubResult> results;
    std::shared_ptr<PubSubNotificationChain> notification;
    // Encode-once payload shared by every receiving owner (MONITOR feed line). Null on every
    // pre-Lane-F event kind, so no existing path pays for it beyond the empty shared_ptr.
    std::shared_ptr<const std::string> blob;
};

}  // namespace tomo
