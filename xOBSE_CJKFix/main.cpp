// main.cpp（xOBSE_CJKFix）
// xOBSE 22.13/22.14 (obse_1_2_416.dll) 的窄字串編碼修補外掛：9 項修補，編碼取自 obCJK.ini。
// log 寫到遊戲根目錄 xOBSE_CJKFix.log。
#include "obse/PluginAPI.h"
#include "include/xOBSE_CJKFix_Install.h"

IDebugLog gLog("xOBSE_CJKFix.log");

extern "C" {

bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info) {
    _MESSAGE("query");
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name = "xOBSE_CJKFix";
    info->version = 1;

    // 只修補遊戲內的 xOBSE，編輯器不載入
    if (obse->isEditor) return false;
    if (obse->obseVersion < OBSE_VERSION_INTEGER) {
        _ERROR("OBSE version too old (got %u expected at least %u)", obse->obseVersion, OBSE_VERSION_INTEGER);
        return false;
    }
    if (obse->oblivionVersion != OBLIVION_VERSION) {
        _ERROR("incorrect Oblivion version (got %08X need %08X)", obse->oblivionVersion, OBLIVION_VERSION);
        return false;
    }
    return true;
}

bool OBSEPlugin_Load(const OBSEInterface*) {
    _MESSAGE("xOBSE_CJKFix load");
    InstallAll();
    return true;
}

}
