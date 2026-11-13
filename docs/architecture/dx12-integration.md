# DX12 統合

Aether がゲームの D3D12 デバイスとキューの上でどう動くか、守っている規則、完了とリソースの寿命の判定、ゲーム側を DX12 で動かすための対処をまとめます。
実装は `src/Aether.Native/Device/UnityBridge.cpp`、`Renderer/Renderer.cpp`、`Session.cpp`、`src/Aether.Managed/`。

## Unity の D3D12 デバイスを使う

Unity Native Plugin Interface の `IUnityGraphicsD3D12v7` から、デバイス、ダイレクトキュー、スワップチェーン、フレームフェンスを取得し、コマンドリストは `ExecuteCommandList` で Unity に渡します。
独自のデバイス、DXGI のプロキシ、Present のフックは使いません。

| 必要なもの | 取得方法 |
|---|---|
| `ID3D12Device` | `GetDevice()` |
| `ID3D12CommandQueue` | `GetCommandQueue()` |
| `IDXGISwapChain` | `GetSwapChain()`（毎フレーム問い合わせる） |
| コマンドリストの実行 | `ExecuteCommandList(list, 0, nullptr)` |
| Unity のフレームフェンス | `GetFrameFence()` / `GetNextFrameFenceValue()`（コマンドリストの再利用の追加条件にだけ使う） |
| 実行完了の判定 | Aether 自身の完了マーカーとフェンス（下の「完了の判定」） |
| 終了時の GPU 待ち | Aether のフェンス（`Aether.IdleFence`）を Unity のキューに `Signal` して待つ（`HostGfx::waitIdle`） |

`UnityBridge.cpp` がこれらを `HostGfx`（`Device/Host.h`）に詰めます。レンダラーは `HostGfx` だけを相手にします（[overview.md](overview.md) の「ホストの境界」）。
Unity のレンダラーが D3D12 でなければ `E_NOT_D3D12` を記録して何もしません。ゲームは `-force-d3d12` で起動します。

### Unity にプラグインとして読み込ませる

`IUnityInterfaces` は、Unity が自分のプラグインローダーで DLL を読み込んだときに呼ぶ `UnityPluginLoad` でしか受け取れない、という前提で実装しています。
そのため `AetherNative.dll` は Unity のプラグインフォルダー（`<Game>_Data\Plugins\x86_64\`）に置き、C# 側の最初の P/Invoke（`Aether_GetApiVersion`）で Unity に読み込ませます。
Mod は起動時、最初の P/Invoke の前に、データフォルダーの `native\AetherNative.dll` をそこへコピーします（無いとき、サイズか更新時刻が違うとき。`Native.Prepare`）。

開発用のスイッチ：設定フォルダーに `dev-preload` というファイルを置くと、コピーせずに `native\` から `LoadLibrary` で読み込みます。この読み込み方で `UnityPluginLoad` が呼ばれることは前提にしていません。

### 描画イベント

C# 側は `Aether_GetRenderEventFunc` の関数を、4 つのイベント番号で呼ばせます。各イベントの設定（`ConfigureEvent`）：

| イベント | 番号 | `graphicsQueueAccess` | `flags` | 理由 |
|---|---|---|---|---|
| `FRAME_BEGIN` | 1 | `Allow` | `SyncWorkerThreads` | タイムスタンプのリストを提出する。提出はキューへのアクセスを許可したイベントの中でしか許されない。Unity の保留分を先に出す必要はない |
| `FRAME_END` | 2 | `Allow` | `FlushCommandBuffers \| SyncWorkerThreads` | Unity がそれまでに記録した描画（UI を含む）を先に提出させ、オーバーレイがその上に載るようにする。Dear ImGui のバックエンドがフォントのテクスチャーをキューに直接アップロードするあいだ、Unity のワーカーをキューから外す |
| `SHUTDOWN` | 3 | `Allow` | `FlushCommandBuffers \| SyncWorkerThreads` | Unity の保留分を提出させてから GPU を待ち、解放する |
| `SCENE` | 4 | `Allow` | `FlushCommandBuffers \| SyncWorkerThreads` | ゲームのカメラの分を先に提出させ、Aether のリストをゲームの UI より前に入れる |

どのイベントも `ensureActiveRenderTextureIsBound = false` です。ゲームの設定は `gfx-threading-mode=6`（`NativeGraphicsJobsSplitThreading`）で、記録と提出が別スレッドです。

### フレームの流れ

```
メインスレッド（C#）                                   Unity の描画スレッド（AetherNative）
Update:
  IssuePluginEvent(FRAME_BEGIN) ──────────────────▶ GPU タイムスタンプ 1 個のリストを提出
AetherPipeline.Render（renderMode aether）
  カメラごとに Cull、ターゲットのクリア
  画面のカメラ：SceneCapture.SubmitFrame（このフレームのシーンを渡す）
                コマンドバッファーで IssuePluginEvent(SCENE) ─▶ シーンの変更を適用、シーンの 1 フレームを記録して提出
  カメラの UI（DrawRenderers）、context.Submit
（Unity のオーバーレイ UI）
WaitForEndOfFrame:
  IssuePluginEvent(FRAME_END) ────────────────────▶ バックバッファーを取得
                                                   テスト三角形 → オーバーレイ →（要求があれば）キャプチャ、提出
                                                 Unity が PRESENT へ遷移して Present
```

- `SCENE` はパイプラインのコマンドストリーム（`ScriptableRenderContext` で実行するコマンドバッファー）に入れるので、描画スレッドでパイプラインが記録したものと順序がそろいます。
- `renderMode` が `compare` のときはゲームの HDRP が描き、`RenderPipelineManager.endContextRendering` で同じように `SCENE` を出します。`hdrp` のときはシーンの記録はしません（キャプチャを止めた直後に 1 回だけ、ネイティブ側がハンドルを手放すための `SCENE` を出します）。
- 描画スレッドの入口は `Session.cpp` です。1 度に 1 つの入口しか走らせず、構造化例外と C++ 例外を受け止めます。

## 守っている規則

1. **Unity のリソースの状態を変えない。宣言もしない。** バックバッファーは、Aether のイベントが来る時点（シーンのイベントもフレーム末尾も）で `RENDER_TARGET` になっています。Aether はそのまま描き、状態を変えずに返します。`ExecuteCommandList` の `states` は使いません（0 個）。リストの中で一時的に変える場合（キャプチャの `COPY_SOURCE`）は必ず元に戻します。レンダーグラフもホストのリソースは遷移させません。Unity から借りているメッシュバッファーとテクスチャーは読むだけです。起動直後はバックバッファーがまだ `RENDER_TARGET` でないことがあるので、フレーム末尾のリストは最初の 8 フレームは何も描きません。
2. **バックバッファーへの参照をフレームをまたいで持たない。** 毎フレーム `GetBuffer` し、提出したら手放します（`ResizeBuffers` を妨げない）。テストホストの `--resize` がこれを検査します。
3. **コマンドリストとアロケーターは、(a) Aether 自身がそのリストの完了を確認し（下の「完了の判定」）、(b) Unity のフレームフェンスが `ExecuteCommandList` の返した値を超え、(c) その後 24 回の提出が過ぎてから再利用する。** 空きがなければ新しく作り（上限 48）、それでも無ければそのフレームの Aether の描画を飛ばします。
4. **リストを提出するのは、キューへのアクセスを許可したイベント（`kUnityD3D12GraphicsQueueAccess_Allow`）の中だけ。** 描画スレッド以外から D3D12 のオブジェクトに触りません。
5. **オーバーレイのバッファー**（Dear ImGui のバックエンドが `NumFramesInFlight` ごとに使い回す）も Aether の完了番号で判断し、古い描画が終わっていなければそのフレームのオーバーレイを飛ばします。
6. **失敗したら Aether だけが止まる。** HRESULT の失敗は `Renderer::Fail` で以後の描画を止めます。原因、デバイス削除の理由、取れれば DRED（ページフォルトのアドレス、途中で止まったリスト）、GPU 情報、ログの末尾をクラッシュレポートに書きます。シーンのフレームの前にデバイスが削除されていれば、直前のシーンの統計を記録して止めます（`E_DEVICE_REMOVED`）。描画スレッドの入口で例外を受け止めた場合も同じく止めます（`E_FAULT`）。ゲームは動き続けます。
7. **解放は GPU が終わってから。** 終了時は `SHUTDOWN` イベント（描画スレッド、Unity の保留分を提出済み）で、Aether の最後のリストの完了を待ち（`waitIdle` で後押しする）、それから解放します。確認できないとき（描画スレッドが応答しない、障害の後）は解放せずに放置します（解放して GPU を落とすよりよい）。Unity がデバイスを破棄するとき（`kUnityGfxDeviceEventShutdown`）も同じ手順で解放します。
8. **グローバルなフックを入れない。** 入力はゲームのウィンドウのサブクラス化（終了時に戻す）だけです。Present、D3D12、DXGI のフックはありません。

## 完了の判定

Unity のフレームフェンスは、`ExecuteCommandList` が返した値でリストの完了を保証しません（記録と提出が別スレッドなので、そのリストが実際にキューへ入る前に値が進むことがある）。
そのため Aether は自分の時計を持ちます（`Renderer::Submit` / `Renderer::Completed`）。

1. **完了マーカー**：Aether のリストは必ず最後に `CopyBufferRegion` で自分の通し番号を読み戻しバッファーへ書きます。CPU がその番号を読めたら、Unity が実際にそのリストをキューへ入れ、GPU が最後まで実行したことになります。
2. **自前のフェンス**：マーカーで確認した番号を、Unity のキューに対して Aether のフェンスで `Signal` します。この `Signal` はそのリストより後ろに並ぶので、フェンスがその値になれば D3D12 ランタイムから見ても実行は完了しています。完了番号はこのフェンスの値です。

キューの中の実行順は提出順なので、完了番号より小さい番号のリストもすべて終わっています。

`Completed()` が返す番号が、次のすべての時計です。

- コマンドアロケーターの再利用（規則 3 の (a)）
- アップロードアリーナ、ディスクリプター範囲、退役キュー（Unity から借りた参照を含む。[gpu-resources.md](gpu-resources.md)）
- タイムスタンプとキャプチャの回収
- オーバーレイのバッファー
- 終了時の待機

`SceneRenderer` とオーバーレイには、`HostGfx` のフェンスの関数を Aether の完了番号に置き換えたもの（`Renderer::clock_`）を渡します。シーンのフレームが退役させるものは、そのフレーム自身のリストの番号に対して退役させます。

### コマンドリストの再利用

- 1 つの `Context` がアロケーターとリストを 1 組持ちます。規則 3 の 3 条件をすべて満たすものだけを `Reset` して使います。
- (c) の 24 回は、Unity が渡されたリストの記録を自分で持っているためです。その記録が生きているうちに同じリストオブジェクトを渡し直すと、Unity の管理が壊れます。
- 空きが無いときの内訳は `get_status` の `renderer.commandLists`（GPU 待ち、ホストのフェンス待ち、再利用の間隔待ち、空きが無くて飛ばした回数）で見られます。Aether の描画が抜けたフレーム（ちらつき）は、ここの `noneFree` で分かります。

## デバッグレイヤー

デバイスに `ID3D12InfoQueue1` があれば（デバッグレイヤーが有効なら）メッセージコールバックを登録し、エラーと破損を Aether のログに `E_D3D12_VALIDATION` / `E_D3D12_CORRUPTION` として書きます（メッセージ ID とカテゴリー付き、1 セッション 60 件まで）。`get_logs` で読めます。
Unity の `Player.log` には D3D12 のメッセージは出ません。

- ゲーム：`scripts\launch-game.ps1 -DebugLayer`（Unity の `-force-d3d12-debug`）で起動し、`get_logs` を読みます。
- テストホスト（`tests/host`）：自前のデバイスでデバッグレイヤーを有効にし、Unity と同じ順序（フレーム開始、「ゲーム」の描画、`RENDER_TARGET` のバックバッファーでのフレーム末尾、Present）で Aether を動かします。D3D12 のエラー、オブジェクトのリーク、Aether の障害のどれかがあれば終了コードが 0 以外になります。

## ゲームの HDRP は DX12 だけでは構築できない

`-force-d3d12` で起動したゲームは、Aether の有無にかかわらず HDRP を構築できません。毎フレーム `HDRenderPipeline` のコンストラクター（`MipGenerator`）から次の例外が出ます。

```
ArgumentException: Kernel 'KDepthDownsample8DualUav' not found.
```

ゲームのビルドに入っている HDRP のコンピュートシェーダーに DX12 用のカーネルが無いためです。何も描かれません（黒い画面、UI も出ない）。

`renderMode` が `aether`（既定）のときは、ゲームの HDRP がそもそも作られないので、この問題は起きません（[hdrp-independence.md](hdrp-independence.md)）。HDRP が動くのは `hdrp` と `compare` のとき、それと Aether が起動できなかったとき（ネイティブ DLL が無い、API の版が合わない、Unity が D3D12 のインターフェースを渡さない、Scene Data Contract が合わない）です。そのときだけ次の対処を入れます。

### HDRP のコンピュートシェーダーの差し替え（`GameIntegration/HdrpDx12Fix.cs`）

HDRP のランタイムリソース（`HDRenderPipelineGlobalSettings.m_RenderPipelineResources.shaders`）の `ComputeShader` を、同じ版の HDRP（16.0.6）を DX12 向けにビルドしたものに名前で差し替えます。名前を変えてあるものは、バンドルに入れた対応表（`aether_hdrp_compute_map`）で元の名前を引きます。
HDRP のほかの部分には触りません。Unity は構築を毎フレームやり直すので、差し替えた次のフレームから HDRP が動きます。

- バンドル：データフォルダーの `hdrp\aether_hdrp_dx12.bundle`。`scripts\build-hdrp-bundle.ps1` で、リポジトリの外の Unity プロジェクトから作ります。
- HDRP はビルド時に、ビルドに含まれる HDRP アセットの設定に合わないカーネルを除去します（アセットが無ければ全部除去する）。そのためゲームが使う設定（影のフィルター品質、スクリーンスペースシャドウ、ライトプローブの方式など）を `unity/hdrp-game-settings.json` に記録し、その値の HDRP アセット 1 つでビルドします。値は `hdrp` / `compare` で動いているゲームから読めます（MCP の `get_status` の `managed.renderPipeline.hdrp`）。
- 水、SSS、ライン描画、デバッグ用のシェーダーも丸ごと入れています（HDRP は使わないシェーダーのカーネルも探すため）。
- 設定フォルダーに `no-hdrp-fix` を置くと差し替えません。状態は `get_status` の `managed.hdrpDx12Fix` とゲーム内の表示に出ます。

### ゲーム独自のカスタムパス（`GameIntegration/GameDx12Workarounds.cs`）

HDRP が構築できても、ゲーム独自の HDRP カスタムパスが同じ理由で毎フレーム例外を投げます。

| パス | 例外 |
|---|---|
| `GpuCulling.GpuCullingPass` | `Kernel 'CalculateCullResult' not found` |
| `ParticleRenderPass` | `Kernel 'BlurMS' not found` |

例外は HDRP のレンダーグラフの実行中に出るので、そのフレームの残りが実行されません。これらはゲーム独自のシェーダーでソースが無く、作り直せません。DX12 では該当パスを無効化します（`CustomPass.enabled = false`）。
既知の 2 つに加え、Unity のログに「カスタムパスの実行中に `Kernel '...' not found`」が出たら、スタックトレースからパスの型を読んで同じ扱いにします。起動時とシーンを読み込んだあと（と新しい型が見つかったとき）、15 秒間、0.5 秒ごとに探します。
設定フォルダーに `keep-game-passes` を置くと無効化しません。状態は `get_status` の `managed.gamePassesDx12`。

## DX12 では Unity の VideoPlayer を止める

ゲームの広告 SDK（ANZU）は動画広告を Unity の `VideoPlayer` で再生します。DX12 では、クリップが替わるときに Unity が動画のフレーム（`A8_UNORM`、1080x608）を、その間に差し替えられたプレーンのテクスチャーへアップロードします。この不正なコピーはデバッグレイヤーに報告され、デバイスの喪失かプレイヤーのクラッシュになります。ANZU のネイティブ描画は D3D9 / D3D11 / OpenGL 用で、DX12 は想定されていません。

そのため DX12 では、有効な `VideoPlayer` を 2 秒ごとに探して無効にします（`GameDx12Workarounds.UpdateVideos`）。画像の広告と ANZU のほかの部分はそのままです。状態はゲーム内の表示（Video playback on DX12）とログに出ます。

| 設定フォルダーのファイル | 効果 |
|---|---|
| `keep-game-videos` | 何も止めない |
| `disable-game-ads` | `VideoPlayer` に加えて、型名に `Anzu` を含むコンポーネントも止める |

## 実験用スイッチ

設定フォルダーの `dev-options.json`（`Core/DevOptions.h`）で、ホストとの統合を切り分けるための動作を変えられます。リストの提出方法（`directSubmit`）、イベントのフラグ（`beginFlags` / `endFlags` / `sceneFlags`）、完了をマーカーだけで判定する（`noSignal`）、記録だけして提出しない（`recordOnly`）、1 種類の呼び出しだけをする（`probe`）、描画スレッドを待たせる（`sleepMs`）、キャプチャの制限（`captureFlags`）、提出の統計のログ間隔（`statsSeconds`）。
ファイルが無ければすべて既定値で、何も変わりません。設定ではないので、オーバーレイにも MCP にも出ません。

## 制限

- `hdrp` / `compare` で、ゲーム独自のカスタムパス（GPU カリング、パーティクル）が描くもの・カリングするものは出ません。
- ゲームが HDRP アセットの設定を変えると、バンドルに無い組み合わせのカーネルが足りなくなります。そのときは `unity/hdrp-game-settings.json` を更新してバンドルを作り直します。
- バンドルは HDRP のシェーダーをコンパイルしたもの（Unity Companion License）を含みます。配布の扱いは決まっていません。AssetBundle は KSL の署名の対象ではありません。
- DX12 では動画広告が再生されません。
