#pragma once

#include "core/query/index_manager.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bisondb::query {

// One database directory holding many collections. Collection objects are
// created lazily and cached; the per-collection shared_mutex inside
// IndexedCollection provides all data synchronization — this class only
// guards its own registry map.
class Database {
  public:
    explicit Database(std::string dbdir) : dbdir_(std::move(dbdir)) {}

    // Valid names: [A-Za-z0-9_][A-Za-z0-9_-]{0,127} — no dots or path
    // separators (file names embed the collection name).
    static bool isValidCollectionName(const std::string& name);

    // Opens (or creates) a collection; throws StoreError for invalid names.
    IndexedCollection& collection(const std::string& name);

    // Explicitly creates an empty collection. Returns false when it already
    // exists (on disk or open). Throws StoreError for invalid names.
    bool createCollection(const std::string& name);

    // True when the collection exists on disk or is open.
    bool collectionExists(const std::string& name);

    // Collections present on disk or already open.
    std::vector<std::string> listCollections();

    // Closes the collection and deletes all its files. False if absent.
    bool dropCollection(const std::string& name);

    void syncAll();

    const std::string& dir() const noexcept { return dbdir_; }

  private:
    std::string dbdir_;
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<IndexedCollection>> collections_;
};

} // namespace bisondb::query
