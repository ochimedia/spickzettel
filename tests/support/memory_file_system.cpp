#include "support/memory_file_system.h"

#include <functional>
#include <system_error>

namespace sz::core::fakes {

struct MemoryFileSystem::Node {
    explicit Node(Kind k, Node* up) : kind(k), parent(up) {}

    Kind kind;
    Node* parent;
    std::string data;                 // a file's contents
    std::filesystem::path target;     // a link's, absolute
    std::map<std::filesystem::path, std::unique_ptr<Node>> children;  // a directory's
};

namespace {

// Absolute and lexically normal, with no trailing separator - one spelling
// per place, which is what the lookup below keys on.
std::filesystem::path Normalize(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    std::filesystem::path normal = absolute.lexically_normal();
    if (normal.filename().empty() && normal.has_relative_path()) {
        normal = normal.parent_path();
    }
    return normal;
}

// The root ("C:\", "/") and then every name on the way down.
std::vector<std::filesystem::path> Components(const std::filesystem::path& normal) {
    std::vector<std::filesystem::path> parts;
    if (normal.has_root_path()) {
        parts.push_back(normal.root_path());
    }
    for (const std::filesystem::path& part : normal.relative_path()) {
        if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
    }
    return parts;
}

}  // namespace

MemoryFileSystem::MemoryFileSystem() : universe_(std::make_unique<Node>(Kind::Directory, nullptr)) {}

MemoryFileSystem::~MemoryFileSystem() = default;

MemoryFileSystem::Node* MemoryFileSystem::Find(const std::filesystem::path& path, bool followLast, int depth) {
    if (depth > 16) {
        return nullptr;  // a loop of links, as the real thing gives up on
    }
    const std::vector<std::filesystem::path> parts = Components(Normalize(path));
    if (parts.empty()) {
        return nullptr;
    }
    Node* node = universe_.get();
    for (size_t i = 0; i < parts.size(); ++i) {
        auto it = node->children.find(parts[i]);
        if (it == node->children.end()) {
            if (i != 0) {
                return nullptr;
            }
            // A root is always there.
            it = node->children.emplace(parts[0], std::make_unique<Node>(Kind::Directory, node)).first;
        }
        node = it->second.get();
        const bool last = i + 1 == parts.size();
        if (node->kind == Kind::Link && (!last || followLast)) {
            node = Find(node->target, /*followLast=*/true, depth + 1);
            if (!node) {
                return nullptr;
            }
        }
        if (!last && node->kind != Kind::Directory) {
            return nullptr;
        }
    }
    return node;
}

MemoryFileSystem::Node* MemoryFileSystem::ParentOf(const std::filesystem::path& path, std::filesystem::path& name) {
    const std::filesystem::path normal = Normalize(path);
    name = normal.filename();
    if (name.empty() || !normal.has_relative_path()) {
        return nullptr;  // a root has no parent to be created in
    }
    Node* dir = Find(normal.parent_path(), /*followLast=*/true);
    return dir && dir->kind == Kind::Directory ? dir : nullptr;
}

FileSystem::Kind MemoryFileSystem::Status(const std::filesystem::path& path) {
    const Node* node = Find(path, /*followLast=*/true);
    return node ? node->kind : Kind::None;
}

FileSystem::Kind MemoryFileSystem::LinkStatus(const std::filesystem::path& path) {
    const Node* node = Find(path, /*followLast=*/false);
    return node ? node->kind : Kind::None;
}

std::optional<uintmax_t> MemoryFileSystem::FileSize(const std::filesystem::path& path) {
    const Node* node = Find(path, /*followLast=*/true);
    if (!node || node->kind != Kind::File) {
        return std::nullopt;
    }
    return node->data.size();
}

std::optional<uintmax_t> MemoryFileSystem::HardLinkCount(const std::filesystem::path& path) {
    return Find(path, /*followLast=*/true) ? std::optional<uintmax_t>(1) : std::nullopt;
}

std::optional<std::vector<FileSystem::Entry>> MemoryFileSystem::List(const std::filesystem::path& dir) {
    const Node* node = Find(dir, /*followLast=*/true);
    if (!node || node->kind != Kind::Directory) {
        return std::nullopt;
    }
    std::vector<Entry> entries;
    for (const auto& [name, child] : node->children) {
        entries.push_back({name, child->kind});
    }
    return entries;
}

std::optional<std::string> MemoryFileSystem::Read(const std::filesystem::path& path, uintmax_t maxBytes) {
    const Node* node = Find(path, /*followLast=*/true);
    if (!node || node->kind != Kind::File || node->data.size() > maxBytes) {
        return std::nullopt;
    }
    return node->data;
}

bool MemoryFileSystem::MakeDirectory(const std::filesystem::path& path) {
    std::filesystem::path name;
    Node* dir = ParentOf(path, name);
    if (!dir) {
        return Status(path) == Kind::Directory;  // a root
    }
    if (dir->children.count(name) > 0) {
        return Status(path) == Kind::Directory;
    }
    dir->children.emplace(name, std::make_unique<Node>(Kind::Directory, dir));
    return true;
}

FileSystem::WriteResult MemoryFileSystem::WriteNewFile(const std::filesystem::path& path, const void* data,
                                                       size_t size) {
    std::filesystem::path name;
    Node* dir = ParentOf(path, name);
    if (!dir) {
        return WriteResult::Failed;
    }
    if (dir->children.count(name) > 0) {
        return WriteResult::NameTaken;
    }
    auto file = std::make_unique<Node>(Kind::File, dir);
    file->data.assign(static_cast<const char*>(data), size);
    dir->children.emplace(name, std::move(file));
    return WriteResult::Written;
}

bool MemoryFileSystem::Rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::path fromName;
    std::filesystem::path toName;
    Node* fromDir = ParentOf(from, fromName);
    Node* toDir = ParentOf(to, toName);
    if (!fromDir || !toDir) {
        return false;
    }
    const auto moving = fromDir->children.find(fromName);
    if (moving == fromDir->children.end()) {
        return false;
    }
    if (fromDir == toDir && fromName == toName) {
        return true;
    }
    // Windows replaces a file at the destination and nothing else - not a
    // directory, and not a link to one, which is what every link here is.
    if (const auto there = toDir->children.find(toName); there != toDir->children.end()) {
        if (there->second->kind != Kind::File || moving->second->kind == Kind::Directory) {
            return false;
        }
    }
    for (const Node* up = toDir; up; up = up->parent) {
        if (up == moving->second.get()) {
            return false;  // under itself
        }
    }
    std::unique_ptr<Node> node = std::move(moving->second);
    fromDir->children.erase(moving);
    node->parent = toDir;
    toDir->children[toName] = std::move(node);
    return true;
}

bool MemoryFileSystem::Remove(const std::filesystem::path& path) {
    std::filesystem::path name;
    Node* dir = ParentOf(path, name);
    if (!dir) {
        return LinkStatus(path) == Kind::None;
    }
    const auto it = dir->children.find(name);
    if (it == dir->children.end()) {
        return true;
    }
    if (it->second->kind == Kind::Directory && !it->second->children.empty()) {
        return false;
    }
    dir->children.erase(it);
    return true;
}

bool MemoryFileSystem::MakeLink(const std::filesystem::path& path, const std::filesystem::path& target) {
    std::filesystem::path name;
    Node* dir = ParentOf(path, name);
    if (!dir || dir->children.count(name) > 0) {
        return false;
    }
    auto link = std::make_unique<Node>(Kind::Link, dir);
    link->target = Normalize(target);
    dir->children.emplace(name, std::move(link));
    return true;
}

bool MemoryFileSystem::Put(const std::filesystem::path& path, const std::string& text) {
    CreateDirectories(Normalize(path).parent_path());
    std::filesystem::path name;
    Node* dir = ParentOf(path, name);
    if (!dir) {
        return false;
    }
    const auto it = dir->children.find(name);
    if (it != dir->children.end()) {
        if (it->second->kind != Kind::File) {
            return false;
        }
        it->second->data = text;
        return true;
    }
    return WriteNewFile(path, text.data(), text.size()) == WriteResult::Written;
}

std::map<std::filesystem::path, std::string> MemoryFileSystem::FilesUnder(const std::filesystem::path& root) {
    std::map<std::filesystem::path, std::string> files;
    const std::function<void(const Node&, const std::filesystem::path&)> walk = [&](const Node& node,
                                                                                    const std::filesystem::path& at) {
        for (const auto& [name, child] : node.children) {
            if (child->kind == Kind::File) {
                files.emplace(at / name, child->data);
            } else if (child->kind == Kind::Directory) {
                walk(*child, at / name);
            }
        }
    };
    if (const Node* node = Find(root, /*followLast=*/true); node && node->kind == Kind::Directory) {
        walk(*node, Normalize(root));
    }
    return files;
}

}  // namespace sz::core::fakes
