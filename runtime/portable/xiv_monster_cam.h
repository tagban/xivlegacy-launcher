/* The camera around a monster body (xiv_monster_cam.c). */
#pragma once

#include "runtime.h"

/* The recompiler's hook at the end of 0x616380, the chara the camera follows (--hooks camera_follow=0x6163a6). */
void xiv_monster_cam(Guest* g);
