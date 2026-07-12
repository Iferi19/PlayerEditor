#include "ffmpeg.h"
#include "platform_utf8.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    bool fileExists(const std::string& p)
    {
        std::ifstream f(p);
        return f.good();
    }

#ifdef _WIN32
    // CreateProcess 用に引数を1つクォート（内部の " は \" に）。
    std::wstring quoteW(const std::wstring& s)
    {
        std::wstring r = L"\"";
        for (wchar_t c : s) { if (c == L'"') r += L"\\\""; else r += c; }
        r += L"\"";
        return r;
    }

    // 子プロセスを「窓なし」で起動し、stdout+stderr をまとめて取得する。
    // cmd.exe を介さないので一瞬のコンソール窓も出ない。exitCode に終了コードを返す(任意)。
    std::string runHiddenW(const std::wstring& cmdline, DWORD* exitCode = nullptr)
    {
        std::string out;
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return out;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);  // 親の読み取り端は継承しない

        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = wr;
        si.hStdError = wr;

        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
        buf.push_back(L'\0');

        BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(wr);   // 親は書かない
        if (!ok)
        {
            CloseHandle(rd);
            if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
            return out;
        }

        char tmp[4096];
        DWORD n = 0;
        while (ReadFile(rd, tmp, sizeof(tmp), &n, nullptr) && n > 0)
            out.append(tmp, n);

        CloseHandle(rd);
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (exitCode) { DWORD ec = 1; GetExitCodeProcess(pi.hProcess, &ec); *exitCode = ec; }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        return out;
    }
#else
    std::string runCapture(const std::string& cmd)
    {
        std::string out;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return out;
        std::array<char, 4096> buf;
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
            out.append(buf.data(), n);
        pclose(pipe);
        return out;
    }
#endif

    double parseNum(const std::string& s, const std::string& key)
    {
        auto p = s.find("\"" + key + "\"");
        if (p == std::string::npos) return NAN;
        p = s.find(':', p);
        if (p == std::string::npos) return NAN;
        auto q1 = s.find('"', p);
        if (q1 == std::string::npos) return NAN;
        auto q2 = s.find('"', q1 + 1);
        if (q2 == std::string::npos) return NAN;
        std::string v = s.substr(q1 + 1, q2 - q1 - 1);
        if (v == "-inf") return -INFINITY;
        if (v == "inf") return INFINITY;
        try { return std::stod(v); } catch (...) { return NAN; }
    }

    std::string locate(const char* tool)
    {
#ifdef _WIN32
        std::string exe = std::string(tool) + ".exe";
        if (fileExists("ffmpeg/" + exe)) return "ffmpeg/" + exe;
        const char* lad = std::getenv("LOCALAPPDATA");
        if (lad)
        {
            std::string w = std::string(lad) + "\\Microsoft\\WinGet\\Links\\" + exe;
            if (fileExists(w)) return w;
        }
        for (std::string c : { "C:\\ffmpeg\\bin\\" + exe, "C:\\ProgramData\\chocolatey\\bin\\" + exe })
            if (fileExists(c)) return c;
        // PATH 探索も窓なしで
        std::string r = runHiddenW(L"where.exe " + plat::utf8ToWide(tool));
        if (!r.empty())
        {
            auto nl = r.find_first_of("\r\n");
            std::string first = (nl == std::string::npos) ? r : r.substr(0, nl);
            if (fileExists(first)) return first;
        }
#else
        for (std::string c : { std::string("/usr/bin/") + tool, std::string("/usr/local/bin/") + tool,
                               std::string("/opt/homebrew/bin/") + tool })
            if (fileExists(c)) return c;
        std::string r = runCapture(std::string("which ") + tool + " 2>/dev/null");
        if (!r.empty())
        {
            auto nl = r.find_first_of("\r\n");
            std::string first = (nl == std::string::npos) ? r : r.substr(0, nl);
            if (fileExists(first)) return first;
        }
#endif
        return "";
    }

    // JSON の "key" : "value" の value を軽くアンエスケープ
    std::string jsonUnescape(const std::string& s)
    {
        std::string r;
        for (size_t i = 0; i < s.size(); i++)
        {
            if (s[i] == '\\' && i + 1 < s.size())
            {
                char c = s[++i];
                switch (c)
                {
                    case 'n': r += '\n'; break;
                    case 't': r += '\t'; break;
                    case 'r': break;
                    case 'u': i += 4; r += '?'; break;   // 簡易対応
                    default: r += c; break;              // \" \\ \/ など
                }
            }
            else r += s[i];
        }
        return r;
    }
}

namespace Ffmpeg
{
    const std::string& findFfmpeg()
    {
        static std::string cached;
        static bool searched = false;
        if (!searched) { cached = locate("ffmpeg"); searched = true; }
        return cached;
    }

    const std::string& findFfprobe()
    {
        static std::string cached;
        static bool searched = false;
        if (!searched)
        {
            // まず ffmpeg と同じフォルダを見る
            const std::string& ff = findFfmpeg();
            if (!ff.empty())
            {
                auto slash = ff.find_last_of("/\\");
                if (slash != std::string::npos)
                {
#ifdef _WIN32
                    std::string cand = ff.substr(0, slash + 1) + "ffprobe.exe";
#else
                    std::string cand = ff.substr(0, slash + 1) + "ffprobe";
#endif
                    if (fileExists(cand)) cached = cand;
                }
            }
            if (cached.empty()) cached = locate("ffprobe");
            searched = true;
        }
        return cached;
    }

    bool available() { return !findFfmpeg().empty(); }

    LoudnessResult measure(const std::string& input, std::string& err)
    {
        LoudnessResult res;
        const std::string& ff = findFfmpeg();
        if (ff.empty()) { err = "ffmpeg が見つかりません。"; return res; }

        const char* args = " -hide_banner -nostdin -nostats -i ";
        const char* flt = " -map 0:a:0 -af loudnorm=I=-14:TP=-1:LRA=11:print_format=json -f null -";
#ifdef _WIN32
        std::wstring wcmd = L"\"" + plat::utf8ToWide(ff) + L"\"" + plat::utf8ToWide(args) +
                            L"\"" + plat::utf8ToWide(input) + L"\"" + plat::utf8ToWide(flt);
        std::string out = runHiddenW(wcmd);
#else
        std::string cmd = "\"" + ff + "\"" + args + "\"" + input + "\"" + flt + " 2>&1";
        std::string out = runCapture(cmd);
#endif

        double i = parseNum(out, "input_i");
        double tp = parseNum(out, "input_tp");
        double lra = parseNum(out, "input_lra");
        if (std::isnan(i)) { err = "loudnorm 出力を解析できませんでした。"; return res; }

        res.integratedLufs = i;
        res.truePeakDb = tp;
        res.loudnessRange = lra;
        res.ok = true;
        return res;
    }

    Tags readTags(const std::string& input)
    {
        Tags t;
        const std::string& fp = findFfprobe();
        if (fp.empty()) return t;

#ifdef _WIN32
        std::wstring cmd = quoteW(plat::utf8ToWide(fp)) +
            L" -v quiet -print_format json -show_format " + quoteW(plat::utf8ToWide(input));
        std::string out = runHiddenW(cmd);
#else
        std::string out = runCapture("\"" + fp + "\" -v quiet -print_format json -show_format \"" + input + "\" 2>/dev/null");
#endif

        // "tags" オブジェクト内の "key": "value" を素朴に走査
        auto tpos = out.find("\"tags\"");
        if (tpos == std::string::npos) return t;
        auto brace = out.find('{', tpos);
        if (brace == std::string::npos) return t;
        auto end = out.find('}', brace);   // tags は文字列のみでネストしない
        if (end == std::string::npos) return t;

        size_t i = brace + 1;
        while (i < end)
        {
            auto k1 = out.find('"', i);
            if (k1 == std::string::npos || k1 >= end) break;
            auto k2 = out.find('"', k1 + 1);
            if (k2 == std::string::npos || k2 >= end) break;
            std::string key = out.substr(k1 + 1, k2 - k1 - 1);

            auto colon = out.find(':', k2);
            if (colon == std::string::npos || colon >= end) break;
            auto v1 = out.find('"', colon);
            if (v1 == std::string::npos || v1 >= end) break;
            size_t v2 = v1 + 1;
            while (v2 < end && !(out[v2] == '"' && out[v2 - 1] != '\\')) v2++;
            std::string val = jsonUnescape(out.substr(v1 + 1, v2 - v1 - 1));
            i = v2 + 1;

            std::string lk;
            for (char c : key) lk += (char)tolower((unsigned char)c);
            if (lk == "title") t.title = val;
            else if (lk == "artist") t.artist = val;
            else if (lk == "album") t.album = val;
            else if (lk == "album_artist" || lk == "albumartist") t.albumArtist = val;
            else if (lk == "genre") t.genre = val;
            else if (lk == "date" || lk == "year") t.year = val;
            else if (lk == "track" || lk == "tracknumber") t.track = val;
        }
        return t;
    }

    bool transcode(const std::string& inWav, const std::string& outPath,
                   const std::string& fmt, const Tags& tags, std::string& err,
                   int outSampleRate, int bitDepth)
    {
        const std::string& ff = findFfmpeg();
        if (ff.empty()) { err = "ffmpeg が見つかりません。"; return false; }

        std::vector<std::string> args = { "-y", "-hide_banner", "-loglevel", "error", "-i", inWav };

        std::vector<std::string> codec;
        if (fmt == "wav")
        {
            const char* c = (bitDepth == 16) ? "pcm_s16le" : (bitDepth == 32) ? "pcm_f32le" : "pcm_s24le";
            codec = { "-c:a", c };
        }
        else if (fmt == "aif" || fmt == "aiff")
        {
            const char* c = (bitDepth == 16) ? "pcm_s16be" : "pcm_s24be";
            codec = { "-c:a", c };
        }
        else if (fmt == "mp3") codec = { "-c:a", "libmp3lame", "-b:a", "320k" };
        else if (fmt == "m4a") codec = { "-c:a", "aac", "-b:a", "256k" };
        else if (fmt == "flac")
        {
            codec = { "-c:a", "flac", "-sample_fmt", (bitDepth == 16) ? "s16" : "s32" };
        }
        else if (fmt == "ogg") codec = { "-c:a", "libvorbis", "-q:a", "6" };
        for (auto& c : codec) args.push_back(c);

        if (outSampleRate > 0)
        {
            args.push_back("-ar");
            args.push_back(std::to_string(outSampleRate));
        }

        auto addMeta = [&](const char* k, const std::string& v) {
            if (!v.empty()) { args.push_back("-metadata"); args.push_back(std::string(k) + "=" + v); }
        };
        addMeta("title", tags.title);
        addMeta("artist", tags.artist);
        addMeta("album", tags.album);
        addMeta("album_artist", tags.albumArtist);
        addMeta("genre", tags.genre);
        addMeta("date", tags.year);
        addMeta("track", tags.track);

        args.push_back(outPath);

#ifdef _WIN32
        std::wstring cmd = quoteW(plat::utf8ToWide(ff));
        for (auto& a : args) cmd += L" " + quoteW(plat::utf8ToWide(a));
        DWORD ec = 1;
        std::string log = runHiddenW(cmd, &ec);
        if (ec != 0) { err = "エンコード失敗: " + (log.empty() ? std::string("ffmpeg error") : log.substr(0, 400)); return false; }
        return true;
#else
        std::string cmd = "\"" + ff + "\"";
        for (auto& a : args) cmd += " \"" + a + "\"";
        cmd += " 2>&1";
        std::string log = runCapture(cmd);
        std::ifstream f(outPath);
        if (!f.good()) { err = "エンコード失敗: " + log.substr(0, 400); return false; }
        return true;
#endif
    }
}
