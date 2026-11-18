/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * irc (devplan/irc.md): an IRC client for BMDS's desktop. Its networks are networks.ini's and
 * its settings settings.ini's, in its data folder ($HOME/.local/share/irc); the first start
 * writes a networks.ini to fill in.
 */

#include "app.h"

int main() {
        irc::App app;
        return app.run();
}
