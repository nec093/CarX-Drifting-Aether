# Phase 1 完了報告（2026-11-11）

基盤構築・DX12 統合・診断環境。検証環境：CarX Drift Racing Online 2.28.3 MODDABLE（Unity 2023.2.22f1、HDRP、KSL 1.0.14）、
`-force-d3d12`、1920x1080 ウィンドウ、NVIDIA GeForce RTX 4070 Ti（ドライバー 32.0.16.2114）、Windows 11。

「確認済み」は次の 3 種類を区別します：**ゲーム内**（実機のゲームで確認）、**テストホスト**（ゲームなし、D3D12 デバッグレイヤー）、**未確認**。

## 1. 実装済み機能

| 機能 | 場所 |
|---|---|
| KSL Mod（署名 `.ksm`）、ネイティブランタイムの読み込み、描画イベント | `src/Aether.Managed` |
| Unity の D3D12 デバイスとの統合（`IUnityGraphicsD3D12v7`）、コマンドリスト、フェンス同期、安全な解放 | `src/Aether.Native/Device`、`Renderer`、`Session.cpp` |
| テスト三角形、フレームキャプチャ（PNG）、GPU タイムスタンプ | `Renderer` |
| GPU 情報と D3D12 機能の実行時判定 | `Device/GpuInfo` |
| Profiler（FPS、フレーム時間、1% low、GPU 時間、Unity の統計）、VRAM の内訳、リソース登録簿、ベンチマーク | `Profiler`、`Resources`、`Diagnostics.cpp` |
| Dear ImGui オーバーレイ（9 タブ、承認ダイアログ） | `ImGui/Overlay` |
| 構造化ログ、設定（JSON、検証つき）、クラッシュレポート | `Core`、`Runtime.cpp` |
| MCP サーバー（19 ツール：読み取り 13、操作 6）と名前付きパイプの IPC、権限 3 段 | `src/Aether.MCP`、`Ipc` |
| HDRP の DX12 対応（コンピュートシェーダーの差し替え、ゲーム独自パスの回避） | `GameIntegration`、`unity/`、`scripts/build-hdrp-bundle.ps1` |
| テストホスト、結合テスト、ビルド・配置・起動スクリプト | `tests/host`、`scripts` |

## 2. 確認結果

| 項目 | ゲーム内 | テストホスト |
|---|---|---|
| KSL からのロード、`UnityPluginLoad`、D3D12 デバイス / キューの取得 | ○ | — |
| コマンドリストの実行、フェンス同期（再利用、タイムスタンプ回収、終了時の待機） | ○ | ○（D3D12 エラー 0） |
| テスト三角形、ゲーム画面（HDRP + UI）への合成 | ○（PNG で確認） | ○ |
| オーバーレイの表示、HDRP + UI の上への合成 | ○（PNG で確認） | ○ |
| シーン切り替え（Start → Empty → SelectCar + 追加 3 シーン） | ○（落ちない、エラーなし） | — |
| GPU 情報、FPS、GPU フレーム時間、VRAM、リソース、マテリアル、シーン情報 | ○（MCP 経由） | ○ |
| MCP の全 19 ツール、権限（読み取り専用、承認の時間切れ、不正な引数）、切断と再接続 | 主要ツールのみ | ○（`MCP TEST OK`） |
| シェーダーのコンパイルエラーの検出と再読み込み | — | ○ |
| ウィンドウのリサイズ | 未確認 | ○ |
| 終了時の解放（リークなし） | ○（`Device objects released`、`stopped cleanly`、警告なし） | ○（デバイス参照 0） |
| F10 での開閉 | △ ウィンドウへ投稿した `WM_KEYDOWN` で切り替わることを確認。実キーは未確認 | — |
| オーバーレイのマウス・キーボード操作 | **未確認**（4 章） | — |
| D3D12 デバッグレイヤー（`-force-d3d12-debug`）でのゲーム内検証 | 未確認 | ○ |

## 3. HDRP の DX12 描画

- **結果**：ガレージ（`SelectCar` + `vp_showroom`）で、車両・路面・環境・影・UI が表示されることを PNG で確認。`Player.log` のカーネル不足エラーは 0 件。
- **原因と対処**（詳細は `docs/architecture/dx12-integration.md`）：
  1. ゲームのビルドの HDRP コンピュートシェーダーに DX12 のカーネルが無い（`Kernel 'KDepthDownsample8DualUav' not found`）。→ `aether_hdrp_dx12.bundle` の 118 個に差し替え。
  2. ゲーム独自の HDRP カスタムパス 2 つも同じ理由で毎フレーム例外を投げ、フレームの残りが実行されなかった：`GpuCulling.GpuCullingPass`（`CalculateCullResult`）、`ParticleRenderPass`（`BlurMS`）。ゲーム独自のシェーダーでソースが無いため、DX12 ではこの 2 つのパスを無効化。
- **バリアント**：ゲームの HDRP 設定（影の品質 Low / Low / Medium、スクリーンスペースシャドウ無効、レガシーライトプローブ、R11G11B10、HDR 無効、品質レベル 1 つ）の組み合わせだけをビルド。全 119 シェーダーで 518 バリアント（全組み合わせなら `Deferred` だけで 6,696）、1.66 MB。設定は `unity/hdrp-game-settings.json`。
- バンドルは AssetBundle で、KSL の署名の対象ではない（署名するのは `.ksm` だけ）。
- **既知の影響**：無効化した 2 つのパスが描いていたもの（GPU カリング対象のオブジェクト、パーティクルのパス）は DX12 では出ない / カリングされない。ガレージでは見た目の欠落は確認できなかったが、DX11 の絵との比較はしていない。コースでは未確認。

## 4. ImGui の入力：手動で確認していただきたい項目

入力の自動化はしていません（ゲームのウィンドウを前面に出して実入力を注入することは避けました）。ゲームを DX12 で起動して次を確認してください。

1. **F10** を押す → 左上に「Aether 0.1.0」のウィンドウが出る。もう一度押す → 消える。
2. タブ（GPU、VRAM、Resources、Pipeline、Shaders、Scene、Settings、Log）を**クリック**して切り替わる。
3. ウィンドウのタイトルバーを**ドラッグ**して動く。右下の角をドラッグして大きさが変わる。
4. Settings タブで「Test triangle」のチェックを外す → 右上の三角形が消える。「UI scale」のスライダーを動かす → 大きさが変わる。
5. オーバーレイの上でクリック・ホイールしたとき、**後ろのゲームのメニューが反応しない**こと。オーバーレイの外ではゲームが普段どおり反応すること。
6. Log タブの「Minimum severity」のコンボを**キーボード**（矢印キー、Enter）で操作できること。
7. オーバーレイを閉じた状態で、ゲームの操作に影響が無いこと。

## 5. ベンチマーク基準値

`docs/benchmarks/phase1-baseline-garage-hdrp-dx12.json`（`run_benchmark seconds=30`）。

| 条件 | 値 |
|---|---|
| シーン | 起動後に何も操作しない状態のメインメニュー（`SelectCar` + `vp_showroom`、カメラは自動で周回） |
| 解像度 / 表示 | 1920x1080 ウィンドウ、VSync 1（60 Hz で頭打ち） |
| レンダラー | HDRP（DX12、上記の修正つき）。Aether はテスト三角形のみ、オーバーレイ非表示 |
| GPU | RTX 4070 Ti |

| 指標 | 値 |
|---|---|
| 平均 FPS / 1% low / 0.1% low | 59.99 / 52.37 / 48.65（VSync で頭打ち） |
| フレーム時間 平均 / p99 / 最大 | 16.67 / 18.75 / 20.56 ms |
| **GPU フレーム時間 平均 / p99** | **9.33 / 10.70 ms**（VSync の影響を受けない比較用の値） |
| Aether の GPU 時間 | 0.003 ms |
| 専用 VRAM（プロセス） | 1,866 MB（予算 11,228 MB）、共有 327 MB |
| うち Aether | 0 MB（システムメモリ側 128 KB） |
| Unity のレンダーテクスチャー | 約 628 MB / 245 個（最大：`PromotionRenderTexture` 2560x1440 98 MB、カスケードシャドウアトラス 3072² 36 MB、ボリュメトリクスの VBuffer 22 MB × 4） |
| テクスチャーストリーミング | 572 MB |
| Unity のドローコール（平均） | 904 |

再現手順：`scripts\launch-game.ps1` → ガレージの読み込み完了を待つ → `scripts\mcp-call.ps1 run_benchmark seconds=30 label=<名前> -AllowControl`。
VSync を切った計測、コース走行中の計測はしていません（Phase 2 の比較では VSync を切った条件を追加する）。

## 6. 終了処理

| 条件 | ウィンドウを閉じてからプロセス終了まで |
|---|---|
| Aether あり、DX12、HDRP 正常（2 回） | 17.9 秒、17.3 秒で正常終了 |
| Aether なし、DX12（HDRP が構築できない状態、80 秒動作後） | 5.8 秒で正常終了 |
| Aether あり / なし、DX12、HDRP が構築できない状態で数分動作後（修正前の 2 回） | 30 秒以上残留（応答なし）。Aether を読み込んでいない回でも発生 |

- Aether 自身の終了：描画スレッドで GPU のアイドルを確認してからデバイスオブジェクトを解放（`Device objects released`）→ IPC とワーカーを停止 → `stopped cleanly`。リークの警告なし、次回起動時の「前回異常終了」フラグなし。所要は数 ms。
- **判定**：プロセスの残留は Aether 固有ではない。HDRP が構築できない状態（毎フレーム例外）で長く動かした後にだけ起き、Aether の無い起動でも起きた。HDRP が正常な状態では残留しない。
- 17 秒と 5.8 秒の差は条件が違う（HDRP が動いてガレージを読み込んだ状態か否か）ため、Aether の影響とは切り分けていない。Aether の停止はゲームの終了処理の最後の数 ms。
- Native Plugin：`UnityPluginUnload` は実装済み。ゲーム実行中に DLL を外すこと（KSL の Mod の再読み込み）は未確認。

## 7. GPU 互換性

| GPU | 状態 |
|---|---|
| NVIDIA GeForce RTX 4070 Ti | **実機確認済み**。Feature Level 12_2、SM 6.8、Resource Binding Tier 3、Resource Heap Tier 2、Wave Ops、DXR 1.1、Mesh Shader、VRS Tier 2、Enhanced Barriers |
| AMD | **実機未検証** |
| Intel | **実機未検証** |

設計上の対応：Phase 1 の経路はベンダー固有の機能を使わない（D3D12 の基本機能、SM 5.1 のシェーダー、従来型のバリア、タイムスタンプクエリ、BC 形式は未使用）。
機能の有無は `GpuInfo` が実行時に `CheckFeatureSupport` で調べる。テストホストは WARP（`scripts\test-host.ps1 -Warp`）でも動かせるが、今回は実行していない。

## 8. 未確認事項

- オーバーレイのマウス・キーボード操作、実キーの F10（4 章）。
- コース（走行中）、マルチプレイヤー、ワークショップ、写真モードなどガレージ以外の画面。
- ウィンドウのリサイズ、フルスクリーン切り替え、HDR 表示（ゲーム内）。
- ゲーム内での D3D12 デバッグレイヤー。
- `LoadLibrary` だけで `UnityPluginLoad` が呼ばれるか（呼ばれるなら `_Data\Plugins` へのコピーが不要になる）。
- AMD / Intel の実機。
- DX11 での HDRP の絵・性能との比較。

## 9. 既知の制限

- DX12 では、ゲーム独自の GPU カリングのパスとパーティクルのパスを無効化している（3 章）。
- オーバーレイに入力中、ウィンドウメッセージ経由の入力はゲームに渡さないが、ゲームのコントローラー / 生入力（Rewired）までは止めていない。
- `set_render_quality` は値を保存するだけ（使うレンダラーが Phase 2）。デバッグビューは置き場所だけ。
- `AetherNative.dll` をゲームの `_Data\Plugins\x86_64\` にコピーする（ゲーム本体のファイルは書き換えない）。
- 署名（maykr）は KSL のサーバーに依存し、連続すると拒否されることがある。
- HDRP のバンドルは HDRP のコンパイル済みシェーダーを含む（Unity Companion License）。配布時の扱いは未決定。

## 10. Phase 2 へ引き継ぐ情報

- **統合方式は確定**：Unity の D3D12 デバイス + `ExecuteCommandList`、フレーム末尾のイベント（`FlushCommandBuffers | SyncWorkerThreads`）。規則は `docs/architecture/dx12-integration.md`。
- **HDRP の設定**（ガレージ）：Lit モード Both、R11G11B10、MSAA なし、SSR / SSAO / SSGI / ボリュメトリクス / ボリュメトリッククラウド / モーションベクター有効、デカール / SSS / レイトレーシング / 動的解像度なし、シャドウアトラス 2048、影のフィルター Low。
- **マテリアル**（ガレージ）：レンダラー 748、マテリアル 172、シェーダー 18 種。`HDRP/Lit` 100、車は Shader Graph（`CarPaint`、`CarGeneric`、`CarGlass`、`CarPaintRim`、`CarPaintStitches`、`Colormask`、`CarGenericAlpha`）、植生 `CarXFoliageSimple`。透明は少数。
- **VRAM の大口**（HDRP 側）：レンダーテクスチャー 628 MB のうち、`PromotionRenderTexture` 98 MB、シャドウアトラス、ボリュメトリクスの 3D テクスチャー 88 MB、SSR。テクスチャーストリーミング 572 MB（予算 1,536 MB）。
- **UI** は Screen Space - Overlay（バックバッファーへ直接）。メインカメラ 1 台（`GarageCamera`）。HDRP のカスタムパスのボリューム `HDRPCustomPasses` がある。
- **ゲーム独自の描画機能**：GPU カリング（`GpuCulling.*`）、パーティクル（`ParticleRenderPass`）、`HDRP/Custom/PhysicsMotionVectors`。Aether のレンダラーで代替が要る。
- **基準値**：5 章。Phase 2 では同じ手順で Aether の絵と比較する。
- **足場の撤去条件**：`HdrpDx12Fix`、`GameDx12Workarounds`、バンドルは、HDRP がフレームを描かなくなったら削除する。
