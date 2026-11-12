/* backline CLI: recorder daemon and clock commands. SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include "backline/backline.h"
bool cli_rec_is_service(const char *sub);
int cli_rec(bl_client *c, bool json, int argc, char **argv);
int cli_transport(bl_client *c, bool json, int argc, char **argv);
int cli_play(bl_client *c, bool json, int argc, char **argv);
int cli_map(bl_client *c, bool json, int argc, char **argv);
int cli_surfaces(bl_client *c, bool json, int argc, char **argv);
