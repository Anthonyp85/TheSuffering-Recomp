// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "common/update_checker.h"
#include "common/ui_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "app/app.h"
#include "core/log.h"

namespace suffering {

namespace {

namespace fs = std::filesystem;

constexpr const char* kGitHubApiLatestUrl =
    "https://api.github.com/repos/" SUFFERING_GITHUB_REPO "/releases/latest";
constexpr const char* kGitHubApiReleasesUrl =
    "https://api.github.com/repos/" SUFFERING_GITHUB_REPO "/releases?per_page=5";
constexpr const char* kGitHubApiTagsUrl =
    "https://api.github.com/repos/" SUFFERING_GITHUB_REPO "/tags?per_page=5";

constexpr size_t kMaxJsonBytes = 4u * 1024u * 1024u;        // 4 MiB cap on API payloads
constexpr uint64_t kMaxArchiveBytes = 1024ull << 20;        // 1 GiB cap on release archives

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string TrimAscii(std::string_view sv) {
    size_t b = 0;
    size_t e = sv.size();
    while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(b, e - b));
}

bool StartsWithCaseInsensitive(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool EndsWithCaseInsensitive(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    size_t off = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[off + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

std::string GetEnvUtf8(const char* name) {
#ifdef _WIN32
    std::wstring wname = Utf8ToWideStr(name ? name : "");
    if (!wname.empty()) {
        if (const wchar_t* wv = _wgetenv(wname.c_str()); wv && *wv) {
            return WideToUtf8Str(wv);
        }
    }
#endif
    if (const char* v = std::getenv(name); v && *v) {
        return std::string(v);
    }
    return {};
}

bool HasUnsafeUrlChars(std::string_view url) {
    if (url.empty() || url.size() > 2048) return true;
    for (unsigned char c : url) {
        if (c <= 0x20u || c >= 0x7Fu) return true;
        switch (c) {
            case '"':
            case '\'':
            case '`':
            case '\\':
            case '<':
            case '>':
            case '|':
            case ';':
            case '$':
                return true;
            default:
                break;
        }
    }
    return false;
}

bool IsAllowedReleasePageUrl(std::string_view url) {
    if (HasUnsafeUrlChars(url)) return false;
    constexpr std::string_view kPrefix = "https://github.com/anthonyp85/thesuffering-recomp";
    if (!StartsWithCaseInsensitive(url, kPrefix)) return false;
    if (url.size() == kPrefix.size()) return true;
    char next = url[kPrefix.size()];
    return next == '/' || next == '?' || next == '#';
}

bool IsAllowedAssetDownloadUrl(std::string_view url) {
    if (HasUnsafeUrlChars(url)) return false;
    return StartsWithCaseInsensitive(url, "https://github.com/anthonyp85/thesuffering-recomp/") ||
           StartsWithCaseInsensitive(url, "https://objects.githubusercontent.com/") ||
           StartsWithCaseInsensitive(url, "https://release-assets.githubusercontent.com/");
}

bool IsAllowedRedirectUrl(std::string_view url) {
    if (IsAllowedAssetDownloadUrl(url)) return true;
    if (HasUnsafeUrlChars(url)) return false;
    return StartsWithCaseInsensitive(url, "https://api.github.com/repos/anthonyp85/thesuffering-recomp/") ||
           StartsWithCaseInsensitive(url, "https://github-releases.githubusercontent.com/") ||
           StartsWithCaseInsensitive(url, "https://codeload.github.com/anthonyp85/thesuffering-recomp/");
}

std::string SanitizeAssetFilename(std::string_view raw) {
    size_t slash = raw.find_last_of("/\\");
    if (slash != std::string_view::npos) raw = raw.substr(slash + 1);
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '+') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    if (out.empty() || out == "." || out == "..") {
#ifdef _WIN32
        return "TheSuffering-Recomp-update.zip";
#else
        return "TheSuffering-Recomp-update.tar.gz";
#endif
    }
    return out;
}

// ---------------------------------------------------------------------------
// Self-contained FIPS 180-4 SHA-256 (zero external library dependencies)
// ---------------------------------------------------------------------------
class Sha256Hasher {
public:
    Sha256Hasher() { Reset(); }

    void Reset() {
        state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        bit_len_ = 0;
        buf_len_ = 0;
    }

    void Update(const void* data, size_t len) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            buf_[buf_len_++] = bytes[i];
            if (buf_len_ == 64) {
                Transform(buf_.data());
                bit_len_ += 512;
                buf_len_ = 0;
            }
        }
    }

    std::string FinalHex() {
        uint64_t total_bits = bit_len_ + uint64_t(buf_len_) * 8u;
        buf_[buf_len_++] = 0x80u;
        if (buf_len_ > 56) {
            while (buf_len_ < 64) buf_[buf_len_++] = 0;
            Transform(buf_.data());
            buf_len_ = 0;
        }
        while (buf_len_ < 56) buf_[buf_len_++] = 0;
        for (int i = 7; i >= 0; --i) {
            buf_[buf_len_++] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFFu);
        }
        Transform(buf_.data());

        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint32_t word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                out.push_back(kHex[(word >> shift) & 0xFu]);
            }
        }
        return out;
    }

private:
    static constexpr uint32_t Rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); }

    void Transform(const uint8_t block[64]) {
        static constexpr uint32_t kRoundK[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                   (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + s1 + ch + kRoundK[i] + w[i];
            uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<uint32_t, 8> state_{};
    std::array<uint8_t, 64> buf_{};
    uint64_t bit_len_ = 0;
    size_t buf_len_ = 0;
};

// ---------------------------------------------------------------------------
// Minimal RFC 8259 Recursive-Descent JSON Parser (depth-bounded, zero deps)
// ---------------------------------------------------------------------------
struct JsonVal {
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool bval = false;
    uint64_t uval = 0;
    std::string sval;
    std::vector<JsonVal> arr;
    std::vector<std::pair<std::string, JsonVal>> obj;

    const JsonVal* Find(std::string_view key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    std::string GetString(std::string_view key, std::string_view def = "") const {
        const JsonVal* v = Find(key);
        return (v && v->type == Type::String) ? v->sval : std::string(def);
    }

    bool GetBool(std::string_view key, bool def = false) const {
        const JsonVal* v = Find(key);
        return (v && v->type == Type::Bool) ? v->bval : def;
    }

    uint64_t GetUint64(std::string_view key, uint64_t def = 0) const {
        const JsonVal* v = Find(key);
        return (v && v->type == Type::Number) ? v->uval : def;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view src) : src_(src) {
        if (src_.size() >= 3 && static_cast<uint8_t>(src_[0]) == 0xEFu &&
            static_cast<uint8_t>(src_[1]) == 0xBBu && static_cast<uint8_t>(src_[2]) == 0xBFu) {
            pos_ = 3;  // Strip UTF-8 BOM
        }
    }

    bool Parse(JsonVal& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        return pos_ == src_.size();
    }

private:
    static constexpr int kMaxDepth = 32;
    std::string_view src_;
    size_t pos_ = 0;

    void SkipWs() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    static void AppendUtf8(std::string& dst, uint32_t cp) {
        if (cp < 0x80u) {
            dst.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            dst.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            dst.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            dst.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            dst.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            dst.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0x10FFFFu) {
            dst.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            dst.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            dst.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            dst.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }

    bool ParseHex4(uint32_t& out) {
        if (pos_ + 4 > src_.size()) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = src_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }

    bool ParseStringRaw(std::string& out) {
        if (pos_ >= src_.size() || src_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20u) return false;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= src_.size()) return false;
            char esc = src_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!ParseHex4(cp)) return false;
                    if (cp >= 0xD800u && cp <= 0xDBFFu && pos_ + 6 <= src_.size() &&
                        src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        uint32_t low = 0;
                        if (!ParseHex4(low)) return false;
                        if (low >= 0xDC00u && low <= 0xDFFFu) {
                            cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool MatchLiteral(std::string_view lit) {
        if (pos_ + lit.size() > src_.size()) return false;
        if (src_.substr(pos_, lit.size()) != lit) return false;
        pos_ += lit.size();
        return true;
    }

    bool ParseNumber(JsonVal& out) {
        size_t start = pos_;
        bool neg = false;
        if (pos_ < src_.size() && src_[pos_] == '-') {
            neg = true;
            ++pos_;
        }
        if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) return false;
        uint64_t int_part = 0;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            uint64_t digit = uint64_t(src_[pos_] - '0');
            if (int_part <= (UINT64_MAX - digit) / 10u) {
                int_part = int_part * 10u + digit;
            }
            ++pos_;
        }
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) return false;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) return false;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        (void)start;
        out.type = JsonVal::Type::Number;
        out.uval = neg ? 0 : int_part;
        return true;
    }

    bool ParseValue(JsonVal& out, int depth) {
        if (depth > kMaxDepth || pos_ >= src_.size()) return false;
        SkipWs();
        if (pos_ >= src_.size()) return false;
        char c = src_[pos_];
        if (c == 'n') {
            if (!MatchLiteral("null")) return false;
            out.type = JsonVal::Type::Null;
            return true;
        }
        if (c == 't') {
            if (!MatchLiteral("true")) return false;
            out.type = JsonVal::Type::Bool;
            out.bval = true;
            return true;
        }
        if (c == 'f') {
            if (!MatchLiteral("false")) return false;
            out.type = JsonVal::Type::Bool;
            out.bval = false;
            return true;
        }
        if (c == '"') {
            out.type = JsonVal::Type::String;
            return ParseStringRaw(out.sval);
        }
        if (c == '[') {
            ++pos_;
            out.type = JsonVal::Type::Array;
            out.arr.clear();
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            while (pos_ < src_.size()) {
                JsonVal elem;
                if (!ParseValue(elem, depth + 1)) return false;
                out.arr.push_back(std::move(elem));
                SkipWs();
                if (pos_ >= src_.size()) return false;
                if (src_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                if (src_[pos_] != ',') return false;
                ++pos_;
                SkipWs();
            }
            return false;
        }
        if (c == '{') {
            ++pos_;
            out.type = JsonVal::Type::Object;
            out.obj.clear();
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            while (pos_ < src_.size()) {
                SkipWs();
                std::string key;
                if (!ParseStringRaw(key)) return false;
                SkipWs();
                if (pos_ >= src_.size() || src_[pos_] != ':') return false;
                ++pos_;
                JsonVal val;
                if (!ParseValue(val, depth + 1)) return false;
                out.obj.emplace_back(std::move(key), std::move(val));
                SkipWs();
                if (pos_ >= src_.size()) return false;
                if (src_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                if (src_[pos_] != ',') return false;
                ++pos_;
            }
            return false;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return ParseNumber(out);
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Release notes markdown -> clean 1-3 line plain-text summary
// ---------------------------------------------------------------------------
std::string CleanMarkdownLine(std::string_view line) {
    std::string s = TrimAscii(line);
    if (s.empty()) return {};

    // Skip horizontal rules, fenced code markers, and HTML comments
    if (s.rfind("```", 0) == 0 || s.rfind("<!--", 0) == 0) return {};
    bool all_rule = s.size() >= 3;
    for (char c : s) {
        if (c != '-' && c != '*' && c != '_' && c != ' ') {
            all_rule = false;
            break;
        }
    }
    if (all_rule) return {};

    // Strip leading heading / quote / bullet markers
    size_t i = 0;
    while (i < s.size() && (s[i] == '#' || s[i] == '>')) ++i;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i + 1 < s.size() && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') {
        i += 2;
    }
    s = TrimAscii(std::string_view(s).substr(i));
    if (s.empty()) return {};

    // Convert [label](url) -> label and strip **, __, `
    std::string plain;
    plain.reserve(s.size());
    for (size_t p = 0; p < s.size();) {
        if (s[p] == '[') {
            size_t close_br = s.find(']', p + 1);
            if (close_br != std::string::npos && close_br + 1 < s.size() && s[close_br + 1] == '(') {
                size_t close_par = s.find(')', close_br + 2);
                if (close_par != std::string::npos) {
                    plain.append(s.substr(p + 1, close_br - (p + 1)));
                    p = close_par + 1;
                    continue;
                }
            }
        }
        if ((s[p] == '*' || s[p] == '_') && p + 1 < s.size() && s[p + 1] == s[p]) {
            p += 2;
            continue;
        }
        if (s[p] == '`') {
            ++p;
            continue;
        }
        plain.push_back(s[p++]);
    }

    // Collapse consecutive spaces
    std::string compact;
    compact.reserve(plain.size());
    bool prev_space = false;
    for (unsigned char c : plain) {
        if (std::isspace(c)) {
            if (!prev_space) compact.push_back(' ');
            prev_space = true;
        } else {
            compact.push_back(static_cast<char>(c));
            prev_space = false;
        }
    }
    return TrimAscii(compact);
}

std::string SummarizeReleaseNotes(std::string_view body, std::string_view fallback_title, std::string_view tag) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < body.size() && lines.size() < 3) {
        size_t nl = body.find('\n', pos);
        std::string_view raw = (nl == std::string_view::npos) ? body.substr(pos) : body.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? body.size() : (nl + 1);

        std::string cleaned = CleanMarkdownLine(raw);
        if (cleaned.empty()) continue;

        std::string low = LowerAscii(cleaned);
        if (low == "what's new" || low == "whats new" || low == "changelog" || low == "release notes" ||
            low == "highlights" || low == LowerAscii(std::string(tag)) ||
            low == LowerAscii(std::string(fallback_title))) {
            continue;
        }
        // Skip SHA256 checksum table lines in the summary
        if (low.find("sha256") != std::string::npos && cleaned.size() > 64) continue;
        lines.push_back(std::move(cleaned));
    }

    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    constexpr size_t kMaxSummaryChars = 260;
    if (out.size() > kMaxSummaryChars) {
        out.resize(kMaxSummaryChars - 3);
        while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
        out += "...";
    }
    if (out.empty()) {
        if (!fallback_title.empty() && LowerAscii(std::string(fallback_title)) != LowerAscii(std::string(tag))) {
            return std::string(fallback_title);
        }
        return "A new release of The Suffering Recomp is available on GitHub.";
    }
    return out;
}

std::string ParseSha256DigestField(std::string_view digest) {
    std::string d = LowerAscii(TrimAscii(digest));
    if (d.rfind("sha256:", 0) == 0) d.erase(0, 7);
    if (d.size() != 64) return {};
    for (unsigned char c : d) {
        if (!std::isxdigit(c)) return {};
    }
    return d;
}

// Match best platform archive asset from a release object's "assets" array.
int ScorePlatformAsset(std::string_view asset_name) {
    std::string low = LowerAscii(std::string(asset_name));
    if (low.find("source") != std::string::npos || low.find("-src") != std::string::npos ||
        low.find("symbols") != std::string::npos || low.find("debug") != std::string::npos) {
        return 0;
    }
#ifdef _WIN32
    if (!EndsWithCaseInsensitive(low, ".zip")) return 0;
    if (low.find("linux") != std::string::npos || low.find("macos") != std::string::npos ||
        low.find("darwin") != std::string::npos) {
        return 0;
    }
    if (low.find("win64") != std::string::npos && low.find("launcher") != std::string::npos) return 100;
    if (low.find("win64") != std::string::npos || low.find("win-x64") != std::string::npos ||
        low.find("windows") != std::string::npos) {
        return 90;
    }
    if (low == "suffering.zip") return 80;
    return 0;
#else
    if (!EndsWithCaseInsensitive(low, ".tar.gz") && !EndsWithCaseInsensitive(low, ".tgz")) return 0;
    if (low.find("win64") != std::string::npos || low.find("windows") != std::string::npos ||
        low.find("macos") != std::string::npos) {
        return 0;
    }
    if (low.find("linux") != std::string::npos && low.find("x86_64") != std::string::npos) return 100;
    if (low.find("linux") != std::string::npos &&
        (low.find("amd64") != std::string::npos || low.find("x64") != std::string::npos ||
         low.find("steamdeck") != std::string::npos)) {
        return 90;
    }
    if (low.find("linux") != std::string::npos) return 80;
    return 0;
#endif
}

std::string FormatSemVer(const SemVer& v) {
    std::string s = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    if (v.is_prerelease && !v.prerelease.empty()) s += "-" + v.prerelease;
    return s;
}

UpdateStatusSnapshot MakeUpToDateSnapshot(bool manual, std::string_view tag_override = "") {
    UpdateStatusSnapshot snap{};
    snap.state = UpdateState::UpToDate;
    snap.manual_check = manual;
    snap.current_version = SUFFERING_VERSION_STR;
    snap.latest_version = SUFFERING_VERSION_STR;
    snap.latest_tag = tag_override.empty() ? std::string(SUFFERING_VERSION_TAG) : std::string(tag_override);
    snap.html_url = SUFFERING_RELEASES_URL;
    snap.status_message = "You are running the latest version (" SUFFERING_VERSION_TAG ").";
    snap.can_self_update = false;
    return snap;
}

UpdateStatusSnapshot MakeErrorSnapshot(bool manual, std::string msg) {
    UpdateStatusSnapshot snap{};
    snap.state = UpdateState::Error;
    snap.manual_check = manual;
    snap.current_version = SUFFERING_VERSION_STR;
    snap.html_url = SUFFERING_RELEASES_URL;
    snap.status_message = std::move(msg);
    snap.can_self_update = false;
    return snap;
}

UpdateStatusSnapshot EvaluateReleaseObject(const JsonVal& rel, bool manual) {
    std::string tag = TrimAscii(rel.GetString("tag_name"));
    if (tag.empty()) tag = TrimAscii(rel.GetString("name"));
    if (tag.empty()) {
        return MakeUpToDateSnapshot(manual);
    }

    SemVer local_sv{};
    ParseSemVer(SUFFERING_VERSION_STR, local_sv);

    SemVer remote_sv{};
    if (!ParseSemVer(tag, remote_sv)) {
        return MakeErrorSnapshot(manual, "Could not parse version tag '" + tag + "' from GitHub release.");
    }

    UpdateStatusSnapshot snap{};
    snap.manual_check = manual;
    snap.current_version = SUFFERING_VERSION_STR;
    snap.latest_version = FormatSemVer(remote_sv);
    snap.latest_tag = tag;
    snap.release_title = TrimAscii(rel.GetString("name", tag));
    if (snap.release_title.empty()) snap.release_title = tag;

    std::string html = TrimAscii(rel.GetString("html_url"));
    if (IsAllowedReleasePageUrl(html)) {
        snap.html_url = html;
    } else {
        snap.html_url = std::string(SUFFERING_RELEASES_URL) + "/tag/" + tag;
        if (!IsAllowedReleasePageUrl(snap.html_url)) snap.html_url = SUFFERING_RELEASES_URL;
    }

    std::string body = rel.GetString("body");
    snap.release_notes_summary = SummarizeReleaseNotes(body, snap.release_title, tag);

    const JsonVal* assets = rel.Find("assets");
    if (assets && assets->type == JsonVal::Type::Array) {
        int best_score = 0;
        for (const JsonVal& a : assets->arr) {
            if (a.type != JsonVal::Type::Object) continue;
            std::string aname = TrimAscii(a.GetString("name"));
            std::string aurl = TrimAscii(a.GetString("browser_download_url"));
            int score = ScorePlatformAsset(aname);
            if (score > best_score && IsAllowedAssetDownloadUrl(aurl)) {
                best_score = score;
                snap.asset_name = aname;
                snap.asset_url = aurl;
                snap.asset_size = a.GetUint64("size", 0);
                snap.asset_sha256 = ParseSha256DigestField(a.GetString("digest"));
                snap.can_self_update = true;
            }
        }
    }

    if (CompareSemVer(remote_sv, local_sv) > 0) {
        snap.state = UpdateState::UpdateAvailable;
        snap.status_message = "Update available: " + snap.latest_tag + " (current: " SUFFERING_VERSION_TAG ").";
    } else {
        snap.state = UpdateState::UpToDate;
        snap.can_self_update = false;
        snap.status_message = "You are running the latest version (" SUFFERING_VERSION_TAG ").";
    }
    return snap;
}

UpdateStatusSnapshot EvaluateTagsArray(const JsonVal& arr, bool manual) {
    if (arr.type != JsonVal::Type::Array || arr.arr.empty()) {
        return MakeUpToDateSnapshot(manual);
    }

    SemVer best_sv{};
    std::string best_tag;
    for (const JsonVal& item : arr.arr) {
        if (item.type != JsonVal::Type::Object) continue;
        std::string t = TrimAscii(item.GetString("name"));
        SemVer sv{};
        if (!ParseSemVer(t, sv)) continue;
        if (!best_sv.valid || CompareSemVer(sv, best_sv) > 0) {
            best_sv = sv;
            best_tag = t;
        }
    }
    if (!best_sv.valid) return MakeUpToDateSnapshot(manual);

    SemVer local_sv{};
    ParseSemVer(SUFFERING_VERSION_STR, local_sv);
    if (CompareSemVer(best_sv, local_sv) <= 0) {
        return MakeUpToDateSnapshot(manual, best_tag);
    }

    UpdateStatusSnapshot snap{};
    snap.state = UpdateState::UpdateAvailable;
    snap.manual_check = manual;
    snap.current_version = SUFFERING_VERSION_STR;
    snap.latest_version = FormatSemVer(best_sv);
    snap.latest_tag = best_tag;
    snap.release_title = best_tag;
    snap.release_notes_summary = "New tagged release " + best_tag + " is available on GitHub.";
    std::string tag_url = std::string(SUFFERING_RELEASES_URL) + "/tag/" + best_tag;
    snap.html_url = IsAllowedReleasePageUrl(tag_url) ? tag_url : std::string(SUFFERING_RELEASES_URL);
    snap.status_message = "Update available: " + best_tag + " (current: " SUFFERING_VERSION_TAG ").";
    snap.can_self_update = false;
    return snap;
}

// ---------------------------------------------------------------------------
// Global thread-safe state & cancellation coordination
// ---------------------------------------------------------------------------
std::mutex g_state_mutex;
std::mutex g_worker_join_mutex;
UpdateStatusSnapshot g_snapshot{};
fs::path g_staged_root_dir;
fs::path g_staging_dir;
std::atomic<uint64_t> g_live_downloaded_bytes{0};
std::atomic<bool> g_worker_active{false};
std::atomic<bool> g_shutdown_requested{false};
std::thread g_worker;

#ifdef _WIN32
struct WinHttpCancelSlot {
    std::mutex lock;
    HINTERNET h_req = nullptr;
    bool redirect_blocked = false;

    void Attach(HINTERNET h) {
        std::lock_guard<std::mutex> g(lock);
        h_req = h;
    }

    void CloseOnce() {
        HINTERNET h = nullptr;
        {
            std::lock_guard<std::mutex> g(lock);
            h = std::exchange(h_req, nullptr);
        }
        if (h) WinHttpCloseHandle(h);
    }
};

std::mutex g_cancel_slot_mutex;
std::shared_ptr<WinHttpCancelSlot> g_active_winhttp_slot;

void CALLBACK WinHttpRedirectCallback(HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus,
                                      LPVOID lpvStatusInformation, DWORD dwStatusInformationLength) {
    (void)hInternet;
    if (dwInternetStatus != WINHTTP_CALLBACK_STATUS_REDIRECT || !dwContext || !lpvStatusInformation) return;
    auto* slot = reinterpret_cast<WinHttpCancelSlot*>(dwContext);
    const auto* wurl = static_cast<const wchar_t*>(lpvStatusInformation);
    size_t wlen = dwStatusInformationLength > 0 ? (dwStatusInformationLength - 1) : std::wcslen(wurl);
    std::string target = WideToUtf8Str(std::wstring(wurl, wlen));
    if (!IsAllowedRedirectUrl(target)) {
        LOGW("update_checker: blocked redirect to untrusted URL: %s", target.c_str());
        slot->redirect_blocked = true;
        slot->CloseOnce();
    }
}
#else
std::mutex g_child_pid_mutex;
pid_t g_active_child_pid = -1;
#endif

void AbortActiveNetworkOp() {
#ifdef _WIN32
    std::shared_ptr<WinHttpCancelSlot> slot;
    {
        std::lock_guard<std::mutex> g(g_cancel_slot_mutex);
        slot = g_active_winhttp_slot;
    }
    if (slot) slot->CloseOnce();
#else
    std::lock_guard<std::mutex> g(g_child_pid_mutex);
    if (g_active_child_pid > 1) {
        kill(g_active_child_pid, SIGTERM);
    }
#endif
}

struct WorkerGuard {
    ~WorkerGuard() {
        g_shutdown_requested.store(true, std::memory_order_release);
        AbortActiveNetworkOp();
        if (g_worker.joinable()) {
#ifdef _WIN32
            // Avoid Win32 loader-lock deadlock if process exits before explicit ShutdownUpdateChecker().
            HANDLE h = static_cast<HANDLE>(g_worker.native_handle());
            if (h && WaitForSingleObject(h, 250) == WAIT_OBJECT_0) {
                g_worker.join();
            } else {
                g_worker.detach();
            }
#else
            g_worker.join();
#endif
        }
    }
};
WorkerGuard g_worker_guard;

// ---------------------------------------------------------------------------
// Platform HTTPS GET & File Download
// ---------------------------------------------------------------------------
struct HttpTextResponse {
    bool network_ok = false;
    int status_code = 0;
    std::string body;
    std::string error;
};

#ifdef _WIN32

bool CrackHttpsUrl(const std::string& url, std::wstring& out_host, INTERNET_PORT& out_port, std::wstring& out_path) {
    std::wstring wurl = Utf8ToWideStr(url);
    if (wurl.empty()) return false;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), DWORD(wurl.size()), 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS || !uc.lpszHostName || uc.dwHostNameLength == 0) return false;

    out_host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out_port = uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT;
    out_path.clear();
    if (uc.lpszUrlPath && uc.dwUrlPathLength > 0) out_path.append(uc.lpszUrlPath, uc.dwUrlPathLength);
    else out_path.push_back(L'/');
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength > 0) out_path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    return true;
}

HINTERNET OpenWinHttpSession() {
    std::wstring ua = Utf8ToWideStr(SUFFERING_USER_AGENT);
    HINTERNET h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h) {
        h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    return h;
}

HttpTextResponse HttpGetTextPlatform(const std::string& url) {
    HttpTextResponse resp{};
    std::wstring host, path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (!CrackHttpsUrl(url, host, port, path)) {
        resp.error = "Invalid HTTPS URL.";
        return resp;
    }

    HINTERNET session = OpenWinHttpSession();
    if (!session) {
        resp.error = "Could not initialize WinHTTP.";
        return resp;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);

    HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        resp.error = "Could not connect to GitHub.";
        return resp;
    }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        resp.error = "Could not open HTTPS request.";
        return resp;
    }

    DWORD redir_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redir_policy, sizeof(redir_policy));

    auto slot = std::make_shared<WinHttpCancelSlot>();
    slot->Attach(req);
    {
        std::lock_guard<std::mutex> g(g_cancel_slot_mutex);
        g_active_winhttp_slot = slot;
    }
    DWORD_PTR ctx_ptr = reinterpret_cast<DWORD_PTR>(slot.get());
    WinHttpSetOption(req, WINHTTP_OPTION_CONTEXT_VALUE, &ctx_ptr, sizeof(ctx_ptr));
    WinHttpSetStatusCallback(req, WinHttpRedirectCallback, WINHTTP_CALLBACK_FLAG_REDIRECT, 0);

    const wchar_t* headers =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";

    bool sent = WinHttpSendRequest(req, headers, static_cast<DWORD>(-1L),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, ctx_ptr) &&
                WinHttpReceiveResponse(req, nullptr);

    if (!sent || slot->redirect_blocked || g_shutdown_requested.load(std::memory_order_acquire)) {
        resp.error = slot->redirect_blocked ? "Blocked unsafe redirect." : "Could not reach GitHub releases.";
    } else {
        DWORD status = 0;
        DWORD status_sz = sizeof(status);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX)) {
            resp.status_code = static_cast<int>(status);
            resp.network_ok = true;
        }
        std::array<char, 16384> buf{};
        while (!g_shutdown_requested.load(std::memory_order_acquire)) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) break;
            if (avail == 0) break;
            DWORD to_read = std::min<DWORD>(avail, DWORD(buf.size()));
            DWORD got = 0;
            if (!WinHttpReadData(req, buf.data(), to_read, &got) || got == 0) break;
            if (resp.body.size() + got > kMaxJsonBytes) {
                resp.network_ok = false;
                resp.error = "GitHub response exceeded maximum size.";
                break;
            }
            resp.body.append(buf.data(), got);
        }
    }

    {
        std::lock_guard<std::mutex> g(g_cancel_slot_mutex);
        if (g_active_winhttp_slot == slot) g_active_winhttp_slot.reset();
    }
    slot->CloseOnce();
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return resp;
}

bool HttpDownloadFilePlatform(const std::string& url, const fs::path& dest_file,
                              uint64_t& io_expected_size, Sha256Hasher& sha, std::string& out_err) {
    std::wstring host, path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (!CrackHttpsUrl(url, host, port, path)) {
        out_err = "Invalid download URL.";
        return false;
    }

    HINTERNET session = OpenWinHttpSession();
    if (!session) {
        out_err = "Could not initialize WinHTTP for download.";
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 120000);

    HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        out_err = "Could not connect to GitHub asset host.";
        return false;
    }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        out_err = "Could not open HTTPS download request.";
        return false;
    }

    DWORD redir_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redir_policy, sizeof(redir_policy));

    auto slot = std::make_shared<WinHttpCancelSlot>();
    slot->Attach(req);
    {
        std::lock_guard<std::mutex> g(g_cancel_slot_mutex);
        g_active_winhttp_slot = slot;
    }
    DWORD_PTR ctx_ptr = reinterpret_cast<DWORD_PTR>(slot.get());
    WinHttpSetOption(req, WINHTTP_OPTION_CONTEXT_VALUE, &ctx_ptr, sizeof(ctx_ptr));
    WinHttpSetStatusCallback(req, WinHttpRedirectCallback, WINHTTP_CALLBACK_FLAG_REDIRECT, 0);

    const wchar_t* headers = L"Accept: application/octet-stream\r\n";
    bool ok = false;
    FILE* fp = _wfopen(dest_file.c_str(), L"wb");
    if (!fp) {
        out_err = "Could not create staging file on disk.";
    } else if (!WinHttpSendRequest(req, headers, static_cast<DWORD>(-1L),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, ctx_ptr) ||
               !WinHttpReceiveResponse(req, nullptr)) {
        out_err = slot->redirect_blocked ? "Blocked redirect to untrusted download host."
                                         : "Failed while connecting to GitHub asset server.";
    } else {
        DWORD status = 0;
        DWORD status_sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            out_err = "Asset download failed (HTTP " + std::to_string(status) + ").";
        } else {
            if (io_expected_size == 0) {
                wchar_t cl_buf[64]{};
                DWORD cl_sz = sizeof(cl_buf);
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                        cl_buf, &cl_sz, WINHTTP_NO_HEADER_INDEX)) {
                    uint64_t parsed = std::wcstoull(cl_buf, nullptr, 10);
                    if (parsed > 0 && parsed <= kMaxArchiveBytes) {
                        io_expected_size = parsed;
                        std::lock_guard<std::mutex> lk(g_state_mutex);
                        g_snapshot.asset_size = parsed;
                    }
                }
            }

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            std::vector<uint8_t> buf(32768);
            uint64_t total = 0;
            bool read_err = false;
            while (!g_shutdown_requested.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() > deadline) {
                    out_err = "Download timed out after 120 seconds.";
                    read_err = true;
                    break;
                }
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail)) {
                    out_err = "Network error while downloading update.";
                    read_err = true;
                    break;
                }
                if (avail == 0) break;
                DWORD want = std::min<DWORD>(avail, DWORD(buf.size()));
                DWORD got = 0;
                if (!WinHttpReadData(req, buf.data(), want, &got) || got == 0) {
                    out_err = "Interrupted while reading update archive.";
                    read_err = true;
                    break;
                }
                total += got;
                if (total > kMaxArchiveBytes) {
                    out_err = "Update archive exceeds maximum allowed size.";
                    read_err = true;
                    break;
                }
                if (std::fwrite(buf.data(), 1, got, fp) != got) {
                    out_err = "Disk write failed while saving update archive.";
                    read_err = true;
                    break;
                }
                sha.Update(buf.data(), got);
                g_live_downloaded_bytes.store(total, std::memory_order_relaxed);
            }
            if (!read_err && !g_shutdown_requested.load(std::memory_order_acquire) && !slot->redirect_blocked) {
                ok = true;
            } else if (out_err.empty()) {
                out_err = "Download cancelled.";
            }
        }
    }

    if (fp) std::fclose(fp);
    {
        std::lock_guard<std::mutex> g(g_cancel_slot_mutex);
        if (g_active_winhttp_slot == slot) g_active_winhttp_slot.reset();
    }
    slot->CloseOnce();
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok;
}

#else  // !_WIN32 (Native Linux x86_64 — fork + execvp + pipe, zero DT_NEEDED bloat)

void CloseExtraFdsInChild(int keep_fd = -1) {
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 4096) max_fd = 1024;
    for (int fd = 3; fd < int(max_fd); ++fd) {
        if (fd != keep_fd) close(fd);
    }
}

struct SubprocessResult {
    bool launched = false;
    bool tool_missing = false;
    bool timed_out = false;
    int exit_code = -1;
};

SubprocessResult RunPipedTool(const std::vector<const char*>& argv_null_term, int timeout_sec,
                              const std::function<bool(const uint8_t*, size_t)>& on_chunk) {
    SubprocessResult res{};
    int pfd[2]{-1, -1};
    if (pipe(pfd) != 0) return res;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return res;
    }
    if (pid == 0) {
        close(pfd[0]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        dup2(pfd[1], STDOUT_FILENO);
        if (pfd[1] > 2) close(pfd[1]);
        CloseExtraFdsInChild();
        execvp(argv_null_term[0], const_cast<char* const*>(argv_null_term.data()));
        _exit(errno == ENOENT ? 127 : 126);
    }

    close(pfd[1]);
    res.launched = true;
    {
        std::lock_guard<std::mutex> g(g_child_pid_mutex);
        g_active_child_pid = pid;
    }

    int flags = fcntl(pfd[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::vector<uint8_t> buf(32768);
    bool aborted = false;

    while (true) {
        if (g_shutdown_requested.load(std::memory_order_acquire)) {
            aborted = true;
            kill(pid, SIGTERM);
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            res.timed_out = true;
            kill(pid, SIGKILL);
            break;
        }
        pollfd pf{};
        pf.fd = pfd[0];
        pf.events = POLLIN | POLLHUP;
        int pr = poll(&pf, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        ssize_t n = read(pfd[0], buf.data(), buf.size());
        if (n > 0) {
            if (!on_chunk(buf.data(), size_t(n))) {
                aborted = true;
                kill(pid, SIGTERM);
                break;
            }
        } else if (n == 0) {
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
    }

    close(pfd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    {
        std::lock_guard<std::mutex> g(g_child_pid_mutex);
        if (g_active_child_pid == pid) g_active_child_pid = -1;
    }

    if (WIFEXITED(st)) {
        res.exit_code = WEXITSTATUS(st);
        if (res.exit_code == 127) res.tool_missing = true;
    } else if (aborted) {
        res.exit_code = -2;
    }
    return res;
}

HttpTextResponse ParseTaggedHttpOutput(std::string&& raw) {
    HttpTextResponse r{};
    constexpr std::string_view kMarker = "\n__SUF_HTTP_CODE__:";
    size_t pos = raw.rfind(kMarker);
    if (pos == std::string::npos) return r;
    std::string code_str = TrimAscii(std::string_view(raw).substr(pos + kMarker.size()));
    int code = std::atoi(code_str.c_str());
    raw.resize(pos);
    if (code > 0) {
        r.network_ok = true;
        r.status_code = code;
        r.body = std::move(raw);
    }
    return r;
}

HttpTextResponse HttpGetTextPlatform(const std::string& url) {
    // 1. Primary: curl
    {
        std::string out;
        std::vector<const char*> args = {
            "curl", "-sSL", "--max-time", "8", "--connect-timeout", "5", "--proto", "=https",
            "-H", "Accept: application/vnd.github+json",
            "-H", "X-GitHub-Api-Version: 2022-11-28",
            "-A", SUFFERING_USER_AGENT,
            "-w", "\n__SUF_HTTP_CODE__:%{http_code}",
            url.c_str(), nullptr};
        auto sr = RunPipedTool(args, 10, [&](const uint8_t* p, size_t n) {
            if (out.size() + n > kMaxJsonBytes) return false;
            out.append(reinterpret_cast<const char*>(p), n);
            return true;
        });
        if (!sr.tool_missing) {
            HttpTextResponse r = ParseTaggedHttpOutput(std::move(out));
            if (r.network_ok) return r;
            r.error = sr.timed_out ? "Timed out contacting GitHub." : "Could not reach GitHub via curl.";
            return r;
        }
    }

    // 2. Fallback: wget
    {
        std::string out;
        std::vector<const char*> args = {
            "wget", "-qO-", "--timeout=8", "--tries=1", "--https-only",
            "--header=Accept: application/vnd.github+json",
            "--header=X-GitHub-Api-Version: 2022-11-28",
            "--user-agent=" SUFFERING_USER_AGENT,
            url.c_str(), nullptr};
        auto sr = RunPipedTool(args, 10, [&](const uint8_t* p, size_t n) {
            if (out.size() + n > kMaxJsonBytes) return false;
            out.append(reinterpret_cast<const char*>(p), n);
            return true;
        });
        if (!sr.tool_missing) {
            HttpTextResponse r{};
            if (sr.exit_code == 0) {
                r.network_ok = true;
                r.status_code = 200;
                r.body = std::move(out);
                return r;
            }
            if (sr.exit_code == 8) {
                r.network_ok = true;
                r.status_code = 404;
                return r;
            }
            r.error = sr.timed_out ? "Timed out contacting GitHub." : "Could not reach GitHub via wget.";
            return r;
        }
    }

    // 3. Fallback: python3 urllib.request (isolated mode, URL via sys.argv[1])
    {
        static constexpr const char* kPyScript =
            "import sys, urllib.request, urllib.error\n"
            "u = sys.argv[1]\n"
            "if not u.lower().startswith('https://'): sys.exit(2)\n"
            "req = urllib.request.Request(u, headers={\n"
            "  'Accept': 'application/vnd.github+json',\n"
            "  'X-GitHub-Api-Version': '2022-11-28',\n"
            "  'User-Agent': '" SUFFERING_USER_AGENT "'\n"
            "})\n"
            "try:\n"
            "  with urllib.request.urlopen(req, timeout=8) as r:\n"
            "    b = r.read(4194304)\n"
            "    sys.stdout.buffer.write(b)\n"
            "    sys.stdout.buffer.write(f'\\n__SUF_HTTP_CODE__:{r.status}'.encode())\n"
            "except urllib.error.HTTPError as e:\n"
            "  b = e.read(65536) if e.fp else b''\n"
            "  sys.stdout.buffer.write(b)\n"
            "  sys.stdout.buffer.write(f'\\n__SUF_HTTP_CODE__:{e.code}'.encode())\n";
        std::string out;
        std::vector<const char*> args = {"python3", "-I", "-c", kPyScript, url.c_str(), nullptr};
        auto sr = RunPipedTool(args, 10, [&](const uint8_t* p, size_t n) {
            if (out.size() + n > kMaxJsonBytes) return false;
            out.append(reinterpret_cast<const char*>(p), n);
            return true;
        });
        if (!sr.tool_missing) {
            HttpTextResponse r = ParseTaggedHttpOutput(std::move(out));
            if (r.network_ok) return r;
        }
    }

    HttpTextResponse err{};
    err.error = "Could not reach GitHub (ensure curl, wget, or python3 is installed).";
    return err;
}

bool HttpDownloadFilePlatform(const std::string& url, const fs::path& dest_file,
                              uint64_t& io_expected_size, Sha256Hasher& sha, std::string& out_err) {
    (void)io_expected_size;

    auto run_stream_to_file = [&](const std::vector<const char*>& args, bool& tool_missing) -> bool {
        tool_missing = false;
        sha.Reset();
        g_live_downloaded_bytes.store(0, std::memory_order_relaxed);
        FILE* fp = std::fopen(dest_file.c_str(), "wb");
        if (!fp) {
            out_err = "Could not create staging archive file.";
            return false;
        }
        uint64_t total = 0;
        bool write_ok = true;
        auto sr = RunPipedTool(args, 125, [&](const uint8_t* p, size_t n) {
            total += n;
            if (total > kMaxArchiveBytes || std::fwrite(p, 1, n, fp) != n) {
                write_ok = false;
                return false;
            }
            sha.Update(p, n);
            g_live_downloaded_bytes.store(total, std::memory_order_relaxed);
            return true;
        });
        std::fclose(fp);
        tool_missing = sr.tool_missing;
        if (sr.tool_missing) return false;
        if (!write_ok) {
            out_err = "Disk write failed while downloading update archive.";
            return false;
        }
        if (sr.timed_out) {
            out_err = "Download timed out after 120 seconds.";
            return false;
        }
        if (sr.exit_code != 0 || total == 0) {
            out_err = "Failed to download release archive from GitHub.";
            return false;
        }
        return true;
    };

    bool missing = false;
    if (run_stream_to_file({"curl", "-fsSL", "--max-time", "120", "--connect-timeout", "5", "--proto", "=https",
                            "-H", "Accept: application/octet-stream", "-A", SUFFERING_USER_AGENT,
                            url.c_str(), nullptr},
                           missing)) {
        return true;
    }
    if (!missing) return false;

    if (run_stream_to_file({"wget", "-qO-", "--timeout=120", "--tries=1", "--https-only",
                            "--user-agent=" SUFFERING_USER_AGENT, url.c_str(), nullptr},
                           missing)) {
        return true;
    }
    if (!missing) return false;

    static constexpr const char* kPyDl =
        "import sys, urllib.request\n"
        "u = sys.argv[1]\n"
        "if not u.lower().startswith('https://'): sys.exit(2)\n"
        "req = urllib.request.Request(u, headers={\n"
        "  'Accept': 'application/octet-stream',\n"
        "  'User-Agent': '" SUFFERING_USER_AGENT "'\n"
        "})\n"
        "with urllib.request.urlopen(req, timeout=120) as r:\n"
        "  while True:\n"
        "    b = r.read(32768)\n"
        "    if not b: break\n"
        "    sys.stdout.buffer.write(b)\n"
        "    sys.stdout.buffer.flush()\n";
    if (run_stream_to_file({"python3", "-I", "-c", kPyDl, url.c_str(), nullptr}, missing)) {
        return true;
    }
    if (missing) out_err = "No HTTPS downloader found (install curl, wget, or python3).";
    return false;
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// Extraction & Staging Verification
// ---------------------------------------------------------------------------
bool ExtractArchiveToDir(const fs::path& archive_path, const fs::path& extracted_dir, std::string& out_err) {
    std::error_code ec;
    fs::remove_all(extracted_dir, ec);
    fs::create_directories(extracted_dir, ec);
    if (ec) {
        out_err = "Could not create extraction directory: " + extracted_dir.string();
        return false;
    }

#ifdef _WIN32
    wchar_t sys_dir[MAX_PATH]{};
    UINT n = GetSystemDirectoryW(sys_dir, MAX_PATH);
    fs::path pwsh = (n > 0) ? (fs::path(sys_dir) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe")
                            : fs::path(L"powershell.exe");
    if (!fs::exists(pwsh, ec)) pwsh = L"powershell.exe";

    // Pass literal paths via child process environment variables so spaces, quotes, '$', and parentheses
    // in user directories can never break or inject into Expand-Archive.
    SetEnvironmentVariableW(L"SUFFERING_STAGE_ZIP", archive_path.c_str());
    SetEnvironmentVariableW(L"SUFFERING_STAGE_OUT", extracted_dir.c_str());

    std::wstring cmd = L"\"" + pwsh.wstring() +
                       L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
                       L"\"$ProgressPreference = 'SilentlyContinue'; "
                       L"Expand-Archive -LiteralPath $env:SUFFERING_STAGE_ZIP "
                       L"-DestinationPath $env:SUFFERING_STAGE_OUT -Force\"";

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                  CREATE_NO_WINDOW, nullptr, extracted_dir.c_str(), &si, &pi);
    SetEnvironmentVariableW(L"SUFFERING_STAGE_ZIP", nullptr);
    SetEnvironmentVariableW(L"SUFFERING_STAGE_OUT", nullptr);

    if (!started) {
        out_err = "Could not launch PowerShell to extract update archive.";
        return false;
    }
    DWORD wait_rc = WaitForSingleObject(pi.hProcess, 120000);
    DWORD exit_code = 1;
    if (wait_rc == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        out_err = "Timed out while extracting update archive.";
    } else {
        GetExitCodeProcess(pi.hProcess, &exit_code);
        if (exit_code != 0) out_err = "Archive extraction failed (Expand-Archive exit code " + std::to_string(exit_code) + ").";
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait_rc == WAIT_OBJECT_0 && exit_code == 0;
#else
    std::string arch_s = archive_path.string();
    std::string dst_s = extracted_dir.string();
    pid_t pid = fork();
    if (pid < 0) {
        out_err = "fork() failed before extracting tarball.";
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        CloseExtraFdsInChild();
        const char* argv[] = {"tar", "-xzf", arch_s.c_str(), "-C", dst_s.c_str(), "--no-same-owner", nullptr};
        execvp("tar", const_cast<char* const*>(argv));
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        out_err = "tar -xzf failed to extract Linux update archive.";
        return false;
    }
    return true;
#endif
}

bool SanitizeAndLocateExtractedRoot(const fs::path& extracted_dir, fs::path& out_root, std::string& out_err) {
#ifdef _WIN32
    constexpr const char* kExeName = "suffering.exe";
#else
    constexpr const char* kExeName = "suffering";
#endif
    std::error_code ec;
    fs::path canon_extract = fs::weakly_canonical(extracted_dir, ec);
    if (ec) canon_extract = extracted_dir;

    // Reject/strip any symlinks escaping extracted_dir (Tar-Slip / Zip-Slip defense)
    std::vector<fs::path> unsafe_links;
    for (fs::recursive_directory_iterator it(extracted_dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (it->is_symlink(ec)) {
            fs::path resolved = fs::weakly_canonical(it->path(), ec);
            std::string rel = resolved.lexically_relative(canon_extract).string();
            if (ec || rel.empty() || rel.rfind("..", 0) == 0) {
                unsafe_links.push_back(it->path());
            }
        }
    }
    for (const fs::path& bad : unsafe_links) {
        fs::remove(bad, ec);
    }

    auto has_valid_exe = [&](const fs::path& dir) -> bool {
        fs::path exe = dir / kExeName;
        return fs::is_regular_file(exe, ec) && fs::file_size(exe, ec) > 4096;
    };

    fs::path found;
    if (has_valid_exe(extracted_dir)) {
        found = extracted_dir;
    } else {
        for (const auto& e1 : fs::directory_iterator(extracted_dir, ec)) {
            if (!e1.is_directory(ec)) continue;
            if (has_valid_exe(e1.path())) {
                found = e1.path();
                break;
            }
            for (const auto& e2 : fs::directory_iterator(e1.path(), ec)) {
                if (e2.is_directory(ec) && has_valid_exe(e2.path())) {
                    found = e2.path();
                    break;
                }
            }
            if (!found.empty()) break;
        }
    }

    if (found.empty()) {
        out_err = std::string("Extracted archive did not contain ") + kExeName + ".";
        return false;
    }

    // Prune any runtime/user-state paths from the staged directory as defense-in-depth before copy
    static constexpr const char* kProtectedEntries[] = {
        "settings.toml", "hdd", "assets", "cache", "logs", "CrashDumps", "crashdumps", "textures",
    };
    for (const char* name : kProtectedEntries) {
        fs::remove_all(found / name, ec);
    }
    fs::remove_all(found / "compiler" / "work", ec);

    for (const auto& e : fs::directory_iterator(found, ec)) {
        std::string ext = LowerAscii(e.path().extension().string());
        if (ext == ".iso" || ext == ".xiso" || ext == ".xbe" || ext == ".7z") {
            fs::remove(e.path(), ec);
        }
    }

    // Ensure the staged executable mtime is current so PrepareGameCode refreshes suffering.dll / libsuffering.so
    // cleanly if exports/compiler changed.
    fs::last_write_time(found / kExeName, fs::file_time_type::clock::now(), ec);
#ifndef _WIN32
    chmod((found / kExeName).c_str(), 0755);
#endif

    out_root = found;
    return true;
}

UpdateStatusSnapshot PerformNetworkUpdateCheck(bool manual) {
    HttpTextResponse latest = HttpGetTextPlatform(kGitHubApiLatestUrl);
    if (g_shutdown_requested.load(std::memory_order_acquire)) {
        return MakeErrorSnapshot(manual, "Update check cancelled.");
    }

    if (latest.network_ok && latest.status_code == 200) {
        return EvaluateReleaseJson(latest.body, manual);
    }

    if (latest.network_ok && latest.status_code == 404) {
        // Repository has no 'latest' non-prerelease published yet; check /releases then /tags.
        HttpTextResponse rels = HttpGetTextPlatform(kGitHubApiReleasesUrl);
        if (rels.network_ok && rels.status_code == 200) {
            JsonVal parsed;
            if (JsonParser(rels.body).Parse(parsed) && parsed.type == JsonVal::Type::Array) {
                for (const JsonVal& item : parsed.arr) {
                    if (item.type == JsonVal::Type::Object && !item.GetBool("draft", false)) {
                        return EvaluateReleaseObject(item, manual);
                    }
                }
            }
        }
        HttpTextResponse tags = HttpGetTextPlatform(kGitHubApiTagsUrl);
        if (tags.network_ok && tags.status_code == 200) {
            JsonVal parsed;
            if (JsonParser(tags.body).Parse(parsed) && parsed.type == JsonVal::Type::Array) {
                return EvaluateTagsArray(parsed, manual);
            }
        }
        if (tags.network_ok && (tags.status_code == 200 || tags.status_code == 404)) {
            return MakeUpToDateSnapshot(manual);
        }
    }

    if (latest.network_ok && (latest.status_code == 403 || latest.status_code == 429)) {
        return MakeErrorSnapshot(manual, "GitHub API rate limit reached. Try again later or open the release page.");
    }
    if (latest.network_ok && latest.status_code > 0) {
        return MakeErrorSnapshot(manual, "GitHub returned HTTP " + std::to_string(latest.status_code) + ".");
    }
    return MakeErrorSnapshot(manual,
                             latest.error.empty() ? "Could not connect to GitHub to check for updates." : latest.error);
}

#ifndef _WIN32
std::string ShellQuoteSingle(std::string_view s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Public API Implementation
// ---------------------------------------------------------------------------

const char* UpdateStateName(UpdateState state) {
    switch (state) {
        case UpdateState::Idle: return "Idle";
        case UpdateState::Checking: return "Checking";
        case UpdateState::UpToDate: return "UpToDate";
        case UpdateState::UpdateAvailable: return "UpdateAvailable";
        case UpdateState::Downloading: return "Downloading";
        case UpdateState::ReadyToRestart: return "ReadyToRestart";
        case UpdateState::Error: return "Error";
    }
    return "Idle";
}

bool ParseSemVer(const std::string& text, SemVer& out) {
    out = SemVer{};
    std::string s = TrimAscii(text);
    if (s.empty()) return false;

    // Locate the first version core pattern: optional 'v'/'V' before digits.digits
    size_t pos = 0;
    bool found = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((s[i] == 'v' || s[i] == 'V') && i + 1 < s.size() &&
            std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
            (i == 0 || !std::isalnum(static_cast<unsigned char>(s[i - 1])))) {
            pos = i + 1;
            found = true;
            break;
        }
        if (std::isdigit(static_cast<unsigned char>(s[i])) &&
            (i == 0 || !std::isalpha(static_cast<unsigned char>(s[i - 1])))) {
            size_t d = i;
            while (d < s.size() && std::isdigit(static_cast<unsigned char>(s[d]))) ++d;
            if (d < s.size() && s[d] == '.' && d + 1 < s.size() &&
                std::isdigit(static_cast<unsigned char>(s[d + 1]))) {
                pos = i;
                found = true;
                break;
            }
        }
    }
    if (!found) return false;

    auto read_int = [&](int& val) -> bool {
        if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos]))) return false;
        int acc = 0;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            int digit = s[pos] - '0';
            if (acc > (1000000 - digit) / 10) return false;
            acc = acc * 10 + digit;
            ++pos;
        }
        val = acc;
        return true;
    };

    if (!read_int(out.major)) return false;
    if (pos >= s.size() || s[pos] != '.') return false;
    ++pos;
    if (!read_int(out.minor)) return false;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        if (!read_int(out.patch)) return false;
    } else {
        out.patch = 0;
    }

    if (pos < s.size() && s[pos] == '-') {
        ++pos;
        size_t start = pos;
        while (pos < s.size() && s[pos] != '+' && !std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        out.prerelease = s.substr(start, pos - start);
        out.is_prerelease = !out.prerelease.empty();
    }

    out.valid = true;
    return true;
}

int CompareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.is_prerelease != b.is_prerelease) {
        // Normal release outranks pre-release of the same major.minor.patch (e.g. 1.5.0 > 1.5.0-rc.1)
        return a.is_prerelease ? -1 : 1;
    }
    if (!a.is_prerelease) return 0;

    size_t pa = 0, pb = 0;
    while (pa < a.prerelease.size() || pb < b.prerelease.size()) {
        if (pa >= a.prerelease.size()) return -1;
        if (pb >= b.prerelease.size()) return 1;
        size_t ea = a.prerelease.find('.', pa);
        size_t eb = b.prerelease.find('.', pb);
        if (ea == std::string::npos) ea = a.prerelease.size();
        if (eb == std::string::npos) eb = b.prerelease.size();
        std::string_view ta(a.prerelease.data() + pa, ea - pa);
        std::string_view tb(b.prerelease.data() + pb, eb - pb);

        auto is_digits = [](std::string_view v) {
            if (v.empty()) return false;
            for (unsigned char c : v)
                if (!std::isdigit(c)) return false;
            return true;
        };
        bool na = is_digits(ta);
        bool nb = is_digits(tb);
        if (na && nb) {
            if (ta.size() != tb.size()) return ta.size() < tb.size() ? -1 : 1;
            int cmp = ta.compare(tb);
            if (cmp != 0) return cmp < 0 ? -1 : 1;
        } else if (na != nb) {
            return na ? -1 : 1;
        } else {
            int cmp = ta.compare(tb);
            if (cmp != 0) return cmp < 0 ? -1 : 1;
        }
        pa = ea + 1;
        pb = eb + 1;
    }
    return 0;
}

bool IsRemoteVersionNewer(const std::string& local_ver, const std::string& remote_tag_or_ver) {
    SemVer local_sv{}, remote_sv{};
    if (!ParseSemVer(local_ver, local_sv) || !ParseSemVer(remote_tag_or_ver, remote_sv)) return false;
    return CompareSemVer(remote_sv, local_sv) > 0;
}

std::string ComputeSha256Hex(const void* data, size_t size) {
    Sha256Hasher h;
    if (data && size > 0) h.Update(data, size);
    return h.FinalHex();
}

UpdateStatusSnapshot EvaluateReleaseJson(const std::string& json_text, bool manual_check) {
    JsonVal root;
    if (!JsonParser(json_text).Parse(root)) {
        return MakeErrorSnapshot(manual_check, "Invalid JSON payload from release server.");
    }

    if (root.type == JsonVal::Type::Object) {
        if (const JsonVal* msg = root.Find("message");
            msg && msg->type == JsonVal::Type::String && !root.Find("tag_name")) {
            std::string low = LowerAscii(msg->sval);
            if (low.find("not found") != std::string::npos) {
                return MakeUpToDateSnapshot(manual_check);
            }
            return MakeErrorSnapshot(manual_check, "GitHub API: " + msg->sval);
        }
        if (root.GetBool("draft", false)) {
            return MakeUpToDateSnapshot(manual_check);
        }
        return EvaluateReleaseObject(root, manual_check);
    }

    if (root.type == JsonVal::Type::Array) {
        if (root.arr.empty()) {
            return MakeUpToDateSnapshot(manual_check);
        }
        // Array of releases (contains tag_name) vs array of tags (contains name + commit)
        for (const JsonVal& item : root.arr) {
            if (item.type == JsonVal::Type::Object && item.Find("tag_name") && !item.GetBool("draft", false)) {
                return EvaluateReleaseObject(item, manual_check);
            }
        }
        return EvaluateTagsArray(root, manual_check);
    }

    return MakeErrorSnapshot(manual_check, "Unexpected release response format.");
}

bool ShouldAutoCheckForUpdates() {
    if (!TrimAscii(GetEnvUtf8("SUFFERING_UPDATE_MOCK_JSON")).empty()) {
        return true;
    }
    std::string disable_env = LowerAscii(TrimAscii(GetEnvUtf8("SUFFERING_DISABLE_UPDATE_CHECK")));
    if (disable_env == "1" || disable_env == "true" || disable_env == "yes") {
        return false;
    }
    if (app::IsReplayOrAutotestActive()) {
        return false;
    }
    for (const char* env_key : {"SUFFERING_AUTOTEST", "XBE_AUTO_START", "XBE_INPUT_REPLAY"}) {
        std::string v = TrimAscii(GetEnvUtf8(env_key));
        if (!v.empty() && v != "0") return false;
    }

    auto args = GetProcessArgsUtf8();
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--launcher-exit" || a == "--capture-ui-exit" || a == "--auto-start" || a == "--headless" ||
            a.rfind("--capture-launcher-bmp", 0) == 0 || a.rfind("--capture-ui-bmp", 0) == 0 ||
            a.rfind("--replay", 0) == 0 || a.rfind("--record", 0) == 0 ||
            a.rfind("--autotest", 0) == 0 || a.rfind("--validate-iso", 0) == 0) {
            return false;
        }
    }
    return true;
}

void StartUpdateCheck(bool manual) {
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        if (g_snapshot.state == UpdateState::Checking || g_snapshot.state == UpdateState::Downloading ||
            g_worker_active.load(std::memory_order_acquire)) {
            if (manual) g_snapshot.manual_check = true;
            return;
        }
    }

    // Offline deterministic mock support (evaluated synchronously so 10-frame UI captures are race-free)
    std::string mock = TrimAscii(GetEnvUtf8("SUFFERING_UPDATE_MOCK_JSON"));
    if (!mock.empty()) {
        UpdateStatusSnapshot result{};
        if (mock.front() == '{' || mock.front() == '[') {
            result = EvaluateReleaseJson(mock, manual);
        } else {
#ifdef _WIN32
            fs::path mp(Utf8ToWideStr(mock));
#else
            fs::path mp(mock);
#endif
            std::ifstream f(mp, std::ios::binary);
            if (!f) {
                result = MakeErrorSnapshot(manual, "Could not open SUFFERING_UPDATE_MOCK_JSON file.");
            } else {
                std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                result = EvaluateReleaseJson(data, manual);
            }
        }
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_snapshot = std::move(result);
        LOGR("update_checker: evaluated mock JSON -> state=%s latest=%s",
             UpdateStateName(g_snapshot.state), g_snapshot.latest_tag.c_str());
        return;
    }

    std::lock_guard<std::mutex> join_lk(g_worker_join_mutex);
    if (g_worker.joinable()) g_worker.join();

    g_shutdown_requested.store(false, std::memory_order_release);
    g_worker_active.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_snapshot.state = UpdateState::Checking;
        g_snapshot.manual_check = manual;
        g_snapshot.current_version = SUFFERING_VERSION_STR;
        g_snapshot.status_message = "Checking GitHub for updates...";
    }

    g_worker = std::thread([manual]() {
        UpdateStatusSnapshot res = PerformNetworkUpdateCheck(manual);
        if (!g_shutdown_requested.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_snapshot = std::move(res);
            LOGR("update_checker: check complete -> state=%s tag=%s self_update=%s",
                 UpdateStateName(g_snapshot.state), g_snapshot.latest_tag.c_str(),
                 g_snapshot.can_self_update ? "true" : "false");
        }
        g_worker_active.store(false, std::memory_order_release);
    });
}

UpdateStatusSnapshot GetUpdateSnapshot() {
    std::lock_guard<std::mutex> lk(g_state_mutex);
    UpdateStatusSnapshot copy = g_snapshot;
    if (copy.state == UpdateState::Downloading) {
        copy.downloaded_bytes = g_live_downloaded_bytes.load(std::memory_order_relaxed);
    }
    return copy;
}

void OpenReleasePageInBrowser(const std::string& url) {
    std::string target = TrimAscii(url);
    if (target.empty()) target = SUFFERING_RELEASES_URL;
    if (!IsAllowedReleasePageUrl(target)) {
        LOGW("update_checker: refused to open non-allowlisted URL: %s", target.c_str());
        return;
    }

#ifdef _WIN32
    std::wstring wurl = Utf8ToWideStr(target);
    ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        pid_t grand = fork();
        if (grand == 0) {
            setsid();
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > 2) close(devnull);
            }
            CloseExtraFdsInChild();
            const char* argv[] = {"xdg-open", target.c_str(), nullptr};
            execvp("xdg-open", const_cast<char* const*>(argv));
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
    }
#endif
}

bool StartDownloadAndStageUpdate() {
    std::string asset_url;
    std::string asset_name;
    std::string asset_sha256;
    std::string latest_tag;
    uint64_t expected_size = 0;

    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        if (g_snapshot.state == UpdateState::Checking || g_snapshot.state == UpdateState::Downloading ||
            g_worker_active.load(std::memory_order_acquire)) {
            return false;
        }
        if (g_snapshot.state == UpdateState::ReadyToRestart && !g_staged_root_dir.empty()) {
            return true;
        }
        if (!g_snapshot.can_self_update || g_snapshot.asset_url.empty() ||
            !IsAllowedAssetDownloadUrl(g_snapshot.asset_url)) {
            g_snapshot.state = UpdateState::Error;
            g_snapshot.status_message = "No allowlisted release archive available for automatic update.";
            return false;
        }
        asset_url = g_snapshot.asset_url;
        asset_name = SanitizeAssetFilename(g_snapshot.asset_name);
        asset_sha256 = g_snapshot.asset_sha256;
        latest_tag = g_snapshot.latest_tag;
        expected_size = g_snapshot.asset_size;
    }

    std::lock_guard<std::mutex> join_lk(g_worker_join_mutex);
    if (g_worker.joinable()) g_worker.join();

    g_shutdown_requested.store(false, std::memory_order_release);
    g_live_downloaded_bytes.store(0, std::memory_order_relaxed);
    g_worker_active.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_snapshot.state = UpdateState::Downloading;
        g_snapshot.downloaded_bytes = 0;
        g_snapshot.status_message = "Downloading " + asset_name + "...";
    }

    g_worker = std::thread([asset_url, asset_name, asset_sha256, latest_tag, expected_size]() mutable {
        fs::path exe_dir = app::ExeDir();
        fs::path staging_dir = exe_dir / "update_staging";
        std::error_code ec;
        fs::remove_all(staging_dir, ec);
        fs::create_directories(staging_dir, ec);
        if (ec) {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_snapshot.state = UpdateState::Error;
            g_snapshot.status_message =
                "Game folder is not writable (" + staging_dir.string() + "). Move the game to a writable folder.";
            g_worker_active.store(false, std::memory_order_release);
            return;
        }

        fs::path archive_path = staging_dir / asset_name;
        Sha256Hasher sha;
        std::string err;
        bool dl_ok = false;

        std::string mock_archive = TrimAscii(GetEnvUtf8("SUFFERING_UPDATE_MOCK_ARCHIVE"));
        if (!mock_archive.empty()) {
#ifdef _WIN32
            fs::path src_arch(Utf8ToWideStr(mock_archive));
#else
            fs::path src_arch(mock_archive);
#endif
            std::ifstream in(src_arch, std::ios::binary);
            std::ofstream out(archive_path, std::ios::binary);
            if (!in || !out) {
                err = "Could not copy SUFFERING_UPDATE_MOCK_ARCHIVE.";
            } else {
                std::vector<char> buf(32768);
                uint64_t total = 0;
                while (in) {
                    in.read(buf.data(), std::streamsize(buf.size()));
                    std::streamsize got = in.gcount();
                    if (got <= 0) break;
                    out.write(buf.data(), got);
                    sha.Update(buf.data(), size_t(got));
                    total += uint64_t(got);
                    g_live_downloaded_bytes.store(total, std::memory_order_relaxed);
                }
                dl_ok = out.good() && total > 0;
            }
        } else {
            dl_ok = HttpDownloadFilePlatform(asset_url, archive_path, expected_size, sha, err);
        }

        if (!dl_ok || g_shutdown_requested.load(std::memory_order_acquire)) {
            fs::remove_all(staging_dir, ec);
            if (!g_shutdown_requested.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(g_state_mutex);
                g_snapshot.state = UpdateState::Error;
                g_snapshot.status_message = err.empty() ? "Failed to download update archive." : err;
            }
            g_worker_active.store(false, std::memory_order_release);
            return;
        }

        uint64_t actual_size = fs::file_size(archive_path, ec);
        if (ec || actual_size == 0 || (expected_size > 0 && actual_size != expected_size)) {
            fs::remove_all(staging_dir, ec);
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_snapshot.state = UpdateState::Error;
            g_snapshot.status_message = "Downloaded archive size mismatch (" + std::to_string(actual_size) +
                                        " vs expected " + std::to_string(expected_size) + " bytes).";
            g_worker_active.store(false, std::memory_order_release);
            return;
        }

        std::string actual_sha = sha.FinalHex();
        if (!asset_sha256.empty() && LowerAscii(actual_sha) != LowerAscii(asset_sha256)) {
            fs::remove_all(staging_dir, ec);
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_snapshot.state = UpdateState::Error;
            g_snapshot.status_message = "Downloaded archive SHA-256 verification failed.";
            LOGE("update_checker: SHA-256 mismatch (got %s, expected %s)", actual_sha.c_str(), asset_sha256.c_str());
            g_worker_active.store(false, std::memory_order_release);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_snapshot.downloaded_bytes = actual_size;
            g_snapshot.status_message = "Extracting and verifying " + asset_name + "...";
        }

        fs::path extracted_dir = staging_dir / "extracted";
        fs::path staged_root;
        if (!ExtractArchiveToDir(archive_path, extracted_dir, err) ||
            !SanitizeAndLocateExtractedRoot(extracted_dir, staged_root, err)) {
            fs::remove_all(staging_dir, ec);
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_snapshot.state = UpdateState::Error;
            g_snapshot.status_message = err.empty() ? "Failed to extract update archive." : err;
            g_worker_active.store(false, std::memory_order_release);
            return;
        }

        // Remove the compressed archive now that extracted/ is verified to save disk space
        fs::remove(archive_path, ec);

        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_staging_dir = staging_dir;
            g_staged_root_dir = staged_root;
            g_snapshot.downloaded_bytes = actual_size;
            g_snapshot.state = UpdateState::ReadyToRestart;
            g_snapshot.status_message =
                "Update " + latest_tag + " staged and verified. Click Restart & Apply Update to finish.";
        }
        LOGR("update_checker: staged %s (%llu bytes, sha256=%s) at %s",
             asset_name.c_str(), static_cast<unsigned long long>(actual_size), actual_sha.c_str(),
             staged_root.string().c_str());
        g_worker_active.store(false, std::memory_order_release);
    });
    return true;
}

std::string BuildUpdateApplyScript(const fs::path& exe_dir,
                                   const fs::path& staged_root,
                                   const fs::path& staging_dir,
                                   uint32_t wait_pid) {
#ifdef _WIN32
    auto norm = [](fs::path p) {
        std::string s = WideToUtf8Str(p.make_preferred().wstring());
        while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
        return s;
    };
    std::string target_s = norm(exe_dir);
    std::string root_s = norm(staged_root);
    std::string stage_s = norm(staging_dir);

    std::ostringstream ss;
    ss << "@echo off\r\n"
       << "setlocal EnableExtensions\r\n"
       << "set \"WAIT_PID=" << wait_pid << "\"\r\n"
       << "set \"TARGET_DIR=" << target_s << "\"\r\n"
       << "set \"STAGED_ROOT=" << root_s << "\"\r\n"
       << "set \"STAGE_DIR=" << stage_s << "\"\r\n"
       << "cd /d \"%TARGET_DIR%\"\r\n"
       << "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
          "\"try { Wait-Process -Id " << wait_pid << " -Timeout 30 -ErrorAction SilentlyContinue } catch {}\" >nul 2>&1\r\n"
       << "rem Copy updated application files without touching settings.toml, hdd, assets, cache, textures, or .iso files\r\n"
       << "robocopy \"%STAGED_ROOT%\" \"%TARGET_DIR%\" /E /IS /IT /IM /NP /NFL /NDL /NJH /NJS /R:8 /W:1 "
          "/XF \"settings.toml\" \"*.iso\" \"*.xiso\" \"*.xbe\" \"*.7z\" \"built.txt\" "
          "/XD \"hdd\" \"assets\" \"cache\" \"logs\" \"CrashDumps\" \"crashdumps\" \"textures\" \"update_staging\" \"work\" >nul\r\n"
       << "if %ERRORLEVEL% GEQ 8 exit /b 1\r\n"
       << "if not exist \"%TARGET_DIR%\\assets\" mkdir \"%TARGET_DIR%\\assets\" >nul 2>&1\r\n"
       << "if exist \"%TARGET_DIR%\\compiler\\work\\suffering\\built.txt\" del /f /q \"%TARGET_DIR%\\compiler\\work\\suffering\\built.txt\" >nul 2>&1\r\n"
       << "start \"\" /D \"%TARGET_DIR%\" \"%TARGET_DIR%\\suffering.exe\"\r\n"
       << "(rd /s /q \"%STAGE_DIR%\" >nul 2>&1 & exit /b 0)\r\n";
    return ss.str();
#else
    std::string target_q = ShellQuoteSingle(exe_dir.string());
    std::string root_q = ShellQuoteSingle(staged_root.string());
    std::string stage_q = ShellQuoteSingle(staging_dir.string());

    std::ostringstream ss;
    ss << "#!/bin/sh\n"
       << "set -eu\n"
       << "WAIT_PID=" << wait_pid << "\n"
       << "TARGET_DIR=" << target_q << "\n"
       << "STAGED_ROOT=" << root_q << "\n"
       << "STAGE_DIR=" << stage_q << "\n"
       << "cd \"$TARGET_DIR\"\n"
       << "i=0\n"
       << "while [ \"$i\" -lt 150 ] && kill -0 \"$WAIT_PID\" 2>/dev/null; do\n"
       << "  sleep 0.2 2>/dev/null || sleep 1\n"
       << "  i=$((i + 1))\n"
       << "done\n"
       << "for entry in \"$STAGED_ROOT\"/*; do\n"
       << "  [ -e \"$entry\" ] || continue\n"
       << "  base=$(basename \"$entry\")\n"
       << "  case \"$base\" in\n"
       << "    settings.toml|hdd|assets|cache|logs|CrashDumps|crashdumps|textures|update_staging|*.iso|*.xiso|*.xbe|*.7z)\n"
       << "      continue\n"
       << "      ;;\n"
       << "    compiler)\n"
       << "      mkdir -p \"$TARGET_DIR/compiler\"\n"
       << "      for citem in \"$entry\"/*; do\n"
       << "        [ -e \"$citem\" ] || continue\n"
       << "        cbase=$(basename \"$citem\")\n"
       << "        [ \"$cbase\" = \"work\" ] && continue\n"
       << "        cp -rf \"$citem\" \"$TARGET_DIR/compiler/\"\n"
       << "      done\n"
       << "      ;;\n"
       << "    *)\n"
       << "      cp -rf \"$entry\" \"$TARGET_DIR/\"\n"
       << "      ;;\n"
       << "  esac\n"
       << "done\n"
       << "mkdir -p \"$TARGET_DIR/assets\"\n"
       << "rm -f \"$TARGET_DIR/compiler/work/suffering/built.txt\"\n"
       << "chmod 0755 \"$TARGET_DIR/suffering\" 2>/dev/null || true\n"
       << "[ -f \"$TARGET_DIR/install-steamdeck.sh\" ] && chmod 0755 \"$TARGET_DIR/install-steamdeck.sh\" 2>/dev/null || true\n"
       << "for tool in xbe_recomp clang ld.lld; do\n"
       << "  [ -f \"$TARGET_DIR/compiler/$tool\" ] && chmod 0755 \"$TARGET_DIR/compiler/$tool\" 2>/dev/null || true\n"
       << "done\n"
       << "rm -rf \"$STAGE_DIR\"\n"
       << "nohup \"$TARGET_DIR/suffering\" >/dev/null 2>&1 &\n"
       << "exit 0\n";
    return ss.str();
#endif
}

bool ApplyStagedUpdateAndRelaunch() {
    fs::path exe_dir = app::ExeDir();
    fs::path staged_root;
    fs::path staging_dir;
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        if (g_snapshot.state != UpdateState::ReadyToRestart || g_staged_root_dir.empty()) {
            return false;
        }
        staged_root = g_staged_root_dir;
        staging_dir = g_staging_dir.empty() ? (exe_dir / "update_staging") : g_staging_dir;
    }

    std::error_code ec;
#ifdef _WIN32
    if (!fs::is_regular_file(staged_root / "suffering.exe", ec)) {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_snapshot.state = UpdateState::Error;
        g_snapshot.status_message = "Staged executable missing. Please re-run Check for Updates.";
        return false;
    }
    uint32_t pid = static_cast<uint32_t>(GetCurrentProcessId());
    std::string script = BuildUpdateApplyScript(exe_dir, staged_root, staging_dir, pid);
    fs::path script_path = staging_dir / "update_apply.cmd";
    {
        std::ofstream out(script_path, std::ios::binary);
        if (!out) {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_snapshot.state = UpdateState::Error;
            g_snapshot.status_message = "Could not write update_apply.cmd helper script.";
            return false;
        }
        out.write(script.data(), std::streamsize(script.size()));
    }

    wchar_t sys_dir[MAX_PATH]{};
    UINT n = GetSystemDirectoryW(sys_dir, MAX_PATH);
    fs::path cmd_exe = (n > 0) ? (fs::path(sys_dir) / L"cmd.exe") : fs::path(L"cmd.exe");
    std::wstring cmd_line = L"\"" + cmd_exe.wstring() + L"\" /d /c \"\"" + script_path.wstring() + L"\"\"";

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(cmd_exe.c_str(), cmd_line.data(), nullptr, nullptr, FALSE,
                        flags, nullptr, exe_dir.c_str(), &si, &pi)) {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_snapshot.state = UpdateState::Error;
        g_snapshot.status_message = "Could not launch update_apply.cmd helper process.";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOGR("update_checker: launched update_apply.cmd for PID %u", pid);
    return true;
#else
    if (!fs::is_regular_file(staged_root / "suffering", ec)) {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_snapshot.state = UpdateState::Error;
        g_snapshot.status_message = "Staged executable missing. Please re-run Check for Updates.";
        return false;
    }
    uint32_t pid = static_cast<uint32_t>(getpid());
    std::string script = BuildUpdateApplyScript(exe_dir, staged_root, staging_dir, pid);
    fs::path script_path = staging_dir / "update_apply.sh";
    {
        std::ofstream out(script_path, std::ios::binary);
        if (!out) {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_snapshot.state = UpdateState::Error;
            g_snapshot.status_message = "Could not write update_apply.sh helper script.";
            return false;
        }
        out.write(script.data(), std::streamsize(script.size()));
    }
    chmod(script_path.c_str(), 0700);

    std::string script_s = script_path.string();
    pid_t child = fork();
    if (child < 0) {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_snapshot.state = UpdateState::Error;
        g_snapshot.status_message = "fork() failed while launching update_apply.sh.";
        return false;
    }
    if (child == 0) {
        pid_t grand = fork();
        if (grand == 0) {
            setsid();
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > 2) close(devnull);
            }
            CloseExtraFdsInChild();
            const char* argv[] = {"/bin/sh", script_s.c_str(), nullptr};
            execvp("/bin/sh", const_cast<char* const*>(argv));
            _exit(127);
        }
        _exit(0);
    }
    int st = 0;
    while (waitpid(child, &st, 0) < 0 && errno == EINTR) {
    }
    LOGR("update_checker: launched update_apply.sh for PID %u", pid);
    return true;
#endif
}

void ShutdownUpdateChecker() {
    g_shutdown_requested.store(true, std::memory_order_release);
    AbortActiveNetworkOp();
    std::lock_guard<std::mutex> join_lk(g_worker_join_mutex);
    if (g_worker.joinable()) {
        g_worker.join();
    }
    g_worker_active.store(false, std::memory_order_release);
}

}  // namespace suffering
