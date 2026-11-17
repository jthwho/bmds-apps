/**
 * @file      des.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * DES (FIPS 46-3), encryption of one block, straight from the standard's tables: VNC's
 * authentication (RFB 3.8 §7.2.2) is its only use, a handshake's two blocks.
 */

#include "des.h"

namespace vnc {

        namespace {

                // The tables count bits from 1 at the most significant of 64 (as the standard does).
                constexpr uint8_t PC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
                                             35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
                                             46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
                constexpr uint8_t PC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                                             26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                                             51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
                constexpr uint8_t Shifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
                constexpr uint8_t IP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                                            62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                                            57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                                            61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
                constexpr uint8_t FP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                                            38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                                            36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                                            34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
                constexpr uint8_t E[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                                           12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                                           22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
                constexpr uint8_t P[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                                           2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
                constexpr uint8_t S[8][64] = {
                        {14, 4,  13, 1, 2,  15, 11, 8, 3, 10, 6, 12, 5,  9,  0,  7,  0,  15, 7,  4,  14, 2,
                         13, 1,  10, 6, 12, 11, 9,  5, 3, 8,  4, 1,  14, 8,  13, 6,  2,  11, 15, 12, 9,  7,
                         3,  10, 5,  0, 15, 12, 8,  2, 4, 9,  1, 7,  5,  11, 3,  14, 10, 0,  6,  13},
                        {15, 1,  8,  14, 6,  11, 3,  4, 9,  7,  2, 13, 12, 0,  5,  10, 3,  13, 4,  7, 15, 2,
                         8,  14, 12, 0,  1,  10, 6,  9, 11, 5,  0, 14, 7,  11, 10, 4,  13, 1,  5,  8, 12, 6,
                         9,  3,  2,  15, 13, 8,  10, 1, 3,  15, 4, 2,  11, 6,  7,  12, 0,  5,  14, 9},
                        {10, 0,  9,  14, 6, 3,  15, 5,  1,  13, 12, 7, 11, 4,  2,  8,  13, 7, 0,  9, 3, 4,
                         6,  10, 2,  8,  5, 14, 12, 11, 15, 1,  13, 6, 4,  9,  8,  15, 3,  0, 11, 1, 2, 12,
                         5,  10, 14, 7,  1, 10, 13, 0,  6,  9,  8,  7, 4,  15, 14, 3,  11, 5, 2,  12},
                        {7, 13, 14, 3, 0, 6,  9, 10, 1,  2, 8,  5, 11, 12, 4,  15, 13, 8,  11, 5, 6, 15,
                         0, 3,  4,  7, 2, 12, 1, 10, 14, 9, 10, 6, 9,  0,  12, 11, 7,  13, 15, 1, 3, 14,
                         5, 2,  8,  4, 3, 15, 0, 6,  10, 1, 13, 8, 9,  4,  5,  11, 12, 7,  2,  14},
                        {2,  12, 4, 1,  7,  10, 11, 6, 8, 5,  3, 15, 13, 0,  14, 9,  14, 11, 2,  12, 4,  7,
                         13, 1,  5, 0,  15, 10, 3,  9, 8, 6,  4, 2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,
                         6,  3,  0, 14, 11, 8,  12, 7, 1, 14, 2, 13, 6,  15, 0,  9,  10, 4,  5,  3},
                        {12, 1,  10, 15, 9,  2,  6, 8,  0, 13, 3,  4,  14, 7,  5, 11, 10, 15, 4, 2, 7, 12,
                         9,  5,  6,  1,  13, 14, 0, 11, 3, 8,  9,  14, 15, 5,  2, 8,  12, 3,  7, 0, 4, 10,
                         1,  13, 11, 6,  4,  3,  2, 12, 9, 5,  15, 10, 11, 14, 1, 7,  6,  0,  8, 13},
                        {4, 11, 2,  14, 15, 0,  8,  13, 3, 12, 9,  7, 5,  10, 6,  1,  13, 0,  11, 7,  4, 9,
                         1, 10, 14, 3,  5,  12, 2,  15, 8, 6,  1,  4, 11, 13, 12, 3,  7,  14, 10, 15, 6, 8,
                         0, 5,  9,  2,  6,  11, 13, 8,  1, 4,  10, 7, 9,  5,  0,  15, 14, 2,  3,  12},
                        {13, 2, 8,  4, 6, 15, 11, 1,  10, 9,  3, 14, 5,  0,  12, 7,  1,  15, 13, 8, 10, 3,
                         7,  4, 12, 5, 6, 11, 0,  14, 9,  2,  7, 11, 4,  1,  9,  12, 14, 2,  0,  6, 10, 13,
                         15, 3, 5,  8, 2, 1,  14, 7,  4,  10, 8, 13, 15, 12, 9,  0,  3,  5,  6,  11}};

                // @p in's bits (@p inBits wide, numbered from 1 at the top) chosen by @p table.
                uint64_t permute(uint64_t in, unsigned inBits, const uint8_t *table, unsigned outBits) {
                        uint64_t out = 0;
                        for (unsigned i = 0; i < outBits; ++i) out = out << 1 | ((in >> (inBits - table[i])) & 1u);
                        return out;
                }

                uint32_t rotate28(uint32_t v, unsigned n) {
                        return ((v << n) | (v >> (28 - n))) & 0x0fffffffu;
                }

                uint32_t feistel(uint32_t r, uint64_t subkey) {
                        const uint64_t x = permute(r, 32, E, 48) ^ subkey;
                        uint32_t       s = 0;
                        for (unsigned box = 0; box < 8; ++box) {
                                const unsigned six = unsigned(x >> (42 - 6 * box)) & 0x3fu;
                                const unsigned row = (six & 0x20u) >> 4 | (six & 1u);
                                const unsigned col = (six >> 1) & 0xfu;
                                s = s << 4 | S[box][row * 16 + col];
                        }
                        return uint32_t(permute(s, 32, P, 32));
                }

                uint64_t load(const uint8_t b[8]) {
                        uint64_t v = 0;
                        for (int i = 0; i < 8; ++i) v = v << 8 | b[i];
                        return v;
                }

        } // namespace

        namespace {

                // One block each way: decryption is the rounds with the subkeys in reverse.
                void des(const uint8_t key[8], const uint8_t in[8], uint8_t out[8], bool decrypt) {
                        const uint64_t kp = permute(load(key), 64, PC1, 56);
                        uint32_t       c = uint32_t(kp >> 28) & 0x0fffffffu, d = uint32_t(kp) & 0x0fffffffu;
                        uint64_t       subkeys[16];
                        for (unsigned round = 0; round < 16; ++round) {
                                c = rotate28(c, Shifts[round]);
                                d = rotate28(d, Shifts[round]);
                                subkeys[round] = permute(uint64_t(c) << 28 | d, 56, PC2, 48);
                        }
                        const uint64_t ip = permute(load(in), 64, IP, 64);
                        uint32_t       l = uint32_t(ip >> 32), r = uint32_t(ip);
                        for (unsigned round = 0; round < 16; ++round) {
                                const uint32_t next = l ^ feistel(r, subkeys[decrypt ? 15 - round : round]);
                                l = r;
                                r = next;
                        }
                        const uint64_t fp = permute(uint64_t(r) << 32 | l, 64, FP, 64); // the halves swapped
                        for (int i = 0; i < 8; ++i) out[i] = uint8_t(fp >> (56 - 8 * i));
                        return;
                }

                // VNC's fixed key for stored passwords (vncpasswd's), its bits reversed as VNC's DES
                // has them.
                constexpr uint8_t StoreKey[8] = {0xe8, 0x4a, 0xd6, 0x60, 0xc4, 0x72, 0x1a, 0xe0};

        } // namespace

        void desEncrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]) {
                des(key, in, out, false);
                return;
        }

        void desDecrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]) {
                des(key, in, out, true);
                return;
        }

        void obfuscatePassword(const char *plain, char hex[StoredChars + 1]) {
                uint8_t block[8] = {};
                for (size_t i = 0; i < 8 && plain[i] != '\0'; ++i) block[i] = uint8_t(plain[i]);
                uint8_t out[8];
                desEncrypt(StoreKey, block, out);
                static constexpr char Digits[] = "0123456789abcdef";
                for (size_t i = 0; i < 8; ++i) {
                        hex[2 * i] = Digits[out[i] >> 4];
                        hex[2 * i + 1] = Digits[out[i] & 0xfu];
                }
                hex[StoredChars] = '\0';
                for (uint8_t &b : block) b = 0;
                return;
        }

        bool revealPassword(const char *hex, char plain[9]) {
                uint8_t in[8];
                for (size_t i = 0; i < 2 * 8; ++i) {
                        const char ch = hex[i];
                        int        v = ch >= '0' && ch <= '9'   ? ch - '0'
                                       : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                       : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                                                : -1;
                        if (v < 0) return false;
                        in[i / 2] = uint8_t(i % 2 == 0 ? v << 4 : (in[i / 2] | v));
                }
                if (hex[StoredChars] != '\0') return false;
                desDecrypt(StoreKey, in, reinterpret_cast<uint8_t *>(plain));
                plain[8] = '\0';
                return true;
        }

        void vncResponse(const char *password, const uint8_t challenge[ChallengeBytes],
                         uint8_t response[ChallengeBytes]) {
                uint8_t key[8] = {};
                for (int i = 0; i < 8 && password != nullptr && password[i] != '\0'; ++i) {
                        uint8_t b = uint8_t(password[i]), reversed = 0;
                        for (int bit = 0; bit < 8; ++bit) reversed = uint8_t(reversed << 1 | ((b >> bit) & 1u));
                        key[i] = reversed;
                }
                desEncrypt(key, challenge, response);
                desEncrypt(key, challenge + 8, response + 8);
                return;
        }

        bool sameBytes(const uint8_t *a, const uint8_t *b, size_t n) {
                uint8_t diff = 0;
                for (size_t i = 0; i < n; ++i) diff = uint8_t(diff | (a[i] ^ b[i]));
                return diff == 0;
        }

} // namespace vnc
