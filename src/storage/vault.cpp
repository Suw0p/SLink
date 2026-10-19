// 便携凭据库实现：管理 vault.key、逐记录随机 nonce 加密、认证解密及敏感内存清理。
// 密文格式包含版本和完整性验证，损坏或篡改记录不会返回部分明文。
#include "storage/vault.h"

#ifndef _WIN32
#error Vault is a Windows-only implementation
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(SLINK_USE_LIBSODIUM)
#include <sodium.h>
#endif

namespace SLink::storage {
namespace {

StorageStatus UnsupportedStatus() {
    return StorageStatus::Error(
        StorageErrc::unsupported,
        L"Vault support is unavailable because this build was compiled without libsodium");
}

#if defined(SLINK_USE_LIBSODIUM)

constexpr std::array<std::byte, 8> kVaultMagic{std::byte{'S'},
                                               std::byte{'W'},
                                               std::byte{'V'},
                                               std::byte{'A'},
                                               std::byte{'U'},
                                               std::byte{'L'},
                                               std::byte{'T'},
                                               std::byte{'1'}};
constexpr std::array<unsigned char, 28> kVerifier{'s', 's', 'h', '-', 'w', 'o', 'r', 'k', 'b', 'e',
                                                  'n', 'c', 'h', '-', 'v', 'a', 'u', 'l', 't', '-',
                                                  'c', 'h', 'e', 'c', 'k', '-', 'v', '1'};
constexpr std::array<unsigned char, 14> kVerifierAad{
    'v', 'a', 'u', 'l', 't', '-', 'v', 'e', 'r', 'i', 'f', 'i', 'e', 'r'};
constexpr std::size_t kMaxVaultBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxSecretBytes = 32ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxKeyBytes = 4096;
constexpr std::size_t kPortableKeyBytes = crypto_aead_xchacha20poly1305_ietf_KEYBYTES;

struct EncryptedRecord {
    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    std::vector<unsigned char> ciphertext;
};

std::wstring WinError(const wchar_t* operation, DWORD error) {
    return std::wstring(operation) + L" (Windows error " + std::to_wstring(error) + L")";
}

template <typename UInt> void AppendUnsigned(std::vector<std::byte>& output, UInt value) {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        output.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xff));
    }
}

template <typename UInt>
bool ReadUnsigned(std::span<const std::byte> input, std::size_t& cursor, UInt& value) {
    static_assert(std::is_unsigned_v<UInt>);
    if (cursor > input.size() || input.size() - cursor < sizeof(UInt)) {
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

StorageResult<std::vector<std::byte>> ReadVaultFile(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return StorageResult<std::vector<std::byte>>::Failure(StorageStatus::Error(
            error == ERROR_FILE_NOT_FOUND ? StorageErrc::not_found : StorageErrc::io_error,
            error == ERROR_FILE_NOT_FOUND ? L"Vault has not been initialized"
                                          : WinError(L"Cannot open vault", error)));
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > kMaxVaultBytes) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return StorageResult<std::vector<std::byte>>::Failure(StorageStatus::Error(
            StorageErrc::corrupt_data,
            error == ERROR_SUCCESS ? L"Vault file is too large"
                                   : WinError(L"Cannot inspect vault", error)));
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
            return StorageResult<std::vector<std::byte>>::Failure(
                StorageStatus::Error(StorageErrc::io_error, WinError(L"Cannot read vault", error)));
        }
        offset += read;
    }
    CloseHandle(file);
    return StorageResult<std::vector<std::byte>>::Success(std::move(bytes));
}

StorageStatus WriteVaultAtomically(const std::filesystem::path& path,
                                   std::span<const std::byte> bytes) {
    auto pending = path;
    pending += L".pending";
    HANDLE file = CreateFileW(
        pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return StorageStatus::Error(StorageErrc::io_error,
                                    WinError(L"Cannot create pending vault", GetLastError()));
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
                                        WinError(L"Cannot write vault", error));
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        DeleteFileW(pending.c_str());
        return StorageStatus::Error(StorageErrc::io_error, WinError(L"Cannot flush vault", error));
    }
    CloseHandle(file);
    if (!MoveFileExW(
            pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(pending.c_str());
        return StorageStatus::Error(StorageErrc::io_error,
                                    WinError(L"Cannot replace vault", error));
    }
    return StorageStatus::Ok();
}

StorageResult<std::array<std::byte, kPortableKeyBytes>>
ReadPortableKey(const std::filesystem::path& path) {
    std::array<std::byte, kPortableKeyBytes> key{};
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return StorageResult<decltype(key)>::Failure(StorageStatus::Error(
            error == ERROR_FILE_NOT_FOUND ? StorageErrc::not_found : StorageErrc::io_error,
            error == ERROR_FILE_NOT_FOUND ? L"Portable vault key does not exist"
                                          : WinError(L"Cannot open portable vault key", error)));
    }
    DWORD read = 0;
    const BOOL ok = ReadFile(file, key.data(), static_cast<DWORD>(key.size()), &read, nullptr);
    LARGE_INTEGER size{};
    const BOOL size_ok = GetFileSizeEx(file, &size);
    CloseHandle(file);
    if (!ok || !size_ok || read != key.size() ||
        size.QuadPart != static_cast<LONGLONG>(key.size())) {
        sodium_memzero(key.data(), key.size());
        return StorageResult<decltype(key)>::Failure(StorageStatus::Error(
            StorageErrc::corrupt_data, L"Portable vault key has an invalid size"));
    }
    return StorageResult<decltype(key)>::Success(std::move(key));
}

StorageStatus WritePortableKey(const std::filesystem::path& path, std::span<const std::byte> key) {
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return StorageStatus::Error(StorageErrc::io_error,
                                    WinError(L"Cannot create portable vault key", GetLastError()));
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, key.data(), static_cast<DWORD>(key.size()), &written, nullptr);
    const BOOL flushed = ok ? FlushFileBuffers(file) : FALSE;
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!ok || !flushed || written != key.size()) {
        DeleteFileW(path.c_str());
        return StorageStatus::Error(StorageErrc::io_error,
                                    WinError(L"Cannot write portable vault key", error));
    }
    return StorageStatus::Ok();
}

StorageStatus ValidateVaultKey(const std::string& key) {
    if (key.empty() || key.size() > kMaxKeyBytes) {
        return StorageStatus::Error(StorageErrc::invalid_argument,
                                    L"Vault key is empty or too long");
    }
    return StorageStatus::Ok();
}

#endif

} // namespace

// Vault::Impl 独占数据库句柄和解锁密钥。锁定或析构时先清零主密钥，再关闭后端；
// 每条记录使用独立随机 nonce 和认证加密，读取时任何认证失败都视为损坏数据。
struct Vault::Impl {
    explicit Impl(std::filesystem::path vault_path) : path(std::move(vault_path)) {}
    std::filesystem::path path;
    mutable std::mutex mutex;

#if defined(SLINK_USE_LIBSODIUM)
    bool initialized{false};
    std::uint64_t ops_limit{crypto_pwhash_OPSLIMIT_INTERACTIVE};
    std::uint64_t memory_limit{crypto_pwhash_MEMLIMIT_INTERACTIVE};
    std::array<unsigned char, crypto_pwhash_SALTBYTES> salt{};
    EncryptedRecord verifier;
    std::map<std::string, EncryptedRecord> records;
    SecureBuffer master_key;
#endif
};

#if defined(SLINK_USE_LIBSODIUM)
namespace {

template <typename VaultImpl>
StorageResult<SecureBuffer> DeriveKey(std::span<const std::byte> password, const VaultImpl& impl) {
    try {
        SecureBuffer key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
        if (crypto_pwhash(reinterpret_cast<unsigned char*>(key.bytes().data()),
                          key.size(),
                          reinterpret_cast<const char*>(password.data()),
                          static_cast<unsigned long long>(password.size()),
                          impl.salt.data(),
                          impl.ops_limit,
                          static_cast<std::size_t>(impl.memory_limit),
                          crypto_pwhash_ALG_ARGON2ID13) != 0) {
            return StorageResult<SecureBuffer>::Failure(StorageStatus::Error(
                StorageErrc::backend_error, L"Unable to allocate memory for Argon2id"));
        }
        return StorageResult<SecureBuffer>::Success(std::move(key));
    } catch (const std::bad_alloc&) {
        return StorageResult<SecureBuffer>::Failure(
            StorageStatus::Error(StorageErrc::backend_error, L"Secure memory allocation failed"));
    }
}

StorageResult<EncryptedRecord> Encrypt(std::span<const std::byte> plaintext,
                                       std::span<const unsigned char> associated_data,
                                       std::span<const std::byte> key) {
    EncryptedRecord record;
    randombytes_buf(record.nonce.data(), record.nonce.size());
    record.ciphertext.resize(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long ciphertext_size = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            record.ciphertext.data(),
            &ciphertext_size,
            reinterpret_cast<const unsigned char*>(plaintext.data()),
            plaintext.size(),
            associated_data.data(),
            associated_data.size(),
            nullptr,
            record.nonce.data(),
            reinterpret_cast<const unsigned char*>(key.data())) != 0) {
        return StorageResult<EncryptedRecord>::Failure(
            StorageStatus::Error(StorageErrc::backend_error, L"Secret encryption failed"));
    }
    record.ciphertext.resize(static_cast<std::size_t>(ciphertext_size));
    return StorageResult<EncryptedRecord>::Success(std::move(record));
}

StorageResult<SecureBuffer> Decrypt(const EncryptedRecord& record,
                                    std::span<const unsigned char> associated_data,
                                    std::span<const std::byte> key,
                                    StorageErrc failure_code) {
    if (record.ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return StorageResult<SecureBuffer>::Failure(
            StorageStatus::Error(StorageErrc::corrupt_data, L"Encrypted record is truncated"));
    }
    try {
        SecureBuffer plaintext(record.ciphertext.size() -
                               crypto_aead_xchacha20poly1305_ietf_ABYTES);
        unsigned long long plaintext_size = 0;
        if (crypto_aead_xchacha20poly1305_ietf_decrypt(
                reinterpret_cast<unsigned char*>(plaintext.bytes().data()),
                &plaintext_size,
                nullptr,
                record.ciphertext.data(),
                record.ciphertext.size(),
                associated_data.data(),
                associated_data.size(),
                record.nonce.data(),
                reinterpret_cast<const unsigned char*>(key.data())) != 0) {
            return StorageResult<SecureBuffer>::Failure(
                StorageStatus::Error(failure_code,
                                     failure_code == StorageErrc::authentication_failed
                                         ? L"Master password is incorrect"
                                         : L"Secret authentication failed"));
        }
        return StorageResult<SecureBuffer>::Success(std::move(plaintext));
    } catch (const std::bad_alloc&) {
        return StorageResult<SecureBuffer>::Failure(
            StorageStatus::Error(StorageErrc::backend_error, L"Secure memory allocation failed"));
    }
}

template <typename VaultImpl> std::vector<std::byte> SerializeVault(const VaultImpl& impl) {
    std::vector<std::byte> output;
    output.insert(output.end(), kVaultMagic.begin(), kVaultMagic.end());
    AppendUnsigned<std::uint32_t>(output, 1);
    AppendUnsigned<std::uint64_t>(output, impl.ops_limit);
    AppendUnsigned<std::uint64_t>(output, impl.memory_limit);
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(impl.salt.data()),
                  reinterpret_cast<const std::byte*>(impl.salt.data() + impl.salt.size()));
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(impl.verifier.nonce.data()),
                  reinterpret_cast<const std::byte*>(impl.verifier.nonce.data() +
                                                     impl.verifier.nonce.size()));
    AppendUnsigned<std::uint32_t>(output,
                                  static_cast<std::uint32_t>(impl.verifier.ciphertext.size()));
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(impl.verifier.ciphertext.data()),
                  reinterpret_cast<const std::byte*>(impl.verifier.ciphertext.data() +
                                                     impl.verifier.ciphertext.size()));
    AppendUnsigned<std::uint32_t>(output, static_cast<std::uint32_t>(impl.records.size()));
    for (const auto& [key, record] : impl.records) {
        AppendUnsigned<std::uint32_t>(output, static_cast<std::uint32_t>(key.size()));
        AppendUnsigned<std::uint64_t>(output, static_cast<std::uint64_t>(record.ciphertext.size()));
        output.insert(output.end(),
                      reinterpret_cast<const std::byte*>(key.data()),
                      reinterpret_cast<const std::byte*>(key.data() + key.size()));
        output.insert(
            output.end(),
            reinterpret_cast<const std::byte*>(record.nonce.data()),
            reinterpret_cast<const std::byte*>(record.nonce.data() + record.nonce.size()));
        output.insert(output.end(),
                      reinterpret_cast<const std::byte*>(record.ciphertext.data()),
                      reinterpret_cast<const std::byte*>(record.ciphertext.data() +
                                                         record.ciphertext.size()));
    }
    AppendUnsigned<std::uint64_t>(output, Fnv1a(output));
    return output;
}

template <typename VaultImpl>
StorageStatus DeserializeVault(std::span<const std::byte> bytes, VaultImpl& impl) {
    constexpr std::size_t fixed_minimum = kVaultMagic.size() + 4 + 8 + 8 + crypto_pwhash_SALTBYTES +
                                          crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + 4 + 4 + 8;
    if (bytes.size() < fixed_minimum ||
        !std::equal(kVaultMagic.begin(), kVaultMagic.end(), bytes.begin())) {
        return StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid vault header");
    }
    const auto payload = bytes.first(bytes.size() - sizeof(std::uint64_t));
    std::size_t checksum_cursor = payload.size();
    std::uint64_t stored_checksum = 0;
    if (!ReadUnsigned(bytes, checksum_cursor, stored_checksum) ||
        stored_checksum != Fnv1a(payload)) {
        return StorageStatus::Error(StorageErrc::corrupt_data, L"Vault checksum mismatch");
    }

    std::size_t cursor = kVaultMagic.size();
    std::uint32_t version = 0;
    if (!ReadUnsigned(payload, cursor, version) || version != 1 ||
        !ReadUnsigned(payload, cursor, impl.ops_limit) ||
        !ReadUnsigned(payload, cursor, impl.memory_limit) ||
        impl.ops_limit < crypto_pwhash_OPSLIMIT_MIN ||
        impl.ops_limit > crypto_pwhash_OPSLIMIT_SENSITIVE ||
        impl.memory_limit < crypto_pwhash_MEMLIMIT_MIN ||
        impl.memory_limit > 1024ULL * 1024ULL * 1024ULL) {
        return StorageStatus::Error(StorageErrc::corrupt_data, L"Unsupported vault parameters");
    }
    if (payload.size() - cursor < impl.salt.size() + impl.verifier.nonce.size()) {
        return StorageStatus::Error(StorageErrc::corrupt_data, L"Vault header is truncated");
    }
    std::memcpy(impl.salt.data(), payload.data() + cursor, impl.salt.size());
    cursor += impl.salt.size();
    std::memcpy(impl.verifier.nonce.data(), payload.data() + cursor, impl.verifier.nonce.size());
    cursor += impl.verifier.nonce.size();
    std::uint32_t verifier_size = 0;
    if (!ReadUnsigned(payload, cursor, verifier_size) ||
        verifier_size != kVerifier.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES ||
        payload.size() - cursor < verifier_size) {
        return StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid vault verifier");
    }
    impl.verifier.ciphertext.assign(
        reinterpret_cast<const unsigned char*>(payload.data() + cursor),
        reinterpret_cast<const unsigned char*>(payload.data() + cursor + verifier_size));
    cursor += verifier_size;

    std::uint32_t count = 0;
    if (!ReadUnsigned(payload, cursor, count) || count > 1'000'000) {
        return StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid vault record count");
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t key_size = 0;
        std::uint64_t cipher_size = 0;
        if (!ReadUnsigned(payload, cursor, key_size) ||
            !ReadUnsigned(payload, cursor, cipher_size) || key_size == 0 ||
            key_size > kMaxKeyBytes ||
            cipher_size > kMaxSecretBytes + crypto_aead_xchacha20poly1305_ietf_ABYTES ||
            payload.size() - cursor <
                key_size + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + cipher_size) {
            return StorageStatus::Error(StorageErrc::corrupt_data, L"Invalid vault record");
        }
        std::string key(reinterpret_cast<const char*>(payload.data() + cursor), key_size);
        cursor += key_size;
        EncryptedRecord record;
        std::memcpy(record.nonce.data(), payload.data() + cursor, record.nonce.size());
        cursor += record.nonce.size();
        record.ciphertext.assign(
            reinterpret_cast<const unsigned char*>(payload.data() + cursor),
            reinterpret_cast<const unsigned char*>(payload.data() + cursor + cipher_size));
        cursor += static_cast<std::size_t>(cipher_size);
        if (!impl.records.emplace(std::move(key), std::move(record)).second) {
            return StorageStatus::Error(StorageErrc::corrupt_data, L"Duplicate vault record");
        }
    }
    if (cursor != payload.size()) {
        return StorageStatus::Error(StorageErrc::corrupt_data, L"Trailing vault data");
    }
    impl.initialized = true;
    return StorageStatus::Ok();
}

template <typename VaultImpl> StorageStatus SaveVault(const VaultImpl& impl) {
    const auto bytes = SerializeVault(impl);
    if (bytes.size() > kMaxVaultBytes) {
        return StorageStatus::Error(StorageErrc::io_error, L"Vault exceeds its size limit");
    }
    return WriteVaultAtomically(impl.path, bytes);
}

} // namespace
#endif

Vault::Vault(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Vault::~Vault() {
    Lock();
}
Vault::Vault(Vault&&) noexcept = default;
Vault& Vault::operator=(Vault&&) noexcept = default;

bool Vault::IsSupported() noexcept {
#if defined(SLINK_USE_LIBSODIUM)
    return true;
#else
    return false;
#endif
}

StorageResult<std::unique_ptr<Vault>> Vault::Open(const std::filesystem::path& vault_path) {
#if !defined(SLINK_USE_LIBSODIUM)
    (void)vault_path;
    return StorageResult<std::unique_ptr<Vault>>::Failure(UnsupportedStatus());
#else
    // 先初始化 libsodium 并验证绝对路径，再只读取/反序列化密文结构；Open 不自动解锁，
    // 主密钥在 Unlock/UnlockPortable 成功前始终为空。
    if (sodium_init() < 0) {
        return StorageResult<std::unique_ptr<Vault>>::Failure(
            StorageStatus::Error(StorageErrc::backend_error, L"libsodium initialization failed"));
    }
    if (vault_path.empty() || !vault_path.is_absolute()) {
        return StorageResult<std::unique_ptr<Vault>>::Failure(
            StorageStatus::Error(StorageErrc::invalid_argument, L"Vault path must be absolute"));
    }
    std::error_code error;
    if (!std::filesystem::is_directory(vault_path.parent_path(), error) || error) {
        return StorageResult<std::unique_ptr<Vault>>::Failure(
            StorageStatus::Error(StorageErrc::io_error, L"Vault parent directory does not exist"));
    }
    auto impl = std::make_unique<Impl>(vault_path.lexically_normal());
    auto bytes = ReadVaultFile(impl->path);
    if (bytes) {
        if (auto status = DeserializeVault(bytes.value(), *impl); !status) {
            return StorageResult<std::unique_ptr<Vault>>::Failure(std::move(status));
        }
    } else if (bytes.status().code != StorageErrc::not_found) {
        return StorageResult<std::unique_ptr<Vault>>::Failure(bytes.status());
    }
    return StorageResult<std::unique_ptr<Vault>>::Success(
        std::unique_ptr<Vault>(new Vault(std::move(impl))));
#endif
}

StorageStatus Vault::Initialize(std::span<const std::byte> master_password) {
#if !defined(SLINK_USE_LIBSODIUM)
    (void)master_password;
    return UnsupportedStatus();
#else
    if (master_password.empty()) {
        return StorageStatus::Error(StorageErrc::invalid_argument,
                                    L"Master password must not be empty");
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->initialized || std::filesystem::exists(impl_->path)) {
        return StorageStatus::Error(StorageErrc::already_exists, L"Vault already exists");
    }
    randombytes_buf(impl_->salt.data(), impl_->salt.size());
    auto key = DeriveKey(master_password, *impl_);
    if (!key) {
        return key.status();
    }
    const auto verifier_plaintext = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(kVerifier.data()), kVerifier.size());
    auto verifier = Encrypt(verifier_plaintext, kVerifierAad, key.value().bytes());
    if (!verifier) {
        return verifier.status();
    }
    impl_->verifier = std::move(verifier).value();
    impl_->master_key = std::move(key).value();
    impl_->initialized = true;
    auto status = SaveVault(*impl_);
    if (!status) {
        impl_->master_key = {};
        impl_->verifier = {};
        impl_->initialized = false;
    }
    return status;
#endif
}

StorageStatus Vault::Unlock(std::span<const std::byte> master_password) {
#if !defined(SLINK_USE_LIBSODIUM)
    (void)master_password;
    return UnsupportedStatus();
#else
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->initialized) {
        return StorageStatus::Error(StorageErrc::not_found, L"Vault is not initialized");
    }
    auto key = DeriveKey(master_password, *impl_);
    if (!key) {
        return key.status();
    }
    auto verifier = Decrypt(
        impl_->verifier, kVerifierAad, key.value().bytes(), StorageErrc::authentication_failed);
    if (!verifier || verifier.value().size() != kVerifier.size() ||
        sodium_memcmp(verifier.value().bytes().data(), kVerifier.data(), kVerifier.size()) != 0) {
        return StorageStatus::Error(StorageErrc::authentication_failed,
                                    L"Master password is incorrect");
    }
    impl_->master_key = std::move(key).value();
    return StorageStatus::Ok();
#endif
}

StorageStatus Vault::UnlockPortable() {
#if !defined(SLINK_USE_LIBSODIUM)
    return UnsupportedStatus();
#else
    // 步骤 1：读取同目录 vault.key；首次运行生成 256 位随机密钥并原子写入。该模式
    // 实现便携自动解锁，不依赖 Windows 凭据管理器或用户配置目录。
    if (is_unlocked()) {
        return StorageStatus::Ok();
    }

    const std::filesystem::path key_path = impl_->path.parent_path() / L"vault.key";
    auto portable_key = ReadPortableKey(key_path);
    if (!portable_key && portable_key.status().code != StorageErrc::not_found) {
        return portable_key.status();
    }

    if (!portable_key) {
        std::array<std::byte, kPortableKeyBytes> generated{};
        randombytes_buf(generated.data(), generated.size());
        if (auto status = WritePortableKey(key_path, generated); !status) {
            sodium_memzero(generated.data(), generated.size());
            return status;
        }
        portable_key = StorageResult<decltype(generated)>::Success(std::move(generated));
    }

    // 步骤 2：已有 vault.db 用密钥校验 verifier，新库则用同一密钥初始化。无论结果
    // 如何，栈上的 portable_key 都在返回前清零，只保留锁页 master_key 副本。
    bool initialized = false;
    {
        std::scoped_lock lock(impl_->mutex);
        initialized = impl_->initialized;
    }
    auto& bytes = portable_key.value();
    StorageStatus status = initialized ? Unlock(bytes) : Initialize(bytes);
    sodium_memzero(bytes.data(), bytes.size());
    return status;
#endif
}

void Vault::Lock() noexcept {
#if defined(SLINK_USE_LIBSODIUM)
    if (impl_) {
        std::scoped_lock lock(impl_->mutex);
        impl_->master_key = {};
    }
#endif
}

bool Vault::is_unlocked() const noexcept {
#if defined(SLINK_USE_LIBSODIUM)
    if (!impl_) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    return !impl_->master_key.empty();
#else
    return false;
#endif
}

StorageStatus Vault::Put(std::string key, std::span<const std::byte> secret) {
#if !defined(SLINK_USE_LIBSODIUM)
    (void)key;
    (void)secret;
    return UnsupportedStatus();
#else
    if (auto status = ValidateVaultKey(key); !status) {
        return status;
    }
    if (secret.size() > kMaxSecretBytes) {
        return StorageStatus::Error(StorageErrc::invalid_argument, L"Secret is too large");
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->master_key.empty()) {
        return StorageStatus::Error(StorageErrc::locked, L"Vault is locked");
    }
    const auto aad = std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(key.data()), key.size());
    auto encrypted = Encrypt(secret, aad, impl_->master_key.bytes());
    if (!encrypted) {
        return encrypted.status();
    }
    auto old = impl_->records.extract(key);
    impl_->records.emplace(key, std::move(encrypted).value());
    auto status = SaveVault(*impl_);
    if (!status) {
        impl_->records.erase(key);
        if (!old.empty()) {
            impl_->records.insert(std::move(old));
        }
    }
    return status;
#endif
}

StorageResult<SecureBuffer> Vault::Get(const std::string& key) const {
#if !defined(SLINK_USE_LIBSODIUM)
    (void)key;
    return StorageResult<SecureBuffer>::Failure(UnsupportedStatus());
#else
    if (auto status = ValidateVaultKey(key); !status) {
        return StorageResult<SecureBuffer>::Failure(std::move(status));
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->master_key.empty()) {
        return StorageResult<SecureBuffer>::Failure(
            StorageStatus::Error(StorageErrc::locked, L"Vault is locked"));
    }
    const auto found = impl_->records.find(key);
    if (found == impl_->records.end()) {
        return StorageResult<SecureBuffer>::Failure(
            StorageStatus::Error(StorageErrc::not_found, L"Secret does not exist"));
    }
    const auto aad = std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(key.data()), key.size());
    return Decrypt(found->second, aad, impl_->master_key.bytes(), StorageErrc::corrupt_data);
#endif
}

StorageStatus Vault::Remove(const std::string& key) {
#if !defined(SLINK_USE_LIBSODIUM)
    (void)key;
    return UnsupportedStatus();
#else
    if (auto status = ValidateVaultKey(key); !status) {
        return status;
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->master_key.empty()) {
        return StorageStatus::Error(StorageErrc::locked, L"Vault is locked");
    }
    auto old = impl_->records.extract(key);
    if (old.empty()) {
        return StorageStatus::Error(StorageErrc::not_found, L"Secret does not exist");
    }
    auto status = SaveVault(*impl_);
    if (!status) {
        impl_->records.insert(std::move(old));
    }
    return status;
#endif
}

} // namespace SLink::storage
