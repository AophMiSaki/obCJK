#pragma once
// Bridges NorthernUI's xxnFontPath-loaded fonts (FontShim::_fontsByPath,
// never written into FontManager::fontInfos[]) into obCJK's own compact
// fontID space, so Path A/B/C (obCJK_GlyphHook.h) can recognize and
// CJK-substitute them exactly like a native vanilla/MenuQue slot.
//
//   1. At load, parse Data\Menus\NorthernUI\datastore.xml for the 5 role
//      traits (_fontNormal/_fontLarge/_fontMediumLargeUpper/_fontShadowed/
//      _fontSmall) and their candidate literal paths (3-encoding
//      _fonts<Role>_1/_2/_3 leaves, or the single-string _font<Role>). Every
//      candidate maps to its role regardless of _textEncoding.
//   2. Check each candidate exists on disk; a missing file is logged and
//      that role simply never resolves.
//   3a. 新DLL mode (recompiled NorthernUI): FontShim.cpp broadcasts
//      (path, FontInfo*, rawID) via OBSE Messaging when it loads a by-path
//      font; the path is matched against step 1 and the pair is cached.
//   3b. 原DLL mode (official NorthernUI.dll, no broadcast): FontShim::GetFont
//      writes rawID=(vectorIndex<<8)|0xFF into FontInfo+0x08, so rawID>>8
//      indexes _fontsByPath directly; that entry's path is matched against
//      step 1 once per role and cached.
//   4. ObCJKFontIDFromInfoPtr/ObCJKFontInfoPtrFromID (obCJK_GlyphHook.h)
//      fall back to this bridge after the native fontInfos[] lookup fails,
//      using compact fontID = kObCJKMaxFontIDNative + roleIndex. Path C's
//      raw node fontID goes through ObCJKCompactFontIDFromRaw first.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "common/IDebugLog.h"
#include "obse/PluginAPI.h"
#include "obCJK_Path.h"        // k_iniMain
#include "obCJK_GlyphAtlas.h"  // kObCJKMaxFontIDNative / kObCJKNorthernUIRoleCount
#include "obCJK_HookUtil.h"    // ObCJKModuleRvaToAddr

// ini [obCJK] NorthernUIEnable:
//   0 = off (no NorthernUI bridge)
//   1 = 原DLL — official NorthernUI.dll (direct memory read)
//   2 = 新DLL — recompiled NorthernUI (OBSE Messaging broadcast)
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
// in xml. Returns false if the content contains '<' (a computed trait with
// child tags, not a literal path).
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
    (void)lstrcpynA(g_nuiCandidates[g_nuiCandidateCount].path, path, MAX_PATH);  // C6031: truncation is intended
    g_nuiCandidates[g_nuiCandidateCount].roleIndex = roleIndex;
    g_nuiCandidateCount++;
}

// Load-time only: builds the path->roleIndex candidate table from
// datastore.xml and checks each path on disk. Does not touch g_nuiSlots.
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
    if (!xml) {  // C6387/C6011
        fclose(f);
        _WARNING("obCJK:NorthernUICompat:Parse: out of memory reading %s", kObCJKNorthernUIDatastorePath);
        return;
    }
    size_t got = fread(xml, 1, (size_t)size, f);  // C6031: terminate at what was actually read
    xml[got] = '\0';
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

    // A candidate missing from disk is never loaded by FontShim, so its role
    // stays inactive; log it.
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

// ---- 原DLL mode: official NorthernUI.dll, no broadcast ----
// Reads the official DLL's internal layout from live memory (top comment 3b).
static const char* const kObCJKNuiOfficialModuleName    = "NorthernUI.dll";
static const DWORD       kObCJKNuiOfficialPreferredBase = 0x10000000;  // file's own declared ImageBase
static const DWORD       kObCJKVA_NuiFontShimInstance   = 0x100396C0;  // sub_100396C0 — magic-static singleton getter
static const int         kObCJKNuiEntrySize             = 0x1C;        // 28 bytes: FontShim::_Entry{string path; FontInfo* font;}
static const int         kObCJKNuiEntryFontOffset       = 0x18;        // _Entry.font
static const int         kObCJKNuiEntryPathCapOffset    = 0x14;        // _Entry.path._Myres (SSO threshold field)
static const int         kObCJKNuiFontInfoRawIDOffset   = 0x08;        // FontInfo.rawID, written by FontShim::GetFont

// sub_100396C0: no args, returns FontShim* (constructs the magic-static on
// first call); idempotent.
typedef void* (__cdecl *ObCJKFnNuiFontShimInstance)();

// Pointer sanity check (rejects the low 64KB), same as GlyphHook's
// ObCJKIsPlausiblePtr, which isn't visible here.
static inline bool ObCJKNuiIsPlausiblePtr(const void* p)
{
    return (UInt32)p >= 0x10000;
}

// _Entry.path data pointer: MSVC 32-bit std::string, inline buffer when
// capacity < 0x10 (SSO).
static inline const char* ObCJKNuiEntryPathData(const BYTE* entry)
{
    UInt32 capacity = *(const UInt32*)(entry + kObCJKNuiEntryPathCapOffset);
    return (capacity < 0x10) ? (const char*)entry : *(const char* const*)entry;
}

// FontInfo::path offset (same as TexSwap's kFontInfo_Path).
static const int kObCJKNuiFontInfoPathOffset = 0x04;

// Resolves a FontInfo* to a role from its own `path` field, without rawID
// or the broadcast. Used by TexSwap's hook, which runs inside the FontInfo
// constructor before FontShim has written rawID or added the vector entry.
// Works in both modes; caches into g_nuiSlots[roleIndex] on success.
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

// 原DLL mode: resolves rawID ((vectorIndex<<8)|0xFF) by indexing
// _fontsByPath[rawID>>8], matching that entry's path to a role and caching
// it in g_nuiSlots. `knownFontInfoPtr` is an optional cross-check (Path A/B);
// nullptr when only rawID is known (Path C). Returns roleIndex, or -1
// (module not loaded yet, index out of range, or unknown path).
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

// 新DLL broadcast payload; must match FontShim.cpp's ObCJKFontByPathMsg
// byte-for-byte. Delivered synchronously, so `path` is only read during
// the handler call.
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

// Called once from OBSEPlugin_Load (after ini is read); file I/O only.
static void ObCJKNorthernUICompat_Init()
{
    if (!ObCJKNorthernUICompatEnabled()) return;
    ObCJKNorthernUIParseDatastore();
}

// Called from main.cpp's kMessage_GameInitialized handler (all plugins
// loaded). 新DLL mode only.
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

    // 原DLL mode cache miss: resolve via this FontInfo's rawID (+0x08).
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

    // 原DLL mode: Path C has no FontInfo* to cross-check.
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
