#pragma once

#include <string>

// Server-wide authentication settings. One instance, owned by main.
struct AuthConfig {
    std::string requirepass;  // empty => authentication disabled

    bool enabled() const { return !requirepass.empty(); }
};
