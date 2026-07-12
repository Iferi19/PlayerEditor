#pragma once
// UTF-8 <-> UTF-16 変換（Windows専用）。複数の .cpp から使うため inline で共通化。
#ifdef _WIN32
#include <string>
#include <windows.h>

namespace plat
{
    inline std::wstring utf8ToWide(const std::string& s)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (n <= 0) return std::wstring();
        std::wstring w(n - 1, L'\0');           // n には終端分(+1)が含まれるので中身は n-1
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);  // C++17: data() は書込可
        return w;
    }

    inline std::string wideToUtf8(const std::wstring& w)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();
        std::string s(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    }
}
#endif
