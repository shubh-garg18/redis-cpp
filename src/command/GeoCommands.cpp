#include "command/GeoCommands.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "protocol/RESPParser.hpp"
#include "storage/SortedSetStore.hpp"

// Redis-compatible geohash parameters. Coordinates are normalised into these
// ranges, quantised to 26 bits each, and bit-interleaved into a 52-bit score.
static const double GEO_LON_MIN = -180.0;
static const double GEO_LON_MAX = 180.0;
static const double GEO_LAT_MIN = -85.05112878;
static const double GEO_LAT_MAX = 85.05112878;
static const int GEO_STEP = 26;
static const double EARTH_RADIUS_M = 6372797.560856;
static const double GEO_PI = 3.14159265358979323846;

// Spread the low bits of a 32-bit value into even positions of a 64-bit word;
// lat goes to even bits, lon to odd bits. (Hacker's-Delight style, as in Redis.)
static uint64_t interleave64(uint32_t xlo, uint32_t ylo){
    static const uint64_t B[] = {
        0x5555555555555555ULL, 0x3333333333333333ULL,
        0x0F0F0F0F0F0F0F0FULL, 0x00FF00FF00FF00FFULL,
        0x0000FFFF0000FFFFULL};
    static const unsigned int S[] = {1, 2, 4, 8, 16};

    uint64_t x = xlo;
    uint64_t y = ylo;
    x = (x | (x << S[4])) & B[4];
    x = (x | (x << S[3])) & B[3];
    x = (x | (x << S[2])) & B[2];
    x = (x | (x << S[1])) & B[1];
    x = (x | (x << S[0])) & B[0];
    y = (y | (y << S[4])) & B[4];
    y = (y | (y << S[3])) & B[3];
    y = (y | (y << S[2])) & B[2];
    y = (y | (y << S[1])) & B[1];
    y = (y | (y << S[0])) & B[0];
    return x | (y << 1);
}

// Inverse of interleave64: pull the even bits back out into a 32-bit value.
static uint64_t deinterleave64(uint64_t bits){
    static const uint64_t B[] = {
        0x5555555555555555ULL, 0x3333333333333333ULL,
        0x0F0F0F0F0F0F0F0FULL, 0x00FF00FF00FF00FFULL,
        0x0000FFFF0000FFFFULL, 0x00000000FFFFFFFFULL};
    static const unsigned int S[] = {0, 1, 2, 4, 8, 16};

    uint64_t x = bits & B[0];
    x = (x | (x >> S[1])) & B[1];
    x = (x | (x >> S[2])) & B[2];
    x = (x | (x >> S[3])) & B[3];
    x = (x | (x >> S[4])) & B[4];
    x = (x | (x >> S[5])) & B[5];
    return x;
}

static uint64_t geoEncode(double lon, double lat){
    double lat_offset = (lat - GEO_LAT_MIN) / (GEO_LAT_MAX - GEO_LAT_MIN);
    double lon_offset = (lon - GEO_LON_MIN) / (GEO_LON_MAX - GEO_LON_MIN);
    lat_offset *= (double)(1ULL << GEO_STEP);
    lon_offset *= (double)(1ULL << GEO_STEP);
    return interleave64((uint32_t)lat_offset, (uint32_t)lon_offset);
}

// Decode a score back to the centre of its geohash cell.
static void geoDecode(uint64_t bits, double& lon, double& lat){
    uint32_t ilat = (uint32_t)deinterleave64(bits);
    uint32_t ilon = (uint32_t)deinterleave64(bits >> 1);

    double scale = (double)(1ULL << GEO_STEP);
    double lat_min = GEO_LAT_MIN + (ilat / scale) * (GEO_LAT_MAX - GEO_LAT_MIN);
    double lat_max = GEO_LAT_MIN + ((ilat + 1) / scale) * (GEO_LAT_MAX - GEO_LAT_MIN);
    double lon_min = GEO_LON_MIN + (ilon / scale) * (GEO_LON_MAX - GEO_LON_MIN);
    double lon_max = GEO_LON_MIN + ((ilon + 1) / scale) * (GEO_LON_MAX - GEO_LON_MIN);
    lat = (lat_min + lat_max) / 2.0;
    lon = (lon_min + lon_max) / 2.0;
}

static double degToRad(double d){ return d * GEO_PI / 180.0; }

static double haversineMeters(double lon1, double lat1, double lon2, double lat2){
    double lat1r = degToRad(lat1);
    double lat2r = degToRad(lat2);
    double u = std::sin((lat2r - lat1r) / 2.0);
    double v = std::sin((degToRad(lon2) - degToRad(lon1)) / 2.0);
    double a = u * u + std::cos(lat1r) * std::cos(lat2r) * v * v;
    return 2.0 * EARTH_RADIUS_M * std::asin(std::sqrt(a));
}

// Meters per unit, or -1 for an unknown unit.
static double unitToMeters(const std::string& unit){
    std::string u = toUpper(unit);
    if(u == "M") return 1.0;
    if(u == "KM") return 1000.0;
    if(u == "MI") return 1609.34;
    if(u == "FT") return 0.3048;
    return -1.0;
}

static bool validLonLat(double lon, double lat){
    return lon >= GEO_LON_MIN && lon <= GEO_LON_MAX &&
           lat >= GEO_LAT_MIN && lat <= GEO_LAT_MAX;
}

static std::string wrongTypeError(){
    return encodeRESPError("WRONGTYPE Operation against a key holding the wrong kind of value");
}

static std::string handleGeoadd(Context& context, const std::vector<RESPMessage>& args){
    // GEOADD key longitude latitude member [longitude latitude member ...]
    if(args.size() < 4 || (args.size() - 1) % 3 != 0)
        return encodeRESPError("ERR wrong number of arguments for 'geoadd'");

    const std::string& key = args[0].str;
    if(context.db->hasWrongType(key, ValueType::SortedSet)) return wrongTypeError();

    std::vector<std::pair<std::string, double>> entries;
    for(size_t i = 1; i < args.size(); i += 3){
        double lon, lat;
        try{
            lon = std::stod(args[i].str);
            lat = std::stod(args[i + 1].str);
        }catch(...){
            return encodeRESPError("ERR value is not a valid float");
        }
        if(!validLonLat(lon, lat)){
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "ERR invalid longitude,latitude pair %.6f,%.6f", lon, lat);
            return encodeRESPError(buf);
        }
        uint64_t bits = geoEncode(lon, lat);
        entries.push_back({args[i + 2].str, (double)bits});
    }

    int added = context.db->sortedSetStore.zadd(key, entries);
    context.db->touch(key);
    context.db->recordAccess(key);
    return encodeRESPInteger(added);
}

static std::string handleGeopos(Context& context, const std::vector<RESPMessage>& args){
    // GEOPOS key [member ...]
    if(args.empty()) return encodeRESPError("ERR wrong number of arguments for 'geopos'");

    const std::string& key = args[0].str;
    if(context.db->hasWrongType(key, ValueType::SortedSet)) return wrongTypeError();

    std::string out = encodeRESPArrayHeader(args.size() - 1);
    for(size_t i = 1; i < args.size(); i++){
        auto score = context.db->sortedSetStore.zscore(key, args[i].str);
        if(!score){
            out += "*-1\r\n";  // unknown member -> null array element
            continue;
        }
        double lon, lat;
        geoDecode((uint64_t)*score, lon, lat);

        char lonBuf[32], latBuf[32];
        std::snprintf(lonBuf, sizeof(lonBuf), "%.17g", lon);
        std::snprintf(latBuf, sizeof(latBuf), "%.17g", lat);

        out += encodeRESPArrayHeader(2);
        out += encodeRESPBulk(lonBuf);
        out += encodeRESPBulk(latBuf);
    }
    context.db->recordAccess(key);
    return out;
}

static std::string handleGeodist(Context& context, const std::vector<RESPMessage>& args){
    // GEODIST key member1 member2 [unit]
    if(args.size() < 3 || args.size() > 4)
        return encodeRESPError("ERR wrong number of arguments for 'geodist'");

    const std::string& key = args[0].str;
    if(context.db->hasWrongType(key, ValueType::SortedSet)) return wrongTypeError();

    double perUnit = 1.0;
    if(args.size() == 4){
        perUnit = unitToMeters(args[3].str);
        if(perUnit < 0)
            return encodeRESPError("ERR unsupported unit provided. please use M, KM, FT, MI");
    }

    context.db->recordAccess(key);
    auto s1 = context.db->sortedSetStore.zscore(key, args[1].str);
    auto s2 = context.db->sortedSetStore.zscore(key, args[2].str);
    if(!s1 || !s2) return encodeRESPNull();

    double lon1, lat1, lon2, lat2;
    geoDecode((uint64_t)*s1, lon1, lat1);
    geoDecode((uint64_t)*s2, lon2, lat2);

    double dist = haversineMeters(lon1, lat1, lon2, lat2) / perUnit;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", dist);
    return encodeRESPBulk(buf);
}

static std::string handleGeosearch(Context& context, const std::vector<RESPMessage>& args){
    // GEOSEARCH key FROMLONLAT lon lat BYRADIUS radius unit [ASC|DESC]
    if(args.size() < 7) return encodeRESPError("ERR wrong number of arguments for 'geosearch'");

    const std::string& key = args[0].str;
    if(context.db->hasWrongType(key, ValueType::SortedSet)) return wrongTypeError();

    bool haveCenter = false, haveRadius = false, asc = false, desc = false;
    double centerLon = 0, centerLat = 0, radiusMeters = 0;

    for(size_t i = 1; i < args.size(); i++){
        std::string opt = toUpper(args[i].str);
        if(opt == "FROMLONLAT" && i + 2 < args.size()){
            try{
                centerLon = std::stod(args[i + 1].str);
                centerLat = std::stod(args[i + 2].str);
            }catch(...){
                return encodeRESPError("ERR value is not a valid float");
            }
            haveCenter = true;
            i += 2;
        }else if(opt == "BYRADIUS" && i + 2 < args.size()){
            double radius;
            try{
                radius = std::stod(args[i + 1].str);
            }catch(...){
                return encodeRESPError("ERR value is not a valid float");
            }
            double perUnit = unitToMeters(args[i + 2].str);
            if(perUnit < 0)
                return encodeRESPError("ERR unsupported unit provided. please use M, KM, FT, MI");
            radiusMeters = radius * perUnit;
            haveRadius = true;
            i += 2;
        }else if(opt == "ASC"){
            asc = true;
        }else if(opt == "DESC"){
            desc = true;
        }
    }

    if(!haveCenter || !haveRadius) return encodeRESPError("ERR syntax error");

    // Brute-force scan of every member — fine at our scale (Redis prunes by
    // geohash neighbours, but the result set is identical).
    std::vector<std::pair<double, std::string>> hits;  // (distance, member)
    auto members = context.db->sortedSetStore.zrange(key, 0, -1);
    for(const auto& member : members){
        auto score = context.db->sortedSetStore.zscore(key, member);
        if(!score) continue;
        double lon, lat;
        geoDecode((uint64_t)*score, lon, lat);
        double d = haversineMeters(centerLon, centerLat, lon, lat);
        if(d <= radiusMeters) hits.push_back({d, member});
    }
    context.db->recordAccess(key);

    if(asc || desc){
        std::sort(hits.begin(), hits.end());
        if(desc) std::reverse(hits.begin(), hits.end());
    }

    std::string out = encodeRESPArrayHeader(hits.size());
    for(const auto& h : hits) out += encodeRESPBulk(h.second);
    return out;
}

void registerGeoCommands(Dispatcher& dispatcher){
    dispatcher.add("GEOADD",    handleGeoadd);
    dispatcher.add("GEOPOS",    handleGeopos);
    dispatcher.add("GEODIST",   handleGeodist);
    dispatcher.add("GEOSEARCH", handleGeosearch);
}
