// Helpers every plugin-hosting program shares (backline-plugin-worker and
// backline-slot-host): the format-free loading entry point, plugin lookup,
// and output helpers.
// SPDX-License-Identifier: MIT
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>

#include "plugin.h"

namespace bl {

FILE *out = stdout;
const char *prog = "backline-plugin-worker";

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

std::string slug(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c))
            o += (char)tolower(c);
        else if (!o.empty() && o.back() != '-')
            o += '-';
    }
    while (!o.empty() && o.back() == '-')
        o.pop_back();
    return o.empty() ? "plugin" : o;
}

std::string json_str(const std::string &v)
{
    std::string o = "\"";
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

static bool ends_with(const std::string &s, const char *suf)
{
    size_t n = strlen(suf);
    return s.size() >= n && !strcasecmp(s.c_str() + s.size() - n, suf);
}

static bool is_uri(const std::string &s)
{
    return s.find("://") != std::string::npos || s.rfind("urn:", 0) == 0;
}

std::pair<Fmt, std::string> resolve_plugin(std::string p, Fmt want)
{
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    struct stat st;
    if (stat(p.c_str(), &st) == 0) {
        Fmt f = want;
        if (f == Fmt::Auto)
            f = ends_with(p, ".vst3") ? Fmt::Vst3 : ends_with(p, ".lv2") && S_ISDIR(st.st_mode) ? Fmt::Lv2 : Fmt::Clap;
        return {f, p};
    }
    if (is_uri(p) && (want == Fmt::Auto || want == Fmt::Lv2))
        return {Fmt::Lv2, p};
    const char *h = getenv("HOME");
    std::string home = h ? h : "";
    struct Dir { Fmt f; std::string dir; const char *suffix; };
    const Dir dirs[] = {
        {Fmt::Clap, home + "/.clap", ".clap"}, {Fmt::Clap, "/usr/lib/clap", ".clap"},
        {Fmt::Clap, "/usr/local/lib/clap", ".clap"},
        {Fmt::Vst3, home + "/.vst3", ".vst3"}, {Fmt::Vst3, "/usr/lib/vst3", ".vst3"},
        {Fmt::Vst3, "/usr/local/lib/vst3", ".vst3"},
        {Fmt::Lv2, home + "/.lv2", ".lv2"}, {Fmt::Lv2, "/usr/lib/lv2", ".lv2"}, {Fmt::Lv2, "/usr/local/lib/lv2", ".lv2"},
    };
    std::string looked;
    for (auto &d : dirs) {
        if (want != Fmt::Auto && want != d.f)
            continue;
        std::string c = d.dir + "/" + p + (ends_with(p, d.suffix) ? "" : d.suffix);
        if (stat(c.c_str(), &st) == 0)
            return {d.f, c};
        looked += (looked.empty() ? "" : ", ") + d.dir;
    }
    die("plugin not found: %s (looked in %s)", p.c_str(), looked.c_str());
}

std::unique_ptr<PluginInstance> load_plugin(Host *h, Fmt f, const std::string &path, const std::string &plugin_id, bool list_only)
{
    switch (f) {
    case Fmt::Vst3:
#ifdef BL_HAVE_VST3
        return load_vst3(h, path, plugin_id, list_only);
#else
        die("built without VST3: cannot host %s (configure with the VST3 SDK, BACKLINE_VST3=ON)", path.c_str());
#endif
    case Fmt::Lv2:
#ifdef BL_HAVE_LV2
        return load_lv2(h, path, plugin_id, list_only);
#else
        die("built without LV2: cannot host %s (configure with lilv, BACKLINE_LV2=ON)", path.c_str());
#endif
    default:
        return load_clap(h, path, plugin_id, list_only);
    }
}

} // namespace bl
