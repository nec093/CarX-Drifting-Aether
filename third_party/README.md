# third_party

| ライブラリ | 版 | ライセンス | 用途 |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.93.0（無改変。本体と `backends/imgui_impl_dx12`、`imgui_impl_win32`） | MIT（`imgui/LICENSE.txt`） | ゲーム内オーバーレイ |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0（`single_include` の `json.hpp`） | MIT（`nlohmann/LICENSE.MIT`） | ログ、設定、IPC の JSON |
| [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO) | 主パスを HLSL へ移植（`shaders/scene_post.hlsl` の `PSAmbientOcclusion`。詳細は `XeGTAO/README.md`） | MIT（`XeGTAO/LICENSE`） | アンビエントオクルージョン |

| [MathLib](https://github.com/NVIDIA-RTX/MathLib) | v11 の `ml.hlsli`（無改変、`shaders/ml.hlsli` として配置） | MIT（`MathLib/LICENSE.txt`） | 反射の光線の GGX ローブのサンプリング（VNDF） |

**NRD（NVIDIA Real-time Denoisers）v4.18.0** は、反射のノイズ除去に使います。NVIDIA RTX SDKs License で、配布はオブジェクトコードとしてだけ認められるため、リポジトリには入れません。`scripts\fetch-deps.ps1` が NRD と、その依存の MathLib・ShaderMake を `build\deps\` に取得します（このスクリプトを実行することは、NRD のライセンスに同意することです）。NRD のシェーダーは Windows SDK の DXC でコンパイルし、`AetherNative.dll` に埋め込みます。Aether の `Renderer/NrdPasses.hlsl` も `NRD.hlsli` を使うので、同じくビルド時にコンパイルして埋め込みます。`build\deps\` に NRD が無ければ、ノイズ除去なしでビルドされます。

DXC（`dxcompiler.dll`、`dxil.dll`）はここに置かず、ビルド時に Windows SDK の再配布用フォルダー（`Redist\D3D\x64`、CMake の `AETHER_DXC_DIR`）からビルド出力へコピーします。SM 6.5 のシェーダー（DXR）を実行時にコンパイルするために使います。

Unity Native Plugin API のヘッダー（`IUnityGraphicsD3D12.h` ほか、Unity Companion License）はここに置かず、Unity エディターの `Editor\Data\PluginAPI` を参照します。
