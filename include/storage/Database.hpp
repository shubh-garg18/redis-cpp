#pragma once

#include "StringStore.hpp"
#include "ListStore.hpp"

struct Database {
    StringStore stringStore;
    ListStore listStore;
};