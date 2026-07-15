#pragma once

#include "command/CommandDispatcher.hpp"

// GEOADD GEOPOS GEODIST GEOSEARCH. Geo data lives in an ordinary sorted set
// whose score is a 52-bit geohash, exactly like real Redis — so TYPE on a geo
// key reports "zset".
void registerGeoCommands(Dispatcher& dispatcher);
