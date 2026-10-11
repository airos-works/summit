#pragma once

// What this build of Summit is for.
//
// SUMMIT_AIROS=1 (the default): air/OS. Summit asks its app_server for what
// only air/OS has (B_DIRECT_DEVICE_PIXELS, so pages can go straight into the
// screen) and the engine uses air/OS drivers and its Mesa where it finds them.
//
// SUMMIT_AIROS=0: a stock Haiku release, built against that release's SDK
// (airos-ci AIROS_TARGET=r1beta6). Summit asks for nothing a stock app_server
// rejects; the engine finds the air/OS features missing and draws through
// app_server. SUMMIT_TARGET_NAME names the release ("Haiku R1/beta6").
#ifndef SUMMIT_AIROS
#define SUMMIT_AIROS 1
#endif

#ifndef SUMMIT_TARGET_NAME
#if SUMMIT_AIROS
#define SUMMIT_TARGET_NAME "air/OS"
#else
#define SUMMIT_TARGET_NAME "Haiku"
#endif
#endif
