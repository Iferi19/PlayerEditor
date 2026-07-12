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
    // 子プロセスを「窓なし」で起動し、stdout+stderr をまとめて取得する。
    // cmd.exe を介さないので一瞬のコンソール窓も出ない。
    std::string runHiddenW(const std::wstring& cmdline)
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

    std::string locate()
    {
#ifdef _WIN32
        const char* exe = "ffmpeg.exe";
        if (fileExists(std::string("ffmpeg/") + exe)) return std::string("ffmpeg/") + exe;
        const char* lad = std::getenv("LOCALAPPDATA");
        if (lad)
        {
            std::string w = std::string(lad) + "\\Microsoft\\WinGet\\Links\\ffmpeg.exe";
            if (fileExists(w)) return w;
        }
        for (const char* c : { "C:\\ffmpeg\\bin\\ffmpeg.exe", "C:\\ProgramData\\chocolatey\\bin\\ffmpeg.exe" })
            if (fileExists(c)) return c;
        // PATH 探索も窓なしで
        std::string r = runHiddenW(L"where.exe ffmpeg");
        if (!r.empty())
        {
            auto nl = r.find_first_of("\r\n");
            std::string first = (nl == std::string::npos) ? r : r.substr(0, nl);
            if (fileExists(first)) return first;
        }
#else
        for (const char* c : { "/usr/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/opt/homebrew/bin/ffmpeg" })
            if (fileExists(c)) return c;
        std::string r = runCapture("which ffmpeg 2>/dev/null");
        if (!r.empty())
        {
            auto nl = r.find_first_of("\r\n");
            std::string first = (nl == std::string::npos) ? r : r.substr(0, nl);
            if (fileExists(first)) return first;
        }
#endif
        return "";
    }
}

namespace Ffmpeg
{
    const std::string& findFfmpeg()
    {
        static std::string cached;
        static bool searched = false;
        if (!searched) { cached = locate(); searched = true; }
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
}
