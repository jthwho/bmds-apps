/**
 * @file      des.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace vnc {

        /**
         * @brief DES encryption of one block (FIPS 46-3), what VNC's authentication needs and
         *        nothing more. DES is broken: VNC's challenge is only offered on the board's loopback
         *        or inside TLS (devplan/vnc.md A5).
         */
        void desEncrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);

        /** @brief DES decryption of one block (a stored password's: `revealPassword()`). */
        void desDecrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);

        /** @brief A stored password's characters: 8 bytes in hex. */
        inline constexpr size_t StoredChars = 16;

        /**
         * @brief @p plain (its first 8 bytes) as VNC stores passwords (vncpasswd's, TigerVNC's):
         *        DES with VNC's fixed key, in hex. Not a secret from anyone with the board: the
         *        challenge needs the password itself, so it can't be a hash; it only keeps it out of
         *        sight (devplan/vnc.md §28).
         */
        void obfuscatePassword(const char *plain, char hex[StoredChars + 1]);

        /**
         * @brief The password @p hex stores (`obfuscatePassword()`), NUL-ended in @p plain.
         * @return False if @p hex isn't 16 hex digits.
         */
        bool revealPassword(const char *hex, char plain[9]);

        /** @brief The challenge's bytes. */
        inline constexpr size_t ChallengeBytes = 16;

        /**
         * @brief VNC authentication's response to @p challenge for @p password (its first 8 bytes,
         *        zeros after; each byte's bits reversed, as VNC's DES key has them): the challenge's
         *        two halves encrypted.
         */
        void vncResponse(const char *password, const uint8_t challenge[ChallengeBytes],
                         uint8_t response[ChallengeBytes]);

        /** @brief Compares @p n bytes in a time that doesn't depend on where they differ. */
        bool sameBytes(const uint8_t *a, const uint8_t *b, size_t n);

} // namespace vnc
