// Offline checks of the cache name repair; no game needed.
#include "../XenoPBEngineFix/XenoPBEngineFix.cpp"

#include <cstdio>

namespace
{
    int g_failures = 0;

    void Expect(const wchar_t* path, const wchar_t* expected)
    {
        const size_t kept = RepairedLength(path);
        const std::wstring actual = kept ? std::wstring(path).substr(0, kept) : std::wstring();
        if (actual != (expected ? expected : L""))
        {
            ++g_failures;
            std::fwprintf(stderr, L"FAIL %ls -> %ls\n", path, actual.c_str());
        }
    }
}

int main()
{
    const wchar_t* cache = L"C:\\Users\\U\\Documents\\SpaceRangersHD\\Cache\\";
    const std::wstring base(cache);
    // Leftovers seen in game logs.
    Expect((base + L"Desert2_2e.cmap.FNBBDCBJ4.txt").c_str(),
        (base + L"Desert2_2e.cmap.FNBBDCBJ").c_str());
    Expect((base + L"Mars4_3e.cmap.BHAKNFNA\\BPele").c_str(),
        (base + L"Mars4_3e.cmap.BHAKNFNA").c_str());
    // Intact names and unrelated files stay untouched.
    Expect((base + L"Desert2_2e.cmap.FNBBDCBJ").c_str(), nullptr);
    Expect((base + L"Desert2_2e.cmap.FNBB").c_str(), nullptr);
    Expect((base + L"Desert2_2e.cmap.FNBBDCBZtail").c_str(), nullptr);
    Expect(L"D:\\Game\\Matrix\\Desert2_2e.cmap.FNBBDCBJ4.txt", nullptr);
    Expect(L"D:\\Game\\cfg\\robots\\data.txt", nullptr);
    Expect(nullptr, nullptr);
    // The ANSI path uses the same rule.
    const char* ansi = "C:\\Docs\\SpaceRangersHD\\CACHE\\Training.cmap.PHBCAAAAjunk";
    if (RepairedLength(ansi) != std::strlen(ansi) - 4)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL ansi\n");
    }
    std::printf("native_tests=%s failures=%d\n", g_failures ? "failed" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
