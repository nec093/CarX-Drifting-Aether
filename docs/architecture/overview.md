# アーキテクチャの概要

## 全体

```
CarX Drift Racing Online（Unity 2023.2、-force-d3d12）
      │
   Mod ローダー（BepInEx か KSL）── Aether.Managed（C#、src/Aether.Managed）
      │                              ・AetherNative.dll を Unity のプラグインとして読み込ませる
      │                              ・Aether のレンダーパイプライン（HDRP の代わり）
      │                              ・シーンのキャプチャ（Scene Data Contract で毎フレーム渡す）
      │                              ・描画イベントの発行、Unity の統計の送信
      ▼
AetherNative.dll（C++、src/Aether.Native）
      ├─ Device      IUnityGraphicsD3D12v7 → HostGfx、C API
      ├─ Renderer    コマンドリスト、シーンのレンダラー、レンダーグラフ、BVH
      ├─ Resources   GPU リソースマネージャー、リソースの登録簿
      ├─ Scene       Scene Data Contract のネイティブ側
      ├─ Shaders     HLSL の実行時コンパイル
      ├─ ImGui       ゲーム内オーバーレイ（F10）
      ├─ Profiler    フレーム統計、ベンチマーク
      ├─ Core        構造化ログ、設定、実験用スイッチ
      └─ Ipc         名前付きパイプ \\.\pipe\Aether.Runtime（JSON 1 行 = 1 要求）
                                   ▲
Aether.MCP.exe（C#、src/Aether.MCP）──┘   stdio の MCP ⇔ パイプ。ゲームとは別プロセス
      ▲
   AI エージェント
```

- Unity は物理、ゲームロジック、シーン管理、入力、カリング、スキニング、UI を担当します。Aether が受け持つのはシーンの描画と、その計測・診断です。
- 既定の `renderMode`（`aether`）では、Aether のパイプラインアセットがゲームの HDRP の代わりに入り、HDRP は作られません（[hdrp-independence.md](hdrp-independence.md)）。`hdrp`（ゲームの描画だけ）と `compare`（左半分がゲーム、右半分が Aether）は、参照用の絵と計測のためにあります。`standalone` は `aether` の古い名前です。
- Aether の設定は `aether.json` にあり、オーバーレイ（F10）と MCP の `set_setting` から変えます。ゲームのグラフィックスメニューの値は使いません。
- MCP サーバーはゲームと別プロセスです。ゲームが居なければ各ツールが `E_NOT_RUNNING` を返し、次の呼び出しで再接続します（[mcp-api.md](../diagnostics/mcp-api.md)）。

## モジュール

### C#（`src/Aether.Managed`）

| 場所 | 役割 |
|---|---|
| `Loaders/BepInEx/AetherPlugin.cs`、`Loaders/Ksl/AetherKslMod.cs` | ローダーごとの入口。`IAetherHost`（データフォルダー、設定フォルダー、ゲームのフォルダー、ログ）を実装して `AetherCore` を作る |
| `AetherCore.cs` | ネイティブの起動と終了、描画イベントの発行、統計とシーン情報の送信、`renderMode` に合わせたパイプラインの切り替え |
| `Native.cs` | C API の宣言（`include/AetherApi.h` と対）と、DLL を Unity のプラグインフォルダーへ置く処理 |
| `Pipeline/AetherPipeline.cs` | `AetherPipelineAsset` / `AetherPipeline`（カメラごとのカリング、ターゲットのクリア、画面のカメラでのシーンイベント、カメラの UI）、`PipelineSwitch`（パイプラインアセットの入れ替え、Unity の `VolumeManager` の起動） |
| `SceneCapture/` | メッシュ、テクスチャー、マテリアル、インスタンス、ライト、プローブ、パーティクル、タイヤスモークを集めて Scene Data Contract で渡す（[scene-capture.md](scene-capture.md)、[material-translation.md](material-translation.md)） |
| `GameIntegration/` | DX12 でゲームを動かすための対処（`HdrpDx12Fix`、`GameDx12Workarounds`）、ゲームの設定の読み取り（`HostFrameSettings`）、Aether の画質設定を Unity に書く（`HostQualitySettings`）、オーバーレイ使用中のゲームの入力の遮断（`GameInputBlock`） |
| `Diagnostics/` | Unity の統計（`UnityStats`）、開発用の固定カメラ（`DevCamera`） |

### C++（`src/Aether.Native`）

| 場所 | 役割 |
|---|---|
| `Device/UnityBridge.cpp` | Unity のプラグインの入口（`UnityPluginLoad`）、描画イベントの設定と受付、`HostGfx` の実装、C API の実装 |
| `Device/Host.h` | レンダラーとデバイスの持ち主の境界（下の「ホストの境界」） |
| `Device/GpuInfo.cpp` | アダプターの情報、VRAM の使用量と予算 |
| `Session.cpp` | 起動と終了。描画スレッドの入口（1 度に 1 つ、例外を受け止める） |
| `Runtime.cpp` | フォルダー、ワーカースレッド、C# 側から届いた状態、クラッシュレポート、AI エージェントの操作要求の承認 |
| `Diagnostics.cpp` | `get_status` と `get_vram_usage` の中身 |
| `Renderer/` | `Renderer`（イベントごとのコマンドリスト、完了の判定、タイムスタンプ、キャプチャ）、`SceneRenderer`、`RenderGraph`、`GpuTimer`、`Bvh`、`Smoke`、`Png`（[renderer.md](renderer.md)） |
| `Resources/` | `GpuResources`（ディスクリプター、テクスチャー、アップロードアリーナ、退役）、`ResourceRegistry`（Aether が作った全リソースの台帳）（[gpu-resources.md](gpu-resources.md)） |
| `Scene/SceneStore.cpp` | C# から届いたシーンの記述を保持し、描画スレッドで適用する（[scene-data-contract.md](scene-data-contract.md)） |
| `Shaders/ShaderManager.cpp` | `shaders/*.hlsl` を DXC（シェーダーモデル 6.5、HLSL 2018）でコンパイル。エラーはデータとして残し、再読み込みできる |
| `ImGui/Overlay.cpp` | Dear ImGui のオーバーレイ（設定、統計、VRAM、シェーダー、ログ、承認ダイアログ） |
| `Profiler/` | フレームの CPU / GPU 時間、パスごとの時間、Unity の統計、ベンチマーク |
| `Core/` | 構造化ログ、設定（値、スキーマ、検証を `Config.cpp` の 1 か所に持つ）、`DevOptions`（実験用スイッチ） |
| `Ipc/` | 名前付きパイプのサーバーと、MCP の各メソッドの実装（`Api.cpp`） |
| `include/` | C API（`AetherApi.h`）と Scene Data Contract（`SceneContract.h`） |

### そのほか

| 場所 | 役割 |
|---|---|
| `shaders/` | Aether の HLSL（シーン、全画面パス、パーティクル、BVH、テスト三角形） |
| `src/Aether.MCP` | MCP サーバー。ツールの定義は `Tools/ToolCatalog.cs` |
| `tests/host` | テストホスト。自前のデバイスとウィンドウで Aether を Unity と同じ順序で動かす |

## スレッド

| スレッド | 何をするか | 規則 |
|---|---|---|
| Unity のメインスレッド | C# の Mod。パイプラインの `Render`、シーンのキャプチャ、描画イベントの発行、統計の送信。オーバーレイのウィンドウプロシージャ（入力をキューに積むだけ） | 描画スレッドを待たない |
| Unity の描画スレッド | 描画イベント。`Renderer`、`SceneRenderer`、`SceneStore::Apply`、`Overlay`（ImGui のフレーム構築と記録） | D3D12 のオブジェクトに触るのはここだけ。入口は `Session.cpp` で例外を受け止める |
| IPC スレッド（受付 1 + クライアントごと） | MCP の要求に答える。スナップショットを読むだけ | 描画スレッドを待たせない、待たない。承認待ちや C# 側の収集待ちはここで行う |
| ワーカー 1 本（`Runtime`） | PNG のエンコード、ファイル書き込み | |
| BVH のワーカー 1 本 | メッシュごとの BVH の構築と更新 | 結果は描画スレッドが GPU バッファーに入れる |

C# とネイティブのあいだを流れるもの：

- 毎フレームの数値（Unity の統計、double の配列：`Aether_SubmitUnityStats`）。
- シーン（Scene Data Contract の構造体：`Aether_SceneUpsert*`、`Aether_SceneSubmitFrame` ほか）。メインスレッドは記述を保留リストにコピーするだけで、描画スレッドが次のシーンのフレームの初めに適用します。
- 要求があったときだけの JSON（シーンの調査、マテリアル、Unity のレンダーテクスチャー、調査用の survey）。ネイティブ側が欲しいもののビットを立て、C# 側が次のフレームで集めて渡します（`Aether_TakeManagedRequests` / `Aether_SetManagedState`）。
- C# 側が読む設定値（`Aether_SettingNumber`、`Aether_GetRenderMode`）。

## ホストの境界（`Device/Host.h`）

レンダラーは `HostGfx` だけを相手にします。

| メンバー | 意味 |
|---|---|
| `device`、`queue` | ホストのデバイスとダイレクトキュー（ホストが持つ） |
| `swapChain()` | ホストが Present するスワップチェーン（毎フレーム問い合わせる） |
| `execute(list, count, states)` | 閉じたリストをホストに渡して提出順に実行させる。ホストのフレームフェンスの値を返す |
| `completedFenceValue()`、`nextFenceValue()` | ホストのフレームフェンスの完了値と次の値 |
| `waitIdle(timeoutMs)` | それまでに提出したものが GPU で終わるまで待つ |

ゲームでは Unity（`UnityBridge.cpp`）、テストではテストホスト（`tests/host/HostMain.cpp`、自前のデバイスとウィンドウ）がこれを埋めます。
グラフィックス API の抽象化ではありません（中身はすべて D3D12）。ゲームを起動せずに、デバッグレイヤー付きで統合の規則を検査するための境界です。

`Renderer` は受け取った `HostGfx` のフェンスの関数を Aether 自身の完了番号に置き換えたものを作り、`SceneRenderer` とオーバーレイにはそちらを渡します（[dx12-integration.md](dx12-integration.md) の「完了の判定」）。

## フレームの流れ

`renderMode` が `aether` のとき：

1. **メインスレッド `Update`**：`FRAME_BEGIN` を発行（GPU タイムスタンプの起点）。統計の送信、C# 側への要求の収集、シーンのキャプチャの更新（Unity のネイティブポインターの問い合わせはここだけで行う）。
2. **Unity が `AetherPipeline.Render` を呼ぶ**：カメラごとに `context.Cull`（`Renderer.isVisible`、LOD、GPU スキニングを生かすため）、`SetupCameraProperties`、ターゲットのクリア。
3. **画面のカメラ**（`PickScreenCamera`：画面に描く、有効なゲームのカメラのうち最も深度の大きいもの）：`AetherCore.DrawScene` がこのフレームのカメラ、インスタンスのトランスフォーム、ライトを `Aether_SceneSubmitFrame` で渡し、コマンドバッファーで `SCENE` イベントをコンテキストに入れます。
4. **描画スレッドの `SCENE`**：`Renderer::SceneFrame` がリストを取り、`SceneRenderer::Render` が保留中のシーンの変更を適用してフレームを記録し、`Composite` がバックバッファーに書きます。リストは Unity のキューに提出されます。
5. **メインスレッドに戻って**：カメラの UI（`DrawRenderers`、UI レイヤー）、`context.Submit`。オーバーレイのキャンバスは Unity 自身が描きます。最後に Aether の画質設定（VSync、LOD バイアス）を書き直します。
6. **`WaitForEndOfFrame`**：`FRAME_END` を発行。描画スレッドでテスト三角形、オーバーレイ、要求があればキャプチャを記録して提出。
7. Unity がバックバッファーを `PRESENT` に遷移させて Present します。

`compare` ではゲームの HDRP がカメラを描き、`RenderPipelineManager.endContextRendering` で 3. と同じことをします。`hdrp` ではシーンを記録しません。
イベントの設定（キューへのアクセス、Unity の保留分の提出）と、各段階で守っている規則は [dx12-integration.md](dx12-integration.md)。

## 関連する文書

- [dx12-integration.md](dx12-integration.md)：Unity のデバイスとキューの使い方、規則、完了の判定、DX12 でゲームを動かすための対処
- [renderer.md](renderer.md)：レンダーグラフ、フレームのパス、バインディング、ライティングの骨組み、影
- [rendering-features.md](rendering-features.md)：個々の描画機能
- [gpu-resources.md](gpu-resources.md)：GPU リソース、ディスクリプター、退役、VRAM の内訳
- [hdrp-independence.md](hdrp-independence.md)：HDRP なしで動く仕組み
- [scene-capture.md](scene-capture.md)、[scene-data-contract.md](scene-data-contract.md)、[material-translation.md](material-translation.md)：シーンの取り込み
- [../diagnostics/mcp-api.md](../diagnostics/mcp-api.md)、[../diagnostics/troubleshooting.md](../diagnostics/troubleshooting.md)：診断
- [../development/build.md](../development/build.md)：ビルドと配置
