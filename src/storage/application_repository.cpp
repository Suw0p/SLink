// 非敏感领域对象到 StateStore 键值记录的序列化层。Schema 只面向当前开发版本，
// 不执行旧版数据库迁移。
#include "storage/application_repository.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

namespace SLink::storage {
namespace {

constexpr std::string_view kSchemaNamespace = "schema";
constexpr std::string_view kSchemaVersionKey = "application";
constexpr std::string_view kServerGroupsNamespace = "server_groups";
constexpr std::string_view kServerProfilesNamespace = "server_profiles";
constexpr std::string_view kSettingsNamespace = "settings";
constexpr std::string_view kEditorRecoveryNamespace = "editor_recovery";
constexpr std::string_view kTransferRecoveryNamespace = "transfer_recovery";
constexpr std::string_view kPortForwardsNamespace = "port_forwards";
constexpr std::string_view kFirewallRulesNamespace = "firewall_rules";
constexpr std::size_t kMaximumIdBytes = 128;
constexpr std::size_t kMaximumTextBytes = 64 * 1024;
constexpr std::size_t kMaximumRecordBytes = 256 * 1024;

StorageStatus Invalid(std::wstring message) {
    return StorageStatus::Error(StorageErrc::invalid_argument, std::move(message));
}

bool IsValidId(std::string_view id) {
    if (id.empty() || id.size() > kMaximumIdBytes)
        return false;
    return std::ranges::all_of(id, [](unsigned char value) {
        return std::isalnum(value) || value == '-' || value == '_' || value == '.';
    });
}

bool IsValidText(std::string_view value, bool allow_empty = true) {
    return (allow_empty || !value.empty()) && value.size() <= kMaximumTextBytes &&
           value.find('\0') == std::string_view::npos;
}

bool IsSafeRelativeCachePath(std::string_view value) {
    if (!IsValidText(value, false))
        return false;
    std::filesystem::path path{std::string(value)};
    if (path.is_absolute() || path.has_root_path())
        return false;
    for (const auto& part : path) {
        if (part == ".." || part == ".")
            return false;
    }
    return path.filename() == path && !path.filename().empty();
}

bool LooksSensitiveSettingKey(std::string_view key) {
    std::string lowered(key);
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    constexpr std::array sensitive{
        "password", "passphrase", "private_key", "private-key", "secret", "token", "credential"};
    return std::ranges::any_of(sensitive, [&](std::string_view marker) {
        return lowered.find(marker) != std::string::npos;
    });
}

class Encoder {
public:
    template <typename UInt> void unsigned_value(UInt value) {
        static_assert(std::is_unsigned_v<UInt>);
        for (std::size_t index = 0; index < sizeof(UInt); ++index) {
            bytes_.push_back(static_cast<std::byte>((value >> (index * 8)) & 0xff));
        }
    }
    void signed32(std::int32_t value) {
        unsigned_value(std::bit_cast<std::uint32_t>(value));
    }
    void boolean(bool value) {
        unsigned_value<std::uint8_t>(value ? 1 : 0);
    }
    void string(std::string_view value) {
        unsigned_value<std::uint32_t>(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(),
                      reinterpret_cast<const std::byte*>(value.data()),
                      reinterpret_cast<const std::byte*>(value.data() + value.size()));
    }
    std::vector<std::byte> finish() && {
        return std::move(bytes_);
    }

private:
    std::vector<std::byte> bytes_;
};

class Decoder {
public:
    explicit Decoder(std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename UInt> bool unsigned_value(UInt& value) {
        static_assert(std::is_unsigned_v<UInt>);
        if (cursor_ > bytes_.size() || bytes_.size() - cursor_ < sizeof(UInt))
            return false;
        value = 0;
        for (std::size_t index = 0; index < sizeof(UInt); ++index) {
            value |= static_cast<UInt>(std::to_integer<unsigned char>(bytes_[cursor_ + index]))
                     << (index * 8);
        }
        cursor_ += sizeof(UInt);
        return true;
    }
    bool signed32(std::int32_t& value) {
        std::uint32_t bits = 0;
        if (!unsigned_value(bits))
            return false;
        value = std::bit_cast<std::int32_t>(bits);
        return true;
    }
    bool boolean(bool& value) {
        std::uint8_t raw = 0;
        if (!unsigned_value(raw) || raw > 1)
            return false;
        value = raw != 0;
        return true;
    }
    bool string(std::string& value) {
        std::uint32_t size = 0;
        if (!unsigned_value(size) || size > kMaximumTextBytes || cursor_ > bytes_.size() ||
            bytes_.size() - cursor_ < size) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(bytes_.data() + cursor_), size);
        cursor_ += size;
        return true;
    }
    [[nodiscard]] bool finished() const noexcept {
        return cursor_ == bytes_.size();
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t cursor_{};
};

StorageStatus PutAndFlush(StateStore& store,
                          std::string_view name_space,
                          std::string key,
                          std::span<const std::byte> value) {
    auto status = store.Set(std::string(name_space), std::move(key), value);
    return status ? store.Flush() : status;
}

StorageStatus
RemoveAndFlush(StateStore& store, std::string_view name_space, const std::string& key) {
    auto status = store.Remove(std::string(name_space), key);
    return status ? store.Flush() : status;
}

std::vector<std::byte> Encode(const ServerGroup& group) {
    Encoder encoder;
    encoder.unsigned_value<std::uint32_t>(1);
    encoder.string(group.id);
    encoder.string(group.name);
    encoder.signed32(group.sort_order);
    return std::move(encoder).finish();
}

StorageResult<ServerGroup> DecodeGroup(std::span<const std::byte> bytes) {
    Decoder decoder(bytes);
    std::uint32_t version = 0;
    ServerGroup value;
    if (bytes.size() > kMaximumRecordBytes || !decoder.unsigned_value(version) || version != 1 ||
        !decoder.string(value.id) || !decoder.string(value.name) ||
        !decoder.signed32(value.sort_order) || !decoder.finished() || !IsValidId(value.id) ||
        !IsValidText(value.name, false)) {
        return StorageResult<ServerGroup>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid server group record"));
    }
    return StorageResult<ServerGroup>::Success(std::move(value));
}

std::vector<std::byte> Encode(const ServerProfile& profile) {
    Encoder encoder;
    encoder.unsigned_value<std::uint32_t>(2);
    encoder.string(profile.id);
    encoder.boolean(profile.group_id.has_value());
    if (profile.group_id)
        encoder.string(*profile.group_id);
    encoder.string(profile.name);
    encoder.string(profile.host);
    encoder.unsigned_value(profile.port);
    encoder.string(profile.username);
    encoder.string(profile.initial_directory);
    encoder.unsigned_value<std::uint8_t>(static_cast<std::uint8_t>(profile.authentication));
    encoder.boolean(profile.credential_ref.has_value());
    if (profile.credential_ref)
        encoder.string(*profile.credential_ref);
    return std::move(encoder).finish();
}

StorageResult<ServerProfile> DecodeProfile(std::span<const std::byte> bytes) {
    Decoder decoder(bytes);
    std::uint32_t version = 0;
    std::uint8_t authentication = 0;
    bool has_group = false;
    bool has_credential = false;
    ServerProfile value;
    std::string group_id;
    std::string credential_ref;
    if (bytes.size() > kMaximumRecordBytes || !decoder.unsigned_value(version) ||
        (version != 1 && version != 2) || !decoder.string(value.id) ||
        !decoder.boolean(has_group) || (has_group && !decoder.string(group_id)) ||
        !decoder.string(value.name) || !decoder.string(value.host) ||
        !decoder.unsigned_value(value.port) || !decoder.string(value.username) ||
        (version >= 2 && !decoder.string(value.initial_directory)) ||
        !decoder.unsigned_value(authentication) ||
        authentication > static_cast<std::uint8_t>(AuthenticationKind::KeyboardInteractive) ||
        !decoder.boolean(has_credential) || (has_credential && !decoder.string(credential_ref)) ||
        !decoder.finished()) {
        return StorageResult<ServerProfile>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid server profile record"));
    }
    if (has_group)
        value.group_id = std::move(group_id);
    if (has_credential)
        value.credential_ref = std::move(credential_ref);
    value.authentication = static_cast<AuthenticationKind>(authentication);
    if (!IsValidId(value.id) || (value.group_id && !IsValidId(*value.group_id)) ||
        !IsValidText(value.name, false) || !IsValidText(value.host, false) ||
        !IsValidText(value.username) || !IsValidText(value.initial_directory) || value.port == 0 ||
        (value.credential_ref && !IsValidText(*value.credential_ref, false))) {
        return StorageResult<ServerProfile>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid server profile fields"));
    }
    return StorageResult<ServerProfile>::Success(std::move(value));
}

std::vector<std::byte> Encode(const EditorRecoveryState& state) {
    Encoder encoder;
    encoder.unsigned_value<std::uint32_t>(1);
    encoder.string(state.document_id);
    encoder.string(state.remote_uri);
    encoder.string(state.content_relative_path);
    encoder.string(state.metadata_relative_path);
    encoder.boolean(state.dirty);
    return std::move(encoder).finish();
}

StorageResult<EditorRecoveryState> DecodeEditorRecovery(std::span<const std::byte> bytes) {
    Decoder decoder(bytes);
    std::uint32_t version = 0;
    EditorRecoveryState value;
    if (bytes.size() > kMaximumRecordBytes || !decoder.unsigned_value(version) || version != 1 ||
        !decoder.string(value.document_id) || !decoder.string(value.remote_uri) ||
        !decoder.string(value.content_relative_path) ||
        !decoder.string(value.metadata_relative_path) || !decoder.boolean(value.dirty) ||
        !decoder.finished() || !IsValidId(value.document_id) ||
        !IsValidText(value.remote_uri, false) ||
        !IsSafeRelativeCachePath(value.content_relative_path) ||
        !IsSafeRelativeCachePath(value.metadata_relative_path)) {
        return StorageResult<EditorRecoveryState>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid editor recovery record"));
    }
    return StorageResult<EditorRecoveryState>::Success(std::move(value));
}

std::vector<std::byte> Encode(const TransferRecoveryRecord& record) {
    Encoder encoder;
    encoder.unsigned_value<std::uint32_t>(2);
    encoder.unsigned_value(record.id);
    encoder.string(record.source_uri);
    encoder.string(record.target_uri);
    encoder.string(record.effective_target_uri);
    encoder.unsigned_value(record.bytes_transferred);
    encoder.unsigned_value(record.bytes_total);
    encoder.unsigned_value<std::uint8_t>(static_cast<std::uint8_t>(record.state));
    encoder.unsigned_value<std::uint8_t>(static_cast<std::uint8_t>(record.conflict));
    encoder.boolean(record.move);
    encoder.boolean(record.verify_sha256);
    encoder.string(record.error);
    return std::move(encoder).finish();
}

StorageResult<TransferRecoveryRecord> DecodeTransferRecovery(std::span<const std::byte> bytes) {
    Decoder decoder(bytes);
    std::uint32_t version = 0;
    std::uint8_t state = 0;
    std::uint8_t conflict = 0;
    TransferRecoveryRecord value;
    bool decoded = false;
    if (bytes.size() <= kMaximumRecordBytes && decoder.unsigned_value(version) &&
        (version == 1 || version == 2) && decoder.unsigned_value(value.id) &&
        decoder.string(value.source_uri) && decoder.string(value.target_uri)) {
        if (version == 1) {
            value.effective_target_uri = value.target_uri;
            decoded = decoder.unsigned_value(value.bytes_transferred) &&
                      decoder.unsigned_value(value.bytes_total) && decoder.unsigned_value(state) &&
                      decoder.boolean(value.verify_sha256) && decoder.finished();
            // Version 1 did not persist the conflict policy. Resume is the only
            // safe interpretation for a potentially partial destination.
            value.conflict = TransferRecoveryConflictPolicy::Resume;
        } else {
            decoded = decoder.string(value.effective_target_uri) &&
                      decoder.unsigned_value(value.bytes_transferred) &&
                      decoder.unsigned_value(value.bytes_total) && decoder.unsigned_value(state) &&
                      decoder.unsigned_value(conflict) && decoder.boolean(value.move) &&
                      decoder.boolean(value.verify_sha256) && decoder.string(value.error) &&
                      decoder.finished();
            if (decoded) {
                value.conflict = static_cast<TransferRecoveryConflictPolicy>(conflict);
            }
        }
    }
    if (!decoded || value.id == 0 ||
        state > static_cast<std::uint8_t>(TransferRecoveryState::Failed) ||
        conflict > static_cast<std::uint8_t>(TransferRecoveryConflictPolicy::Resume) ||
        !IsValidText(value.source_uri, false) || !IsValidText(value.target_uri, false) ||
        !IsValidText(value.effective_target_uri, false) || !IsValidText(value.error, true) ||
        value.bytes_transferred > value.bytes_total) {
        return StorageResult<TransferRecoveryRecord>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid transfer recovery record"));
    }
    value.state = static_cast<TransferRecoveryState>(state);
    return StorageResult<TransferRecoveryRecord>::Success(std::move(value));
}

std::vector<std::byte> Encode(const PortForwardRecord& record) {
    Encoder encoder;
    encoder.unsigned_value<std::uint32_t>(1);
    encoder.unsigned_value(record.local_port);
    encoder.string(record.bind_address);
    encoder.string(record.target_address);
    encoder.unsigned_value(record.target_port);
    encoder.boolean(record.proxy_enabled);
    encoder.boolean(record.firewall_enabled);
    return std::move(encoder).finish();
}

bool ValidPortForward(const PortForwardRecord& record) {
    return record.local_port != 0 && record.target_port != 0 &&
           (record.bind_address == "127.0.0.1" || record.bind_address == "0.0.0.0") &&
           IsValidText(record.target_address, false);
}

std::string PortForwardKey(std::string_view bind_address, std::uint16_t local_port) {
    return std::string(bind_address) + "." + std::to_string(local_port);
}

StorageResult<PortForwardRecord> DecodePortForward(std::span<const std::byte> bytes) {
    Decoder decoder(bytes);
    std::uint32_t version{};
    PortForwardRecord value;
    if (bytes.size() > kMaximumRecordBytes || !decoder.unsigned_value(version) || version != 1 ||
        !decoder.unsigned_value(value.local_port) || !decoder.string(value.bind_address) ||
        !decoder.string(value.target_address) || !decoder.unsigned_value(value.target_port) ||
        !decoder.boolean(value.proxy_enabled) || !decoder.boolean(value.firewall_enabled) ||
        !decoder.finished() || !ValidPortForward(value)) {
        return StorageResult<PortForwardRecord>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid port forward record"));
    }
    return StorageResult<PortForwardRecord>::Success(std::move(value));
}

std::string FirewallRuleKey(std::string_view session_id, std::uint16_t local_port) {
    return std::string(session_id) + "." + std::to_string(local_port);
}

bool ValidFirewallRule(const FirewallRuleRecord& record) {
    return IsValidId(record.session_id) && record.session_id.size() == 32 &&
           record.local_port != 0 && IsValidText(record.display_name, false);
}

std::vector<std::byte> Encode(const FirewallRuleRecord& record) {
    Encoder encoder;
    encoder.unsigned_value<std::uint32_t>(1);
    encoder.string(record.session_id);
    encoder.unsigned_value(record.local_port);
    encoder.string(record.display_name);
    return std::move(encoder).finish();
}

StorageResult<FirewallRuleRecord> DecodeFirewallRule(std::span<const std::byte> bytes) {
    Decoder decoder(bytes);
    std::uint32_t version{};
    FirewallRuleRecord value;
    if (bytes.size() > kMaximumRecordBytes || !decoder.unsigned_value(version) || version != 1 ||
        !decoder.string(value.session_id) || !decoder.unsigned_value(value.local_port) ||
        !decoder.string(value.display_name) || !decoder.finished() || !ValidFirewallRule(value)) {
        return StorageResult<FirewallRuleRecord>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid firewall rule record"));
    }
    return StorageResult<FirewallRuleRecord>::Success(std::move(value));
}

template <typename Value, typename DecoderFunction>
StorageResult<std::optional<Value>> FindRecord(const StateStore& store,
                                               std::string_view name_space,
                                               const std::string& key,
                                               DecoderFunction decode) {
    auto bytes = store.Get(std::string(name_space), key);
    if (!bytes && bytes.status().code == StorageErrc::not_found) {
        return StorageResult<std::optional<Value>>::Success(std::nullopt);
    }
    if (!bytes) {
        return StorageResult<std::optional<Value>>::Failure(bytes.status());
    }
    auto value = decode(bytes.value());
    if (!value) {
        return StorageResult<std::optional<Value>>::Failure(value.status());
    }
    return StorageResult<std::optional<Value>>::Success(
        std::optional<Value>(std::move(value).value()));
}

template <typename Value, typename DecoderFunction>
StorageResult<std::vector<Value>>
ListRecords(const StateStore& store, std::string_view name_space, DecoderFunction decode) {
    auto entries = store.List(std::string(name_space));
    if (!entries) {
        return StorageResult<std::vector<Value>>::Failure(entries.status());
    }
    std::vector<Value> values;
    values.reserve(entries.value().size());
    for (const auto& entry : entries.value()) {
        auto value = decode(entry.value);
        if (!value) {
            return StorageResult<std::vector<Value>>::Failure(value.status());
        }
        values.push_back(std::move(value).value());
    }
    return StorageResult<std::vector<Value>>::Success(std::move(values));
}

} // namespace

StorageResult<std::unique_ptr<ApplicationRepository>>
ApplicationRepository::Open(StateStore& store) {
    // 首次打开直接写入当前 schema 版本；已有库必须完整解码且版本完全一致。开发阶段
    // 不执行隐式迁移，版本不匹配时明确失败，由调用方按“删库重建”策略处理。
    auto schema = store.Get(std::string(kSchemaNamespace), std::string(kSchemaVersionKey));
    if (!schema && schema.status().code == StorageErrc::not_found) {
        Encoder encoder;
        encoder.unsigned_value<std::uint32_t>(kCurrentSchemaVersion);
        auto bytes = std::move(encoder).finish();
        auto status = PutAndFlush(store, kSchemaNamespace, std::string(kSchemaVersionKey), bytes);
        if (!status) {
            return StorageResult<std::unique_ptr<ApplicationRepository>>::Failure(
                std::move(status));
        }
    } else if (!schema) {
        return StorageResult<std::unique_ptr<ApplicationRepository>>::Failure(schema.status());
    } else {
        Decoder decoder(schema.value());
        std::uint32_t version = 0;
        if (!decoder.unsigned_value(version) || !decoder.finished()) {
            return StorageResult<std::unique_ptr<ApplicationRepository>>::Failure(
                StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid schema version"));
        }
        if (version != kCurrentSchemaVersion) {
            return StorageResult<std::unique_ptr<ApplicationRepository>>::Failure(
                StorageStatus::Error(StorageErrc::unsupported,
                                     L"Unsupported application state schema"));
        }
    }
    return StorageResult<std::unique_ptr<ApplicationRepository>>::Success(
        std::unique_ptr<ApplicationRepository>(new ApplicationRepository(store)));
}

StorageStatus ApplicationRepository::UpsertServerGroup(const ServerGroup& group) {
    if (!IsValidId(group.id) || !IsValidText(group.name, false)) {
        return Invalid(L"Invalid server group");
    }
    const auto bytes = Encode(group);
    return PutAndFlush(store_, kServerGroupsNamespace, group.id, bytes);
}

StorageResult<std::optional<ServerGroup>>
ApplicationRepository::FindServerGroup(const std::string& id) const {
    if (!IsValidId(id)) {
        return StorageResult<std::optional<ServerGroup>>::Failure(Invalid(L"Invalid group id"));
    }
    return FindRecord<ServerGroup>(store_, kServerGroupsNamespace, id, DecodeGroup);
}

StorageResult<std::vector<ServerGroup>> ApplicationRepository::ListServerGroups() const {
    return ListRecords<ServerGroup>(store_, kServerGroupsNamespace, DecodeGroup);
}

StorageStatus ApplicationRepository::RemoveServerGroup(const std::string& id) {
    return RemoveServerGroupAndUngroup(id);
}

StorageStatus ApplicationRepository::RemoveServerGroupAndUngroup(const std::string& id) {
    auto group = FindServerGroup(id);
    if (!group) {
        return group.status();
    }
    if (!group.value()) {
        return StorageStatus::Error(StorageErrc::not_found, L"Server group does not exist");
    }
    auto profiles = ListServerProfiles();
    if (!profiles) {
        return profiles.status();
    }
    std::vector<StateMutation> changes;
    changes.reserve(profiles.value().size() + 1);
    for (auto& profile : profiles.value()) {
        if (profile.group_id != id) {
            continue;
        }
        profile.group_id.reset();
        changes.push_back({StateMutation::Kind::Set,
                           std::string(kServerProfilesNamespace),
                           profile.id,
                           Encode(profile)});
    }
    changes.push_back({StateMutation::Kind::Remove, std::string(kServerGroupsNamespace), id, {}});
    return store_.ApplyBatch(changes);
}

StorageStatus ApplicationRepository::UpsertServerProfile(const ServerProfile& profile) {
    if (!IsValidId(profile.id) || (profile.group_id && !IsValidId(*profile.group_id)) ||
        !IsValidText(profile.name, false) || !IsValidText(profile.host, false) ||
        !IsValidText(profile.username) || !IsValidText(profile.initial_directory) ||
        profile.port == 0 ||
        (profile.credential_ref && !IsValidText(*profile.credential_ref, false))) {
        return Invalid(L"Invalid server profile");
    }
    const auto bytes = Encode(profile);
    return PutAndFlush(store_, kServerProfilesNamespace, profile.id, bytes);
}

StorageResult<std::optional<ServerProfile>>
ApplicationRepository::FindServerProfile(const std::string& id) const {
    if (!IsValidId(id)) {
        return StorageResult<std::optional<ServerProfile>>::Failure(
            Invalid(L"Invalid server profile id"));
    }
    return FindRecord<ServerProfile>(store_, kServerProfilesNamespace, id, DecodeProfile);
}

StorageResult<std::vector<ServerProfile>> ApplicationRepository::ListServerProfiles() const {
    return ListRecords<ServerProfile>(store_, kServerProfilesNamespace, DecodeProfile);
}

StorageStatus ApplicationRepository::RemoveServerProfile(const std::string& id) {
    if (!IsValidId(id))
        return Invalid(L"Invalid server profile id");
    return RemoveAndFlush(store_, kServerProfilesNamespace, id);
}

StorageStatus ApplicationRepository::SetSetting(std::string key, std::string value) {
    if (!IsValidId(key) || !IsValidText(value) || LooksSensitiveSettingKey(key)) {
        return Invalid(L"Invalid or sensitive setting key/value");
    }
    const auto bytes =
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()), value.size());
    return PutAndFlush(store_, kSettingsNamespace, std::move(key), bytes);
}

StorageResult<std::optional<std::string>>
ApplicationRepository::GetSetting(const std::string& key) const {
    if (!IsValidId(key) || LooksSensitiveSettingKey(key)) {
        return StorageResult<std::optional<std::string>>::Failure(
            Invalid(L"Invalid or sensitive setting key"));
    }
    auto bytes = store_.Get(std::string(kSettingsNamespace), key);
    if (!bytes && bytes.status().code == StorageErrc::not_found) {
        return StorageResult<std::optional<std::string>>::Success(std::nullopt);
    }
    if (!bytes) {
        return StorageResult<std::optional<std::string>>::Failure(bytes.status());
    }
    if (bytes.value().empty()) {
        return StorageResult<std::optional<std::string>>::Success(std::string{});
    }
    return StorageResult<std::optional<std::string>>::Success(
        std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
}

StorageStatus ApplicationRepository::RemoveSetting(const std::string& key) {
    if (!IsValidId(key) || LooksSensitiveSettingKey(key)) {
        return Invalid(L"Invalid or sensitive setting key");
    }
    return RemoveAndFlush(store_, kSettingsNamespace, key);
}

StorageStatus ApplicationRepository::UpsertEditorRecovery(const EditorRecoveryState& state) {
    if (!IsValidId(state.document_id) || !IsValidText(state.remote_uri, false) ||
        !IsSafeRelativeCachePath(state.content_relative_path) ||
        !IsSafeRelativeCachePath(state.metadata_relative_path)) {
        return Invalid(L"Invalid editor recovery state");
    }
    const auto bytes = Encode(state);
    return PutAndFlush(store_, kEditorRecoveryNamespace, state.document_id, bytes);
}

StorageResult<std::vector<EditorRecoveryState>> ApplicationRepository::ListEditorRecovery() const {
    return ListRecords<EditorRecoveryState>(store_, kEditorRecoveryNamespace, DecodeEditorRecovery);
}

StorageStatus ApplicationRepository::RemoveEditorRecovery(const std::string& document_id) {
    if (!IsValidId(document_id))
        return Invalid(L"Invalid document id");
    return RemoveAndFlush(store_, kEditorRecoveryNamespace, document_id);
}

StorageStatus ApplicationRepository::UpsertTransferRecovery(const TransferRecoveryRecord& record) {
    if (record.id == 0 || !IsValidText(record.source_uri, false) ||
        !IsValidText(record.target_uri, false) ||
        !IsValidText(record.effective_target_uri, false) || !IsValidText(record.error, true) ||
        record.conflict > TransferRecoveryConflictPolicy::Resume ||
        record.bytes_transferred > record.bytes_total) {
        return Invalid(L"Invalid transfer recovery record");
    }
    const auto bytes = Encode(record);
    return PutAndFlush(store_, kTransferRecoveryNamespace, std::to_string(record.id), bytes);
}

StorageResult<std::vector<TransferRecoveryRecord>>
ApplicationRepository::ListTransferRecovery() const {
    return ListRecords<TransferRecoveryRecord>(
        store_, kTransferRecoveryNamespace, DecodeTransferRecovery);
}

StorageStatus ApplicationRepository::RemoveTransferRecovery(std::uint64_t id) {
    if (id == 0)
        return Invalid(L"Invalid transfer id");
    return RemoveAndFlush(store_, kTransferRecoveryNamespace, std::to_string(id));
}

StorageStatus ApplicationRepository::UpsertPortForward(const PortForwardRecord& record) {
    if (!ValidPortForward(record))
        return Invalid(L"Invalid port forward");
    const auto bytes = Encode(record);
    return PutAndFlush(store_,
                       kPortForwardsNamespace,
                       PortForwardKey(record.bind_address, record.local_port),
                       bytes);
}

StorageResult<std::vector<PortForwardRecord>> ApplicationRepository::ListPortForwards() const {
    return ListRecords<PortForwardRecord>(store_, kPortForwardsNamespace, DecodePortForward);
}

StorageStatus ApplicationRepository::RemovePortForward(const std::string& bind_address,
                                                       std::uint16_t local_port) {
    if ((bind_address != "127.0.0.1" && bind_address != "0.0.0.0") || local_port == 0) {
        return Invalid(L"Invalid port forward identity");
    }
    return RemoveAndFlush(store_, kPortForwardsNamespace, PortForwardKey(bind_address, local_port));
}

StorageStatus ApplicationRepository::UpsertFirewallRule(const FirewallRuleRecord& record) {
    if (!ValidFirewallRule(record))
        return Invalid(L"Invalid firewall rule");
    const auto bytes = Encode(record);
    return PutAndFlush(store_,
                       kFirewallRulesNamespace,
                       FirewallRuleKey(record.session_id, record.local_port),
                       bytes);
}

StorageResult<std::vector<FirewallRuleRecord>> ApplicationRepository::ListFirewallRules() const {
    return ListRecords<FirewallRuleRecord>(store_, kFirewallRulesNamespace, DecodeFirewallRule);
}

StorageStatus ApplicationRepository::RemoveFirewallRule(const std::string& session_id,
                                                        std::uint16_t local_port) {
    if (!IsValidId(session_id) || session_id.size() != 32 || local_port == 0)
        return Invalid(L"Invalid firewall rule identity");
    return RemoveAndFlush(store_, kFirewallRulesNamespace, FirewallRuleKey(session_id, local_port));
}

StorageStatus ApplicationRepository::Flush() {
    return store_.Flush();
}

} // namespace SLink::storage
