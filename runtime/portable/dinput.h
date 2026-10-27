/* DirectInput 8 over SDL3 input (dinput.c, input.c). */
#pragma once

/* Registers the shims (before the images are mapped). */
void dinput_init(void);
/* Builds the COM vtables in guest memory (once the guest heap is up). */
void dinput_setup(void);
/* Writes the gamepad settings file FINAL FANTASY XIV Config would (config.pad) at host_path,
 * unless one is there: the first gamepad, Xbox layout. Without it the game ignores gamepads. */
void dinput_default_pad_config(const char* host_path);
