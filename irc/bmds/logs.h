/**
 * @file      logs.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "buffers.h"
#include "str.h"

#include <stdint.h>
#include <stdio.h>

namespace irc {

        /**
         * @brief Every buffer's log (devplan/irc.md I4): `FOLDER/NETWORK/BUFFER/YYYY-MM-DD.log`,
         *        plain lines with their times; the network's own buffer's as `server`. Lines wait in
         *        memory and `flush()` (every few seconds, and at the end) appends each file's and
         *        closes it: BMDS's FAT locks an open file, and the user may read a log while IRC
         *        runs; and each write on FAT updates its folder's entry, so not one a line.
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class Logs {
                public:
                        /** @brief Flushed this often. */
                        static constexpr int64_t FlushMs = 5000;

                        ~Logs() { closeAll(); }

                        /** @brief Logs go under @p folder (empty: not logged). */
                        void setFolder(Text folder);

                        /** @brief Adds @p line (of buffer @p buffer, named @p name, of @p network) to its day's file. */
                        void write(Text network, uint32_t buffer, Text name, const Line &line,
                                   int32_t utcOffsetMinutes);

                        /** @brief Appends what waits to the files. */
                        void flush();

                        /** @brief Appends buffer @p buffer's lines and forgets it (closed). */
                        void close(Text network, uint32_t buffer);

                        void closeAll();

                private:
                        struct File {
                                        String   network;
                                        uint32_t buffer = 0;
                                        String   path;    // the day's file
                                        String   pending; // lines not yet appended
                        };
                        void         append(File &f);
                        Vector<File> _files;
                        String       _folder;
                        bool         _dirty = false;
        };

} // namespace irc
