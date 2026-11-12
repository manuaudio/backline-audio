// Minimal OSC 1.0 codec (UDP messages, no bundles): enough for parameter
// control. SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

namespace osc {

using Arg = std::variant<int32_t, float, double, std::string>;

struct Message {
    std::string address;
    std::vector<Arg> args;
};

inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline size_t pad4(size_t n) { return (n + 3) & ~size_t(3); }

// Returns false on a malformed packet. Bundles ('#bundle') are not supported.
inline bool parse(const uint8_t *buf, size_t len, Message &out)
{
    auto read_str = [&](size_t &pos, std::string &s) {
        const void *end = memchr(buf + pos, 0, len - pos);
        if (pos >= len || !end)
            return false;
        size_t n = (const uint8_t *)end - (buf + pos);
        s.assign((const char *)buf + pos, n);
        pos += pad4(n + 1);
        return pos <= len;
    };
    size_t pos = 0;
    std::string tags;
    if (!read_str(pos, out.address) || out.address.empty() || out.address[0] != '/')
        return false;
    if (pos == len)
        return true; // no type tags: no arguments
    if (!read_str(pos, tags) || tags.empty() || tags[0] != ',')
        return false;
    for (size_t i = 1; i < tags.size(); i++) {
        switch (tags[i]) {
        case 'i':
            if (pos + 4 > len) return false;
            out.args.emplace_back((int32_t)be32(buf + pos));
            pos += 4;
            break;
        case 'f': {
            if (pos + 4 > len) return false;
            uint32_t u = be32(buf + pos);
            float f;
            memcpy(&f, &u, 4);
            out.args.emplace_back(f);
            pos += 4;
            break;
        }
        case 'd': {
            if (pos + 8 > len) return false;
            uint64_t u = (uint64_t)be32(buf + pos) << 32 | be32(buf + pos + 4);
            double d;
            memcpy(&d, &u, 8);
            out.args.emplace_back(d);
            pos += 8;
            break;
        }
        case 's': {
            std::string s;
            if (!read_str(pos, s)) return false;
            out.args.emplace_back(std::move(s));
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

class Writer {
public:
    explicit Writer(const std::string &address) { str(address); tags_ = ","; }
    Writer &i(int32_t v) { tags_ += 'i'; u32((uint32_t)v); return *this; }
    Writer &f(float v) { uint32_t u; memcpy(&u, &v, 4); tags_ += 'f'; u32(u); return *this; }
    Writer &d(double v) { uint64_t u; memcpy(&u, &v, 8); tags_ += 'd'; u32(u >> 32); u32((uint32_t)u); return *this; }
    Writer &s(const std::string &v) { tags_ += 's'; strbuf(args_, v); return *this; }
    std::vector<uint8_t> bytes() const
    {
        std::vector<uint8_t> out = head_;
        strbuf(out, tags_);
        out.insert(out.end(), args_.begin(), args_.end());
        return out;
    }

private:
    static void strbuf(std::vector<uint8_t> &b, const std::string &v)
    {
        b.insert(b.end(), v.begin(), v.end());
        b.resize(b.size() + (pad4(v.size() + 1) - v.size()), 0);
    }
    void str(const std::string &v) { strbuf(head_, v); }
    void u32(uint32_t v)
    {
        uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        args_.insert(args_.end(), b, b + 4);
    }
    std::vector<uint8_t> head_, args_;
    std::string tags_;
};

inline bool as_double(const Arg &a, double &out)
{
    if (auto p = std::get_if<float>(&a)) { out = *p; return true; }
    if (auto p = std::get_if<double>(&a)) { out = *p; return true; }
    if (auto p = std::get_if<int32_t>(&a)) { out = *p; return true; }
    return false;
}

} // namespace osc
