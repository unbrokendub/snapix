#pragma once

// Arduino's FS.h owns the File type.  Host tests use the in-memory
// LittleFS mock, which exposes the same minimal File API.
#include "LittleFS.h"
