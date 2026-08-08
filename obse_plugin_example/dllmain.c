// dllmain.c — DLL 入口點
// [FIX 2025-06-08]
//   加入 DLL_PROCESS_DETACH：遊戲退出時正確停止 HotReload thread、
//   還原所有 hook byte、釋放 D3D9 Atlas 紋理與 trampoline pool。
//   缺少這段會導致 thread leak、VirtualAlloc 未釋放，及 OBSE 卸載時 AV。

#include <Windows.h>

// 前向宣告（避免引入完整 header chain）
#ifdef __cplusplus
extern "C" {
#endif
void HotReload_Stop();
void Patches_RemoveAll();
void GlyphTex_Release();
#ifdef __cplusplus
}
#endif

BOOL WINAPI DllMain(
        HANDLE  hDllHandle,
        DWORD   dwReason,
        LPVOID  lpreserved
        )
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        // 初始化由 OBSEPlugin_Load 負責，此處不做任何事
        break;

    case DLL_PROCESS_DETACH:
        // lpreserved != NULL 表示程序正在終止（非 FreeLibrary），
        // 此時停止 thread 和釋放資源仍然必要。
        HotReload_Stop();
        Patches_RemoveAll();
        GlyphTex_Release();
        break;
    }
    return TRUE;
}
