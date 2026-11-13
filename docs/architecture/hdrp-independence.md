# ゲームのレンダーパイプラインを置き換える

描画モード `aether`（既定）では、ゲームのレンダーパイプライン（HDRP）は存在しない。Aether は最初のフレームより前に、自分のパイプラインアセット（`AetherPipelineAsset`、`src/Aether.Managed/Pipeline/AetherPipeline.cs`）をゲームのものと入れ替える。Unity は `HDRenderPipeline` を作らないので、HDRP のレンダーターゲット、バッファー、シェーダーバリアントは確保されない。

## 入れ替えの仕組み（`PipelineSwitch`）

- `PipelineSwitch.Apply(true)` が `GraphicsSettings.defaultRenderPipeline` と `QualitySettings.renderPipeline` を Aether のアセットにする。それまでのゲームのアセット（既定のものと品質レベルのもの）は覚えておく。
- 起動時、契約の確認が通った直後に一度呼ぶ（最初のフレームより前）。その後も毎フレーム `Update` で描画モードに合わせて呼ぶ。ゲームが品質レベルを変えて別のアセットを入れた場合も、そのアセットを覚え直して入れ替える。
- `PipelineSwitch.Apply(false)` はゲームのアセットを戻す。
- ゲームのアセットは、Aether のパイプラインが動いている間もデータとして読む（`PipelineSwitch.GameAsset`：ライトレイヤーの有無、ボリュームのプロファイル）。
- Mod がゲームの実行中に外されたときは、ゲームのパイプラインを戻す。ゲームの終了時には戻さない（戻すと Unity が最後の数フレームのためにゲームのパイプラインを作り始める）。

## Aether のパイプラインが 1 フレームにすること（`AetherPipeline.Render`）

1. ボリュームマネージャーが動いていなければ起動する（`PipelineSwitch.EnsureVolumes`）。
2. カメラごとに：
   - `context.Cull` で Unity のカリングを行う。これで `Renderer.isVisible`、LOD の選択、GPU スキニングが働く。Unity による影キャスターのカリングは頼まない（画面外の影キャスターはキャプチャが自分で探す）。
   - カメラのターゲットをクリアする。画面のカメラは黒でクリアする。画面のカメラより後に描く画面向きのカメラは色をクリアしない。
   - 画面のカメラ（[scene-capture.md](scene-capture.md)）なら `AetherCore.DrawScene` がフレームを記述し、Aether のシーンを描くプラグインイベントをコンテキストのコマンドに入れる。
   - ゲームの UI：`SRPDefaultUnlit` パスの `DrawRenderers`。画面のカメラでは UI レイヤー（5）だけ、他のカメラではそのカメラのカリングマスク。オーバーレイのキャンバスは Unity 自身が描く。
3. `context.Submit()`、`EndContextRendering`。
4. `HostQualitySettings.Enforce()`（下の「品質設定」）。

パイプラインのメインスレッドの時間（全体とカリング）は `AetherPipeline.Timing` で測り、シーンキャプチャの状態表示に出る。

## ゲームのパイプラインが提供していたものと、今の出どころ

| もの | 今の出どころ |
|---|---|
| フレームごとの描画の呼び出し | `AetherPipeline.Render` |
| カリング、`Renderer.isVisible`、LOD の段、GPU スキニング | Aether のパイプラインの `context.Cull`（処理は Unity のカリング） |
| 画面外の影キャスター | キャプチャが、影を落とし太陽のレイヤーに入る有効なレンダラーを送る。LOD の段は Unity と同じ規則で選ぶ。カスケードごとのカリングはネイティブ側 |
| ボリュームの値（空、露出、トーンマップ、グレーディング、フォグ、影、AO、ブルーム、モーションブラー、SSR） | Aether が起動した Unity の `VolumeManager`。ゲームの既定のプロファイルと、ゲームのパイプライン用のコンポーネント型で動かし、カメラについて評価する。コンポーネントは HDRP のデータクラスのまま（データとして読む） |
| カメラごとの機能のスイッチ（フレームセッティング） | `HDAdditionalCameraData` とプロジェクトの既定のフレームセッティングをデータとして読む（`HostFrameSettings`） |
| 空 | 既定は Aether 自身の物理的な空（`skyMode = physical`）。`skyMode = scene` では、ボリュームの HDRI の空のキューブマップを HDRP の強さと回転の規則で使う。HDRI でない空のときは環境光プローブ |
| 露出 | ボリュームの値。固定ならその EV と補正、自動ならその範囲と補正で Aether が自分で測光する |
| ライト | `Light` の値。`HDAdditionalLightData` をデータとして読む（有効か、`lightDimmer`、`fadeDistance`） |
| ライトレイヤー | ゲームのパイプラインアセットの設定をデータとして読む |
| 反射プローブ | プローブ自身のキューブマップのテクスチャー。加えて Aether 自身の反射プローブ（`reflectionProbe`） |
| UI | 上の `DrawRenderers` と Unity のオーバーレイのキャンバス |
| 車の下のブロブシャドウ | Aether のデカールのパス |
| アンチエイリアス、ブルーム、モーションブラー、SSR、AO、フォグ、トーンマップ | Aether 自身のパス（[rendering-features.md](rendering-features.md)）。ボリュームの値は入力として使う |
| パーティクル | `ParticleCapture` が粒子を読み、Aether が描く。タイヤスモークは Aether が動かす |

## ゲームのパイプラインが無いと無いもの

- 出力先がテクスチャーのカメラ（鏡、プレビュー、アイコン）のシーン。Aether が描くのは画面のカメラだけ。そのようなカメラには、クリアと `SRPDefaultUnlit` パスの描画しか行わない。
- ゲームの HDRP カスタムパス（`GpuCulling.GpuCullingPass`、`ParticleRenderPass`）。
- ゲームのリアルタイム反射プローブの更新。
- エリアライト。
- `skyMode = scene` での HDRI 以外の空。
- HDRP のコンタクトシャドウ、サブサーフェス散乱、デカールプロジェクター。

## 描画モード（`renderMode`）

| モード | パイプライン | キャプチャ | 用途 |
|---|---|---|---|
| `aether`（既定）、`standalone`（同じもの、古い名前） | Aether | 動く | 通常の遊び方。ゲームのパイプラインは作られない |
| `hdrp` | ゲーム | 止まる（ストアは空） | ゲーム自身の描画の参考の絵と計測 |
| `compare` | ゲーム | 動く | 左半分にゲームの絵、右半分に Aether の絵を並べて比べる |

- `hdrp` と `compare` は比較と計測のためのもので、代わりの手段ではない。Aether の描画はそのどちらにも頼らない。
- `compare` では、ゲームのパイプラインが描き終えた後（`RenderPipelineManager.endContextRendering`）に Aether がフレームを記述して描く。
- ゲームのパイプラインを動かすモードでは、その前に `HdrpDx12Fix` を当てる（DX12 で HDRP を作れるようにするため、[dx12-integration.md](dx12-integration.md)）。Aether が起動できなかった場合も当てる。`aether` では当てない。
- 切り替えは F10 のオーバーレイ、`aether.json`、MCP の `set_render_mode`。

## 品質設定（`HostQualitySettings`）

`src/Aether.Managed/GameIntegration/HostQualitySettings.cs`。Aether の設定（F10 のオーバーレイ）の値で Unity の品質設定を上書きする。ゲームのグラフィックメニューの値は使わない。

| Aether の設定 | Unity の値 | 範囲（既定） |
|---|---|---|
| `lodBias` | `QualitySettings.lodBias` | 0.3〜8（2.8） |
| `anisotropicLevel` | `Texture.SetGlobalAnisotropicFilteringLimits(level, level)` | 1〜16（9） |
| `vsync` | `QualitySettings.vSyncCount` | 0〜2（1） |

- **`Apply()`**：設定を 0.5 秒に 1 回読み、違う値を書き込む。昼夜の追従もここで行う。シーンキャプチャがボリュームを読む時（0.5 秒ごと）に呼ぶので、キャプチャが動くモード（`aether`、`compare`）でだけ働く。
- **`Enforce()`**：Aether のパイプラインが、毎フレーム、カメラを描いた後に呼び、`Apply` が最後に読んだ LOD バイアスと VSync を書き戻す。ゲームは自分の品質用のボリュームコンポーネント（`HDRP.Scalability`）を `RenderPipelineManager.beginCameraRendering` のハンドラー（`HDRP.MasterVolumes.OnBeginCameraRendering`）で毎フレーム読み、メニューの LOD バイアスと VSync を描画の途中で書き込む。Aether のパイプラインもこのイベントを起こすのでハンドラーは動き続け、`Enforce` がその後に Aether の値へ戻す。`compare` では Aether のパイプラインが動かないので `Enforce` も呼ばれず、ゲームのメニューの値が残ることがある。
- ベンチマークが上限なしを求めている間は、`AetherCore` が VSync とフレームレートの上限を外す（終わると戻す）。

### 昼夜の追従

ゲームの昼夜の切り替え（`DayNightController`。昼用と夜用のライティングのシーンを読み込む）にリフレクションで触れる。Aether はゲームのアセンブリにリンクしない。

- **ゲーム → Aether**：`DayNightController.instance` の `IsAppliedMode` と `IsDayLight` から夜かどうかを読み、変わったら `Aether_SetSceneNight` で伝える。`followSceneNight`（既定オン）のとき、物理的な空は時刻の設定にかかわらず夜になる。
- **Aether → ゲーム**：時刻の設定（`timeOfDay`）が昼と夜の境（19.7 時、4.3 時）を越えた時だけ、ゲームの夜のライティング（マップの灯り、車のランプ）を合わせる。`followSceneNight` がオンで、ゲームの状態がすでにそうなっていないときに限る。境を越えた時だけなので、その間はゲーム自身のボタンも使える。
  - ガレージ：ゲームのメニューの昼夜のボタンの処理（`UIMainMenuContext.ToggleDayNightMode`）を呼ぶ。ボタンの表示、保存される設定、車のランプが一緒に変わる。
  - それ以外（コース）：夜はゲームの DLC。`GameManager.NightModeInstalled` が真のときだけ `DayNightController.SetDayNightMode` を呼ぶ。

## 知っておくこと

- Unity の `VolumeManager` は動いているパイプラインが起動・終了し、動いているパイプラインのアセットの型に対応すると宣言したコンポーネント型しか読み込まない。Aether のパイプラインでは Aether が `Initialize` を呼び、ゲームのパイプラインの型を読み込ませ（内部の `LoadBaseTypes`）、既定の状態を作り直す（内部の `EvaluateVolumeDefaultState`）。ゲームのスクリプトが `VolumeManager.instance.stack` で読む既定のスタックも作り直す。既定のプロファイルは、ゲームのパイプラインが動いていた間に覚えたもの、無ければゲームの設定アセットから読んだもの。カスタムの既定プロファイルも戻す。
- Aether のパイプラインが破棄されるとボリュームマネージャーを終了する（ゲームのパイプラインは自分で起動し直す）。起動し直す前に作ったボリュームスタックは使えない。`PipelineSwitch.VolumeGeneration` がそれを示し、キャプチャはスタックを作り直す。
- 実行中にゲームのパイプラインへ戻すときは、HDRP の静的なカメラのキャッシュを消す（`HDCamera.ClearAll`）。Aether のパイプラインの間に作られた、使えなくなったボリュームスタックを持っているため。
- `HDAdditionalLightData` が無効なライトは、`Light.enabled` に関係なくゲームでは消えている。
- ゲームのパイプラインのカスタムパスが DX12 で例外を出す問題は `GameDx12Workarounds` が扱う（[dx12-integration.md](dx12-integration.md)）。
