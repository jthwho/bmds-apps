/**
 * @file      logs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "logs.h"

#include "format.h"

#include <errno.h>
#include <sys/stat.h>

namespace irc {

        namespace {

                // A name as a folder's: lower case, nothing that walks out of it or FAT refuses.
                void folderName(String &out, Text name) {
                        if (name.isEmpty() || name == Text(".") || name == Text("..")) {
                                (void)out.append("server");
                                return;
                        }
                        for (char c : name) {
                                if (c >= 'A' && c <= 'Z') c = char(c + 32);
                                if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                                    c == '>' || c == '|' || uint8_t(c) < 0x20)
                                        c = '_';
                                (void)out.append(c);
                        }
                        return;
                }

                // Makes each folder of @p path (a file's: the last part isn't one).
                void makeFolders(String &path) {
                        for (size_t i = 1; i < path.size(); ++i) {
                                if (path.data()[i] != '/') continue;
                                const_cast<char *>(path.data())[i] = '\0';
                                (void)mkdir(path.data(), 0755);
                                const_cast<char *>(path.data())[i] = '/';
                        }
                        return;
                }

        } // namespace

        void Logs::setFolder(Text folder) {
                closeAll();
                (void)_folder.assign(folder);
                return;
        }

        void Logs::write(Text network, uint32_t buffer, Text name, const Line &line, int32_t utcOffsetMinutes) {
                if (_folder.isEmpty()) return;
                File *f = nullptr;
                for (File &each : _files)
                        if (each.buffer == buffer && each.network == network) f = &each;
                if (f == nullptr) {
                        File fresh;
                        if (fresh.network.assign(network) != Error::Ok || _files.append(std::move(fresh)) != Error::Ok)
                                return;
                        f = &_files.last();
                        f->buffer = buffer;
                }
                // The day's file: a new day's lines go to the next once those waiting are out.
                const Format::LocalTime t = Format::localTime(line.time, utcOffsetMinutes);
                String                  path;
                (void)path.assign(_folder);
                (void)path.append('/');
                folderName(path, network);
                (void)path.append('/');
                folderName(path, name);
                char date[16];
                snprintf(date, sizeof(date), "/%04d-%02d-%02d.log", t.year, t.month, t.day);
                (void)path.append(date);
                (void)path.append('\0');
                if (!(f->path == path.view())) {
                        append(*f);
                        f->path = std::move(path);
                }
                (void)f->pending.append(line.log);
                (void)f->pending.append('\n');
                _dirty = true;
                return;
        }

        void Logs::append(File &f) {
                if (f.pending.isEmpty() || f.path.isEmpty()) return;
                makeFolders(f.path);
                FILE *file = fopen(f.path.data(), "a");
                if (file != nullptr) {
                        (void)fwrite(f.pending.data(), 1, f.pending.size(), file);
                        fclose(file);
                }
                f.pending.clear();
                return;
        }

        void Logs::flush() {
                if (!_dirty) return;
                for (File &f : _files) append(f);
                _dirty = false;
                return;
        }

        void Logs::close(Text network, uint32_t buffer) {
                for (size_t i = 0; i < _files.size(); ++i)
                        if (_files[i].buffer == buffer && _files[i].network == network) {
                                append(_files[i]);
                                _files.removeAt(i);
                                return;
                        }
                return;
        }

        void Logs::closeAll() {
                for (File &f : _files) append(f);
                _files.clear();
                _dirty = false;
                return;
        }

} // namespace irc
