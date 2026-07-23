#pragma once

// Shared LittleFS cache directory helpers.  Pulled out of Fb2.cpp in v2.0.73
// when Txt/Markdown/Html/Xtc/Epub were all migrated from SD to internal flash
// — every content type's setupCacheDir/clearCache now goes through these two
// functions so the recursive logic isn't duplicated five times.

#include <string>

namespace cache_fs {

// Recursively create LittleFS directory tree.  Arduino LittleFS::mkdir is
// non-recursive — it fails if a parent doesn't exist.  Walks parents from
// the root and creates whichever ones are missing.  No-op if `path` already
// exists.  Returns true if the directory exists or was created successfully.
bool ensureFlashDir(const std::string& path);

// Recursively remove a LittleFS directory tree.  Arduino LittleFS::rmdir is
// non-recursive — it fails on non-empty directories.  Walks the tree depth-
// first, removing files then their parent directories.  Returns true if the
// tree was removed (or never existed).  Caller-owned errors (e.g. open file
// handles into the tree) propagate through as `false` from rmdir/remove.
bool rmTree(const std::string& path);

// Verify that a per-book cache belongs to the current source file revision.
// The fingerprint includes the source path, size, FAT modification time and
// samples from the beginning/middle/end of the file.  A missing or mismatched
// fingerprint invalidates the entire cache tree before a new marker is
// published, preventing stale metadata/pages/covers after a file is replaced
// in-place (and making std::hash cache-key collisions fail safe).
bool ensureSourceFingerprint(const std::string& sourcePath, const std::string& cachePath);

}  // namespace cache_fs
