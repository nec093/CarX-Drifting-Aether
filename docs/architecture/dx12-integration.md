# DX12 統合

Aether がゲームの D3D12 デバイス上でどう動くか、その方式を決めた PoC の結果、守っている規則をまとめます。
実装は `src/Aether.Native/Device/UnityBridge.cpp`、`Renderer/Renderer.cpp`、`Session.cpp`、`src/Aether.Managed/`。

## 採用した方式：Unity の D3D12 デバイスを使う

Unity Native Plugin Interface の `IUnityGraphicsD3D12v7`（Unity 2023.2 のエディター付属ヘッダー `IUnityGraphicsD3D12.h`）から、
デバイス、ダイレクトキュー、スワップチェーン、フレームフェンスを取得し、コマンドリストは `ExecuteCommandList` で Unity に渡します。
独自デバイス、DXGI のプロキシ、Present のフックは使いません。

| 必要なもの | 取得方法 |
|---|---|
| `ID3D12Device` | `GetDevice()` |
| `ID3D12CommandQueue` | `GetCommandQueue()` |
| `IDXGISwapChain` | `GetSwapChain()`（毎フレーム問い合わせる） |
| コマンドリストの実行と状態の受け渡し | `ExecuteCommandList(list, stateCount, states)` |
| 実行完了の判定 | Aether 自身の完了マーカーとフェンス（下の「完了の判定」。Unity のフレームフェンスは使わない） |

### Unity にプラグインとして読み込ませる

`IUnityInterfaces` は、Unity が自分のプラグインローダーで DLL を読み込んだときに呼ぶ `UnityPluginLoad` でしか受け取れません。
そのため `AetherNative.dll` は Unity のプラグインフォルダー（`<Game>_Data\Plugins\x86_64\`）に置き、C# 側の最初の P/Invoke（`[DllImport("AetherNative")]`）で Unity に読み込ませます。
Mod は起動時、最初の P/Invoke の前に、`kino\mods\Aether\native\AetherNative.dll` をそこへコピーします（無いか、内容が違うとき）。

- ゲーム内で確認：`Unity interfaces: yes; Direct3D 12 interface: yes`（2026-11-11、コピーしたその起動で有効）。
- 未確認：`kino\mods\Aether\native\` から `LoadLibrary` しただけで `UnityPluginLoad` が呼ばれるか。試すためのスイッチ（`kino\config\Aether\dev-preload` というファイルを置く）は入っていますが、実行していません。呼ばれるなら、ゲームの `_Data` フォルダーにファイルを足さずに済みます。

### フレームの流れ

```
メインスレッド（C#）                         Unity の描画スレッド（AetherNative）
Update:  GL.IssuePluginEvent(FRAME_BEGIN) ─▶ GPU タイムスタンプ 1 個のリストを ExecuteCommandList
         （HDRP がゲームのカメラを描く）
endContextRendering（画面のカメラ）:
         シーンのフレームを渡す（Scene Data Contract）
         GL.IssuePluginEvent(SCENE)       ─▶ シーンの変更を適用、Forward+ の 1 フレームを記録（aether / compare のとき）
                                             ExecuteCommandList(list, 0, null)
         （ゲームのオーバーレイ UI）
WaitForEndOfFrame:
         GL.IssuePluginEvent(FRAME_END)   ─▶ バックバッファーを取得
                                             テスト三角形 → オーバーレイ →（要求があれば）キャプチャ
                                             ExecuteCommandList(list, 0, null)
                                           Unity が PRESENT へ遷移して Present
```

イベントの設定（`ConfigureEvent`）：

| イベント | `graphicsQueueAccess` | `flags` | 理由 |
|---|---|---|---|
| `FRAME_BEGIN` | `DontCare` | 0 | タイムスタンプのリストを渡すだけ |
| `FRAME_END`、`SHUTDOWN`、`SCENE` | `Allow` | `FlushCommandBuffers \| SyncWorkerThreads` | Unity がそれまでに記録した描画（UI を含む）を先に提出させ、Aether のパスがその上に載るようにする。Dear ImGui のバックエンドがフォントのテクスチャーをキューに直接アップロードするあいだ、Unity のワーカーをキューから外す |

ゲームの設定は `gfx-threading-mode=6`（`NativeGraphicsJobsSplitThreading`）で、記録と提出が別スレッドです。上の設定で、オーバーレイが HDRP の絵と UI の上に描かれることをゲーム内のキャプチャで確認しています。

## 守っている規則

1. **Unity のリソースの状態を変えない。宣言もしない。** バックバッファーは、Aether のイベントが来る時点（シーンのイベントもフレーム末尾も）で RENDER_TARGET になっている（デバッグレイヤーで確認）。Aether はそのまま描き、状態を変えずに返す。`ExecuteCommandList` の `states` は使わない（0 個）。リストの中で一時的に変える場合（キャプチャの `COPY_SOURCE`）は必ず元に戻す。Unity から借りているメッシュバッファーとテクスチャーは読むだけ。起動直後の数フレームはバックバッファーがまだ RENDER_TARGET でないことがあるので、最初の 8 フレームは何も描かない。
2. **バックバッファーへの参照をフレームをまたいで持たない。** 毎フレーム `GetBuffer` し、提出したら手放す（`ResizeBuffers` を妨げない）。テストホストはリサイズで確認している。
3. **コマンドリストとアロケーターは、(a) Aether 自身がそのリストの完了を確認し（下の「完了の判定」）、(b) Unity のフレームフェンスが `ExecuteCommandList` の返した値を超え、(c) その後 24 回の提出が過ぎてから再利用する。** 空きがなければ新しく作り（上限 48）、それでも無ければそのフレームは描かない。
4. **オーバーレイのバッファー**（Dear ImGui のバックエンドが `NumFramesInFlight` ごとに使い回す）も同じ規則で、古い描画が終わっていなければそのフレームのオーバーレイを飛ばす。
5. **失敗したら Aether だけが止まる。** HRESULT の失敗は `Renderer::Fail`（原因、デバイス削除理由、取れれば DRED、GPU 情報、ログの末尾をクラッシュレポートに書く）で以後の描画を止める。描画スレッドの入口は構造化例外と C++ 例外を受け止める（`Session.cpp`）。ゲームは動き続ける。
6. **解放は GPU が終わってから。** 終了時は `SHUTDOWN` イベント（描画スレッド、Unity の保留分を提出済み）で、Aether 自身のフェンスをキューに `Signal` して待ち、それから解放する。確認できないとき（描画スレッドが応答しない、障害の後）は解放せずに放置する（解放して GPU を落とすよりよい）。
7. **グローバルなフックを入れない。** 入力はゲームのウィンドウのサブクラス化（終了時に戻す）だけ。Present、D3D12、DXGI のフックは無い。

## 完了の判定（Phase 2 で変更）

Phase 1 は「`ExecuteCommandList` が返したフェンス値を Unity のフレームフェンスが超えたら完了」としていました。
ゲームの設定（`NativeGraphicsJobsSplitThreading`：記録と提出が別スレッド）では、これが成り立ちません。
シーンのリストを毎フレーム出すようになって、デバッグレイヤーが
`ID3D12CommandAllocator::Reset: ... is being reset before previous executions associated with the allocator have completed`
を報告し、続いてデバイスが `DXGI_ERROR_DEVICE_HUNG` で失われました（Unity が返す値は、そのリストが実際にキューへ入る前に完了扱いになることがある）。

現在の方式（`Renderer::Submit` / `Renderer::Completed`）は 2 段階です。

1. **完了マーカー**：Aether のリストは必ず最後に `CopyBufferRegion` で自分の通し番号を読み戻しバッファーへ書く。CPU がその番号を読めたら、Unity が実際にそのリストをキューへ入れ、GPU が最後まで実行したことになる。
2. **自前のフェンス**：マーカーを確認した番号を、Unity のキューに対して Aether のフェンスで `Signal` する。この `Signal` はそのリストより後ろに並ぶので、フェンスがその値になれば D3D12 ランタイムから見ても実行は完了している（マーカーだけだと、GPU の書き込みとランタイムの完了通知のわずかな差をデバッグレイヤーに指摘される）。

`Completed()` が返す通し番号が、アロケーターの再利用、アップロードアリーナ、ディスクリプター範囲、退役キュー（Unity から借りた参照を含む）、タイムスタンプとキャプチャの回収、オーバーレイのバッファー、終了時の待機、すべての時計です。Unity のフレームフェンスは、コマンドリストの再利用の追加条件にだけ使います（規則 3）。

## ゲーム内のデバッグレイヤー（未解決の問題あり）

Aether 自身の検証エラーは 0 件ですが、デバッグレイヤーを有効にして Aether がリストを提出していると、起動後 141〜271 秒で Unity が
`CopyTextureRegion: pResource in the first parameter is corrupt` を出して落ちます。原因は未特定です。実験の一覧と仮説は `docs/development/phase2-report.md` の 1 章。

## デバッグレイヤーのメッセージ

デバイスに `ID3D12InfoQueue1` があれば（デバッグレイヤーが有効なら）メッセージコールバックを登録し、エラーと破損を Aether のログに
`E_D3D12_VALIDATION` / `E_D3D12_CORRUPTION` として書きます（`get_logs` で読める）。Unity の `Player.log` には D3D12 のメッセージは出ません。
## PoC の結果（ゲーム内、2026-11-11、RTX 4070 Ti、1920x1080 ウィンドウ）

| 確認項目 | 結果 |
|---|---|
| KSL からネイティブを読み込める | ○（署名済み `.ksm` から。Unity のプラグインフォルダー経由） |
| Unity の Graphics Device を取得できる | ○ |
| Command Queue を取得できる | ○ |
| DX12 のコマンドリストを実行できる | ○（`ExecuteCommandList`） |
| フェンスによる同期 | ○（リストの再利用、タイムスタンプの回収、終了時の待機） |
| テスト三角形を描ける | ○（`capture_frame` の PNG で確認） |
| ゲーム画面への合成 | ○（HDRP の絵と UI の上に三角形とオーバーレイ。下の節の修正を入れた状態で確認） |
| シーン切り替えで落ちない | ○（Start → Empty → SelectCar + 追加シーン 3 つ） |
| 終了時に落ちない | ○（Aether は `Device objects released`、`stopped cleanly`。リークの警告なし。プロセスは約 17 秒で終了） |
| 他のフックとの共存 | ゲームのフォルダーに別 Mod の `dxgi.dll`（スワップチェーンのラッパー）がある状態で動作 |

テストホスト（`tests/host`、D3D12 デバッグレイヤー有効）では、同じランタイムで D3D12 のエラー 0、終了後のデバイス参照 0 を確認しています。
ゲーム内のデバッグレイヤー（`-force-d3d12-debug`、`scripts\launch-game.ps1 -DebugLayer`）での結果は下の「ゲーム内のデバッグレイヤー」。

## ゲームは DX12 だけでは描画できない（PoC で見つかった問題）

`-force-d3d12` で起動したゲームは、Aether の有無にかかわらず HDRP を構築できません。`Player.log` に毎フレーム次の例外が出ます。

```
ArgumentException: Kernel 'KDepthDownsample8DualUav' not found.
  at UnityEngine.ComputeShader.FindKernel (System.String name)
  at UnityEngine.Rendering.HighDefinition.MipGenerator..ctor (HDRenderPipelineRuntimeResources defaultResources)
  at UnityEngine.Rendering.HighDefinition.HDRenderPipeline..ctor (HDRenderPipelineAsset asset)
```

ゲームのビルドに入っている HDRP のコンピュートシェーダーに DX12 用のカーネルが無いためです。結果として何も描かれず（黒い画面、UI も出ない）、
Unity の描画統計もドローコール 1〜2 のままです。

**対処（`src/Aether.Managed/GameIntegration/HdrpDx12Fix.cs`）**：HDRP のランタイムリソース
（`HDRenderPipelineGlobalSettings.m_RenderPipelineResources.shaders`。ゲームの HDRP アセンブリのメタデータで確認）の `ComputeShader` を、
Unity 2023.2 のエディターに付属する HDRP（16.0.6）から DX12 向けにビルドしたもの（`aether_hdrp_dx12.bundle`、`scripts\build-hdrp-bundle.ps1`）に名前で差し替えます。
HDRP はビルド時に、ビルドに含まれる HDRP アセットの設定に合わないカーネルを除去する（アセットが無ければ全部除去する）ので、
ビルド用のプロジェクトには排他的な設定（影のフィルター品質、スクリーンスペースシャドウ、ライトプローブの方式）の全組み合わせ分の HDRP アセットを生成しています。
Unity は構築を毎フレームやり直すので、差し替えた次のフレームから HDRP が動きます。HDRP のほかの部分には触りません。
`kino\config\Aether\no-hdrp-fix` というファイルを置くと差し替えません。

実際にビルドするのはゲームが使う組み合わせだけです。ゲームの HDRP アセットの該当設定を実行中のゲームから読み（MCP の `get_status` の `managed.renderPipeline.hdrp`）、
`unity/hdrp-game-settings.json` に記録して、その値の HDRP アセット 1 つでビルドします（全 119 シェーダーで 518 バリアント、1.66 MB。全組み合わせだと `Deferred.compute` だけで 6,696）。
水、SSS、ライン描画、デバッグ用のシェーダーは丸ごと残しています（HDRP は使わないシェーダーのカーネルも探すため）。
ゲームがこれらの設定を変えたら、足りない組み合わせのカーネルが無くなります。その場合は JSON を更新して作り直します。

**結果（ゲーム内、2026-11-11）**：`replaced 118 HDRP compute shaders`、`KDepthDownsample8DualUav` のエラー 0 件。

### ゲーム独自のカスタムパス（`GameDx12Workarounds.cs`）

HDRP が構築できるようになると、次にゲーム独自の HDRP カスタムパス（ボリューム `HDRPCustomPasses`）が同じ理由で毎フレーム例外を投げました。

| パス | 注入位置 | 例外 |
|---|---|---|
| `GpuCulling.GpuCullingPass` | AfterOpaqueDepthAndNormal | `Kernel 'CalculateCullResult' not found` |
| `ParticleRenderPass` | BeforePostProcess | `Kernel 'BlurMS' not found` |

例外は HDRP のレンダーグラフの実行中に出るので、そのフレームの残りが実行されず、画面は最後の絵（ロード画面）のまま約 17 fps になります。
これらはゲーム独自のシェーダーでソースが無く、HDRP のものと違って作り直せません。DX12 では該当パスを無効化します（`CustomPass.enabled = false`）。
既知の 2 つに加え、Unity のログで「カスタムパスの実行中に `Kernel '...' not found`」を見つけたら、スタックトレースからパスの型を読んで同じ扱いにします。
`kino\config\Aether\keep-game-passes` を置くと無効化しません。状態は `get_status` の `managed.gamePassesDx12`。

**結果**：2 つを無効化した状態で、ガレージが 60 fps（GPU 約 9〜10 ms）で正常に表示され、`Player.log` の例外は 0 件。
**影響**：これらのパスが描く / カリングするものは DX12 では出ません。ガレージでは欠落を確認できませんでしたが、DX11 の絵との比較と、コースでの確認はしていません。

### 足場としての位置づけ

- `HdrpDx12Fix`、`GameDx12Workarounds`、バンドルは、HDRP が画面の一部でも描いているあいだだけ必要です（Phase 3 で HDRP を止めたら削除）。
- バンドルは HDRP のシェーダーをコンパイルしたものを含みます（Unity Companion License）。配布するときの扱いは未決定です。AssetBundle は KSL の署名の対象ではありません。
