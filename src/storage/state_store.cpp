// SQLite state.db 键值后端实现，使用 namespace + key 作为稳定主键，并提供显式 Flush。
#include "storage/state_store.h"

#ifndef _WIN32
#error StateStore is a Windows-only implementation
#endif

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <type_traits>
#include <utility>

#if defined(SLINK_USE_SQLITE)
#include <sqlite3.h>
#endif

namespace SLink::storage {
namespace {

constexpr std::size_t kMaxDatabaseBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxNameBytes = 4096;
constexpr std::size_t kMaxValueBytes = 64ULL * 1024ULL * 1024ULL;

StorageStatus
ValidateEntry(const std::string& name_space, const std::string& key, std::size_t value_size = 0) {
    if (name_space.empty() || key.empty()) {
        return StorageStatus::Error(StorageErrc::invalid_argument,
                                    L"State namespace and key must not be empty");
    }
    if (name_space.size() > kMaxNameBytes || key.size() > kMaxNameBytes ||
        value_size > kMaxValueBytes) {
        return StorageStatus::Error(StorageErrc::invalid_argument,
                                    L"State namespace, key, or value exceeds its limit");
    }
    return StorageStatus::Ok();
}

std::wstring LastErrorMessage(const wchar_t* operation, DWORD error) {
    return std::wstring(operation) + L" (Windows error " + std::to_wstring(error) + L")";
}

StorageResult<std::vector<std::byte>> ReadFile(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return StorageResult<std::vector<std::byte>>::Failure(
                StorageStatus::Error(StorageErrc::not_found, L"State file does not exist"));
        }
        return StorageResult<std::vector<std::byte>>::Failure(StorageStatus::Error(
            StorageErrc::io_error, LastErrorMessage(L"Cannot open state file", error)));
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > kMaxDatabaseBytes) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return StorageResult<std::vector<std::byte>>::Failure(StorageStatus::Error(
            StorageErrc::corrupt_data,
            error == ERROR_SUCCESS ? L"State file is too large"
                                   : LastErrorMessage(L"Cannot inspect state file", error)));
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min<std::size_t>)(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!::ReadFile(file, bytes.data() + offset, chunk, &read, nullptr) || read == 0) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            return StorageResult<std::vector<std::byte>>::Failure(StorageStatus::Error(
                StorageErrc::io_error, LastErrorMessage(L"Cannot read state file", error)));
        }
        offset += read;
    }
    CloseHandle(file);
    return StorageResult<std::vector<std::byte>>::Success(std::move(bytes));
}

StorageStatus WriteFileAtomically(const std::filesystem::path& path,
                                  std::span<const std::byte> bytes) {
    auto pending = path;
    pending += L".pending";
    HANDLE file = CreateFileW(
        pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return StorageStatus::Error(
            StorageErrc::io_error,
            LastErrorMessage(L"Cannot create pending state file", GetLastError()));
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min<std::size_t>)(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            DeleteFileW(pending.c_str());
            return StorageStatus::Error(StorageErrc::io_error,
                                        LastErrorMessage(L"Cannot write state file", error));
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        DeleteFileW(pending.c_str());
        return StorageStatus::Error(StorageErrc::io_error,
                                    LastErrorMessage(L"Cannot flush state file", error));
    }
    CloseHandle(file);

    if (!MoveFileExW(
            pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(pending.c_str());
        return StorageStatus::Error(StorageErrc::io_error,
                                    LastErrorMessage(L"Cannot replace state file", error));
    }
    return StorageStatus::Ok();
}

#if !defined(SLINK_USE_SQLITE)

using StateKey = std::pair<std::string, std::string>;
using StateMap = std::map<StateKey, std::vector<std::byte>>;
constexpr std::array<std::byte, 8> kStateMagic{std::byte{'S'},
                                               std::byte{'W'},
                                               std::byte{'S'},
                                               std::byte{'T'},
                                               std::byte{'A'},
                                               std::byte{'T'},
                                               std::byte{'E'},
                                               std::byte{'1'}};

template <typename UInt> void AppendUnsigned(std::vector<std::byte>& output, UInt value) {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        output.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xff));
    }
}

template <typename UInt>
bool ReadUnsigned(std::span<const std::byte> input, std::size_t& cursor, UInt& value) {
    static_assert(std::is_unsigned_v<UInt>);
    if (input.size() - (std::min)(input.size(), cursor) < sizeof(UInt)) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        value |= static_cast<UInt>(std::to_integer<unsigned char>(input[cursor + i])) << (i * 8);
    }
    cursor += sizeof(UInt);
    return true;
}

std::uint64_t Fnv1a(std::span<const std::byte> bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        hash ^= std::to_integer<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<std::byte> Serialize(const StateMap& entries) {
    std::vector<std::byte> output;
    output.insert(output.end(), kStateMagic.begin(), kStateMagic.end());
    AppendUnsigned<std::uint32_t>(output, 1);
    AppendUnsigned<std::uint32_t>(output, static_cast<std::uint32_t>(entries.size()));
    for (const auto& [compound_key, value] : entries) {
        const auto& [name_space, key] = compound_key;
        AppendUnsigned<std::uint32_t>(output, static_cast<std::uint32_t>(name_space.size()));
        AppendUnsigned<std::uint32_t>(output, static_cast<std::uint32_t>(key.size()));
        AppendUnsigned<std::uint64_t>(output, static_cast<std::uint64_t>(value.size()));
        output.insert(output.end(),
                      reinterpret_cast<const std::byte*>(name_space.data()),
                      reinterpret_cast<const std::byte*>(name_space.data() + name_space.size()));
        output.insert(output.end(),
                      reinterpret_cast<const std::byte*>(key.data()),
                      reinterpret_cast<const std::byte*>(key.data() + key.size()));
        output.insert(output.end(), value.begin(), value.end());
    }
    AppendUnsigned<std::uint64_t>(output, Fnv1a(output));
    return output;
}

StorageResult<StateMap> Deserialize(std::span<const std::byte> bytes) {
    if (bytes.size() < kStateMagic.size() + 4 + 4 + 8 ||
        !std::equal(kStateMagic.begin(), kStateMagic.end(), bytes.begin())) {
        return StorageResult<StateMap>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid state file header"));
    }

    const std::span payload = bytes.first(bytes.size() - sizeof(std::uint64_t));
    std::size_t checksum_cursor = payload.size();
    std::uint64_t stored_checksum = 0;
    if (!ReadUnsigned(bytes, checksum_cursor, stored_checksum) ||
        stored_checksum != Fnv1a(payload)) {
        return StorageResult<StateMap>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"State file checksum mismatch"));
    }

    std::size_t cursor = kStateMagic.size();
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    if (!ReadUnsigned(payload, cursor, version) || version != 1 ||
        !ReadUnsigned(payload, cursor, count) || count > 1'000'000) {
        return StorageResult<StateMap>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"Unsupported state file format"));
    }

    StateMap entries;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t namespace_size = 0;
        std::uint32_t key_size = 0;
        std::uint64_t value_size = 0;
        if (!ReadUnsigned(payload, cursor, namespace_size) ||
            !ReadUnsigned(payload, cursor, key_size) ||
            !ReadUnsigned(payload, cursor, value_size) || namespace_size == 0 || key_size == 0 ||
            namespace_size > kMaxNameBytes || key_size > kMaxNameBytes ||
            value_size > kMaxValueBytes ||
            value_size + namespace_size + key_size > payload.size() - cursor) {
            return StorageResult<StateMap>::Failure(
                StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid entry in state file"));
        }
        const auto* namespace_data = reinterpret_cast<const char*>(payload.data() + cursor);
        std::string name_space(namespace_data, namespace_size);
        cursor += namespace_size;
        const auto* key_data = reinterpret_cast<const char*>(payload.data() + cursor);
        std::string key(key_data, key_size);
        cursor += key_size;
        std::vector<std::byte> value(payload.begin() + static_cast<std::ptrdiff_t>(cursor),
                                     payload.begin() +
                                         static_cast<std::ptrdiff_t>(cursor + value_size));
        cursor += static_cast<std::size_t>(value_size);
        if (!entries.emplace(StateKey{std::move(name_space), std::move(key)}, std::move(value))
                 .second) {
            return StorageResult<StateMap>::Failure(
                StorageStatus::Error(StorageErrc::corrupt_data, L"Duplicate entry in state file"));
        }
    }
    if (cursor != payload.size()) {
        return StorageResult<StateMap>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"Trailing data in state file"));
    }
    return StorageResult<StateMap>::Success(std::move(entries));
}

#endif

} // namespace

struct StateStore::Impl {
    explicit Impl(std::filesystem::path store_path) : path(std::move(store_path)) {}
    std::filesystem::path path;
    mutable std::mutex mutex;

#if defined(SLINK_USE_SQLITE)
    sqlite3* database{nullptr};
#else
    StateMap entries;
    bool dirty{false};
#endif
};

StateStore::StateStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

StateStore::~StateStore() {
    if (!impl_) {
        return;
    }
#if defined(SLINK_USE_SQLITE)
    if (impl_->database != nullptr) {
        sqlite3_close_v2(impl_->database);
        impl_->database = nullptr;
    }
#else
    Flush();
#endif
}

StateStore::StateStore(StateStore&&) noexcept = default;
StateStore& StateStore::operator=(StateStore&& other) noexcept {
    if (this != &other) {
#if defined(SLINK_USE_SQLITE)
        if (impl_ && impl_->database != nullptr) {
            sqlite3_close_v2(impl_->database);
            impl_->database = nullptr;
        }
#else
        if (impl_) {
            Flush();
        }
#endif
        impl_ = std::move(other.impl_);
    }
    return *this;
}

StorageResult<std::unique_ptr<StateStore>>
StateStore::Open(const std::filesystem::path& database_path) {
    if (database_path.empty() || !database_path.is_absolute()) {
        return StorageResult<std::unique_ptr<StateStore>>::Failure(StorageStatus::Error(
            StorageErrc::invalid_argument, L"State database path must be absolute"));
    }
    std::error_code error;
    if (!std::filesystem::is_directory(database_path.parent_path(), error) || error) {
        return StorageResult<std::unique_ptr<StateStore>>::Failure(StorageStatus::Error(
            StorageErrc::io_error, L"State database parent directory does not exist"));
    }

    auto impl = std::make_unique<Impl>(database_path.lexically_normal());
#if defined(SLINK_USE_SQLITE)
    const int open_result = sqlite3_open16(impl->path.c_str(), &impl->database);
    if (open_result != SQLITE_OK) {
        const char* detail = impl->database ? sqlite3_errmsg(impl->database) : "unknown error";
        std::wstring message(L"Cannot open SQLite state database: ");
        while (*detail != '\0') {
            message.push_back(static_cast<unsigned char>(*detail++));
        }
        if (impl->database) {
            sqlite3_close_v2(impl->database);
            impl->database = nullptr;
        }
        return StorageResult<std::unique_ptr<StateStore>>::Failure(
            StorageStatus::Error(StorageErrc::backend_error, std::move(message)));
    }
    constexpr char schema[] = "PRAGMA journal_mode=WAL;"
                              "PRAGMA synchronous=NORMAL;"
                              // SQLite must not spill temporary query structures to the process or
                              // user TEMP directory; every internal artifact stays beside the EXE.
                              "PRAGMA temp_store=MEMORY;"
                              "CREATE TABLE IF NOT EXISTS app_state("
                              "namespace TEXT NOT NULL,key TEXT NOT NULL,value BLOB NOT NULL,"
                              "PRIMARY KEY(namespace,key)) WITHOUT ROWID;";
    char* sqlite_error = nullptr;
    if (sqlite3_exec(impl->database, schema, nullptr, nullptr, &sqlite_error) != SQLITE_OK) {
        std::wstring message(L"Cannot initialize SQLite state database");
        if (sqlite_error) {
            message.append(L": ");
            const char* cursor = sqlite_error;
            while (*cursor != '\0') {
                message.push_back(static_cast<unsigned char>(*cursor++));
            }
        }
        sqlite3_free(sqlite_error);
        sqlite3_close_v2(impl->database);
        impl->database = nullptr;
        return StorageResult<std::unique_ptr<StateStore>>::Failure(
            StorageStatus::Error(StorageErrc::backend_error, std::move(message)));
    }
#else
    auto bytes = ReadFile(impl->path);
    if (bytes) {
        auto parsed = Deserialize(bytes.value());
        if (!parsed) {
            return StorageResult<std::unique_ptr<StateStore>>::Failure(parsed.status());
        }
        impl->entries = std::move(parsed).value();
    } else if (bytes.status().code != StorageErrc::not_found) {
        return StorageResult<std::unique_ptr<StateStore>>::Failure(bytes.status());
    }
#endif
    return StorageResult<std::unique_ptr<StateStore>>::Success(
        std::unique_ptr<StateStore>(new StateStore(std::move(impl))));
}

StorageStatus
StateStore::Set(std::string name_space, std::string key, std::span<const std::byte> value) {
    if (auto status = ValidateEntry(name_space, key, value.size()); !status) {
        return status;
    }
    std::scoped_lock lock(impl_->mutex);
#if defined(SLINK_USE_SQLITE)
    sqlite3_stmt* statement = nullptr;
    constexpr char sql[] = "INSERT INTO app_state(namespace,key,value) VALUES(?1,?2,?3)"
                           " ON CONFLICT(namespace,key) DO UPDATE SET value=excluded.value";
    if (sqlite3_prepare_v2(impl_->database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return StorageStatus::Error(StorageErrc::backend_error, L"Cannot prepare state update");
    }
    sqlite3_bind_text(
        statement, 1, name_space.data(), static_cast<int>(name_space.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    if (value.empty()) {
        sqlite3_bind_zeroblob(statement, 3, 0);
    } else {
        sqlite3_bind_blob64(statement, 3, value.data(), value.size(), SQLITE_TRANSIENT);
    }
    const int result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return result == SQLITE_DONE
               ? StorageStatus::Ok()
               : StorageStatus::Error(StorageErrc::backend_error, L"Cannot update state");
#else
    impl_->entries[{std::move(name_space), std::move(key)}] =
        std::vector<std::byte>(value.begin(), value.end());
    impl_->dirty = true;
    return StorageStatus::Ok();
#endif
}

StorageResult<std::vector<std::byte>> StateStore::Get(const std::string& name_space,
                                                      const std::string& key) const {
    if (auto status = ValidateEntry(name_space, key); !status) {
        return StorageResult<std::vector<std::byte>>::Failure(std::move(status));
    }
    std::scoped_lock lock(impl_->mutex);
#if defined(SLINK_USE_SQLITE)
    sqlite3_stmt* statement = nullptr;
    constexpr char sql[] = "SELECT value FROM app_state WHERE namespace=?1 AND key=?2";
    if (sqlite3_prepare_v2(impl_->database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return StorageResult<std::vector<std::byte>>::Failure(
            StorageStatus::Error(StorageErrc::backend_error, L"Cannot prepare state query"));
    }
    sqlite3_bind_text(
        statement, 1, name_space.data(), static_cast<int>(name_space.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    const int result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        const auto size = sqlite3_column_bytes(statement, 0);
        const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(statement, 0));
        std::vector<std::byte> value;
        if (size > 0) {
            value.assign(data, data + size);
        }
        sqlite3_finalize(statement);
        return StorageResult<std::vector<std::byte>>::Success(std::move(value));
    }
    sqlite3_finalize(statement);
    return StorageResult<std::vector<std::byte>>::Failure(StorageStatus::Error(
        result == SQLITE_DONE ? StorageErrc::not_found : StorageErrc::backend_error,
        result == SQLITE_DONE ? L"State entry does not exist" : L"Cannot query state"));
#else
    const auto found = impl_->entries.find({name_space, key});
    if (found == impl_->entries.end()) {
        return StorageResult<std::vector<std::byte>>::Failure(
            StorageStatus::Error(StorageErrc::not_found, L"State entry does not exist"));
    }
    return StorageResult<std::vector<std::byte>>::Success(found->second);
#endif
}

StorageResult<std::vector<StateEntry>> StateStore::List(const std::string& name_space) const {
    if (name_space.empty() || name_space.size() > kMaxNameBytes) {
        return StorageResult<std::vector<StateEntry>>::Failure(StorageStatus::Error(
            StorageErrc::invalid_argument, L"State namespace is empty or too long"));
    }
    std::scoped_lock lock(impl_->mutex);
#if defined(SLINK_USE_SQLITE)
    sqlite3_stmt* statement = nullptr;
    constexpr char sql[] = "SELECT key,value FROM app_state WHERE namespace=?1 ORDER BY key";
    if (sqlite3_prepare_v2(impl_->database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return StorageResult<std::vector<StateEntry>>::Failure(
            StorageStatus::Error(StorageErrc::backend_error, L"Cannot prepare state list query"));
    }
    sqlite3_bind_text(
        statement, 1, name_space.data(), static_cast<int>(name_space.size()), SQLITE_TRANSIENT);
    std::vector<StateEntry> entries;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        const auto* key_data = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        const auto key_size = sqlite3_column_bytes(statement, 0);
        const auto* value_data = static_cast<const std::byte*>(sqlite3_column_blob(statement, 1));
        const auto value_size = sqlite3_column_bytes(statement, 1);
        StateEntry entry;
        entry.key.assign(key_data, key_data + key_size);
        if (value_size > 0) {
            entry.value.assign(value_data, value_data + value_size);
        }
        entries.push_back(std::move(entry));
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        return StorageResult<std::vector<StateEntry>>::Failure(
            StorageStatus::Error(StorageErrc::backend_error, L"Cannot enumerate state"));
    }
    return StorageResult<std::vector<StateEntry>>::Success(std::move(entries));
#else
    std::vector<StateEntry> entries;
    const auto begin = impl_->entries.lower_bound({name_space, {}});
    for (auto iterator = begin;
         iterator != impl_->entries.end() && iterator->first.first == name_space;
         ++iterator) {
        entries.push_back({iterator->first.second, iterator->second});
    }
    return StorageResult<std::vector<StateEntry>>::Success(std::move(entries));
#endif
}

StorageStatus StateStore::Remove(const std::string& name_space, const std::string& key) {
    if (auto status = ValidateEntry(name_space, key); !status) {
        return status;
    }
    std::scoped_lock lock(impl_->mutex);
#if defined(SLINK_USE_SQLITE)
    sqlite3_stmt* statement = nullptr;
    constexpr char sql[] = "DELETE FROM app_state WHERE namespace=?1 AND key=?2";
    if (sqlite3_prepare_v2(impl_->database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return StorageStatus::Error(StorageErrc::backend_error, L"Cannot prepare state removal");
    }
    sqlite3_bind_text(
        statement, 1, name_space.data(), static_cast<int>(name_space.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    const int result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return result == SQLITE_DONE
               ? StorageStatus::Ok()
               : StorageStatus::Error(StorageErrc::backend_error, L"Cannot remove state");
#else
    impl_->dirty = impl_->entries.erase({name_space, key}) != 0 || impl_->dirty;
    return StorageStatus::Ok();
#endif
}

StorageStatus StateStore::ApplyBatch(std::span<const StateMutation> mutations) {
    for (const auto& mutation : mutations) {
        if (mutation.kind != StateMutation::Kind::Set &&
            mutation.kind != StateMutation::Kind::Remove) {
            return StorageStatus::Error(StorageErrc::invalid_argument,
                                        L"Invalid state batch operation");
        }
        const auto status =
            ValidateEntry(mutation.name_space,
                          mutation.key,
                          mutation.kind == StateMutation::Kind::Set ? mutation.value.size() : 0);
        if (!status) {
            return status;
        }
    }
    if (mutations.empty()) {
        return StorageStatus::Ok();
    }
    std::scoped_lock lock(impl_->mutex);
#if defined(SLINK_USE_SQLITE)
    if (sqlite3_exec(impl_->database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return StorageStatus::Error(StorageErrc::backend_error, L"Cannot begin state batch");
    }
    bool succeeded = true;
    for (const auto& mutation : mutations) {
        sqlite3_stmt* statement = nullptr;
        constexpr char set_sql[] = "INSERT INTO app_state(namespace,key,value) VALUES(?1,?2,?3)"
                                   " ON CONFLICT(namespace,key) DO UPDATE SET value=excluded.value";
        constexpr char remove_sql[] = "DELETE FROM app_state WHERE namespace=?1 AND key=?2";
        const bool is_set = mutation.kind == StateMutation::Kind::Set;
        if (sqlite3_prepare_v2(
                impl_->database, is_set ? set_sql : remove_sql, -1, &statement, nullptr) !=
            SQLITE_OK) {
            succeeded = false;
            break;
        }
        sqlite3_bind_text(statement,
                          1,
                          mutation.name_space.c_str(),
                          static_cast<int>(mutation.name_space.size()),
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(statement,
                          2,
                          mutation.key.c_str(),
                          static_cast<int>(mutation.key.size()),
                          SQLITE_TRANSIENT);
        if (is_set) {
            if (mutation.value.empty()) {
                sqlite3_bind_zeroblob(statement, 3, 0);
            } else {
                sqlite3_bind_blob64(
                    statement, 3, mutation.value.data(), mutation.value.size(), SQLITE_TRANSIENT);
            }
        }
        succeeded = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        if (!succeeded) {
            break;
        }
    }
    if (!succeeded) {
        (void)sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
        return StorageStatus::Error(StorageErrc::backend_error, L"Cannot apply state batch");
    }
    if (sqlite3_exec(impl_->database, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        (void)sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
        return StorageStatus::Error(StorageErrc::backend_error, L"Cannot commit state batch");
    }
    return StorageStatus::Ok();
#else
    StateMap next = impl_->entries;
    for (const auto& mutation : mutations) {
        StateKey key{mutation.name_space, mutation.key};
        if (mutation.kind == StateMutation::Kind::Set) {
            next[std::move(key)] = mutation.value;
        } else {
            next.erase(key);
        }
    }
    const auto bytes = Serialize(next);
    auto status = WriteFileAtomically(impl_->path, bytes);
    if (status) {
        impl_->entries.swap(next);
        impl_->dirty = false;
    }
    return status;
#endif
}

StorageStatus StateStore::Flush() {
    std::scoped_lock lock(impl_->mutex);
#if defined(SLINK_USE_SQLITE)
    const int result = sqlite3_wal_checkpoint_v2(
        impl_->database, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
    return result == SQLITE_OK
               ? StorageStatus::Ok()
               : StorageStatus::Error(StorageErrc::backend_error, L"Cannot checkpoint state");
#else
    if (!impl_->dirty) {
        return StorageStatus::Ok();
    }
    auto bytes = Serialize(impl_->entries);
    auto status = WriteFileAtomically(impl_->path, bytes);
    if (status) {
        impl_->dirty = false;
    }
    return status;
#endif
}

bool StateStore::uses_sqlite() const noexcept {
#if defined(SLINK_USE_SQLITE)
    return true;
#else
    return false;
#endif
}

} // namespace SLink::storage
