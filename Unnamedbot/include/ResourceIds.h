#ifndef UNNAMEDBOT_RESOURCE_IDS_H
#define UNNAMEDBOT_RESOURCE_IDS_H

// Resource IDs shared by the C++ sources and the .rc file, so the two cannot
// drift apart (.rc can only expand #define, hence no #pragma once here).
//
// Keep this file ASCII-only: rc.exe has no /utf-8 switch and decodes sources
// with the system ANSI codepage. Non-ASCII bytes in a comment get mis-decoded
// and make rc.exe swallow the #define below, which silently turns the icon
// into a *named* resource ("IDI_APPICON") instead of ID 101.
#define IDI_APPICON 101

#endif // UNNAMEDBOT_RESOURCE_IDS_H
