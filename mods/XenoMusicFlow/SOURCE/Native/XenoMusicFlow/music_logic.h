// Game-independent decisions of XenoMusicFlow; native_tests.cpp checks them
// without Rangers.exe.
#pragma once

#include <cwctype>
#include <string>
#include <vector>

namespace xmf
{
    struct Settings
    {
        // How long the old track keeps playing after the location changed.
        unsigned holdMs = 45000;
        // Length of the crossfade from the old track to the new one.
        unsigned crossfadeMs = 8000;
        // With this little left the old track plays to its end instead.
        unsigned endGraceMs = 40000;
        // Categories that always switch at once (battles, menus, endings).
        // Every other one is the music of a place: space, planets, stations
        // of the game and of mods.
        std::vector<std::wstring> immediatePatterns;
    };

    inline std::vector<std::wstring> SplitPatterns(const std::wstring& text)
    {
        std::vector<std::wstring> result;
        std::wstring current;
        for (const wchar_t ch : text + L";")
        {
            if (ch == L';' || ch == L',')
            {
                while (!current.empty() && iswspace(current.back())) current.pop_back();
                size_t start = 0;
                while (start < current.size() && iswspace(current[start])) ++start;
                if (start < current.size()) result.push_back(current.substr(start));
                current.clear();
            }
            else
            {
                current.push_back(ch);
            }
        }
        return result;
    }

    // Case-insensitive; a trailing '*' matches any rest ("Nation.*").
    inline bool CategoryMatches(const std::wstring& category, const std::vector<std::wstring>& patterns)
    {
        for (const auto& pattern : patterns)
        {
            const bool prefix = !pattern.empty() && pattern.back() == L'*';
            const size_t length = prefix ? pattern.size() - 1 : pattern.size();
            if (prefix ? category.size() < length : category.size() != length) continue;
            bool equal = true;
            for (size_t i = 0; i < length && equal; ++i)
            {
                equal = towlower(category[i]) == towlower(pattern[i]);
            }
            if (equal) return true;
        }
        return false;
    }

    // Estimates how long the playing track still lasts from how far its
    // decoder has read the compressed file. The game streams the file through
    // two alternating read buffers, so consumption is the sum of the buffers
    // already swapped out plus the position in the current one.
    class TrackProgress
    {
    public:
        // The decoder runs this far ahead of what is audible (three 2 s
        // chunks of PCM, partly played).
        static constexpr unsigned kDecodeLeadMs = 5000;
        static constexpr unsigned kMinimumElapsedMs = 8000;

        void Observe(const void* stream, const void* readBuffer, unsigned readPosition,
            unsigned readAvailable, unsigned fillAvailable, unsigned fileSize,
            bool endOfFile, unsigned nowMs)
        {
            if (stream != stream_)
            {
                stream_ = stream;
                startMs_ = nowMs;
                swappedBytes_ = 0;
                readBuffer_ = readBuffer;
                bufferFill_ = readPosition + readAvailable;
            }
            else if (readBuffer != readBuffer_)
            {
                swappedBytes_ += bufferFill_;
                readBuffer_ = readBuffer;
                bufferFill_ = readPosition + readAvailable;
            }
            else
            {
                bufferFill_ = readPosition + readAvailable;
            }
            consumed_ = swappedBytes_ + readPosition;
            unreadBuffered_ = readAvailable + fillAvailable;
            fileSize_ = fileSize;
            endOfFile_ = endOfFile;
            nowMs_ = nowMs;
        }

        void Reset()
        {
            *this = TrackProgress();
        }

        const void* Stream() const { return stream_; }

        // Remaining audible time in ms, or -1 while it cannot be estimated.
        long long RemainingMs() const
        {
            if (!stream_ || fileSize_ == 0) return -1;
            const unsigned elapsed = nowMs_ - startMs_;
            if (elapsed < kMinimumElapsedMs || consumed_ == 0) return -1;
            // Bytes of the file per second of audio.
            const double rate = static_cast<double>(consumed_) * 1000.0 / (elapsed + kDecodeLeadMs);
            if (rate <= 0.0) return -1;
            if (endOfFile_)
            {
                // The whole rest of the file is in memory: exact byte count.
                return static_cast<long long>(unreadBuffered_ * 1000.0 / rate) + kDecodeLeadMs;
            }
            const double total = fileSize_ * 1000.0 / rate;
            const double remaining = total - elapsed;
            return remaining > 0 ? static_cast<long long>(remaining) : 0;
        }

    private:
        const void* stream_ = nullptr;
        const void* readBuffer_ = nullptr;
        unsigned startMs_ = 0;
        unsigned nowMs_ = 0;
        unsigned long long swappedBytes_ = 0;
        unsigned long long consumed_ = 0;
        unsigned bufferFill_ = 0;
        unsigned unreadBuffered_ = 0;
        unsigned fileSize_ = 0;
        bool endOfFile_ = false;
    };

    // An empty category comes from MusicControl resetting its playlist.
    inline bool IsPlaceCategory(const std::wstring& category, const Settings& settings)
    {
        return !category.empty() && !CategoryMatches(category, settings.immediatePatterns);
    }

    enum class PendingAction
    {
        Wait,      // keep the old track for now
        Crossfade, // blend into a track of the new location
        LetEnd     // the old track ends soon; switch normally after it
    };

    inline PendingAction DecidePending(unsigned pendingMs, long long remainingMs, const Settings& settings)
    {
        if (pendingMs < settings.holdMs) return PendingAction::Wait;
        // An unknown remainder means the track has only just started.
        if (remainingMs >= 0 &&
            remainingMs <= static_cast<long long>(settings.endGraceMs) + settings.crossfadeMs)
        {
            return PendingAction::LetEnd;
        }
        return PendingAction::Crossfade;
    }

    // The game ramps volume by Step every Interval ms (100 ms, +-0.1, so one
    // second). Stretch it over the crossfade, keeping the direction.
    inline float CrossfadeStep(unsigned intervalMs, float step, unsigned crossfadeMs)
    {
        if (crossfadeMs == 0 || intervalMs == 0) return step;
        const float magnitude = static_cast<float>(intervalMs) / static_cast<float>(crossfadeMs);
        return step < 0 ? -magnitude : magnitude;
    }
}
