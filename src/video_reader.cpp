#include "video_reader.h"
#include "ffmpeg.h"
#include <chrono>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include "platform_utf8.h"
#endif

namespace
{
    const size_t kMaxQueue = 4;   // ここで詰まらせて ffmpeg に自然なバックプレッシャを掛ける
}

VideoReader::~VideoReader() { stop(); }

bool VideoReader::start(const std::string& path, double t, int outW, int outH, double fps)
{
    stop();
    if (!Ffmpeg::available() || outW <= 0 || outH <= 0 || fps <= 0) return false;

    char ss[32];
    std::snprintf(ss, sizeof(ss), "%.3f", t < 0 ? 0.0 : t);
    char scale[64];
    std::snprintf(scale, sizeof(scale), "scale=%d:%d:flags=lanczos", outW, outH);

#ifdef _WIN32
    std::wstring cmd = L"\"" + plat::utf8ToWide(Ffmpeg::findFfmpeg()) + L"\""
        L" -hide_banner -loglevel error"
        L" -ss " + plat::utf8ToWide(ss) +
        L" -i \"" + plat::utf8ToWide(path) + L"\""
        L" -an -vf " + plat::utf8ToWide(scale) +
        L" -f rawvideo -pix_fmt rgb24 -";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return false; }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    pipe_ = rd;
#else
    char cmd[2048];
    std::snprintf(cmd, sizeof(cmd),
        "\"%s\" -hide_banner -loglevel error -ss %s -i \"%s\" -an -vf %s -f rawvideo -pix_fmt rgb24 - 2>/dev/null",
        Ffmpeg::findFfmpeg().c_str(), ss, path.c_str(), scale);
    FILE* p = popen(cmd, "r");
    if (!p) return false;
    pipe_ = p;
#endif

    stopFlag_ = false;
    running_ = true;
    startT_ = t < 0 ? 0.0 : t;
    size_t frameSize = (size_t)outW * outH * 3;
    thread_ = std::thread(&VideoReader::readLoop, this, frameSize, startT_, fps);
    return true;
}

void VideoReader::readLoop(size_t frameSize, double t, double fps)
{
    long long n = 0;
    std::vector<uint8_t> buf(frameSize);

    for (;;)
    {
        if (stopFlag_) break;

        // キューが満杯なら待つ(消費されるまでデコードを進めない=バックプレッシャ)
        bool full;
        {
            std::lock_guard<std::mutex> lk(m_);
            full = queue_.size() >= kMaxQueue;
        }
        if (full)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        size_t got = 0;
        while (got < frameSize && !stopFlag_)
        {
#ifdef _WIN32
            DWORD rd = 0;
            if (!ReadFile((HANDLE)pipe_, buf.data() + got, (DWORD)(frameSize - got), &rd, nullptr) || rd == 0)
                break;
            got += rd;
#else
            size_t rd = fread(buf.data() + got, 1, frameSize - got, (FILE*)pipe_);
            if (rd == 0) break;
            got += rd;
#endif
        }
        if (got < frameSize) break;   // EOF/エラー/停止

        Frame f;
        f.rgb = buf;   // コピー(bufは再利用)
        f.pts = t + (double)n / fps;
        n++;
        {
            std::lock_guard<std::mutex> lk(m_);
            queue_.push_back(std::move(f));
        }
    }
    running_ = false;
}

void VideoReader::stop()
{
    stopFlag_ = true;
#ifdef _WIN32
    if (proc_) TerminateProcess((HANDLE)proc_, 0);
#endif
    if (thread_.joinable()) thread_.join();
#ifdef _WIN32
    if (pipe_) { CloseHandle((HANDLE)pipe_); pipe_ = nullptr; }
    if (proc_) { CloseHandle((HANDLE)proc_); proc_ = nullptr; }
#else
    if (pipe_) { pclose((FILE*)pipe_); pipe_ = nullptr; }
#endif
    std::lock_guard<std::mutex> lk(m_);
    queue_.clear();
    running_ = false;
    startT_ = -1;
}

bool VideoReader::popUpTo(double maxPts, Frame& out)
{
    std::lock_guard<std::mutex> lk(m_);
    bool found = false;
    while (!queue_.empty() && queue_.front().pts <= maxPts)
    {
        out = std::move(queue_.front());
        queue_.pop_front();
        found = true;
    }
    return found;
}

bool VideoReader::popFirst(Frame& out)
{
    std::lock_guard<std::mutex> lk(m_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}
