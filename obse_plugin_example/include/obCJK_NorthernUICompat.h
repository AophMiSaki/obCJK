#pragma once
// Bridges NorthernUI's xxnFontPath-loaded fonts (FontShim::_fontsByPath,
// never written into FontManager::fontInfos[]) into obCJK's own compact
// fontID space, so Path A/B/C (obCJK_GlyphHook.h) can recognize and
// CJK-substitute them exactly like a native vanilla/MenuQue slot.
//
// Design (see memory obcjk_northernui_font_compat + this conversation):
//   1. At load, statically parse Data\Menus\NorthernUI\datastore.xml for the
//      5 role traits (_fontNormal/_fontLarge/_fontMediumLargeUpper/
//      _fontShadowed/_fontSmall) and their candidate literal paths (either
//      the 3-encoding computed form or the single-string vanilla-lite
//      form — see ObCJKXmlExtractLeafTag). _textEncoding itself is
//      irrelevant to us: every candidate for a role maps to that same role
//      regardless of which one NorthernUI actually resolves at runtime.
//   2. Verify each candidate against disk (GetFileAttributesA). A missing
//      file just means that role's slot silently never gets populated
//      (FontShim itself will fail to load it too) — log it so the user can
//      tell datastore.xml and Fonts\ apart, per their decision that
//      datastore.xml is the source of truth for "what should be there".
//   3a. "新DLL" mode (user's own recompiled NorthernUI fork): its
//      FontShim.cpp broadcasts (path, FontInfo*, rawID) via OBSE Messaging
//      the moment it lazily loads a new by-path font (see that file's
//      matching kMsgType_ObCJKFontByPath). We match the path against step
//      1's table to learn which role this is, then cache the (FontInfo*,
//      rawID) pair.
//   3b. "原DLL" mode (official/unmodified NorthernUI.dll — no broadcast
//      exists in that build): resolved by direct memory read instead, see
//      ObCJKNorthernUIResolveOfficialByRawID below. IDA-verified 2026-07-30
//      against the actual installed NorthernUI.dll (imagebase 0x10000000):
//      FontShim::GetFont writes rawID=(vectorIndex<<8)|0xFF into the new
//      FontInfo's own +0x08 the instant it's allocated, so any FontInfo* we
//      already have (from Path A/B/C) tells us its own vector index
//      directly (rawID>>8) with no scanning — same arithmetic the DLL's own
//      internal reverse-lookup (sub_1000E4A0) performs. We use that index
//      once per newly-seen rawID to read the vector Entry's path string
//      (needed to tell which of the 5 roles this is — that mapping is not
//      recoverable from rawID alone, it's assigned in whatever order the
//      UI happens to first request each role) and match it against step
//      1's table, then cache the result exactly like the 新DLL path does —
//      so this lookup only actually walks memory once per role, not every
//      frame.
//   4. ObCJKFontIDFromInfoPtr/ObCJKFontInfoPtrFromID (obCJK_GlyphHook.h)
//      fall back to this bridge after their native fontInfos[] lookup
//      fails, using compact fontID = kObCJKMaxFontIDNative + roleIndex.
//      Path C's raw node fontID (FontShim's own (vectorIdx<<8)|0xFF
//      encoding when it's a by-path font, not a small native int) is
//      resolved via ObCJKCompactFontIDFromRaw first.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "common/IDebugLog.h"
#include "obse/PluginAPI.h"
#include "obCJK_Path.h"        // k_iniMain
#include "obCJK_GlyphAtlas.h"  // kObCJKMaxFontIDNative / kObCJKNorthernUIRoleCount
#include "obCJK_HookUtil.h"    // ObCJKModuleRvaToAddr (原DLL path's RVA resolution)

// ini key NorthernUIEnable ([obCJK] 節) — was a plain 0/1 bool, now a
// 3-way choice per the user's 2026-07-30 decision to support both fork
// scenarios (see this file's top comment, 3a/3b):
//   0 = off (vanilla — no NorthernUI bridge at all, prior default behavior)
//   1 = 原DLL — official/unmodified NorthernUI.dll (direct memory read)
//   2 = 新DLL — user's own recompiled fork (OBSE Messaging broadcast)
// obCJK_iniEdit.py's dropdown (待下次對話) must present exactly these 3
// values in this order.
// Anonymous (not named ObCJKNorthernUIMode) — that identifier is used below
// for the accessor function instead; C++'s enum/function name hiding rule
// would technically allow both, but keeping them visibly distinct avoids
// relying on it.
enum {
    kObCJKNuiMode_Off         = 0,
    kObCJKNuiMode_OfficialDLL = 1,
    kObCJKNuiMode_PatchedDLL  = 2,
};

static int ObCJKNorthernUIMode()
{
    static int cached = -1;
    if (cached < 0) {
        cached = GetPrivateProfileIntA("obCJK", "NorthernUIEnable", kObCJKNuiMode_Off, k_iniMain);
        if (cached != kObCJKNuiMode_Off && cached != kObCJKNuiMode_OfficialDLL && cached != kObCJKNuiMode_PatchedDLL)
            cached = kObCJKNuiMode_Off;
    }
    return cached;
}

static bool ObCJKNorthernUICompatEnabled()
{
    return ObCJKNorthernUIMode() != kObCJKNuiMode_Off;
}

struct ObCJKNorthernUIRoleDef {
    const char* xmlBaseName;   // tag suffix: _fonts<xmlBaseName>_1/_2/_3, _font<xmlBaseName>
    const char* displayName;   // for log lines only
};

static const ObCJKNorthernUIRoleDef kObCJKNorthernUIRoles[kObCJKNorthernUIRoleCount] = {
    { "Normal",           "Normal"           },
    { "Large",            "Large"            },
    { "MediumLargeUpper", "MediumLargeUpper" },
    { "Shadowed",         "Shadowed"         },
    { "Small",            "Small"            },
};

static const char* const kObCJKNorthernUIDatastorePath = "Data\\Menus\\NorthernUI\\datastore.xml";

struct ObCJKNorthernUICandidate {
    char path[MAX_PATH];
    int  roleIndex;
};
static ObCJKNorthernUICandidate g_nuiCandidates[kObCJKNorthernUIRoleCount * 3];
static int g_nuiCandidateCount = 0;

struct ObCJKNorthernUISlot {
    bool   valid;
    void*  fontInfoPtr;
    UInt32 rawID;
};
static ObCJKNorthernUISlot g_nuiSlots[kObCJKNorthernUIRoleCount] = {};

// Extracts the trimmed text content of the first "<tag>...</tag>" occurrence
// in xml. Refuses (returns false) if that content contains '<' — datastore.xml
// encodes NorthernUI's computed traits (e.g. _fontNormal itself) as a block
// of child tags (<copy>/<max>/<add>...), and only the flat leaf tags
// (_fontsNormal_1 etc., or the vanilla-lite single-string _fontNormal) are
// genuine literal paths we can use here.
static bool ObCJKXmlExtractLeafTag(const char* xml, const char* tagName, char* out, size_t outSize)
{
    char openTag[80];
    wsprintfA(openTag, "<%s>", tagName);
    const char* start = strstr(xml, openTag);
    if (!start) return false;
    start += strlen(openTag);

    char closeTag[80];
    wsprintfA(closeTag, "</%s>", tagName);
    const char* end = strstr(start, closeTag);
    if (!end || end <= start) return false;

    for (const char* p = start; p < end; p++)
        if (*p == '<') return false;  // computed trait, not a leaf value

    size_t len = (size_t)(end - start);
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' ||
                       start[len - 1] == '\r' || start[len - 1] == '\n')) len--;
    while (len > 0 && (*start == ' ' || *start == '\t' ||
                       *start == '\r' || *start == '\n')) { start++; len--; }
    if (len == 0) return false;
    if (len >= outSize) len = outSize - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static void ObCJKNorthernUIAddCandidate(const char* path, int roleIndex)
{
    int cap = (int)(sizeof(g_nuiCandidates) / sizeof(g_nuiCandidates[0]));
    if (g_nuiCandidateCount >= cap) return;
    lstrcpynA(g_nuiCandidates[g_nuiCandidateCount].path, path, MAX_PATH);
    g_nuiCandidates[g_nuiCandidateCount].roleIndex = roleIndex;
    g_nuiCandidateCount++;
}

// Static, load-time only: parses datastore.xml for each role's candidate
// path(s) and checks them against disk. Never touches g_nuiSlots (that's
// only ever filled by ObCJKNorthernUIMessageHandler at actual font-load
// time) — this just builds the path->roleIndex table used to interpret
// those messages, plus emits diagnostics.
static void ObCJKNorthernUIParseDatastore()
{
    g_nuiCandidateCount = 0;

    FILE* f = nullptr;
    fopen_s(&f, kObCJKNorthernUIDatastorePath, "rb");
    if (!f) {
        _WARNING("obCJK:NorthernUICompat:Parse: cannot open %s — NorthernUI font bridge inactive",
                  kObCJKNorthernUIDatastorePath);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 1 * 1024 * 1024) {  // sanity cap; real file is a few KB
        fclose(f);
        _WARNING("obCJK:NorthernUICompat:Parse: %s has implausible size %ld, skipping",
                  kObCJKNorthernUIDatastorePath, size);
        return;
    }

    char* xml = (char*)malloc((size_t)size + 1);
    fread(xml, 1, (size_t)size, f);
    xml[size] = '\0';
    fclose(f);

    for (int r = 0; r < kObCJKNorthernUIRoleCount; r++) {
        const char* base = kObCJKNorthernUIRoles[r].xmlBaseName;
        int found = 0;
        for (int enc = 1; enc <= 3; enc++) {
            char tag[80];
            wsprintfA(tag, "_fonts%s_%d", base, enc);
            char val[MAX_PATH];
            if (ObCJKXmlExtractLeafTag(xml, tag, val, sizeof(val))) {
                ObCJKNorthernUIAddCandidate(val, r);
                found++;
            }
        }
        if (!found) {
            char tag[80];
            wsprintfA(tag, "_font%s", base);
            char val[MAX_PATH];
            if (ObCJKXmlExtractLeafTag(xml, tag, val, sizeof(val))) {
                ObCJKNorthernUIAddCandidate(val, r);
                found = 1;
            }
        }
        if (!found) {
            _MESSAGE("obCJK:NorthernUICompat:Parse: role %s not defined in datastore.xml "
                      "(this NorthernUI install may not use it) — slot left inactive",
                      kObCJKNorthernUIRoles[r].displayName);
        }
    }
    free(xml);

    // datastore.xml is the source of truth (user's decision): if it names a
    // path but Fonts\ doesn't actually have that file, that candidate will
    // never be loaded by FontShim (and so will never broadcast), leaving
    // this role's slot permanently inactive — not an error, just log it so
    // the mismatch is legible instead of a silent no-op.
    for (int i = 0; i < g_nuiCandidateCount; i++) {
        if (GetFileAttributesA(g_nuiCandidates[i].path) == INVALID_FILE_ATTRIBUTES) {
            _WARNING("obCJK:NorthernUICompat:Parse: datastore.xml names %s for role %s but the "
                      "file does not exist under Fonts\\ — that candidate will not receive obCJK "
                      "CJK replacement",
                      g_nuiCandidates[i].path, kObCJKNorthernUIRoles[g_nuiCandidates[i].roleIndex].displayName);
        }
    }
}

static int ObCJKNorthernUIRoleIndexForPath(const char* path)
{
    for (int i = 0; i < g_nuiCandidateCount; i++)
        if (_stricmp(g_nuiCandidates[i].path, path) == 0)
            return g_nuiCandidates[i].roleIndex;
    return -1;
}

// ---- "原DLL" mode: official/unmodified NorthernUI.dll, no broadcast ----
// See this file's top comment (3b) for the full derivation. Everything
// below reads live process memory through the officially-installed DLL's
// own exported/internal layout, IDA-verified 2026-07-30 against the actual
// file the user has installed (H:\Oblivion_MOD\MO2_OBLIVION\mods\0004.
// NorthernUI\OBSE\Plugins\NorthernUI.dll).
static const char* const kObCJKNuiOfficialModuleName    = "NorthernUI.dll";
static const DWORD       kObCJKNuiOfficialPreferredBase = 0x10000000;  // file's own declared ImageBase
static const DWORD       kObCJKVA_NuiFontShimInstance   = 0x100396C0;  // sub_100396C0 — magic-static singleton getter
static const int         kObCJKNuiEntrySize             = 0x1C;        // 28 bytes: FontShim::_Entry{string path; FontInfo* font;}
static const int         kObCJKNuiEntryFontOffset       = 0x18;        // _Entry.font
static const int         kObCJKNuiEntryPathCapOffset    = 0x14;        // _Entry.path._Myres (SSO threshold field)
static const int         kObCJKNuiFontInfoRawIDOffset   = 0x08;        // FontInfo.rawID, written by FontShim::GetFont

// sub_100396C0 takes no args and returns FontShim* (lazily constructing the
// magic-static on first call, same function FontShim::GetFont() itself
// calls) — safe/idempotent to call from here too.
typedef void* (__cdecl *ObCJKFnNuiFontShimInstance)();

// Guard-page-only pointer sanity check, same threshold as obCJK_GlyphHook.h's
// ObCJKIsPlausiblePtr — duplicated locally rather than shared/included since
// that one is defined later in GlyphHook.h (which includes this file, not
// the other way around).
static inline bool ObCJKNuiIsPlausiblePtr(const void* p)
{
    return (UInt32)p >= 0x10000;
}

// MSVC 32-bit release std::string layout, same SSO rule as
// obCJK_MenuQueDelimHook.h's ObCJKMQStrData (capacity threshold 0x10) —
// duplicated locally (3 lines, unrelated hook file) rather than shared.
static inline const char* ObCJKNuiEntryPathData(const BYTE* entry)
{
    UInt32 capacity = *(const UInt32*)(entry + kObCJKNuiEntryPathCapOffset);
    return (capacity < 0x10) ? (const char*)entry : *(const char* const*)entry;
}

// FontInfo::path field offset — mirrors obCJK_TexSwap.h's kFontInfo_Path
// (0x04). Duplicated locally rather than shared/included since TexSwap.h
// includes this file (not the other way around) and it's a single constant.
static const int kObCJKNuiFontInfoPathOffset = 0x04;

// Resolves a FontInfo* to a NorthernUI role by reading its own native
// `path` field directly and matching it against datastore.xml's candidate
// table — no dependency on FontShim's rawID field or OBSE broadcast, both
// of which are still unset/unfired at this point. Needed specifically by
// obCJK_TexSwap.h's hook, which fires *during* the native FontInfo
// constructor (mid font-load, while reading the .tex file) — well before
// FontShim::GetFont(const char*) returns and writes rawID/broadcasts (新DLL
// mode) or before the "原DLL" vector-index math is even meaningful (the
// entry isn't in the vector yet either). The native constructor needs the
// path to locate the .fnt/.tex files, so this field should already be
// populated by the time TexSwap's hook runs — unlike rawID/ID, which either
// mode only finalizes after construction completes. Works identically for
// both 原DLL/新DLL modes since it never touches rawID. Populates
// g_nuiSlots[roleIndex] on success so later Path A/B/C draw-time lookups
// (ObCJKNorthernUIFontIDFromInfoPtr) get an instant cache hit too, and Path
// C's rawID-based lookup benefits once FontShim's own writes land later.
static int ObCJKNorthernUIResolveByOwnPath(void* fontInfoPtr)
{
    if (ObCJKNorthernUIMode() == kObCJKNuiMode_Off) return -1;
    if (!ObCJKNuiIsPlausiblePtr(fontInfoPtr)) return -1;

    const char* path = *(const char* const*)((BYTE*)fontInfoPtr + kObCJKNuiFontInfoPathOffset);
    if (!path) return -1;

    int roleIndex = ObCJKNorthernUIRoleIndexForPath(path);
    if (roleIndex < 0) {
        _MESSAGE("obCJK:NorthernUICompat:ResolveByOwnPath: fontInfoPtr=%p path=%s not a known "
                  "role path, ignoring", fontInfoPtr, path);
        return -1;
    }

    g_nuiSlots[roleIndex].valid       = true;
    g_nuiSlots[roleIndex].fontInfoPtr = fontInfoPtr;
    g_nuiSlots[roleIndex].rawID       = *(UInt32*)((BYTE*)fontInfoPtr + kObCJKNuiFontInfoRawIDOffset);
    _MESSAGE("obCJK:NorthernUICompat:ResolveByOwnPath: role=%s path=%s fontInfoPtr=%p "
              "(resolved mid-load, before FontShim rawID/broadcast finalize)",
              kObCJKNorthernUIRoles[roleIndex].displayName, path, fontInfoPtr);
    return roleIndex;
}

// Resolves a NorthernUI-shim rawID ((vectorIndex<<8)|0xFF — see
// ObCJKCompactFontIDFromRaw below) against the live official NorthernUI.dll's
// _fontsByPath vector by indexing straight to vectorIndex=rawID>>8 (the same
// arithmetic FontShim's own internal reverse-lookup performs — no scanning).
// Reads that Entry's path string and matches it against datastore.xml's
// candidate table to learn the role, then populates g_nuiSlots[roleIndex]
// exactly like the 新DLL broadcast handler does. `knownFontInfoPtr` is an
// optional sanity cross-check (Path A/B already have the pointer in hand);
// pass nullptr when only the raw rawID is available (Path C).
// Returns the resolved roleIndex, or -1 if unresolved (module not loaded
// yet, index outside the vector's current bounds, or path not one of ours).
static int ObCJKNorthernUIResolveOfficialByRawID(UInt32 rawID, void* knownFontInfoPtr)
{
    if (rawID == 0xFFFFFFFF) return -1;
    int vectorIndex = (int)(rawID >> 8);
    if (vectorIndex < 0) return -1;

    HMODULE mod = GetModuleHandleA(kObCJKNuiOfficialModuleName);
    if (!mod) return -1;  // NorthernUI not loaded (yet) — caller may retry later

    BYTE* instanceFn = ObCJKModuleRvaToAddr(mod, kObCJKVA_NuiFontShimInstance, kObCJKNuiOfficialPreferredBase);
    BYTE* fontShim = (BYTE*)((ObCJKFnNuiFontShimInstance)instanceFn)();
    if (!ObCJKNuiIsPlausiblePtr(fontShim)) return -1;

    BYTE* first = *(BYTE**)(fontShim + 0x00);  // _fontsByPath._Myfirst
    BYTE* last  = *(BYTE**)(fontShim + 0x04);  // _fontsByPath._Mylast
    if (last <= first) return -1;
    int count = (int)((last - first) / kObCJKNuiEntrySize);
    if (vectorIndex >= count) return -1;

    BYTE* entry = first + (size_t)vectorIndex * kObCJKNuiEntrySize;
    void* entryFontPtr = *(void**)(entry + kObCJKNuiEntryFontOffset);
    if (knownFontInfoPtr && entryFontPtr != knownFontInfoPtr) {
        _WARNING("obCJK:NorthernUICompat:ResolveOfficial: vector entry[%d] font=%p != expected %p, "
                  "rawID formula mismatch — skipping", vectorIndex, entryFontPtr, knownFontInfoPtr);
        return -1;
    }

    const char* path = ObCJKNuiEntryPathData(entry);
    int roleIndex = ObCJKNorthernUIRoleIndexForPath(path);
    if (roleIndex < 0) {
        _MESSAGE("obCJK:NorthernUICompat:ResolveOfficial: vector entry[%d] path=%s not a known "
                  "role path, ignoring", vectorIndex, path);
        return -1;
    }

    g_nuiSlots[roleIndex].valid       = true;
    g_nuiSlots[roleIndex].fontInfoPtr = entryFontPtr;
    g_nuiSlots[roleIndex].rawID       = rawID;
    int compactFontID = kObCJKMaxFontIDNative + roleIndex;
    _MESSAGE("obCJK:NorthernUICompat:ResolveOfficial: role=%s path=%s fontInfoPtr=%p rawID=%u "
              "-> compact fontID=%d (SLOT%d)",
              kObCJKNorthernUIRoles[roleIndex].displayName, path, entryFontPtr, rawID,
              compactFontID, compactFontID + 1);
    return roleIndex;
}

// Private wire format broadcast by NorthernUI's FontShim.cpp — must mirror
// that file's ObCJKFontByPathMsg byte-for-byte (same field order/types).
// Not a shared header (obCJK and NorthernUI are separate DLLs); the two
// copies just need to agree by convention. Dispatch() calls the listener
// synchronously, so `path` only needs to stay valid for this call's
// duration (see ObCJKNorthernUIRoleIndexForPath — copies nothing itself,
// but only reads it before returning).
struct ObCJKFontByPathMsg {
    const char* path;
    void*       fontInfoPtr;
    UInt32      rawID;
};
static const UInt32 kMsgType_ObCJKFontByPath = 1;

static void ObCJKNorthernUIMessageHandler(OBSEMessagingInterface::Message* msg)
{
    if (msg->type != kMsgType_ObCJKFontByPath) return;
    if (msg->dataLen != sizeof(ObCJKFontByPathMsg) || !msg->data) return;

    ObCJKFontByPathMsg* m = (ObCJKFontByPathMsg*)msg->data;
    int roleIndex = ObCJKNorthernUIRoleIndexForPath(m->path);
    if (roleIndex < 0) {
        _MESSAGE("obCJK:NorthernUICompat:OnFontByPath: unrecognized path=%s (not one of "
                  "datastore.xml's known role paths), ignoring", m->path);
        return;
    }

    g_nuiSlots[roleIndex].valid       = true;
    g_nuiSlots[roleIndex].fontInfoPtr = m->fontInfoPtr;
    g_nuiSlots[roleIndex].rawID       = m->rawID;
    int compactFontID = kObCJKMaxFontIDNative + roleIndex;
    _MESSAGE("obCJK:NorthernUICompat:OnFontByPath: role=%s path=%s fontInfoPtr=%p rawID=%u "
              "-> compact fontID=%d (SLOT%d)",
              kObCJKNorthernUIRoles[roleIndex].displayName, m->path, m->fontInfoPtr, m->rawID,
              compactFontID, compactFontID + 1);
}

// Called once from OBSEPlugin_Load (after ini is read) — pure file I/O, no
// dependency on other plugins being loaded yet.
static void ObCJKNorthernUICompat_Init()
{
    if (!ObCJKNorthernUICompatEnabled()) return;
    ObCJKNorthernUIParseDatastore();
}

// Called from main.cpp's kMessage_GameInitialized handler, same place/same
// reasoning as ObCJKInstallMenuQueDelimHook() — every plugin (including
// NorthernUI, if installed) is guaranteed loaded by then. Only meaningful
// in 新DLL mode (原DLL mode has no broadcast to listen for — that path
// resolves lazily via ObCJKNorthernUIResolveOfficialByRawID instead, no
// listener/registration needed at all).
static void ObCJKNorthernUICompat_RegisterListener(OBSEMessagingInterface* msgIntfc, PluginHandle handle)
{
    if (ObCJKNorthernUIMode() != kObCJKNuiMode_PatchedDLL) return;
    if (!msgIntfc) return;
    if (!msgIntfc->RegisterListener(handle, "NorthernUI", ObCJKNorthernUIMessageHandler))
        _MESSAGE("obCJK:NorthernUICompat:RegisterListener: NorthernUI not loaded, font bridge inactive");
}

static int ObCJKNorthernUIFontIDFromInfoPtr(void* fontInfoPtr)
{
    int mode = ObCJKNorthernUIMode();
    if (mode == kObCJKNuiMode_Off) return -1;

    for (int i = 0; i < kObCJKNorthernUIRoleCount; i++)
        if (g_nuiSlots[i].valid && g_nuiSlots[i].fontInfoPtr == fontInfoPtr)
            return kObCJKMaxFontIDNative + i;

    // 原DLL mode: no broadcast populated g_nuiSlots above, so on a cache
    // miss read this FontInfo's own rawID (written by FontShim::GetFont at
    // +0x08 regardless of which DLL build — see this file's top comment
    // 3b) and resolve+cache it via the vector-index lookup.
    if (mode == kObCJKNuiMode_OfficialDLL && ObCJKNuiIsPlausiblePtr(fontInfoPtr)) {
        UInt32 rawID = *(UInt32*)((BYTE*)fontInfoPtr + kObCJKNuiFontInfoRawIDOffset);
        int roleIndex = ObCJKNorthernUIResolveOfficialByRawID(rawID, fontInfoPtr);
        if (roleIndex >= 0) return kObCJKMaxFontIDNative + roleIndex;
    }
    return -1;
}

static int ObCJKNorthernUIFontIDFromRawID(UInt32 rawID)
{
    int mode = ObCJKNorthernUIMode();
    if (mode == kObCJKNuiMode_Off) return -1;

    for (int i = 0; i < kObCJKNorthernUIRoleCount; i++)
        if (g_nuiSlots[i].valid && g_nuiSlots[i].rawID == rawID)
            return kObCJKMaxFontIDNative + i;

    // 原DLL mode: Path C only ever has the raw rawID (no FontInfo* to
    // cross-check against), so pass nullptr for knownFontInfoPtr.
    if (mode == kObCJKNuiMode_OfficialDLL) {
        int roleIndex = ObCJKNorthernUIResolveOfficialByRawID(rawID, nullptr);
        if (roleIndex >= 0) return kObCJKMaxFontIDNative + roleIndex;
    }
    return -1;
}

static void* ObCJKNorthernUIFontInfoPtrFromCompactID(int compactFontID)
{
    int i = compactFontID - kObCJKMaxFontIDNative;
    if (i < 0 || i >= kObCJKNorthernUIRoleCount) return nullptr;
    if (!g_nuiSlots[i].valid) return nullptr;
    return g_nuiSlots[i].fontInfoPtr;
}

// Resolves Path C's raw node fontID (obCJK_GlyphHook.h's node+0x00) into
// obCJK's compact space: a plain native index passes through unchanged;
// anything outside that range is checked against the NorthernUI bridge by
// rawID (FontShim's (vectorIdx<<8)|0xFF encoding).
static int ObCJKCompactFontIDFromRaw(int rawFontID)
{
    if (rawFontID >= 0 && rawFontID < kObCJKMaxFontIDNative) return rawFontID;
    return ObCJKNorthernUIFontIDFromRawID((UInt32)rawFontID);
}
