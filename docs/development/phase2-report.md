# Phase 2 報告（2026-11-11 時点・未完了）

**結論：Phase 2 は完了していません。** Aether のシーンレンダラーはゲーム内で動きますが、D3D12 デバッグレイヤーを有効にすると
（追記 2026-11-12：この問題は ANZU の動画広告が原因としてクローズ済み。`stability-investigation.md` の 10 章。以下は当時の記述。）

Unity 側で「破損したリソース」エラーが出てゲームが落ちる問題が未解決です（1 章）。これを直すまで、Phase 2 の成果は安定動作を保証できません。
HDRP は置き換えていません（全モードで HDRP は動き続けています）。AMD / Intel の実機では未確認です（RTX 4070 Ti のみ）。

区分：**[実装]** コードがある／**[ホスト]** テストホスト（デバッグレイヤー）で確認／**[ゲーム]** ゲーム内で確認／**[未]** 未実装／**[阻害]** 問題あり。

## 1. 未解決：デバッグレイヤー下のリソース破損 [阻害]

> **2026-11-12 追記**：この章は古い。その後の調査、ANZU の中身、現在の対処（DX12 では動画再生を止める）と検証の状態は `docs/development/stability-investigation.md` の 8 章。ローダーは KSL から BepInEx に変わった（`docs/development/build.md`）。

### 症状

`scripts\launch-game.ps1 -DebugLayer`（`-force-d3d12-debug`）で起動し、ガレージで放置すると、起動後 141〜271 秒で落ちる。

- Aether のログ：`E_D3D12_CORRUPTION  ID3D12CommandList::CopyTextureRegion: pResource in the first parameter is corrupt.`
- クラッシュは Unity の描画スレッド。例外 `0x87D`（デバッグレイヤーが破損を検出して投げる）で、呼び出し元は Unity のテクスチャーアップロード処理
  （`UnityPlayer+0x8B5839 ← +0x87D81C`）。別の回は Unity の遅延解放リストの処理（`UnityPlayer+0x684440`）でアクセス違反。
- スタックに Aether（`AetherNative.dll`）のフレームは無い。つまり **Unity が、すでに解放された自分のテクスチャーに書き込もうとしている**。
- デバッグレイヤーなしでは落ちない（下表）。ただし「検出されないだけ」で、同じ解放済み参照が起きている可能性は否定できない。

### 実験（すべてガレージ放置、RTX 4070 Ti）

| # | デバッグレイヤー | Aether の状態 | 結果 |
|---|---|---|---|
| 1 | 有 | compare / aether、バックバッファーの状態を `states` で宣言 | 約 185 秒で落ちる（遅延解放リスト破損） |
| 2 | 有 | aether → hdrp に切り替え | 約 170 秒で落ちる（CopyTextureRegion 破損） |
| 3 | 有 | hdrp 固定（シーンキャプチャ一度も動かさず）、三角形 + GPU 計測 | 161 秒で落ちる |
| 4 | **無** | hdrp 固定、三角形 + GPU 計測 | 325 秒・17,626 フレーム生存、正常終了 |
| 5 | 有 | **Aether は何も提出しない**（三角形・GPU 計測オフ） | 301 秒生存 |
| 6 | 有 | 三角形のみ（フレーム末尾リスト、`states` 宣言あり） | 161 秒で落ちる |
| 7 | 有 | 三角形のみ、`states` なし（自前バリア。バリアは誤りで検証エラー 60 件） | 271 秒生存 |
| 8 | 有 | compare、全機能、`states` なし | 230 秒で落ちる |
| 9 | 有 | 8 + コマンドリストの再利用を遅延（Unity のフェンス通過 + 24 提出） | 141 秒で落ちる |
| 10 | 有 | Aether は何も提出しない（5 の再実行） | 285 秒以上生存 |
| 11 | 有 | 9 + フレーム先頭イベントをキューアクセス許可（`Allow`）に修正 | 271 秒で落ちる |
| 12 | 無 | compare / aether、全機能 | 168 秒・9,696 フレーム、エラー 0、正常終了（短い） |

### 分かったこと

- シーンレンダラー（Unity のメッシュ / テクスチャーの借用）は原因ではない（#3、#6：一度も動かさなくても落ちる）。
- **Aether が `ExecuteCommandList` でリストを提出していると落ち、何も提出しなければ落ちない**（#5、#10 対 #3、#6、#8、#9、#11）。回数が少ないので確率的な偏りの可能性は残る。
- 以下は修正済みだが、どれも単独では解決にならなかった：
  1. Unity のフレームフェンスを完了判定に使っていた（アロケーターの早すぎるリセット → 完了マーカー + 自前フェンスに変更）。
  2. バックバッファーの状態を毎フレーム `states` で宣言していた（→ 宣言をやめ、RENDER_TARGET のまま描いて返す）。
  3. キューへの提出が許されない設定（`DontCare`）のイベントでリストを提出していた（→ `Allow` + `SyncWorkerThreads`）。Unity のヘッダーに「no work must be submitted」と明記されており、Phase 1 からの誤り。
  4. コマンドリストの再利用が Unity 側の記録と重なる可能性（→ 再利用を遅延）。
- Aether 自身の D3D12 の使い方についての検証エラーは、現在 0 件（テストホストとゲーム内の両方）。

### 残っている仮説と次の手

1. **Aether のイベント設定（`FlushCommandBuffers | SyncWorkerThreads`）が Unity 内部の競合を表に出している。**
   フレーム途中 / 末尾で強制的に提出させることで、ゲーム側の動画・広告テクスチャー（毎フレーム更新、数十秒ごとに差し替え）の
   アップロードと破棄の順序が入れ替わる。→ フラグを 1 つずつ外して再実験する。
2. **Aether 自身の D3D12 オブジェクトの二重解放**が、同じアドレスに再確保された Unity のリソースを解放している。
   → Aether の全 `Release` に参照数の検査を入れる / `ID3D12DebugDevice::ReportLiveDeviceObjects` と突き合わせる。
3. **`ExecuteCommandList` 自体をやめる。** Unity のキューに直接 `ExecuteCommandLists` する（`Allow` のイベント内では許されている）。
   Unity の内部記録を一切通らないので、1 が原因なら回避できる。
4. 破損したリソースの特定：デバッグレイヤーのコールバック内でスタックを取り、どのテクスチャーへのアップロードかを Unity の統計と突き合わせる。

1 回の実験が 3〜5 分かかり結果が確率的なので、各条件 3 回以上が必要です。

## 2. 完了した作業

| 項目 | 状態 |
|---|---|
| Scene Data Contract v1（サイズ検査、ハンドル、ゼロコピー、検証）`include/SceneContract.h`、`SceneCapture/Contract.cs` | [実装][ホスト][ゲーム] 契約違反 0 件 |
| シーンキャプチャ（MeshRenderer、マテリアル、テクスチャー、ライト、カメラ、SH、空、露出、Unity の可視判定 / LOD を利用） | [実装][ゲーム] 729 レンダラー、350 メッシュ、247 マテリアル、251 テクスチャー |
| マテリアル変換（13 シェーダー → 14 クラス） | [実装][ゲーム] 未対応 2 個（ゲーム側もエラーシェーダーのもの） |
| GPU リソースマネージャー（ディスクリプター、テクスチャー、フレームアリーナ、退役、予算、計上） | [実装][ホスト][ゲーム] |
| レンダーグラフ（宣言、検証、一時リソースのプール、遷移、パス別計測） | [実装][ホスト][ゲーム] |
| Forward+（カスケードシャドウ、深度プリパス、16×9×24 クラスター、不透明、空、半透明、合成） | [実装][ホスト][ゲーム] |
| モード `hdrp` / `aether` / `compare`、デバッグ表示 8 種 | [実装][ゲーム] |
| 完了マーカーによる寿命管理、デバッグレイヤーのメッセージをログへ | [実装][ホスト][ゲーム] |
| MCP：`set_render_mode`（20 ツール）、`get_material_info detail`、`aetherScene`、`aetherManager`、`shared`、`sceneGraph` | [実装][ゲーム]、`MCP TEST OK` |
| オーバーレイ：描画モード、デバッグ表示、シャドウ / 空 / 露出、シーン統計、レンダーグラフ、リソースマネージャー | [実装] ビルドのみ。**画面での操作は未確認**（入力の自動化なし） |
| ベンチマーク：`aetherSceneGpuTimeMs`、`aetherScene`（パス別） | [実装][ゲーム] |

## 3. 設計上の判断

- **ゼロコピー**：シーンのメッシュとテクスチャーは Unity の `ID3D12Resource` を参照するだけ。VRAM を二重に持たない。HDRP を止めた後に自前の読み込みへ移る受け口はハンドル。
- **Forward+**：G-buffer を持たない（VRAM と帯域の節約、マテリアルモデルの自由度）。
- **プロパティの署名でマテリアルを分類**：シェーダー名や HDRP のキーワードに依存しない。
- **寿命の時計は Aether 自身**：リストの末尾に GPU が書くマーカー + 自前フェンス。
- **Unity のリソースの状態には触れず、宣言もしない。**
- **Phase 1 の統合は維持**：`HdrpDx12Fix`、`GameDx12Workarounds`、バンドル、MCP の契約は変更なし（API は v2 に追加のみ）。

## 4. 変更したファイル

新規：`include/SceneContract.h`、`Core/Math.h`、`Resources/GpuResources.*`、`Scene/SceneStore.*`、`Renderer/{RenderGraph,GpuTimer,SceneRenderer}.*`、
`shaders/{aether_scene.hlsli,scene_forward.hlsl,scene_post.hlsl}`、`SceneCapture/{Contract,MaterialTranslator,SceneCapture,RenderSurvey}.cs`、
`tests/host/ValidationScene.*`、`docs/architecture/{scene-data-contract,scene-capture,material-translation,render-graph,forward-plus}.md`、この報告。

変更：`Renderer/Renderer.*`、`Device/UnityBridge.cpp`、`Session.*`、`Core/Config.*`、`Profiler/*`、`Ipc/Api.cpp`、`Diagnostics.cpp`、`ImGui/Overlay.cpp`、`Runtime.h`、
`include/AetherApi.h`、`CMakeLists.txt`、`AetherMod.cs`、`Native.cs`、`SceneReporter.cs`、`ToolCatalog.cs`、`tests/host/HostMain.cpp`、`scripts/test-*.ps1`、
`docs/architecture/{dx12-integration,gpu-resources}.md`、`docs/diagnostics/mcp-api.md`、`AGENT.md`。コミットはしていません。

## 5. ビルド

ネイティブ `BUILD OK`（Aether のコードの警告 0）。C# Mod は署名済み `.ksm` を配置済み（警告 2：`RenderSurvey` の旧 API、未使用フィールド `className`）。

## 6. 結合テスト

- `scripts\test-host.ps1`：`HOST TEST OK`。デバッグレイヤーのエラー 0、デバイス参照の残り 0、検証シーンのピクセル検査（位置、深度、カリング、鏡像、影、ブレンド、未対応マテリアル、アンロード / 再読み込み）すべて合格。
- `scripts\test-mcp.ps1`：`MCP TEST OK`（20 ツール）。

## 7. ゲーム内の結果（ガレージ、1920×1080）

- [ゲーム] `compare`（左 HDRP / 右 Aether）と `aether` で、車、建物、地面、空、影、ガラス、UI（Aether の絵の上）が表示される。キャプチャは `kino\mods\Aether\captures\`。
- [ゲーム] モード切り替え（hdrp ⇄ aether ⇄ compare）、デバッグ表示の切り替え、シェーダーのホットリロード、終了（`Device objects released`、`stopped cleanly`）。
- [ゲーム] デバッグレイヤー下で Aether 自身の検証エラー 0。**ただし 1 章の破損で落ちる。**
- **絵の差（未解決）**：地面が HDRP の約 2.3 倍明るい（HDRP は地面に太陽光がほぼ当たっていないように見える。ライトレイヤーか影の落とし主の違いが候補。未特定）。
  車のライトは光らない、塗装の特殊効果・リバリーの一部・スキンメッシュ（13 個）・パーティクルは描かれない。SSAO / SSR / 反射プローブ / ポストエフェクトなし。
- **未確認**：コース（別シーン）への切り替え、解像度の変更、長時間（5 分超）の aether / compare、オーバーレイの操作。

## 8. ベンチマーク（Phase 1 基準との比較、VSync 60 fps 上限のまま、25 秒）

| | Phase 1 基準 (HDRP) | hdrp | compare | aether |
|---|---|---|---|---|
| 平均 FPS | 60.0 | 60.0 | 60.0 | 60.0 |
| 1% low | 52.4 | 46.1 | 43.6 | 46.0 |
| GPU フレーム時間 平均 (ms) | 9.33 | 9.51 | 9.68 | 9.21 |
| Aether シーン GPU 時間 平均 (ms) | – | – | 0.70 | 0.57 |
| ドローコール / 三角形（Aether） | – | – | 687 / 36.9 万 | 1,122 / 70.3 万 |

- HDRP は全モードで動いているので、フレーム全体の時間は HDRP が支配しています。Aether のシーン全パスは GPU で 0.6〜0.7 ms（影 0.20、深度 0.08、不透明 0.28、半透明 0.06、合成 0.03）。
  機能が HDRP よりはるかに少ないので、**HDRP との性能比較にはなりません**。
- VSync を外した計測、コースでの計測はしていません。1% low の低下の原因は未調査です。
- 結果ファイル：`kino\mods\Aether\benchmarks\phase2-garage-{hdrp,compare,aether}-*.json`。

## 9. VRAM

| | Phase 1 基準 | Phase 2（ベンチマーク時） |
|---|---|---|
| プロセスの専用 VRAM | 1,957 MB | 2,043 MB（別の計測時 1,896 MB。ゲーム側の変動が大きい） |
| Aether 所有（GPU） | 0 MB | 81 MB：シャドウアトラス 64、SceneColor + SceneDepth 16.9、その他 0.3 |
| Aether 所有（システムメモリ） | 0.1 MB | 16.4 MB（フレームアリーナ） |
| 共有（Unity から借用、Aether の使用量に含めない） | – | メッシュ 10.6 MB、テクスチャー 192.6 MB |

HDRP が動いているあいだ、VRAM は Aether の分だけ増えます（約 +81 MB）。削減は HDRP を止めてからです。`hdrp` モードに戻してもシャドウアトラスは解放していません。

## 10. 制限と次の作業

制限は 1 章、7 章と各設計文書の末尾。次の作業（優先順）：

1. **1 章の破損エラーの解決**（最優先。ここが直るまで先へ進まない）。
2. aether / compare の長時間の安定確認（デバッグレイヤーあり / なし）、コースでの確認、解像度の変更。
3. 地面の明るさの原因特定（ライトレイヤー、影の落とし主）、パックマップのチャンネル割り当ての検証。
4. スキンメッシュ、車のライトの発光、パーティクル。
5. VSync を外したベンチマーク、インスタンシング。

新しい設計文書 5 本は英語で書いています（既存文書は日本語）。必要なら日本語に直します。
