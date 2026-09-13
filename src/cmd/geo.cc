// geo.cc -- Redis-compatible geospatial commands backed by sorted sets.
//
// Coordinates use the public 52-bit geohash scheme (26 longitude bits interleaved with 26
// latitude bits). All key access stays on the shard owner through the narrow t_zset.h bridge.
#include "geo.h"

#include "blocking.h"
#include "command.h"
#include "notify.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "../core/shard.h"
#include "../exec/op.h"
#include "../net/resp.h"
#include "../store/flatstore.h"

namespace tomo {
namespace {

constexpr double kLatitudeMax = 85.05112878;
constexpr double kEarthRadiusMeters = 6372797.560856;
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr uint64_t kAxisCells = uint64_t{1} << 26;

enum class GeoShape : uint8_t { Radius, Box };
enum class GeoOrder : uint8_t { None, Asc, Desc };

struct GeoSearchOptions {
    bool from_member = false;
    Slice member;
    double longitude = 0;
    double latitude = 0;
    GeoShape shape = GeoShape::Radius;
    double radius_m = 0;
    double width_m = 0;
    double height_m = 0;
    double unit_m = 1;
    GeoOrder order = GeoOrder::None;
    uint64_t count = 0;
    bool any = false;
    bool with_coord = false;
    bool with_dist = false;
    bool with_hash = false;
    bool store = false;
    bool store_distance = false;
    uint32_t source_arg = 1;
    uint32_t destination_arg = 0;
};

struct GeoResult {
    std::string member;
    double score = 0;
    double longitude = 0;
    double latitude = 0;
    double distance_m = 0;
};

// Longitudes, latitudes, radii and box sides all reach redis through getDoubleFromObjectOrReply,
// so they take the same VALUE grammar as a score: "0x10" is 16 degrees, "1e309" is an error, and
// "inf" is a legal radius that simply matches everything. See src/base/numeric.h.
bool parse_double(Slice input, double& value) {
    return parse_double_strict(input, value);
}

bool parse_positive_count(Slice input, uint64_t& value) {
    if (!input.n) return false;
    uint64_t parsed = 0;
    for (uint32_t i = 0; i < input.n; i++) {
        const uint8_t c = static_cast<uint8_t>(input.p[i]);
        if (c < '0' || c > '9') return false;
        if (parsed > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10) return false;
        parsed = parsed * 10 + (c - '0');
    }
    if (!parsed) return false;
    value = parsed;
    return true;
}

bool parse_unit(Slice input, double& meters) {
    if (input.eq_icase("m")) meters = 1.0;
    else if (input.eq_icase("km")) meters = 1000.0;
    else if (input.eq_icase("ft")) meters = 0.3048;
    else if (input.eq_icase("mi")) meters = 1609.34;
    else return false;
    return true;
}

void reply_bad_unit(Op& op) {
    reply_err(op.sink(), "ERR unsupported unit provided. please use M, KM, FT, MI");
}

bool valid_coordinates(double longitude, double latitude) {
    return std::isfinite(longitude) && std::isfinite(latitude) && longitude >= -180.0 &&
           longitude <= 180.0 && latitude >= -kLatitudeMax && latitude <= kLatitudeMax;
}

void reply_invalid_coordinates(Op& op, double longitude, double latitude) {
    // 64 bytes could not hold a large magnitude in FIXED notation (DBL_MAX is 309 integer digits
    // plus '.' plus 6). to_chars then returns value_too_large with ptr == last and leaves the
    // buffer UNSPECIFIED, and appending [begin, ptr) shipped 64 bytes of raw stack to the client:
    // "GEOADD k 1e100 0 m" answered with binary. 344 covers the widest double this can format.
    char lon[344], lat[344];
    auto lon_out = std::to_chars(lon, lon + sizeof(lon), longitude, std::chars_format::fixed, 6);
    auto lat_out = std::to_chars(lat, lat + sizeof(lat), latitude, std::chars_format::fixed, 6);
    if (lon_out.ec != std::errc{}) lon_out.ptr = lon;
    if (lat_out.ec != std::errc{}) lat_out.ptr = lat;
    std::string error = "ERR invalid longitude,latitude pair ";
    error.append(lon, lon_out.ptr);
    error.push_back(',');
    error.append(lat, lat_out.ptr);
    reply_err(op.sink(), error.c_str());
}

uint64_t spread26(uint32_t value) {
    uint64_t result = 0;
    for (int bit = 25; bit >= 0; bit--) result = (result << 2) | ((value >> bit) & 1u);
    return result;
}

uint64_t geo_encode(double longitude, double latitude) {
    const double lon_scaled = (longitude + 180.0) / 360.0 * static_cast<double>(kAxisCells);
    const double lat_scaled = (latitude + kLatitudeMax) / (2.0 * kLatitudeMax) *
                              static_cast<double>(kAxisCells);
    const uint32_t lon = static_cast<uint32_t>(std::min<double>(lon_scaled, kAxisCells - 1));
    const uint32_t lat = static_cast<uint32_t>(std::min<double>(lat_scaled, kAxisCells - 1));
    return (spread26(lon) << 1) | spread26(lat);
}

void geo_decode(double stored_score, double& longitude, double& latitude) {
    const uint64_t hash = static_cast<uint64_t>(stored_score);
    uint32_t lon = 0, lat = 0;
    for (uint32_t bit = 0; bit < 26; bit++) {
        lat |= static_cast<uint32_t>((hash >> (bit * 2)) & 1u) << bit;
        lon |= static_cast<uint32_t>((hash >> (bit * 2 + 1)) & 1u) << bit;
    }
    const double lon_low = -180.0 + static_cast<double>(lon) * 360.0 /
                                      static_cast<double>(kAxisCells);
    const double lon_high = -180.0 + static_cast<double>(lon + 1) * 360.0 /
                                       static_cast<double>(kAxisCells);
    const double lat_low = -kLatitudeMax + static_cast<double>(lat) * (2.0 * kLatitudeMax) /
                                          static_cast<double>(kAxisCells);
    const double lat_high = -kLatitudeMax + static_cast<double>(lat + 1) *
                                           (2.0 * kLatitudeMax) /
                                           static_cast<double>(kAxisCells);
    longitude = (lon_low + lon_high) * 0.5;
    latitude = (lat_low + lat_high) * 0.5;
}

double radians(double degrees) { return degrees * (kPi / 180.0); }

__attribute__((optimize("fp-contract=off")))
double geo_distance(double lon1, double lat1, double lon2, double lat2) {
    const double lat1r = radians(lat1);
    const double lat2r = radians(lat2);
    const double lon1r = radians(lon1);
    const double lon2r = radians(lon2);
    const double sin_lon = std::sin((lon2r - lon1r) * 0.5);
    const double sin_lat = std::sin((lat2r - lat1r) * 0.5);
    const double a = sin_lat * sin_lat + std::cos(lat1r) * std::cos(lat2r) * sin_lon * sin_lon;
    const double distance = 2.0 * kEarthRadiusMeters * std::asin(std::sqrt(std::min(a, 1.0)));
    return distance < 1e-9 ? 0.0 : distance;
}

struct ScoreInterval { uint64_t first, last; };

std::vector<ScoreInterval> geo_neighbor_intervals(const GeoSearchOptions& options,
                                                  double longitude, double latitude) {
    double lat_delta = options.shape == GeoShape::Radius
        ? options.radius_m / kEarthRadiusMeters * 180.0 / kPi
        : options.height_m * 0.5 / kEarthRadiusMeters * 180.0 / kPi;
    const double horizontal = options.shape == GeoShape::Radius
        ? options.radius_m : options.width_m * 0.5;
    const double cosine = std::max(std::abs(std::cos(radians(latitude))), 1e-12);
    const double lon_delta = std::min(180.0, horizontal / kEarthRadiusMeters * 180.0 /
                                             kPi / cosine);
    uint32_t step = 26;
    while (step && (360.0 / static_cast<double>(uint64_t{1} << step) < lon_delta ||
                    (2.0 * kLatitudeMax) /
                        static_cast<double>(uint64_t{1} << step) < lat_delta)) step--;
    if (!step) return {{0, (uint64_t{1} << 52) - 1}};

    const uint64_t center = geo_encode(longitude, latitude);
    uint32_t lon = 0, lat = 0;
    for (uint32_t bit = 0; bit < 26; bit++) {
        lat |= static_cast<uint32_t>((center >> (bit * 2)) & 1u) << bit;
        lon |= static_cast<uint32_t>((center >> (bit * 2 + 1)) & 1u) << bit;
    }
    const uint32_t shift = 26 - step;
    const uint32_t cells = uint32_t{1} << step;
    const uint32_t center_x = lon >> shift;
    const uint32_t center_y = lat >> shift;
    const uint32_t suffix_bits = 52 - step * 2;
    std::vector<ScoreInterval> intervals;
    intervals.reserve(9);
    auto add_cell = [&](int dx, int dy) {
        const int64_t y = static_cast<int64_t>(center_y) + dy;
        if (y < 0 || y >= cells) return;
        const uint32_t x = static_cast<uint32_t>(
            (static_cast<int64_t>(center_x) + dx + cells) % cells);
        const uint32_t xlow = x << shift;
        const uint32_t ylow = static_cast<uint32_t>(y) << shift;
        const uint64_t first = ((spread26(xlow) << 1) | spread26(ylow));
        const uint64_t width = uint64_t{1} << suffix_bits;
        const ScoreInterval interval{first, first + width - 1};
        for (const ScoreInterval& existing : intervals)
            if (existing.first == interval.first) return;
        intervals.push_back(interval);
    };
    add_cell(0, 0);
    static constexpr int neighbors[][2] = {
        {0, 1}, {1, 0}, {-1, 0}, {0, -1},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    };
    for (const auto& neighbor : neighbors) add_cell(neighbor[0], neighbor[1]);
    return intervals;
}

bool parse_search(Op& op, GeoSearchOptions& options) {
    const bool search_store = op.cmd_name().eq_icase("geosearchstore");
    const bool search = op.cmd_name().eq_icase("geosearch") || search_store;
    const bool by_member = op.cmd_name().eq_icase("georadiusbymember") ||
                           op.cmd_name().eq_icase("georadiusbymember_ro");
    const bool read_only_radius = op.cmd_name().eq_icase("georadius_ro") ||
                                  op.cmd_name().eq_icase("georadiusbymember_ro");
    options = {};
    uint32_t arg = 0;
    if (search) {
        options.source_arg = search_store ? 2 : 1;
        options.store = search_store;
        options.destination_arg = search_store ? 1 : 0;
        arg = search_store ? 3 : 2;
        if (arg >= op.argc()) { reply_syntax(op.sink()); return false; }
        if (op.arg(arg).eq_icase("frommember") && arg + 1 < op.argc()) {
            options.from_member = true;
            options.member = op.arg(arg + 1);
            arg += 2;
        } else if (op.arg(arg).eq_icase("fromlonlat") && arg + 2 < op.argc()) {
            if (!parse_double(op.arg(arg + 1), options.longitude) ||
                !parse_double(op.arg(arg + 2), options.latitude)) {
                reply_err(op.sink(), "ERR value is not a valid float");
                return false;
            }
            if (!valid_coordinates(options.longitude, options.latitude)) {
                reply_invalid_coordinates(op, options.longitude, options.latitude);
                return false;
            }
            arg += 3;
        } else { reply_syntax(op.sink()); return false; }

        if (arg >= op.argc()) { reply_syntax(op.sink()); return false; }
        if (op.arg(arg).eq_icase("byradius") && arg + 2 < op.argc()) {
            options.shape = GeoShape::Radius;
            if (!parse_double(op.arg(arg + 1), options.radius_m)) {
                reply_err(op.sink(), "ERR need numeric radius"); return false;
            }
            if (options.radius_m < 0) { reply_err(op.sink(), "ERR radius cannot be negative"); return false; }
            if (!parse_unit(op.arg(arg + 2), options.unit_m)) { reply_bad_unit(op); return false; }
            options.radius_m *= options.unit_m;
            arg += 3;
        } else if (op.arg(arg).eq_icase("bybox") && arg + 3 < op.argc()) {
            options.shape = GeoShape::Box;
            if (!parse_double(op.arg(arg + 1), options.width_m)) {
                reply_err(op.sink(), "ERR need numeric width"); return false;
            }
            if (!parse_double(op.arg(arg + 2), options.height_m)) {
                reply_err(op.sink(), "ERR need numeric height"); return false;
            }
            if (options.width_m < 0 || options.height_m < 0) {
                reply_err(op.sink(), "ERR height or width cannot be negative"); return false;
            }
            if (!parse_unit(op.arg(arg + 3), options.unit_m)) { reply_bad_unit(op); return false; }
            options.width_m *= options.unit_m;
            options.height_m *= options.unit_m;
            arg += 4;
        } else { reply_syntax(op.sink()); return false; }
    } else {
        options.source_arg = 1;
        if (by_member) {
            options.from_member = true;
            options.member = op.arg(2);
            if (!parse_double(op.arg(3), options.radius_m)) {
                reply_err(op.sink(), "ERR need numeric radius"); return false;
            }
            if (options.radius_m < 0) { reply_err(op.sink(), "ERR radius cannot be negative"); return false; }
            if (!parse_unit(op.arg(4), options.unit_m)) { reply_bad_unit(op); return false; }
            arg = 5;
        } else {
            if (!parse_double(op.arg(2), options.longitude) ||
                !parse_double(op.arg(3), options.latitude)) {
                reply_err(op.sink(), "ERR value is not a valid float"); return false;
            }
            if (!parse_double(op.arg(4), options.radius_m)) {
                reply_err(op.sink(), "ERR need numeric radius"); return false;
            }
            if (!valid_coordinates(options.longitude, options.latitude)) {
                reply_invalid_coordinates(op, options.longitude, options.latitude); return false;
            }
            if (options.radius_m < 0) { reply_err(op.sink(), "ERR radius cannot be negative"); return false; }
            if (!parse_unit(op.arg(5), options.unit_m)) { reply_bad_unit(op); return false; }
            arg = 6;
        }
        options.radius_m *= options.unit_m;
    }

    bool count_seen = false;
    for (; arg < op.argc(); arg++) {
        if (op.arg(arg).eq_icase("asc")) options.order = GeoOrder::Asc;
        else if (op.arg(arg).eq_icase("desc")) options.order = GeoOrder::Desc;
        else if (op.arg(arg).eq_icase("withcoord")) options.with_coord = true;
        else if (op.arg(arg).eq_icase("withdist")) options.with_dist = true;
        else if (op.arg(arg).eq_icase("withhash")) options.with_hash = true;
        else if (op.arg(arg).eq_icase("count") && arg + 1 < op.argc()) {
            if (!parse_positive_count(op.arg(++arg), options.count)) {
                reply_err(op.sink(), "ERR COUNT must be > 0"); return false;
            }
            count_seen = true;
            if (arg + 1 < op.argc() && op.arg(arg + 1).eq_icase("any")) {
                options.any = true;
                arg++;
            }
        } else if (op.arg(arg).eq_icase("any")) {
            reply_err(op.sink(), "ERR the ANY argument requires COUNT argument"); return false;
        } else if (!search_store && !read_only_radius &&
                   (op.arg(arg).eq_icase("store") || op.arg(arg).eq_icase("storedist")) &&
                   arg + 1 < op.argc()) {
            if (!options.store) options.destination_arg = arg + 1;
            options.store = true;
            options.store_distance = op.arg(arg).eq_icase("storedist");
            arg++;
        } else if (search_store && op.arg(arg).eq_icase("storedist")) {
            options.store_distance = true;
        } else { reply_syntax(op.sink()); return false; }
    }
    (void)count_seen;
    if (options.store && (options.with_coord || options.with_dist || options.with_hash)) {
        reply_err(op.sink(), search_store
            ? "ERR GEOSEARCHSTORE is not compatible with WITHDIST, WITHHASH and WITHCOORD options"
            : "ERR STORE option in GEORADIUS is not compatible with WITHDIST, WITHHASH and WITHCOORD options");
        return false;
    }
    return true;
}

GeoBuildResult run_search(const GeoSearchOptions& options,
                          const std::vector<ZsetEntry>& source,
                          std::vector<GeoResult>& output) {
    output.clear();
    if (source.empty()) return GeoBuildResult::Ok;
    double center_lon = options.longitude;
    double center_lat = options.latitude;
    if (options.from_member) {
        auto found = std::find_if(source.begin(), source.end(), [&](const ZsetEntry& entry) {
            return entry.member.size() == options.member.n &&
                   (!options.member.n || !std::memcmp(entry.member.data(), options.member.p,
                                                       options.member.n));
        });
        if (found == source.end()) return GeoBuildResult::MissingMember;
        geo_decode(found->score, center_lon, center_lat);
    }
    try {
        const std::vector<ScoreInterval> intervals =
            geo_neighbor_intervals(options, center_lon, center_lat);
        output.reserve(source.size());
        bool early = false;
        for (const ScoreInterval& interval : intervals) {
            auto entry = std::lower_bound(source.begin(), source.end(), interval.first,
                [](const ZsetEntry& item, uint64_t score) {
                    return static_cast<uint64_t>(item.score) < score;
                });
            for (; entry != source.end() &&
                   static_cast<uint64_t>(entry->score) <= interval.last; ++entry) {
                double longitude = 0, latitude = 0;
                geo_decode(entry->score, longitude, latitude);
                const double distance = geo_distance(center_lon, center_lat, longitude, latitude);
                bool inside = distance <= options.radius_m;
                if (options.shape == GeoShape::Box) {
                    const double horizontal = geo_distance(center_lon, latitude,
                                                           longitude, latitude);
                    const double vertical = geo_distance(longitude, center_lat,
                                                         longitude, latitude);
                    inside = horizontal <= options.width_m * 0.5 &&
                             vertical <= options.height_m * 0.5;
                }
                if (inside)
                    output.push_back({entry->member, entry->score, longitude, latitude, distance});
                if (options.any && options.count && output.size() >= options.count) {
                    early = true;
                    break;
                }
            }
            if (early) break;
        }
        if (options.order != GeoOrder::None) {
            std::sort(output.begin(), output.end(), [&](const GeoResult& a, const GeoResult& b) {
                if (a.distance_m != b.distance_m)
                    return options.order == GeoOrder::Desc ? a.distance_m > b.distance_m
                                                           : a.distance_m < b.distance_m;
                return a.score < b.score;
            });
        }
        if (options.count && output.size() > options.count)
            output.resize(static_cast<size_t>(options.count));
    } catch (const std::bad_alloc&) {
        output.clear();
        return GeoBuildResult::Oom;
    }
    return GeoBuildResult::Ok;
}

void reply_fixed_bulk(Op& op, double value, int precision) {
    char text[96];
    auto out = std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed, precision);
    reply_bulk(op.sink(), Slice(text, static_cast<uint32_t>(out.ptr - text)));
}

void reply_coordinate(Op& op, double value) {
    char text[96];
    const auto out = std::to_chars(text, text + sizeof(text), value,
                                   std::chars_format::fixed, 17);
    const uint32_t length = static_cast<uint32_t>(out.ptr - text);
    if (!op.resp3()) {
        reply_bulk(op.sink(), Slice(text, length));
        return;
    }
    auto&& sink = op.sink();
    char* frame = sink.reserve(static_cast<size_t>(length) + 3);
    *frame = ',';
    std::memcpy(frame + 1, text, length);
    frame[length + 1] = '\r';
    frame[length + 2] = '\n';
    sink.advance(static_cast<size_t>(length) + 3);
}

void reply_geo_results(Op& op, const GeoSearchOptions& options,
                       const std::vector<GeoResult>& results) {
    reply_array_header(op.sink(), results.size());
    const bool details = options.with_coord || options.with_dist || options.with_hash;
    for (const GeoResult& result : results) {
        if (details) reply_array_header(op.sink(), 1 + options.with_dist + options.with_hash +
                                                   options.with_coord);
        reply_bulk(op.sink(), Slice(result.member.data(), static_cast<uint32_t>(result.member.size())));
        if (options.with_dist) reply_fixed_bulk(op, result.distance_m / options.unit_m, 4);
        if (options.with_hash) reply_int(op.sink(), static_cast<long long>(result.score));
        if (options.with_coord) {
            reply_array_header(op.sink(), 2);
            reply_coordinate(op, result.longitude);
            reply_coordinate(op, result.latitude);
        }
    }
}

void reply_owner_error(Op& op, ZsetOwnerResult result) {
    if (result == ZsetOwnerResult::WrongType) reply_wrongtype(op.sink());
    else if (result == ZsetOwnerResult::Maxmemory) reply_maxmemory_oom(op);
    else if (result == ZsetOwnerResult::InsertFailed)
        reply_err(op.sink(), "ERR keyspace insert failed");
    else reply_err(op.sink(), "ERR out of memory");
}

template <bool kNotify>
void cmd_geoadd(Shard& shard, Op& op) {
    bool nx = false, xx = false, ch = false;
    uint32_t arg = 2;
    while (arg < op.argc()) {
        if (op.arg(arg).eq_icase("nx")) nx = true;
        else if (op.arg(arg).eq_icase("xx")) xx = true;
        else if (op.arg(arg).eq_icase("ch")) ch = true;
        else break;
        arg++;
    }
    if (nx && xx) { reply_syntax(op.sink()); return; }
    if (arg == op.argc() || (op.argc() - arg) % 3) { reply_syntax(op.sink()); return; }

    struct Addition { double longitude, latitude; Slice member; uint64_t score; };
    std::vector<Addition> additions;
    try { additions.reserve((op.argc() - arg) / 3); }
    catch (const std::bad_alloc&) { reply_err(op.sink(), "ERR out of memory"); return; }
    for (; arg < op.argc(); arg += 3) {
        double longitude = 0, latitude = 0;
        if (!parse_double(op.arg(arg), longitude) || !parse_double(op.arg(arg + 1), latitude)) {
            reply_err(op.sink(), "ERR value is not a valid float"); return;
        }
        if (!valid_coordinates(longitude, latitude)) {
            reply_invalid_coordinates(op, longitude, latitude); return;
        }
        additions.push_back({longitude, latitude, op.arg(arg + 2), geo_encode(longitude, latitude)});
    }

    std::vector<ZsetEntry> entries;
    int64_t expire_at_ms = -1;
    const ZsetOwnerResult read = zset_owner_read(shard, op.key(), op.hash, kNotify,
                                                 entries, expire_at_ms);
    if (read != ZsetOwnerResult::Ok && read != ZsetOwnerResult::Missing) {
        reply_owner_error(op, read); return;
    }
    uint64_t added = 0, changed = 0;
    try {
        for (const Addition& addition : additions) {
            auto found = std::find_if(entries.begin(), entries.end(), [&](const ZsetEntry& entry) {
                return entry.member.size() == addition.member.n &&
                       (!addition.member.n || !std::memcmp(entry.member.data(), addition.member.p,
                                                            addition.member.n));
            });
            if (found == entries.end()) {
                if (xx) continue;
                entries.push_back({std::string(addition.member.p, addition.member.n),
                                   static_cast<double>(addition.score)});
                added++;
            } else if (!nx && found->score != static_cast<double>(addition.score)) {
                found->score = static_cast<double>(addition.score);
                changed++;
            }
        }
        std::sort(entries.begin(), entries.end(), [](const ZsetEntry& a, const ZsetEntry& b) {
            if (a.score != b.score) return a.score < b.score;
            return a.member < b.member;
        });
    } catch (const std::bad_alloc&) { reply_err(op.sink(), "ERR out of memory"); return; }
    if (added || changed) {
        const ZsetOwnerResult stored = zset_owner_replace(shard, op.key(), op.hash, kNotify,
                                                          entries, expire_at_ms);
        if (stored != ZsetOwnerResult::Ok) { reply_owner_error(op, stored); return; }
        if constexpr (kNotify)
            notify_record(shard, op, NOTIFY_ZSET, NotifyEventId::Zadd, op.key());
        if (shard.has_blocking_waiters()) blocking_publish_zset_op(shard, op);
    }
    reply_int(op.sink(), static_cast<long long>(ch ? added + changed : added));
}

template <bool kNotify>
void cmd_geopos(Shard& shard, Op& op) {
    std::vector<ZsetEntry> entries;
    int64_t expire = -1;
    const ZsetOwnerResult read = zset_owner_read(shard, op.key(), op.hash, kNotify, entries, expire);
    if (read == ZsetOwnerResult::WrongType) { reply_wrongtype(op.sink()); return; }
    if (read != ZsetOwnerResult::Ok && read != ZsetOwnerResult::Missing) {
        reply_owner_error(op, read); return;
    }
    reply_array_header(op.sink(), op.argc() - 2);
    for (uint32_t arg = 2; arg < op.argc(); arg++) {
        auto found = std::find_if(entries.begin(), entries.end(), [&](const ZsetEntry& entry) {
            return entry.member.size() == op.arg(arg).n &&
                   (!op.arg(arg).n || !std::memcmp(entry.member.data(), op.arg(arg).p, op.arg(arg).n));
        });
        if (found == entries.end()) { reply_null_array(op.sink(), op.resp3()); continue; }
        double longitude = 0, latitude = 0;
        geo_decode(found->score, longitude, latitude);
        reply_array_header(op.sink(), 2);
        reply_coordinate(op, longitude);
        reply_coordinate(op, latitude);
    }
}

template <bool kNotify>
void cmd_geohash(Shard& shard, Op& op) {
    std::vector<ZsetEntry> entries;
    int64_t expire = -1;
    const ZsetOwnerResult read = zset_owner_read(shard, op.key(), op.hash, kNotify, entries, expire);
    if (read == ZsetOwnerResult::WrongType) { reply_wrongtype(op.sink()); return; }
    if (read != ZsetOwnerResult::Ok && read != ZsetOwnerResult::Missing) {
        reply_owner_error(op, read); return;
    }
    static constexpr char alphabet[] = "0123456789bcdefghjkmnpqrstuvwxyz";
    reply_array_header(op.sink(), op.argc() - 2);
    for (uint32_t arg = 2; arg < op.argc(); arg++) {
        auto found = std::find_if(entries.begin(), entries.end(), [&](const ZsetEntry& entry) {
            return entry.member.size() == op.arg(arg).n &&
                   (!op.arg(arg).n || !std::memcmp(entry.member.data(), op.arg(arg).p, op.arg(arg).n));
        });
        if (found == entries.end()) { reply_null(op.sink(), op.resp3()); continue; }
        double longitude = 0, latitude = 0;
        geo_decode(found->score, longitude, latitude);
        char hash[11];
        double lon_min = -180, lon_max = 180, lat_min = -90, lat_max = 90;
        for (uint32_t character = 0; character < 10; character++) {
            uint8_t value = 0;
            for (uint32_t bit = 0; bit < 5; bit++) {
                const bool longitude_bit = ((character * 5 + bit) & 1u) == 0;
                bool high = false;
                if (longitude_bit) {
                    const double mid = (lon_min + lon_max) * 0.5;
                    high = longitude >= mid;
                    if (high) lon_min = mid; else lon_max = mid;
                } else {
                    const double mid = (lat_min + lat_max) * 0.5;
                    high = latitude >= mid;
                    if (high) lat_min = mid; else lat_max = mid;
                }
                value = static_cast<uint8_t>((value << 1) | high);
            }
            hash[character] = alphabet[value];
        }
        hash[10] = '0';
        reply_bulk(op.sink(), Slice(hash, sizeof(hash)));
    }
}

template <bool kNotify>
void cmd_geodist(Shard& shard, Op& op) {
    double unit_m = 1;
    if (op.argc() == 5 && !parse_unit(op.arg(4), unit_m)) { reply_bad_unit(op); return; }
    std::vector<ZsetEntry> entries;
    int64_t expire = -1;
    const ZsetOwnerResult read = zset_owner_read(shard, op.key(), op.hash, kNotify, entries, expire);
    if (read == ZsetOwnerResult::WrongType) { reply_wrongtype(op.sink()); return; }
    if (read != ZsetOwnerResult::Ok && read != ZsetOwnerResult::Missing) {
        reply_owner_error(op, read); return;
    }
    auto find = [&](Slice member) -> const ZsetEntry* {
        auto it = std::find_if(entries.begin(), entries.end(), [&](const ZsetEntry& entry) {
            return entry.member.size() == member.n &&
                   (!member.n || !std::memcmp(entry.member.data(), member.p, member.n));
        });
        return it == entries.end() ? nullptr : &*it;
    };
    const ZsetEntry* first = find(op.arg(2));
    const ZsetEntry* second = find(op.arg(3));
    if (!first || !second) { reply_null(op.sink(), op.resp3()); return; }
    double lon1, lat1, lon2, lat2;
    geo_decode(first->score, lon1, lat1);
    geo_decode(second->score, lon2, lat2);
    reply_fixed_bulk(op, geo_distance(lon1, lat1, lon2, lat2) / unit_m, 4);
}

template <bool kNotify>
void cmd_geosearch(Shard& shard, Op& op) {
    GeoSearchOptions options;
    if (!parse_search(op, options)) return;
    std::vector<ZsetEntry> entries;
    int64_t expire = -1;
    const Slice key = op.arg(options.source_arg);
    const uint64_t hash = FlatStore::hash_key(key);
    const ZsetOwnerResult read = zset_owner_read(shard, key, hash, kNotify, entries, expire);
    if (read == ZsetOwnerResult::WrongType) { reply_wrongtype(op.sink()); return; }
    if (read != ZsetOwnerResult::Ok && read != ZsetOwnerResult::Missing) {
        reply_owner_error(op, read); return;
    }
    std::vector<GeoResult> results;
    const GeoBuildResult built = run_search(options, entries, results);
    if (built == GeoBuildResult::MissingMember) {
        reply_err(op.sink(), "ERR could not decode requested zset member"); return;
    }
    if (built == GeoBuildResult::Oom) { reply_err(op.sink(), "ERR out of memory"); return; }
    reply_geo_results(op, options, results);
}

void cmd_geoadd_clean(Shard& shard, Op& op) { cmd_geoadd<false>(shard, op); }
void cmd_geoadd_armed(Shard& shard, Op& op) { cmd_geoadd<true>(shard, op); }
void cmd_geopos_clean(Shard& shard, Op& op) { cmd_geopos<false>(shard, op); }
void cmd_geopos_armed(Shard& shard, Op& op) { cmd_geopos<true>(shard, op); }
void cmd_geohash_clean(Shard& shard, Op& op) { cmd_geohash<false>(shard, op); }
void cmd_geohash_armed(Shard& shard, Op& op) { cmd_geohash<true>(shard, op); }
void cmd_geodist_clean(Shard& shard, Op& op) { cmd_geodist<false>(shard, op); }
void cmd_geodist_armed(Shard& shard, Op& op) { cmd_geodist<true>(shard, op); }
void cmd_geosearch_clean(Shard& shard, Op& op) { cmd_geosearch<false>(shard, op); }
void cmd_geosearch_armed(Shard& shard, Op& op) { cmd_geosearch<true>(shard, op); }

static const CommandSpec kTable[] = {
    {"GEOADD", 5, -1, CmdFlags::Write | CmdFlags::DenyOom,
     cmd_geoadd_clean, 1, 1, 1, notify_handler<cmd_geoadd_armed>},
    // Minimum 2 for the member-list readers: redis answers "GEOPOS key" with an empty array.
    {"GEOPOS", 2, -1, CmdFlags::Readonly,
     cmd_geopos_clean, 1, 1, 1, notify_handler<cmd_geopos_armed>},
    {"GEODIST", 4, 5, CmdFlags::Readonly,
     cmd_geodist_clean, 1, 1, 1, notify_handler<cmd_geodist_armed>},
    {"GEOHASH", 2, -1, CmdFlags::Readonly,
     cmd_geohash_clean, 1, 1, 1, notify_handler<cmd_geohash_armed>},
    {"GEOSEARCH", 7, -1, CmdFlags::Readonly,
     cmd_geosearch_clean, 1, 1, 1, notify_handler<cmd_geosearch_armed>},
    {"GEOSEARCHSTORE", 8, -1, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard,
     cmd_xshard_only, 1, 2, 1},
    {"GEORADIUS", 6, -1, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard,
     cmd_xshard_only, 1, -1, 1},
    {"GEORADIUSBYMEMBER", 5, -1, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard,
     cmd_xshard_only, 1, -1, 1},
    {"GEORADIUS_RO", 6, -1, CmdFlags::Readonly | CmdFlags::MultiShard,
     cmd_xshard_only, 1, 1, 1},
    {"GEORADIUSBYMEMBER_RO", 5, -1, CmdFlags::Readonly | CmdFlags::MultiShard,
     cmd_xshard_only, 1, 1, 1},
};

}  // namespace

bool geo_prepare_route(Op& op, GeoRoute& route) {
    GeoSearchOptions options;
    if (!parse_search(op, options)) return false;
    route.store = options.store;
    route.store_distance = options.store_distance;
    route.source_arg = options.source_arg;
    route.destination_arg = options.destination_arg;
    return true;
}

GeoBuildResult geo_build_store(Op& op, const std::vector<ZsetEntry>& source,
                               std::vector<ZsetEntry>& destination) {
    GeoSearchOptions options;
    if (!parse_search(op, options)) return GeoBuildResult::Oom;
    std::vector<GeoResult> results;
    const GeoBuildResult built = run_search(options, source, results);
    if (built != GeoBuildResult::Ok) return built;
    destination.clear();
    try {
        destination.reserve(results.size());
        for (GeoResult& result : results)
            destination.push_back({std::move(result.member), options.store_distance
                ? result.distance_m / options.unit_m : result.score});
        std::sort(destination.begin(), destination.end(), [](const ZsetEntry& a, const ZsetEntry& b) {
            if (a.score != b.score) return a.score < b.score;
            return a.member < b.member;
        });
    } catch (const std::bad_alloc&) {
        destination.clear();
        return GeoBuildResult::Oom;
    }
    return GeoBuildResult::Ok;
}

void cmd_geo_xshard_local(Shard& shard, Op& op, bool notify) {
    GeoSearchOptions options;
    if (!parse_search(op, options)) return;
    if (!options.store) {
        if (notify) cmd_geosearch<true>(shard, op);
        else cmd_geosearch<false>(shard, op);
        return;
    }
    const Slice source_key = op.arg(options.source_arg);
    const uint64_t source_hash = FlatStore::hash_key(source_key);
    std::vector<ZsetEntry> source;
    int64_t expire = -1;
    const ZsetOwnerResult read = zset_owner_read(shard, source_key, source_hash, notify,
                                                 source, expire);
    if (read == ZsetOwnerResult::WrongType) { reply_wrongtype(op.sink()); return; }
    if (read != ZsetOwnerResult::Ok && read != ZsetOwnerResult::Missing) {
        reply_owner_error(op, read); return;
    }
    std::vector<ZsetEntry> output;
    const GeoBuildResult built = geo_build_store(op, source, output);
    if (built == GeoBuildResult::MissingMember) {
        reply_err(op.sink(), "ERR could not decode requested zset member"); return;
    }
    if (built == GeoBuildResult::Oom) { reply_err(op.sink(), "ERR out of memory"); return; }
    const Slice destination_key = op.arg(options.destination_arg);
    const ZsetOwnerResult stored = zset_owner_replace(
        shard, destination_key, FlatStore::hash_key(destination_key), notify, output, -1);
    if (stored != ZsetOwnerResult::Ok) { reply_owner_error(op, stored); return; }
    if (notify) {
        const NotifyEventId event = op.cmd_name().eq_icase("geosearchstore")
            ? NotifyEventId::Geosearchstore : NotifyEventId::Georadiusstore;
        notify_record(shard, op, output.empty() ? NOTIFY_GENERIC : NOTIFY_ZSET,
                      output.empty() ? NotifyEventId::Del : event,
                      destination_key);
    }
    reply_int(op.sink(), static_cast<long long>(output.size()));
}

CommandTable geo_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}

}  // namespace tomo
