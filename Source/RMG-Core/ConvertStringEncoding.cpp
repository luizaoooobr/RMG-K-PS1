/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "ConvertStringEncoding.hpp"
#include "Library.hpp"

#include <iconv.h>

#include <mutex>
#include <string>
#include <vector>

//
// Local Variables
//

namespace
{
// libiconv implementations differ: glibc accepts the //TRANSLIT//IGNORE
// suffix, while other C libraries (musl, some mingw builds) only know the
// plain target name. Remember the first target that worked for each source
// encoding so the probe only ever happens once per run.
std::mutex  g_IconvTargetMutex;
std::string g_IconvTarget[2];

const char* iconv_source_encoding(CoreStringEncoding encoding)
{
    return (encoding == CoreStringEncoding::EUC_JP) ? "EUC-JP" : "Shift_JIS";
}

// Opens a converter from sourceEncoding to UTF-8, falling back to narrower
// target names when the C library rejects the //TRANSLIT//IGNORE suffix.
// Returns an invalid handle when no target works.
iconv_t open_utf8_converter(const char* sourceEncoding, int index)
{
    static const char* const targets[] = {
        "UTF-8//TRANSLIT//IGNORE",
        "UTF-8//IGNORE",
        "UTF-8",
    };

    {
        std::lock_guard<std::mutex> lock(g_IconvTargetMutex);
        const std::string remembered = g_IconvTarget[index];
        if (!remembered.empty())
        {
            iconv_t cd = iconv_open(remembered.c_str(), sourceEncoding);
            if (cd != (iconv_t)-1)
            {
                return cd;
            }
            // The remembered target stopped working; probe again below.
            g_IconvTarget[index].clear();
        }
    }

    for (const char* target : targets)
    {
        iconv_t cd = iconv_open(target, sourceEncoding);
        if (cd == (iconv_t)-1)
        {
            continue;
        }

        std::lock_guard<std::mutex> lock(g_IconvTargetMutex);
        g_IconvTarget[index] = target;
        return cd;
    }

    return (iconv_t)-1;
}
} // namespace

//
// Exported Functions
//

CORE_EXPORT std::string CoreConvertStringEncoding(std::string str, CoreStringEncoding encoding)
{
    if (str.empty())
    {
        return str;
    }

    const int   index          = static_cast<int>(encoding);
    const char* sourceEncoding = iconv_source_encoding(encoding);

    iconv_t cd = open_utf8_converter(sourceEncoding, index);
    if (cd == (iconv_t)-1)
    {
        // No usable converter on this system: hand the original text back
        // instead of an empty message. This is a display-only conversion, so
        // it must not raise a core error (it used to flood the log on libc
        // builds that reject the //TRANSLIT//IGNORE suffix).
        return str;
    }

    // UTF-8 needs at most 4 bytes per input byte; +1 keeps room for the
    // terminator.
    std::vector<char> outputBuf(str.size() * 4 + 1, '\0');

    char*  inputBufPtr   = const_cast<char*>(str.c_str());
    char*  outputBufPtr  = outputBuf.data();
    size_t inputBufSize  = str.size();
    size_t outputBufSize = outputBuf.size() - 1;

    const size_t ret = iconv(cd, &inputBufPtr, &inputBufSize, &outputBufPtr, &outputBufSize);

    // reset iconv
    iconv(cd, nullptr, nullptr, nullptr, nullptr);
    iconv_close(cd);

    if (ret == (size_t)-1)
    {
        // Undecodable bytes are not worth surfacing as a core error: the
        // caller only wants a display string, so fall back to the original.
        return str;
    }

    return std::string(outputBuf.data(),
        static_cast<size_t>(outputBufPtr - outputBuf.data()));
}
