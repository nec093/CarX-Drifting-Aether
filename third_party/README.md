# third_party

| ライブラリ | 版 | ライセンス | 用途 |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.93.0（無改変。本体と `backends/imgui_impl_dx12`、`imgui_impl_win32`） | MIT（`imgui/LICENSE.txt`） | ゲーム内オーバーレイ |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0（`single_include` の `json.hpp`） | MIT（`nlohmann/LICENSE.MIT`） | ログ、設定、IPC の JSON |
| [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO) | 主パスを HLSL へ移植（`shaders/scene_post.hlsl` の `PSAmbientOcclusion`。詳細は `XeGTAO/README.md`） | MIT（`XeGTAO/LICENSE`） | アンビエントオクルージョン |

Unity Native Plugin API のヘッダー（`IUnityGraphicsD3D12.h` ほか、Unity Companion License）はここに置かず、Unity エディターの `Editor\Data\PluginAPI` を参照します。
