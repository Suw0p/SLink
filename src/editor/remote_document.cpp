// 远程文档缓存、编码/EOL 检测、二进制识别、修订冲突检查、崩溃恢复和原子保存实现。
#include "editor/remote_document.hpp"
#include "editor/gb2312_codec.hpp"

#ifndef _WIN32
#error RemoteDocument is a Windows-only implementation
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <unordered_set>

namespace SLink::editor {

// 同一缓存目录中的文档身份只能由一个打开的文档持有，防止另存为或恢复
// 创建第二个拥有者，导致某个标签关闭时删掉另一个标签的缓存。
struct DocumentCacheRegistry {
    std::mutex mutex;
    std::unordered_set<std::string> owners;
};

namespace {

std::shared_ptr<DocumentCacheRegistry> RegistryFor(const std::filesystem::path& directory) {
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::weak_ptr<DocumentCacheRegistry>> registries;
    std::scoped_lock lock(mutex);
    auto& weak = registries[directory];
    auto registry = weak.lock();
    if (!registry) {
        registry = std::make_shared<DocumentCacheRegistry>();
        weak = registry;
    }
    return registry;
}

struct DocumentCacheLease {
    std::shared_ptr<DocumentCacheRegistry> registry;
    std::string key;

    static std::unique_ptr<DocumentCacheLease>
    Acquire(const std::shared_ptr<DocumentCacheRegistry>& registry, std::string key) {
        auto lease = std::make_unique<DocumentCacheLease>();
        std::scoped_lock lock(registry->mutex);
        if (!registry->owners.insert(key).second)
            return {};
        lease->registry = registry;
        lease->key = std::move(key);
        return lease;
    }
    ~DocumentCacheLease() {
        if (registry) {
            std::scoped_lock lock(registry->mutex);
            registry->owners.erase(key);
        }
    }
};

using transfer::FileKind;
using transfer::FileStat;
using transfer::FileUri;
using transfer::IFileEndpoint;

constexpr std::array<std::byte, 8> kMetadataMagic{std::byte{'S'},
                                                  std::byte{'W'},
                                                  std::byte{'E'},
                                                  std::byte{'D'},
                                                  std::byte{'M'},
                                                  std::byte{'E'},
                                                  std::byte{'T'},
                                                  std::byte{'A'}};
constexpr std::uint32_t kMetadataVersion = 3;
constexpr std::size_t kMaxMetadataBytes = 64 * 1024;
using ContentHash = std::array<std::byte, 32>;

struct CacheMetadata {
    bool dirty{};
    TextEncoding encoding{TextEncoding::Utf8};
    EolStyle eol{EolStyle::None};
    RemoteRevision revision;
    std::uint64_t content_size{};
    ContentHash content_sha256{};
    std::string uri;
};

EditorStatus IoError(std::string message) {
    return EditorStatus::Error(EditorErrc::IoError, std::move(message));
}

RemoteRevision ToRevision(const FileStat& stat, const ContentHash& content_sha256 = {}) {
    return {stat.size, stat.modified, stat.permissions, content_sha256};
}

bool SameRevision(const RemoteRevision& revision,
                  const FileStat& stat,
                  const ContentHash& content_sha256) {
    return revision.size == stat.size && revision.modified == stat.modified &&
           revision.content_sha256 == content_sha256;
}

// 内存正文和端点流共用同一 SHA-256 实现，避免两种校验路径出现不同的摘要语义。
template <typename Feed> ContentHash HashWithFeed(Feed&& feed) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("Cannot initialize Windows SHA-256 provider");
    }
    struct AlgorithmGuard {
        BCRYPT_ALG_HANDLE value;
        ~AlgorithmGuard() {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    } algorithm_guard{algorithm};

    DWORD object_size{};
    DWORD copied{};
    if (BCryptGetProperty(algorithm,
                          BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size),
                          &copied,
                          0) < 0 ||
        copied != sizeof(object_size)) {
        throw std::runtime_error("Cannot query Windows SHA-256 provider");
    }
    std::vector<std::byte> object(object_size);
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(algorithm,
                         &hash,
                         reinterpret_cast<PUCHAR>(object.data()),
                         static_cast<ULONG>(object.size()),
                         nullptr,
                         0,
                         0) < 0) {
        throw std::runtime_error("Cannot create Windows SHA-256 hash");
    }
    struct HashGuard {
        BCRYPT_HASH_HANDLE value;
        ~HashGuard() {
            BCryptDestroyHash(value);
        }
    } hash_guard{hash};

    const auto update = [&](std::span<const std::byte> bytes) {
        std::size_t offset{};
        while (offset < bytes.size()) {
            const auto count = static_cast<ULONG>(
                (std::min)(bytes.size() - offset,
                           static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())));
            if (BCryptHashData(
                    hash,
                    reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data() + offset)),
                    count,
                    0) < 0) {
                throw std::runtime_error("Cannot update Windows SHA-256 hash");
            }
            offset += count;
        }
    };
    feed(update);
    ContentHash result{};
    if (BCryptFinishHash(
            hash, reinterpret_cast<PUCHAR>(result.data()), static_cast<ULONG>(result.size()), 0) <
        0) {
        throw std::runtime_error("Cannot finish Windows SHA-256 hash");
    }
    return result;
}

ContentHash Sha256(std::span<const std::byte> bytes) {
    return HashWithFeed([bytes](const auto& update) {
        update(bytes);
    });
}

ContentHash Sha256(std::string_view text) {
    return Sha256({reinterpret_cast<const std::byte*>(text.data()), text.size()});
}

std::uint64_t Fnv1a(std::span<const std::byte> bytes,
                    std::uint64_t seed = 14695981039346656037ULL) {
    auto hash = seed;
    for (const auto byte : bytes) {
        hash ^= std::to_integer<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t Fnv1a(std::string_view text, std::uint64_t seed = 14695981039346656037ULL) {
    return Fnv1a({reinterpret_cast<const std::byte*>(text.data()), text.size()}, seed);
}

std::string CacheStem(const FileUri& uri) {
    const auto value = uri.to_string();
    const auto first = Fnv1a(value);
    const auto second = Fnv1a(value, 1099511628211ULL);
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << first << std::setw(16) << second;
    return output.str();
}

template <typename UInt> void AppendUnsigned(std::vector<std::byte>& output, UInt value) {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        output.push_back(static_cast<std::byte>((value >> (index * 8)) & 0xff));
    }
}

template <typename UInt>
bool ReadUnsigned(std::span<const std::byte> input, std::size_t& cursor, UInt& value) {
    static_assert(std::is_unsigned_v<UInt>);
    if (cursor > input.size() || input.size() - cursor < sizeof(UInt)) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        value |= static_cast<UInt>(std::to_integer<unsigned char>(input[cursor + index]))
                 << (index * 8);
    }
    cursor += sizeof(UInt);
    return true;
}

void AppendHash(std::vector<std::byte>& output, const ContentHash& hash) {
    output.insert(output.end(), hash.begin(), hash.end());
}

bool ReadHash(std::span<const std::byte> input, std::size_t& cursor, ContentHash& hash) {
    if (cursor > input.size() || input.size() - cursor < hash.size()) {
        return false;
    }
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(cursor), hash.size(), hash.begin());
    cursor += hash.size();
    return true;
}

std::vector<std::byte> SerializeMetadata(const CacheMetadata& metadata) {
    std::vector<std::byte> output;
    output.insert(output.end(), kMetadataMagic.begin(), kMetadataMagic.end());
    AppendUnsigned<std::uint32_t>(output, kMetadataVersion);
    AppendUnsigned<std::uint32_t>(output, metadata.dirty ? 1U : 0U);
    AppendUnsigned<std::uint32_t>(output, static_cast<std::uint32_t>(metadata.encoding));
    AppendUnsigned<std::uint32_t>(output, static_cast<std::uint32_t>(metadata.eol));
    AppendUnsigned<std::uint64_t>(output, metadata.revision.size);
    const auto modified_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 metadata.revision.modified.time_since_epoch())
                                 .count();
    AppendUnsigned<std::uint64_t>(
        output, std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(modified_ns)));
    AppendUnsigned<std::uint32_t>(output, metadata.revision.permissions);
    AppendHash(output, metadata.revision.content_sha256);
    AppendUnsigned<std::uint64_t>(output, metadata.content_size);
    AppendHash(output, metadata.content_sha256);
    AppendUnsigned<std::uint32_t>(output, static_cast<std::uint32_t>(metadata.uri.size()));
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(metadata.uri.data()),
                  reinterpret_cast<const std::byte*>(metadata.uri.data() + metadata.uri.size()));
    AppendUnsigned<std::uint64_t>(output, Fnv1a(output));
    return output;
}

EditorResult<CacheMetadata> DeserializeMetadata(std::span<const std::byte> bytes) {
    constexpr std::size_t minimum = kMetadataMagic.size() + 4 * 4 + 8 + 8 + 4 + 32 + 8 + 32 + 4 + 8;
    if (bytes.size() < minimum ||
        !std::equal(kMetadataMagic.begin(), kMetadataMagic.end(), bytes.begin())) {
        return EditorResult<CacheMetadata>::Failure(
            EditorStatus::Error(EditorErrc::CorruptCache, "Invalid editor cache metadata"));
    }
    const auto payload = bytes.first(bytes.size() - sizeof(std::uint64_t));
    std::size_t checksum_cursor = payload.size();
    std::uint64_t checksum = 0;
    if (!ReadUnsigned(bytes, checksum_cursor, checksum) || checksum != Fnv1a(payload)) {
        return EditorResult<CacheMetadata>::Failure(
            EditorStatus::Error(EditorErrc::CorruptCache, "Editor cache checksum mismatch"));
    }

    std::size_t cursor = kMetadataMagic.size();
    std::uint32_t version = 0;
    std::uint32_t dirty = 0;
    std::uint32_t encoding = 0;
    std::uint32_t eol = 0;
    std::uint64_t modified_bits = 0;
    std::uint32_t uri_size = 0;
    CacheMetadata metadata;
    if (!ReadUnsigned(payload, cursor, version) || version != kMetadataVersion ||
        !ReadUnsigned(payload, cursor, dirty) || dirty > 1 ||
        !ReadUnsigned(payload, cursor, encoding) ||
        encoding > static_cast<std::uint32_t>(TextEncoding::Gb2312) ||
        !ReadUnsigned(payload, cursor, eol) || eol > static_cast<std::uint32_t>(EolStyle::Mixed) ||
        !ReadUnsigned(payload, cursor, metadata.revision.size) ||
        !ReadUnsigned(payload, cursor, modified_bits) ||
        !ReadUnsigned(payload, cursor, metadata.revision.permissions) ||
        !ReadHash(payload, cursor, metadata.revision.content_sha256) ||
        !ReadUnsigned(payload, cursor, metadata.content_size) ||
        !ReadHash(payload, cursor, metadata.content_sha256) ||
        !ReadUnsigned(payload, cursor, uri_size) || uri_size == 0 || uri_size > kMaxMetadataBytes ||
        payload.size() - cursor != uri_size) {
        return EditorResult<CacheMetadata>::Failure(
            EditorStatus::Error(EditorErrc::CorruptCache, "Malformed editor cache metadata"));
    }
    metadata.dirty = dirty != 0;
    metadata.encoding = static_cast<TextEncoding>(encoding);
    metadata.eol = static_cast<EolStyle>(eol);
    const auto modified_ns = std::bit_cast<std::int64_t>(modified_bits);
    metadata.revision.modified = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds(modified_ns)));
    metadata.uri.assign(reinterpret_cast<const char*>(payload.data() + cursor), uri_size);
    return EditorResult<CacheMetadata>::Success(std::move(metadata));
}

EditorResult<std::vector<std::byte>> ReadLocalFile(const std::filesystem::path& path,
                                                   std::size_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum) {
        return EditorResult<std::vector<std::byte>>::Failure(EditorStatus::Error(
            error ? EditorErrc::IoError : EditorErrc::CorruptCache,
            error ? "Cannot inspect editor cache" : "Editor cache file exceeds its limit"));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return EditorResult<std::vector<std::byte>>::Failure(IoError("Cannot open editor cache"));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        return EditorResult<std::vector<std::byte>>::Failure(IoError("Cannot read editor cache"));
    }
    return EditorResult<std::vector<std::byte>>::Success(std::move(bytes));
}

EditorStatus WriteLocalAtomically(const std::filesystem::path& path,
                                  std::span<const std::byte> bytes) {
    auto pending = path;
    pending += L".pending";
    {
        std::ofstream output(pending, std::ios::binary | std::ios::trunc);
        if (!output) {
            return IoError("Cannot create editor cache");
        }
        if (!bytes.empty()) {
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
        output.flush();
        if (!output) {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(pending, ignored);
            return IoError("Cannot write editor cache");
        }
    }
    if (!MoveFileExW(
            pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(pending, ignored);
        return IoError("Cannot replace editor cache");
    }
    return EditorStatus::Ok();
}

EditorStatus RemoveCacheFile(const std::filesystem::path& path) {
    if (path.empty()) {
        return EditorStatus::Ok();
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    return error ? IoError("Cannot remove editor cache") : EditorStatus::Ok();
}

std::filesystem::path GenerationContentPath(const std::filesystem::path& metadata_path,
                                            const ContentHash& hash) {
    constexpr char hex[] = "0123456789abcdef";
    std::string suffix;
    suffix.reserve(hash.size() * 2);
    for (const auto value : hash) {
        const auto byte = std::to_integer<unsigned>(value);
        suffix.push_back(hex[byte >> 4]);
        suffix.push_back(hex[byte & 15]);
    }
    return metadata_path.parent_path() /
           (metadata_path.stem().string() + "." + suffix + ".content");
}

std::filesystem::path CommittedContentPath(const std::filesystem::path& metadata_path,
                                           const CacheMetadata& metadata) {
    auto path = GenerationContentPath(metadata_path, metadata.content_sha256);
    if (std::filesystem::exists(path))
        return path;
    // 已有的未保存缓存不能丢弃；原布局仍经过内容大小及 SHA-256 验证后读取。
    auto original = metadata_path;
    original.replace_extension(L".content");
    return std::filesystem::exists(original) ? original : path;
}

// 保存校验只需要摘要，固定 64 KiB 缓冲，不再累积整份远端文件。
// 保留完整读取与大小上限：不能用元数据或局部抽样替代冲突保护。
EditorResult<ContentHash>
HashEndpoint(IFileEndpoint& endpoint, const FileUri& uri, std::uint64_t maximum) {
    struct TooLarge {};
    try {
        auto stream = endpoint.open_read(uri);
        std::array<std::byte, 64 * 1024> buffer{};
        std::uint64_t total{};
        auto hash = HashWithFeed([&](const auto& update) {
            for (;;) {
                const auto count = stream->read(buffer);
                if (count == 0)
                    break;
                if (count > buffer.size())
                    throw std::runtime_error("Endpoint returned an invalid read size");
                if (count > maximum - total)
                    throw TooLarge{};
                total += count;
                update(std::span<const std::byte>(buffer.data(), count));
            }
        });
        return EditorResult<ContentHash>::Success(hash);
    } catch (const TooLarge&) {
        return EditorResult<ContentHash>::Failure(
            EditorStatus::Error(EditorErrc::Unsupported, "Remote file exceeds editor limit"));
    } catch (const std::exception& error) {
        return EditorResult<ContentHash>::Failure(IoError(error.what()));
    }
}

EditorResult<std::vector<std::byte>>
ReadEndpoint(IFileEndpoint& endpoint, const FileUri& uri, std::uint64_t maximum) {
    try {
        auto stream = endpoint.open_read(uri);
        std::vector<std::byte> bytes;
        bytes.reserve(static_cast<std::size_t>((std::min<std::uint64_t>)(maximum, 256 * 1024)));
        std::array<std::byte, 64 * 1024> chunk{};
        for (;;) {
            const auto read = stream->read(chunk);
            if (read == 0) {
                break;
            }
            if (read > chunk.size() || bytes.size() > maximum - read) {
                return EditorResult<std::vector<std::byte>>::Failure(EditorStatus::Error(
                    EditorErrc::Unsupported, "Remote file exceeds editor limit"));
            }
            bytes.insert(
                bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read));
        }
        return EditorResult<std::vector<std::byte>>::Success(std::move(bytes));
    } catch (const std::exception& error) {
        return EditorResult<std::vector<std::byte>>::Failure(IoError(error.what()));
    }
}

EditorResult<std::vector<std::byte>>
ReadEndpointProbe(IFileEndpoint& endpoint, const FileUri& uri, std::size_t maximum) {
    try {
        auto stream = endpoint.open_read(uri);
        std::vector<std::byte> bytes(maximum);
        const auto read = stream->read(bytes);
        if (read > bytes.size()) {
            return EditorResult<std::vector<std::byte>>::Failure(
                IoError("Endpoint returned an invalid byte count"));
        }
        bytes.resize(read);
        return EditorResult<std::vector<std::byte>>::Success(std::move(bytes));
    } catch (const std::exception& error) {
        return EditorResult<std::vector<std::byte>>::Failure(IoError(error.what()));
    }
}

bool LooksBinary(std::span<const std::byte> bytes) {
    std::size_t suspicious = 0;
    for (const auto item : bytes) {
        const auto value = std::to_integer<unsigned char>(item);
        if (value == 0) {
            return true;
        }
        if (value < 0x20 && value != '\t' && value != '\n' && value != '\r' && value != '\f' &&
            value != '\b') {
            ++suspicious;
        }
    }
    return !bytes.empty() && suspicious * 100 > bytes.size();
}

bool IsValidUtf8(std::span<const std::byte> bytes) {
    std::size_t index = 0;
    while (index < bytes.size()) {
        const auto lead = std::to_integer<unsigned char>(bytes[index]);
        if (lead <= 0x7f) {
            ++index;
            continue;
        }
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xe0) == 0xc0) {
            continuation = 1;
            codepoint = lead & 0x1f;
            minimum = 0x80;
        } else if ((lead & 0xf0) == 0xe0) {
            continuation = 2;
            codepoint = lead & 0x0f;
            minimum = 0x800;
        } else if ((lead & 0xf8) == 0xf0) {
            continuation = 3;
            codepoint = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (bytes.size() - index <= continuation) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const auto value = std::to_integer<unsigned char>(bytes[index + offset]);
            if ((value & 0xc0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (value & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
        index += continuation + 1;
    }
    return true;
}

struct TextInfo {
    TextEncoding encoding{TextEncoding::Unknown8Bit};
    EolStyle eol{EolStyle::None};
    std::string text;
};

EolStyle DetectEol(std::string_view text) {
    std::size_t lf = 0;
    std::size_t crlf = 0;
    std::size_t cr = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                ++crlf;
                ++index;
            } else {
                ++cr;
            }
        } else if (text[index] == '\n') {
            ++lf;
        }
    }
    const int styles = (lf != 0) + (crlf != 0) + (cr != 0);
    if (styles == 0)
        return EolStyle::None;
    if (styles > 1)
        return EolStyle::Mixed;
    if (crlf != 0)
        return EolStyle::CrLf;
    if (cr != 0)
        return EolStyle::Cr;
    return EolStyle::Lf;
}

TextInfo InspectText(std::span<const std::byte> bytes) {
    constexpr std::array<std::byte, 3> bom{std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}};
    const bool has_bom =
        bytes.size() >= bom.size() && std::equal(bom.begin(), bom.end(), bytes.begin());
    if (has_bom) {
        bytes = bytes.subspan(bom.size());
    }
    TextInfo result;
    result.encoding = has_bom
                          ? TextEncoding::Utf8Bom
                          : (IsValidUtf8(bytes) ? TextEncoding::Utf8 : TextEncoding::Unknown8Bit);
    result.text.assign(reinterpret_cast<const char*>(bytes.data()),
                       reinterpret_cast<const char*>(bytes.data() + bytes.size()));
    result.eol = DetectEol(result.text);
    return result;
}

EditorResult<std::vector<std::byte>> Materialize(const std::string& text, TextEncoding encoding) {
    const auto too_large = [] {
        return EditorResult<std::vector<std::byte>>::Failure(
            EditorStatus::Error(EditorErrc::Unsupported, "Encoded file exceeds 20 MB"));
    };
    if (encoding != TextEncoding::Gb2312 &&
        text.size() + (encoding == TextEncoding::Utf8Bom ? 3ULL : 0ULL) >
            RemoteEditorService::kEditableSizeLimit)
        return too_large();
    std::vector<std::byte> bytes;
    if (encoding == TextEncoding::Utf8Bom) {
        bytes.insert(bytes.end(), {std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}});
    }
    if (encoding == TextEncoding::Gb2312) {
        auto encoded = EncodeGb2312(text);
        if (!encoded) {
            return EditorResult<std::vector<std::byte>>::Failure(EditorStatus::Error(
                EditorErrc::Unsupported, "Text contains characters not representable in GB2312"));
        }
        if (encoded->size() > RemoteEditorService::kEditableSizeLimit)
            return too_large();
        bytes.insert(bytes.end(),
                     reinterpret_cast<const std::byte*>(encoded->data()),
                     reinterpret_cast<const std::byte*>(encoded->data() + encoded->size()));
        return EditorResult<std::vector<std::byte>>::Success(std::move(bytes));
    }
    bytes.insert(bytes.end(),
                 reinterpret_cast<const std::byte*>(text.data()),
                 reinterpret_cast<const std::byte*>(text.data() + text.size()));
    return EditorResult<std::vector<std::byte>>::Success(std::move(bytes));
}

EditorResult<ConflictSnapshot> CaptureConflict(IFileEndpoint& endpoint,
                                               const FileUri& uri,
                                               std::string local_text,
                                               const std::optional<FileStat>& remote_stat,
                                               const TextEncoding preferred_encoding) {
    ConflictSnapshot conflict;
    conflict.local_text = std::move(local_text);
    if (!remote_stat) {
        conflict.remote_missing = true;
        return EditorResult<ConflictSnapshot>::Success(std::move(conflict));
    }

    conflict.remote_revision = ToRevision(*remote_stat);
    conflict.remote_too_large = remote_stat->size > RemoteEditorService::kEditableSizeLimit;
    if (conflict.remote_too_large) {
        return EditorResult<ConflictSnapshot>::Success(std::move(conflict));
    }

    auto bytes = ReadEndpoint(endpoint, uri, RemoteEditorService::kEditableSizeLimit);
    if (!bytes) {
        return EditorResult<ConflictSnapshot>::Failure(bytes.status());
    }
    conflict.remote_revision = ToRevision(*remote_stat, Sha256(bytes.value()));
    conflict.remote_is_binary = LooksBinary(bytes.value());
    if (!conflict.remote_is_binary) {
        auto info = InspectText(bytes.value());
        if (info.encoding == TextEncoding::Unknown8Bit &&
            preferred_encoding == TextEncoding::Gb2312) {
            if (auto decoded = DecodeGb2312(info.text)) {
                info.text = std::move(*decoded);
                info.encoding = TextEncoding::Gb2312;
            }
        }
        conflict.remote_text = std::move(info.text);
        conflict.remote_encoding = info.encoding;
        conflict.remote_eol = info.eol;
    }
    return EditorResult<ConflictSnapshot>::Success(std::move(conflict));
}

bool IsDirectCacheChild(const std::filesystem::path& cache_dir, const std::filesystem::path& path) {
    return path.is_absolute() &&
           path.lexically_normal().parent_path() == cache_dir.lexically_normal();
}

std::string TemporaryRemoteName(const FileUri& uri) {
    static std::atomic<std::uint64_t> sequence{0};
    return "." + uri.filename() + ".SLink-" + std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
}

} // namespace

// 文档对象同时维护远端基准修订和本地恢复缓存身份。dirty=true 时每次 UpdateText 都先
// 原子更新缓存，只有缓存成功才提交内存文本，确保异常退出后恢复内容不落后于 UI 状态。
struct RemoteDocument::Impl {
    std::unique_ptr<DocumentCacheLease> cache_lease;
    std::shared_ptr<IFileEndpoint> endpoint;
    FileUri uri;
    DocumentMode mode{DocumentMode::Editable};
    TextEncoding encoding{TextEncoding::Utf8};
    EolStyle eol{EolStyle::None};
    std::string text;
    RemoteRevision base_revision;
    bool dirty{};
    bool closed{};
    bool checkpoint_needed{true};
    std::filesystem::path cache_path;
    std::filesystem::path metadata_path;
    std::optional<CacheMetadata> baseline;

    std::filesystem::path baseline_metadata_path() const {
        auto path = metadata_path;
        path.replace_extension(L".baseline");
        return path;
    }

    void load_baseline() {
        auto bytes = ReadLocalFile(baseline_metadata_path(), kMaxMetadataBytes);
        if (!bytes)
            return;
        auto saved = DeserializeMetadata(bytes.value());
        if (saved && !saved.value().dirty && saved.value().uri == uri.to_string())
            baseline = std::move(saved).value();
    }

    EditorStatus clear_baseline() {
        if (!baseline)
            load_baseline();
        if (baseline) {
            const auto original = CommittedContentPath(metadata_path, *baseline);
            if (original != cache_path) {
                if (auto status = RemoveCacheFile(original); !status)
                    return status;
            }
        }
        if (auto status = RemoveCacheFile(baseline_metadata_path()); !status)
            return status;
        baseline.reset();
        return EditorStatus::Ok();
    }

    EditorStatus retain_baseline() {
        if (baseline && baseline->revision.content_sha256 != base_revision.content_sha256) {
            if (auto status = clear_baseline(); !status)
                return status;
        }
        if (baseline || dirty)
            return EditorStatus::Ok();
        auto original = metadata();
        if (auto status =
                WriteLocalAtomically(baseline_metadata_path(), SerializeMetadata(original));
            !status)
            return status;
        // 仅保留已有不可变内容代次，不为每个文件长期复制一份内存正文。
        baseline = std::move(original);
        return EditorStatus::Ok();
    }

    bool matches_base(std::string_view current, TextEncoding format) const {
        if (format == TextEncoding::Gb2312) {
            auto encoded = EncodeGb2312(current);
            return encoded && encoded->size() == base_revision.size &&
                   Sha256(*encoded) == base_revision.content_sha256;
        }
        const bool bom = format == TextEncoding::Utf8Bom;
        if (current.size() + (bom ? 3U : 0U) != base_revision.size)
            return false;
        const auto hash = HashWithFeed([&](const auto& update) {
            constexpr std::array prefix{std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}};
            if (bom)
                update(prefix);
            update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(current.data()),
                                              current.size()));
        });
        return hash == base_revision.content_sha256;
    }

    CacheMetadata metadata() const {
        return {dirty,
                encoding,
                eol,
                base_revision,
                static_cast<std::uint64_t>(text.size()),
                Sha256(text),
                uri.to_string()};
    }

    EditorStatus checkpoint() {
        if (!checkpoint_needed)
            return EditorStatus::Ok();
        if (cache_path.empty() || metadata_path.empty()) {
            return EditorStatus::Ok();
        }
        const auto content = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(text.data()), text.size());
        // 内容使用不可变的、按哈希命名的代次文件。只有 .meta 的原子替换是提交点；
        // 新内容写完而元数据提交失败时，旧 .meta 仍能找到完整的旧代次。
        const auto next_metadata = metadata();
        const auto next_content =
            GenerationContentPath(metadata_path, next_metadata.content_sha256);
        std::error_code content_error;
        const bool content_already_committed =
            next_content == cache_path &&
            std::filesystem::is_regular_file(next_content, content_error) && !content_error;
        if (!content_already_committed) {
            if (auto status = WriteLocalAtomically(next_content, content); !status)
                return status;
        }
        const auto metadata_bytes = SerializeMetadata(next_metadata);
        if (auto status = WriteLocalAtomically(metadata_path, metadata_bytes); !status) {
            if (next_content != cache_path)
                (void)RemoveCacheFile(next_content);
            return status;
        }
        const auto previous = std::exchange(cache_path, next_content);
        if (previous != cache_path &&
            (!baseline || previous != CommittedContentPath(metadata_path, *baseline)))
            (void)RemoveCacheFile(previous);
        checkpoint_needed = false;
        if (!dirty)
            (void)clear_baseline();
        return EditorStatus::Ok();
    }
};

RemoteDocument::RemoteDocument(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RemoteDocument::~RemoteDocument() {
    Close();
}
RemoteDocument::RemoteDocument(RemoteDocument&&) noexcept = default;
RemoteDocument& RemoteDocument::operator=(RemoteDocument&& other) noexcept {
    if (this != &other) {
        Close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

const FileUri& RemoteDocument::uri() const noexcept {
    return impl_->uri;
}
DocumentMode RemoteDocument::mode() const noexcept {
    return impl_->mode;
}
TextEncoding RemoteDocument::encoding() const noexcept {
    return impl_->encoding;
}
EolStyle RemoteDocument::eol() const noexcept {
    return impl_->eol;
}
const std::string& RemoteDocument::text() const noexcept {
    return impl_->text;
}
bool RemoteDocument::dirty() const noexcept {
    return impl_->dirty;
}
const std::filesystem::path& RemoteDocument::cache_path() const noexcept {
    return impl_->cache_path;
}

EditorResult<OriginalDocumentText> RemoteDocument::OriginalText() const {
    if (impl_->closed || impl_->mode != DocumentMode::Editable)
        return EditorResult<OriginalDocumentText>::Failure(
            EditorStatus::Error(EditorErrc::Unsupported, "Document is not editable"));
    if (!impl_->dirty)
        return EditorResult<OriginalDocumentText>::Success({impl_->text, impl_->encoding});
    if (!impl_->baseline ||
        impl_->baseline->revision.content_sha256 != impl_->base_revision.content_sha256)
        return EditorResult<OriginalDocumentText>::Failure(
            EditorStatus::Error(EditorErrc::NotFound,
                                "This recovered draft has no original snapshot; it was not "
                                "compared with the current remote file"));
    auto bytes = ReadLocalFile(CommittedContentPath(impl_->metadata_path, *impl_->baseline),
                               RemoteEditorService::kInternalTextSizeLimit);
    if (!bytes || bytes.value().size() != impl_->baseline->content_size ||
        Sha256(bytes.value()) != impl_->baseline->content_sha256)
        return EditorResult<OriginalDocumentText>::Failure(EditorStatus::Error(
            EditorErrc::CorruptCache, "Original snapshot is missing or corrupt"));
    return EditorResult<OriginalDocumentText>::Success(
        {std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()),
         impl_->baseline->encoding});
}

EditorStatus RemoteDocument::RebindEndpoint(std::shared_ptr<IFileEndpoint> endpoint) {
    if (impl_->closed) {
        return EditorStatus::Error(EditorErrc::InvalidArgument, "Document is closed");
    }
    if (!endpoint || !endpoint->accepts(impl_->uri)) {
        return EditorStatus::Error(EditorErrc::InvalidArgument,
                                   "Replacement endpoint does not accept the document URI");
    }
    if (!endpoint->available()) {
        return EditorStatus::Error(EditorErrc::IoError, "Replacement endpoint is unavailable");
    }
    impl_->endpoint = std::move(endpoint);
    return EditorStatus::Ok();
}

EditorStatus RemoteDocument::UpdateText(std::string text,
                                        const std::optional<TextEncoding> encoding) {
    if (impl_->closed) {
        return EditorStatus::Error(EditorErrc::InvalidArgument, "Document is closed");
    }
    if (impl_->mode != DocumentMode::Editable) {
        return EditorStatus::Error(EditorErrc::Unsupported, "Document is read-only");
    }
    if (text.size() > RemoteEditorService::kInternalTextSizeLimit) {
        return EditorStatus::Error(EditorErrc::Unsupported, "Internal text exceeds 40 MB");
    }
    const auto next_encoding = encoding.value_or(impl_->encoding);
    std::size_t output_size = text.size();
    if (next_encoding == TextEncoding::Gb2312 && encoding.has_value()) {
        auto encoded = EncodeGb2312(text);
        if (!encoded) {
            return EditorStatus::Error(EditorErrc::Unsupported,
                                       "Text contains characters not representable in GB2312");
        }
        output_size = encoded->size();
    } else if (next_encoding != TextEncoding::Unknown8Bit &&
               !IsValidUtf8({reinterpret_cast<const std::byte*>(text.data()), text.size()})) {
        return EditorStatus::Error(EditorErrc::Unsupported, "Text is not valid UTF-8");
    }
    if (next_encoding == TextEncoding::Utf8Bom)
        output_size += 3;
    // 显式切换编码时校验实际输出大小；普通编辑优先保住恢复副本，真正保存时
    // Materialize 会再次严格限制输出文件大小，绝不因此绕过远端的 20 MB 限制。
    if (encoding.has_value() && output_size > RemoteEditorService::kEditableSizeLimit) {
        return EditorStatus::Error(EditorErrc::Unsupported, "Edited document exceeds 20 MB");
    }
    // 快照/当前正文已各占一份，回滚只需转移旧缓冲所有权，不再额外复制整份文件。
    if (!impl_->dirty && next_encoding == impl_->encoding && text == impl_->text)
        return impl_->checkpoint();
    const bool next_dirty = !impl_->matches_base(text, next_encoding);
    if (impl_->dirty && next_dirty && next_encoding == impl_->encoding && text == impl_->text)
        return impl_->checkpoint();
    if (next_dirty) {
        if (auto status = impl_->retain_baseline(); !status)
            return status;
    }
    auto previous = std::move(impl_->text);
    const auto previous_encoding = impl_->encoding;
    const auto previous_eol = impl_->eol;
    const bool previous_dirty = impl_->dirty;
    const bool previous_checkpoint_needed = impl_->checkpoint_needed;
    impl_->text = std::move(text);
    impl_->encoding = next_encoding;
    impl_->eol = DetectEol(impl_->text);
    impl_->dirty = next_dirty;
    impl_->checkpoint_needed = true;
    if (auto status = impl_->checkpoint(); !status) {
        impl_->text = std::move(previous);
        impl_->encoding = previous_encoding;
        impl_->eol = previous_eol;
        impl_->dirty = previous_dirty;
        impl_->checkpoint_needed = previous_checkpoint_needed;
        return status;
    }
    return EditorStatus::Ok();
}

EditorResult<SaveResult> RemoteDocument::Save(const SaveMode mode) {
    // 步骤 1：只允许未关闭、可编辑且 dirty 的文档进入远端 I/O；干净文档直接成功，
    // 避免无意义的 stat、下载和写回。
    if (impl_->closed) {
        return EditorResult<SaveResult>::Failure(
            EditorStatus::Error(EditorErrc::InvalidArgument, "Document is closed"));
    }
    if (impl_->mode != DocumentMode::Editable) {
        return EditorResult<SaveResult>::Failure(
            EditorStatus::Error(EditorErrc::Unsupported, "Document is read-only"));
    }
    if (!impl_->dirty) {
        return EditorResult<SaveResult>::Success({});
    }

    // 步骤 2：重新读取远端元数据。目标消失在普通保存模式下视为冲突而不是自动重建，
    // 用户明确选择覆盖后才允许继续。
    std::optional<FileStat> remote_stat;
    try {
        remote_stat = impl_->endpoint->stat(impl_->uri);
    } catch (const std::exception& stat_error) {
        try {
            if (impl_->endpoint->exists(impl_->uri)) {
                return EditorResult<SaveResult>::Failure(IoError(stat_error.what()));
            }
            if (mode == SaveMode::CheckForConflict) {
                auto conflict = CaptureConflict(
                    *impl_->endpoint, impl_->uri, impl_->text, std::nullopt, impl_->encoding);
                if (!conflict) {
                    return EditorResult<SaveResult>::Failure(conflict.status());
                }
                return EditorResult<SaveResult>::Success(
                    {SaveOutcome::Conflict, std::move(conflict).value()});
            }
        } catch (const std::exception& exists_error) {
            return EditorResult<SaveResult>::Failure(IoError(exists_error.what()));
        }
    }

    // 步骤 3：用“大小 + mtime + 实际内容 SHA-256”比较打开时基准。哈希可发现大小和
    // mtime 被保留的远端改写；不一致时抓取远端快照交给 UI 决策，不覆盖用户外部修改。
    if (mode == SaveMode::CheckForConflict && remote_stat) {
        bool same_revision = false;
        if (remote_stat->size <= RemoteEditorService::kEditableSizeLimit) {
            auto hash =
                HashEndpoint(*impl_->endpoint, impl_->uri, RemoteEditorService::kEditableSizeLimit);
            if (!hash) {
                return EditorResult<SaveResult>::Failure(hash.status());
            }
            same_revision = SameRevision(impl_->base_revision, *remote_stat, hash.value());
        }
        if (!same_revision) {
            auto conflict = CaptureConflict(
                *impl_->endpoint, impl_->uri, impl_->text, remote_stat, impl_->encoding);
            if (!conflict) {
                return EditorResult<SaveResult>::Failure(conflict.status());
            }
            return EditorResult<SaveResult>::Success(
                {SaveOutcome::Conflict, std::move(conflict).value()});
        }
    }

    // 不直接截断远端原文件：先在同目录完整写入临时文件，恢复权限后再原子重命名。
    // 写入或连接中断时仅清理临时文件，原文件仍保持可用。
    const auto temporary = impl_->uri.parent().joined(TemporaryRemoteName(impl_->uri));
    auto materialized = Materialize(impl_->text, impl_->encoding);
    if (!materialized) {
        return EditorResult<SaveResult>::Failure(materialized.status());
    }
    const auto& bytes = materialized.value();
    try {
        // 步骤 4：完整写临时文件并关闭流，恢复原权限后执行同目录原子替换；替换成功
        // 再读取新修订作为下一次保存基准。
        auto output = impl_->endpoint->open_write(temporary, 0, true);
        output->write(bytes);
        output->flush();
        output.reset();
        if (impl_->base_revision.permissions != 0) {
            impl_->endpoint->set_permissions(temporary, impl_->base_revision.permissions);
        }
        if (mode == SaveMode::CheckForConflict) {
            // 上传可能耗时数秒：初检通过不表示原文件在最终替换时仍未变化。
            // 仅丢弃本次临时文件，保留外部新版本与本地 dirty/恢复缓存。
            std::optional<FileStat> latest;
            if (impl_->endpoint->exists(impl_->uri))
                latest = impl_->endpoint->stat(impl_->uri);
            bool unchanged = false;
            if (latest && latest->kind == FileKind::Regular &&
                latest->size <= RemoteEditorService::kEditableSizeLimit) {
                auto current = HashEndpoint(
                    *impl_->endpoint, impl_->uri, RemoteEditorService::kEditableSizeLimit);
                if (!current)
                    throw std::runtime_error(current.status().message);
                unchanged = SameRevision(impl_->base_revision, *latest, current.value());
            }
            if (!unchanged) {
                auto conflict = CaptureConflict(
                    *impl_->endpoint, impl_->uri, impl_->text, latest, impl_->encoding);
                impl_->endpoint->remove(temporary, false);
                if (!conflict)
                    return EditorResult<SaveResult>::Failure(conflict.status());
                return EditorResult<SaveResult>::Success(
                    {SaveOutcome::Conflict, std::move(conflict).value()});
            }
        }
        impl_->endpoint->rename(temporary, impl_->uri, true);
        impl_->base_revision = ToRevision(impl_->endpoint->stat(impl_->uri), Sha256(bytes));
    } catch (const std::exception& error) {
        try {
            if (impl_->endpoint->exists(temporary)) {
                impl_->endpoint->remove(temporary, false);
            }
        } catch (...) {}
        return EditorResult<SaveResult>::Failure(IoError(error.what()));
    }
    impl_->dirty = false;
    // 步骤 5：远端成功后把 dirty=false 写入恢复元数据。即使该本地 checkpoint 失败，
    // 也不重复上传，只把缓存错误返回给 UI 处理。
    impl_->checkpoint_needed = true;
    if (auto status = impl_->checkpoint(); !status) {
        return EditorResult<SaveResult>::Failure(std::move(status));
    }
    return EditorResult<SaveResult>::Success({});
}

EditorResult<SaveResult> RemoteDocument::SaveAs(FileUri target, const bool replace_existing) {
    if (impl_->closed) {
        return EditorResult<SaveResult>::Failure(
            EditorStatus::Error(EditorErrc::InvalidArgument, "Document is closed"));
    }
    if (impl_->mode != DocumentMode::Editable) {
        return EditorResult<SaveResult>::Failure(
            EditorStatus::Error(EditorErrc::Unsupported, "Document is read-only"));
    }
    if (!impl_->endpoint->accepts(target)) {
        return EditorResult<SaveResult>::Failure(EditorStatus::Error(
            EditorErrc::InvalidArgument, "Save As target is not accepted by this endpoint"));
    }
    if (target == impl_->uri) {
        return Save(replace_existing ? SaveMode::OverwriteRemote : SaveMode::CheckForConflict);
    }

    const auto stem = CacheStem(target);
    auto target_lease = DocumentCacheLease::Acquire(impl_->cache_lease->registry, stem);
    if (!target_lease) {
        return EditorResult<SaveResult>::Failure(EditorStatus::Error(
            EditorErrc::InvalidArgument, "Save As target is already open in another document"));
    }
    const auto target_metadata = impl_->metadata_path.parent_path() / (stem + ".meta");
    if (std::filesystem::exists(target_metadata)) {
        auto bytes = ReadLocalFile(target_metadata, kMaxMetadataBytes);
        auto metadata = bytes ? DeserializeMetadata(bytes.value())
                              : EditorResult<CacheMetadata>::Failure(bytes.status());
        if (!metadata || metadata.value().dirty) {
            return EditorResult<SaveResult>::Failure(EditorStatus::Error(
                EditorErrc::InvalidArgument, "Save As target has recovery data; open it first"));
        }
    }

    try {
        if (impl_->endpoint->exists(target)) {
            const auto target_stat = impl_->endpoint->stat(target);
            if (target_stat.kind != FileKind::Regular) {
                return EditorResult<SaveResult>::Failure(EditorStatus::Error(
                    EditorErrc::InvalidArgument, "Save As target is not a regular file"));
            }
            if (!replace_existing) {
                auto conflict = CaptureConflict(
                    *impl_->endpoint, target, impl_->text, target_stat, impl_->encoding);
                if (!conflict) {
                    return EditorResult<SaveResult>::Failure(conflict.status());
                }
                return EditorResult<SaveResult>::Success(
                    {SaveOutcome::Conflict, std::move(conflict).value()});
            }
        }
    } catch (const std::exception& error) {
        return EditorResult<SaveResult>::Failure(IoError(error.what()));
    }

    const auto temporary = target.parent().joined(TemporaryRemoteName(target));
    auto materialized = Materialize(impl_->text, impl_->encoding);
    if (!materialized) {
        return EditorResult<SaveResult>::Failure(materialized.status());
    }
    const auto& bytes = materialized.value();
    FileStat saved_stat;
    try {
        auto output = impl_->endpoint->open_write(temporary, 0, true);
        output->write(bytes);
        output->flush();
        output.reset();
        if (impl_->base_revision.permissions != 0) {
            impl_->endpoint->set_permissions(temporary, impl_->base_revision.permissions);
        }
        impl_->endpoint->rename(temporary, target, replace_existing);
        saved_stat = impl_->endpoint->stat(target);
    } catch (const std::exception& error) {
        try {
            if (impl_->endpoint->exists(temporary)) {
                impl_->endpoint->remove(temporary, false);
            }
        } catch (...) {}
        return EditorResult<SaveResult>::Failure(IoError(error.what()));
    }

    const auto old_cache_path = impl_->cache_path;
    const auto old_metadata_path = impl_->metadata_path;
    const auto old_uri = impl_->uri;
    const auto old_revision = impl_->base_revision;
    const bool old_dirty = impl_->dirty;
    const bool old_checkpoint_needed = impl_->checkpoint_needed;
    auto old_baseline = std::exchange(impl_->baseline, std::nullopt);
    const auto cache_dir = old_cache_path.parent_path();
    impl_->uri = std::move(target);
    impl_->base_revision = ToRevision(saved_stat, Sha256(bytes));
    impl_->dirty = false;
    impl_->cache_path = cache_dir / (stem + ".content");
    impl_->metadata_path = cache_dir / (stem + ".meta");
    impl_->checkpoint_needed = true;
    if (auto status = impl_->checkpoint(); !status) {
        // 远端可能已写入，但本地身份只有在新缓存完整落盘后才提交。
        impl_->uri = old_uri;
        impl_->base_revision = old_revision;
        impl_->dirty = old_dirty;
        impl_->cache_path = old_cache_path;
        impl_->metadata_path = old_metadata_path;
        impl_->checkpoint_needed = old_checkpoint_needed;
        impl_->baseline = std::move(old_baseline);
        return EditorResult<SaveResult>::Failure(std::move(status));
    }
    impl_->cache_lease = std::move(target_lease);
    if (old_cache_path != impl_->cache_path) {
        RemoveCacheFile(old_cache_path);
    }
    if (old_metadata_path != impl_->metadata_path) {
        RemoveCacheFile(old_metadata_path);
        if (old_baseline)
            (void)RemoveCacheFile(CommittedContentPath(old_metadata_path, *old_baseline));
        auto old_baseline_path = old_metadata_path;
        old_baseline_path.replace_extension(L".baseline");
        (void)RemoveCacheFile(old_baseline_path);
    }
    return EditorResult<SaveResult>::Success({});
}

EditorStatus RemoteDocument::Close() {
    if (!impl_ || impl_->closed) {
        return EditorStatus::Ok();
    }
    if (impl_->dirty) {
        // Ensure the most recent edit remains recoverable.
        if (auto status = impl_->checkpoint(); !status) {
            return status;
        }
        impl_->closed = true;
        impl_->cache_lease.reset();
        return EditorStatus::Ok();
    }
    if (auto status = impl_->clear_baseline(); !status)
        return status;
    const auto content_status = RemoveCacheFile(impl_->cache_path);
    const auto metadata_status = RemoveCacheFile(impl_->metadata_path);
    if (!content_status)
        return content_status;
    if (!metadata_status)
        return metadata_status;
    impl_->closed = true;
    impl_->cache_lease.reset();
    return EditorStatus::Ok();
}

EditorStatus RemoteDocument::DiscardChanges() {
    if (!impl_ || impl_->closed) {
        return EditorStatus::Ok();
    }
    const bool was_dirty = impl_->dirty;
    impl_->dirty = false;
    const auto status = Close();
    if (!status) {
        impl_->dirty = was_dirty;
        if (was_dirty) {
            // 删除缓存只完成一半时，尽量恢复可供崩溃恢复的原始修改。
            impl_->checkpoint_needed = true;
            (void)impl_->checkpoint();
        }
    }
    return status;
}

RemoteEditorService::RemoteEditorService(const storage::AppPaths& paths)
    : cache_dir_(paths.editor_cache_dir().lexically_normal()),
      cache_registry_(RegistryFor(cache_dir_)) {}

EditorResult<std::unique_ptr<RemoteDocument>>
RemoteEditorService::Open(std::shared_ptr<IFileEndpoint> endpoint, FileUri uri) const {
    if (!endpoint || !endpoint->available() || !endpoint->accepts(uri)) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(
            EditorStatus::Error(EditorErrc::InvalidArgument, "Endpoint cannot open this URI"));
    }

    // 正常关闭未保存标签后再次打开，直接恢复本地修改；不覆盖服务器文件。
    // 在读取远端前检查，远端文件被删除时仍能取回自己的未保存内容。
    const auto stem = CacheStem(uri);
    const auto metadata_path = cache_dir_ / (stem + ".meta");
    if (std::filesystem::exists(metadata_path)) {
        auto bytes = ReadLocalFile(metadata_path, kMaxMetadataBytes);
        if (bytes) {
            auto metadata = DeserializeMetadata(bytes.value());
            if (metadata && metadata.value().dirty && metadata.value().uri == uri.to_string()) {
                return Recover(endpoint,
                               {uri,
                                CommittedContentPath(metadata_path, metadata.value()),
                                metadata_path,
                                metadata.value().encoding,
                                metadata.value().eol,
                                metadata.value().revision});
            }
        }
    }
    auto lease = DocumentCacheLease::Acquire(cache_registry_, stem);
    if (!lease) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(
            EditorStatus::Error(EditorErrc::InvalidArgument, "Document is already open"));
    }

    FileStat initial_stat;
    try {
        initial_stat = endpoint->stat(uri);
    } catch (const std::exception& error) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(IoError(error.what()));
    }
    if (initial_stat.kind != FileKind::Regular) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(
            EditorStatus::Error(EditorErrc::InvalidArgument, "URI is not a regular file"));
    }

    auto impl = std::make_unique<RemoteDocument::Impl>();
    impl->cache_lease = std::move(lease);
    impl->endpoint = std::move(endpoint);
    impl->uri = std::move(uri);
    impl->base_revision = ToRevision(initial_stat);

    if (initial_stat.size > kEditableSizeLimit) {
        auto probe = ReadEndpointProbe(*impl->endpoint, impl->uri, kBinaryProbeBytes);
        if (!probe) {
            return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(probe.status());
        }
        impl->mode =
            LooksBinary(probe.value()) ? DocumentMode::ReadOnlyBinary : DocumentMode::ReadOnlyLarge;
        return EditorResult<std::unique_ptr<RemoteDocument>>::Success(
            std::unique_ptr<RemoteDocument>(new RemoteDocument(std::move(impl))));
    }

    auto bytes = ReadEndpoint(*impl->endpoint, impl->uri, kEditableSizeLimit);
    if (!bytes) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(bytes.status());
    }
    FileStat final_stat;
    try {
        final_stat = impl->endpoint->stat(impl->uri);
    } catch (const std::exception& error) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(IoError(error.what()));
    }
    if (impl->base_revision.size != final_stat.size ||
        impl->base_revision.modified != final_stat.modified ||
        bytes.value().size() != final_stat.size) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(
            IoError("Remote file changed while it was being opened"));
    }
    impl->base_revision = ToRevision(final_stat, Sha256(bytes.value()));
    if (LooksBinary(bytes.value())) {
        impl->mode = DocumentMode::ReadOnlyBinary;
        return EditorResult<std::unique_ptr<RemoteDocument>>::Success(
            std::unique_ptr<RemoteDocument>(new RemoteDocument(std::move(impl))));
    }

    auto info = InspectText(bytes.value());
    impl->encoding = info.encoding;
    impl->eol = info.eol;
    impl->text = std::move(info.text);
    impl->cache_path = cache_dir_ / (stem + ".content");
    impl->metadata_path = cache_dir_ / (stem + ".meta");

    if (std::filesystem::exists(impl->metadata_path)) {
        auto existing = ReadLocalFile(impl->metadata_path, kMaxMetadataBytes);
        if (existing) {
            auto metadata = DeserializeMetadata(existing.value());
            if (metadata && metadata.value().dirty &&
                metadata.value().uri == impl->uri.to_string()) {
                return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(
                    EditorStatus::Error(EditorErrc::InvalidArgument,
                                        "Unsaved recovery data exists for this remote file"));
            }
        }
    }
    if (auto status = impl->checkpoint(); !status) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(std::move(status));
    }
    return EditorResult<std::unique_ptr<RemoteDocument>>::Success(
        std::unique_ptr<RemoteDocument>(new RemoteDocument(std::move(impl))));
}

EditorResult<std::vector<RecoveryEntry>> RemoteEditorService::RecoverableDocuments() const {
    std::vector<RecoveryEntry> result;
    std::error_code error;
    for (const auto& item : std::filesystem::directory_iterator(cache_dir_, error)) {
        if (error) {
            return EditorResult<std::vector<RecoveryEntry>>::Failure(
                IoError("Cannot enumerate editor recovery cache"));
        }
        if (!item.is_regular_file() || item.path().extension() != L".meta") {
            continue;
        }
        auto lease = DocumentCacheLease::Acquire(cache_registry_, item.path().stem().string());
        if (!lease)
            continue; // 正在使用的缓存既不清理，也不重复恢复。
        auto bytes = ReadLocalFile(item.path(), kMaxMetadataBytes);
        if (!bytes) {
            continue;
        }
        auto metadata = DeserializeMetadata(bytes.value());
        if (!metadata) {
            continue;
        }
        auto content_path = CommittedContentPath(item.path(), metadata.value());
        if (!metadata.value().dirty) {
            // 保存后崩溃可能只来得及提交干净 .meta，补清理同一 URI 的原版本指针。
            auto baseline_path = item.path();
            baseline_path.replace_extension(L".baseline");
            if (auto old_bytes = ReadLocalFile(baseline_path, kMaxMetadataBytes); old_bytes) {
                auto old = DeserializeMetadata(old_bytes.value());
                if (old && !old.value().dirty && old.value().uri == metadata.value().uri) {
                    (void)RemoveCacheFile(CommittedContentPath(item.path(), old.value()));
                    (void)RemoveCacheFile(baseline_path);
                }
            }
            std::filesystem::remove(content_path, error);
            error.clear();
            std::filesystem::remove(item.path(), error);
            error.clear();
            continue;
        }
        if (!std::filesystem::is_regular_file(content_path, error) || error) {
            error.clear();
            continue;
        }
        auto content = ReadLocalFile(content_path, kInternalTextSizeLimit);
        if (!content || content.value().size() != metadata.value().content_size ||
            Sha256(content.value()) != metadata.value().content_sha256) {
            continue;
        }
        try {
            result.push_back({FileUri::parse(metadata.value().uri),
                              std::move(content_path),
                              item.path(),
                              metadata.value().encoding,
                              metadata.value().eol,
                              metadata.value().revision});
        } catch (...) {}
    }
    if (error) {
        return EditorResult<std::vector<RecoveryEntry>>::Failure(
            IoError("Cannot enumerate editor recovery cache"));
    }
    return EditorResult<std::vector<RecoveryEntry>>::Success(std::move(result));
}

EditorResult<std::unique_ptr<RemoteDocument>>
RemoteEditorService::Recover(std::shared_ptr<IFileEndpoint> endpoint,
                             const RecoveryEntry& entry) const {
    if (!endpoint || !endpoint->available() || !endpoint->accepts(entry.uri) ||
        !IsDirectCacheChild(cache_dir_, entry.content_path) ||
        !IsDirectCacheChild(cache_dir_, entry.metadata_path) ||
        entry.metadata_path.stem().string() != CacheStem(entry.uri) ||
        entry.content_path.extension() != L".content" ||
        entry.metadata_path.extension() != L".meta") {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(
            EditorStatus::Error(EditorErrc::InvalidArgument, "Invalid recovery entry"));
    }
    auto lease = DocumentCacheLease::Acquire(cache_registry_, entry.metadata_path.stem().string());
    if (!lease) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(
            EditorStatus::Error(EditorErrc::InvalidArgument, "Document is already open"));
    }
    auto metadata_bytes = ReadLocalFile(entry.metadata_path, kMaxMetadataBytes);
    if (!metadata_bytes) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(metadata_bytes.status());
    }
    auto metadata = DeserializeMetadata(metadata_bytes.value());
    if (!metadata || !metadata.value().dirty || metadata.value().uri != entry.uri.to_string()) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(
            EditorStatus::Error(EditorErrc::CorruptCache, "Recovery metadata does not match"));
    }
    if (entry.content_path != CommittedContentPath(entry.metadata_path, metadata.value()))
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(EditorStatus::Error(
            EditorErrc::CorruptCache, "Recovery content generation does not match"));
    auto content = ReadLocalFile(entry.content_path, kInternalTextSizeLimit);
    if (!content || content.value().size() != metadata.value().content_size ||
        Sha256(content.value()) != metadata.value().content_sha256) {
        return EditorResult<std::unique_ptr<RemoteDocument>>::Failure(
            EditorStatus::Error(EditorErrc::CorruptCache, "Recovery content is corrupt"));
    }

    auto impl = std::make_unique<RemoteDocument::Impl>();
    impl->cache_lease = std::move(lease);
    impl->endpoint = std::move(endpoint);
    impl->uri = entry.uri;
    impl->encoding = metadata.value().encoding;
    impl->eol = metadata.value().eol;
    impl->base_revision = metadata.value().revision;
    impl->dirty = true;
    impl->cache_path = entry.content_path;
    impl->metadata_path = entry.metadata_path;
    impl->load_baseline();
    // 内容和元数据刚经哈希校验，关闭时无需再次写入同一份已提交恢复记录。
    impl->checkpoint_needed = false;
    impl->text.assign(
        reinterpret_cast<const char*>(content.value().data()),
        reinterpret_cast<const char*>(content.value().data() + content.value().size()));
    return EditorResult<std::unique_ptr<RemoteDocument>>::Success(
        std::unique_ptr<RemoteDocument>(new RemoteDocument(std::move(impl))));
}

} // namespace SLink::editor
