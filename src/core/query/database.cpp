#include "core/query/database.hpp"

#include <filesystem>

namespace bisondb::query {

namespace fs = std::filesystem;

bool Database::isValidCollectionName(const std::string& name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    auto wordChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    };
    if (!wordChar(name[0])) {
        return false;
    }
    for (char c : name) {
        if (!wordChar(c) && c != '-') {
            return false;
        }
    }
    return true;
}

IndexedCollection& Database::collection(const std::string& name) {
    if (!isValidCollectionName(name)) {
        throw store::StoreError("invalid collection name: \"" + name + "\"");
    }
    std::lock_guard lock(mutex_);
    auto it = collections_.find(name);
    if (it == collections_.end()) {
        it = collections_.emplace(name, std::make_unique<IndexedCollection>(dbdir_, name)).first;
    }
    return *it->second;
}

bool Database::collectionExists(const std::string& name) {
    if (!isValidCollectionName(name)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    return collections_.contains(name) || fs::exists(fs::path(dbdir_) / (name + ".log"));
}

bool Database::createCollection(const std::string& name) {
    if (!isValidCollectionName(name)) {
        throw store::StoreError("invalid collection name: \"" + name + "\"");
    }
    if (collectionExists(name)) {
        return false;
    }
    collection(name); // opening creates the files
    return true;
}

std::vector<std::string> Database::listCollections() {
    std::lock_guard lock(mutex_);
    std::map<std::string, bool> names;
    for (const auto& [name, coll] : collections_) {
        names[name] = true;
    }
    if (fs::exists(dbdir_)) {
        for (const auto& entry : fs::directory_iterator(dbdir_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".log") {
                names[entry.path().stem().string()] = true;
            }
        }
    }
    std::vector<std::string> out;
    for (const auto& [name, present] : names) {
        out.push_back(name);
    }
    return out;
}

bool Database::dropCollection(const std::string& name) {
    if (!isValidCollectionName(name)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    bool existed = collections_.erase(name) > 0;
    if (fs::exists(dbdir_)) {
        for (const char* suffix : {".log", ".meta.json"}) {
            existed = fs::remove(fs::path(dbdir_) / (name + suffix)) || existed;
        }
        // Index files: <name>.<field>.idx
        std::vector<fs::path> toRemove;
        for (const auto& entry : fs::directory_iterator(dbdir_)) {
            std::string file = entry.path().filename().string();
            if (entry.path().extension() == ".idx" && file.rfind(name + ".", 0) == 0) {
                toRemove.push_back(entry.path());
            }
        }
        for (const fs::path& p : toRemove) {
            existed = fs::remove(p) || existed;
        }
    }
    return existed;
}

void Database::syncAll() {
    std::lock_guard lock(mutex_);
    for (auto& [name, coll] : collections_) {
        coll->sync();
    }
}

} // namespace bisondb::query
