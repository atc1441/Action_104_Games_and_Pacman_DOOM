/*
 * hostbox.c - storage for the shared mailbox.
 *
 * The only object in section .mailbox, so it is guaranteed to sit at that
 * section's start and therefore at 0x20000000.
 */

#include "hostbox.h"

__attribute__((section(".mailbox"), used))
hostbox_t g_hostbox;
