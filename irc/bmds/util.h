/**
 * @file      util.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include "str.h"

#include <fcntl.h>
#include <unistd.h>

namespace irc {

        // NUL-ended, for C's calls.
        struct CString {
                        String s;
                        explicit CString(Text t) {
                                (void)s.assign(t);
                                (void)s.append('\0');
                        }
                        const char *c() const { return s.data(); }
        };

        // A whole file's bytes into @p out. False: it isn't there (or can't be read).
        inline bool readFile(const char *path, String &out) {
                const int fd = open(path, O_RDONLY);
                if (fd < 0) return false;
                char buf[1024];
                out.clear();
                for (;;) {
                        const ssize_t n = read(fd, buf, sizeof(buf));
                        if (n <= 0) break;
                        (void)out.append(Text(buf, size_t(n)));
                }
                close(fd);
                return true;
        }

        inline bool writeFile(const char *path, Text bytes) {
                const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (fd < 0) return false;
                const bool ok = write(fd, bytes.data(), bytes.size()) == ssize_t(bytes.size());
                close(fd);
                return ok;
        }

        // A path in the data folder, unless it is absolute.
        inline void inData(String &out, Text dataDir, Text path) {
                out.clear();
                if (!path.startsWith("/")) {
                        (void)out.append(dataDir);
                        (void)out.append('/');
                }
                (void)out.append(path);
                return;
        }

} // namespace irc
