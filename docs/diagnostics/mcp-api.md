# MCP API リファレンス

`Aether.MCP.exe` は stdio の MCP サーバーです（JSON-RPC 2.0、1 行 1 メッセージ、プロトコル `2025-06-18` / `2025-03-26` / `2024-11-05`）。
ゲームの中の Aether ランタイムとは、名前付きパイプ `\\.\pipe\Aether.Runtime` で話します。パイプを開けるのは、この PC の同じユーザー（と SYSTEM）だけです。リモートからの接続は受け付けません。
各ツールの入力・出力スキーマは `tools/list` が返します（定義は `src/Aether.MCP/Tools/ToolCatalog.cs`、ランタイム側の処理は `src/Aether.Native/Ipc/Api.cpp`）。

```
Aether.MCP.exe [--allow-control] [--pipe <name>] [--list-tools]
```

| 引数 / 環境変数 | 意味 |
|---|---|
| `--allow-control` / `AETHER_MCP_ALLOW_CONTROL=1` | 操作ツールを提供する。無ければ読み取り専用 |
| `--pipe <name>` / `AETHER_PIPE_NAME` | パイプ名（`\\.\pipe\` を除く）。既定 `Aether.Runtime` |
| `--list-tools` | ツール名を一覧して終わる（操作ツールには `[control]`） |

コマンドラインから 1 回呼ぶには `scripts\mcp-call.ps1 <tool> [key=value ...] [-AllowControl] [-Raw]`。

ツールは 21 個です（読み取り 13、操作 8）。読み取りツールの待ち時間は 15 秒、操作ツールは 330 秒です（ゲーム内の承認を待つため）。

## 読み取りツール

| ツール | 入力 | 返すもの |
|---|---|---|
| `get_status` | なし | 最初に呼ぶ。バージョン、レンダラーの状態（`renderer`）、GPU、デバイスの状態、オーバーレイの表示、フレーム数、ログとシェーダーのエラー数、Aether のリソース数とバイト数、前回の異常終了の有無、`mcpControl`、`renderMode`、`sceneRenderer`（シーンレンダラーの統計）、`managed`（C# 側の報告） |
| `get_gpu_info` | なし | アダプター（ベンダー、名前、デバイス ID、ドライバー、専用 / 共有メモリ）と D3D12 の機能（シェーダーモデル、Resource Binding Tier、Resource Heap Tier、Wave Ops、レイトレーシング、メッシュシェーダー、VRS、Enhanced Barriers、BC 形式）、デバイスの状態 |
| `get_performance` | `historyFrames`（0〜2000、既定 0） | FPS、フレーム時間（平均、最小、最大、p50 / p95 / p99）、1% low と 0.1% low、GPU フレーム時間、Aether の CPU / GPU 時間とドローコール、パスごとの時間、Unity の値（メイン / 描画スレッド、Present 待ち、ドローコール、バッチ、三角形）、ベンチマークの状態。`historyFrames` を渡すと直近のフレーム時間の列も |
| `get_vram_usage` | なし | プロセスの専用 / 共有メモリと予算（DXGI）、Aether のカテゴリー別の使用量、Unity の申告値、Aether 以外の分（`notAether`）、`aetherManager`（ディスクリプター、フレームメモリ、解放待ち、予算）、`shared`（Unity から借りているメッシュ / テクスチャーのバイト数。Aether の使用量には含めない） |
| `get_resource_list` | `limit`（1〜2000、既定 200）、`category`、`includeUnityRenderTextures`（既定 false） | Aether のリソース（大きい順）、作成 / 解放の回数と直近のイベント。`includeUnityRenderTextures` で Unity のレンダーターゲットも（ゲーム内で走査するので一瞬止まる） |
| `get_render_graph` | なし | スキーマ `aether.rendergraph/2`。`passes` / `resources`（バックバッファー上のフレーム終端の処理：テスト三角形、オーバーレイ、キャプチャ）、`sceneGraph`（シーンレンダラーのパスごとの読み書きと状態、時間、一時リソース）、`sceneStats` |
| `get_shader_errors` | `includeAllShaders`（既定 false） | コンパイルに失敗したシェーダーとコンパイラの出力。指定すれば全シェーダーの状態も |
| `get_material_info` | `refresh`（既定 true）、`detail`、`limit` | 下の表 |
| `get_scene_info` | なし | シーン、カメラ（HDRP の設定つき）、ライト、レンダラー、反射プローブ、HDRP のボリュームとその中身、キャンバス、環境。`game`（C# 側の報告）、`aetherScene`（ネイティブ側が持つメッシュ / テクスチャー / マテリアル / インスタンスの数とバイト数、契約違反の数）、`aetherRenderer`（ドローコール、インスタンシング、ライトとクラスター、シャドウ、反射プローブ、パーティクル）、`rayTracing`（レイトレーシング用の構造の統計） |
| `get_logs` | `limit`（1〜1000、既定 100）、`minSeverity`（`debug` / `info` / `warning` / `error` / `fatal`、既定 `info`）、`afterSeq`、`component` | 構造化ログ。返された `lastSeq` を次の `afterSeq` に渡すと新しい分だけ読める |
| `get_crash_report` | なし | 今回のセッションの障害レポート、前回のレポート、前回が正常終了でなかったか、前回のログの末尾、レンダラーとデバイスの状態 |
| `get_settings` | なし | 設定値（`values`）とそのスキーマ（`schema`：型、許される値、範囲、説明）、`renderFeatures`、`qualityPresets` |
| `get_benchmark` | なし | ベンチマークの状態（`idle` / `running` / `done` と結果） |

`get_material_info` の `detail`：

| `detail` | 返すもの |
|---|---|
| `summary`（既定） | シェーダーごとのマテリアル数、レンダラー数、レンダーキューの範囲、透明 / アルファテストの数、キーワード、マテリアル名の例。`refresh: false` なら前回集めた結果 |
| `survey` | シェーダーのプロパティ配置と値、メッシュの頂点レイアウト、`screenProbes`（画面上の点の下のマテリアル）、`transparentMaterials`、`propertyBlocks`（レンダラーごとの上書き）、`lights`（HDRP 用データつき）、`environment`（`hostExposureMultiplier`：純正のパイプラインが動いているときの露出の実測値）、`volumes`。重い |
| `aether` | Aether のマテリアル変換の結果（クラス、フラグ、数値、束縛テクスチャー、未対応プロパティ数）とクラス別の集計。`limit`（1〜600、既定 200）。描画モードが `aether` か `compare` のときに埋まる |

`get_status` のうち、よく見る項目：

| 項目 | 内容 |
|---|---|
| `renderer.state` | `waiting for the first frame` / `ready` / `failed` |
| `renderer.commandLists` | Aether のコマンドリストの使われ方。`lists`（本数）、`waitingForGpu`、`waitingForHostFence`、`waitingForReuseDelay`（再利用を待っている本数）、`acquires`、`noneFree`（空きが無く、そのフレームの Aether の処理を飛ばした回数） |
| `renderer.lastError` | 最後のエラー（無ければ null） |
| `managed.loader` | ローダー名と版（`BepInEx <版>` など） |
| `managed.nativeLoad` | ネイティブ DLL の読み込み方 |
| `managed.hdrpDx12Fix` | HDRP の DX12 修正の状態 |
| `managed.gamePassesDx12` | DX12 で動かないゲームのカスタムパスを止めた状態 |
| `managed.sceneCapture` | シーンキャプチャの状態（インスタンス、パーティクル、メインスレッドの時間など） |
| `managed.mods` | KSL が読み込んでいる Mod の一覧（BepInEx のときは空） |

## 操作ツール（`--allow-control` が必要）

| ツール | 入力 | すること |
|---|---|---|
| `set_render_quality` | `quality`：`low` / `medium` / `high` / `ultra` | 画質プリセットを設定する。シーンレンダラーのシャドウカスケードの解像度（1024 / 1536 / 2048 / 3072）と距離が変わる |
| `set_render_feature` | `feature`、`enabled` | 描画機能を切り替える（下の表） |
| `set_graphics_option` | `option`、`value`（数値か文字列） | Aether の画質設定を 1 つ変える（下の表）。オーバーレイの Settings タブと同じもの |
| `set_render_mode` | `mode`（省略可）、`debugView`（省略可）。少なくとも一方 | 描画モードとデバッグ表示を切り替える。実行中に切り替えられる |
| `reload_shader` | なし | シェーダーをディスクから読み直してパイプラインを作り直し、コンパイラの出力を返す |
| `capture_frame` | `withOverlay`（既定 true）、`includeImage`（既定 false） | 次のフレームのバックバッファーを PNG で `captures\` に保存してパスを返す（`includeImage` で画像も返す。数 MB になることがある） |
| `capture_render_graph` | なし | レンダーグラフを JSON で `captures\` に保存して返す |
| `run_benchmark` | `seconds`（1〜600、既定 10）、`label`（64 文字まで、既定 `mcp`）、`wait`（既定 true）、`vsync`（既定 true） | 指定秒数だけ計測して JSON で `benchmarks\` に保存し、結果を返す（平均 FPS、1% low、フレーム時間、GPU 時間、VRAM の開始 / 終了 / 最大、リソース統計、レンダーグラフ）。`vsync: false` で計測中だけ VSync とフレームレート上限を外す。`wait: false` ならすぐ返るので `get_benchmark` で待つ |

`captures\` と `benchmarks\` は Aether のデータフォルダー（BepInEx なら `BepInEx\plugins\Aether\`）の下です。

### set_render_mode

`mode`：

| 値 | 意味 |
|---|---|
| `aether`（既定） | Aether がゲームのレンダーパイプラインになる。ゲーム純正のパイプラインは作られない。UI はその上に出る |
| `standalone` | `aether` と同じ（別名） |
| `hdrp` | ゲーム純正のレンダラーだけ。比べるための参照用 |
| `compare` | 左半分が純正、右半分が Aether |

`debugView`：`none`、`albedo`、`normal`、`roughness`、`metallic`、`depth`、`lighting`、`shadow`、`materialClass`、`sun`、`ambient`、`reflection`、`bvh`。描画モードが `aether` か `compare` のときに使われます。

### set_render_feature の機能

機能の一覧は `get_settings` の `renderFeatures` でも分かります（17 個）。

| 機能 | 内容 |
|---|---|
| `testTriangle` | DX12 統合のテスト三角形 |
| `gpuTiming` | タイムスタンプクエリーで GPU 時間を測る |
| `shadows` | 太陽のシャドウカスケード |
| `sky` | 空の背景と環境の映り込み |
| `ssao` | 画面空間アンビエントオクルージョン |
| `antialiasing` | テンポラルアンチエイリアス |
| `bloom` | ブルーム（強さと広がりはゲームの設定に従う） |
| `motionBlur` | モーションブラー（強さはゲームの設定に従う） |
| `ssr` | 画面空間反射（ゲームのボリュームで有効な所だけ。テンポラルアンチエイリアスが要る） |
| `particles` | パーティクル（ゲームのパーティクルシステムとタイヤスモーク） |
| `volumetricFog` | シャドウマップを通した太陽光で照らす霧（光の筋）。物理の空では空気そのものの霞、シーンの空ではシーンの霧 |
| `indirectLight` | 近くの面からの照り返し（画面空間） |
| `reflectionProbe` | 毎フレーム、カメラの周りにキューブを描いて、画面外も含めた周囲の映り込みにする |
| `rayTracedReflections` | シーンの三角形にレイを飛ばした、滑らかな面の反射（画面外も映る） |
| `followSceneNight` | ゲームの昼夜の切り替えが夜の間、物理的な空も夜（真夜中ごろ、月が出る）にする |
| `radianceCascades` | Radiance Cascades の間接光（既定オン）。画面、光線が当たった面、ガラス、煙、反射プローブのすべてが使う。`probeGi` の代わり |
| `rayTracedGi` | 不透明面の環境光を画素ごとのレイトレース（最大 60 m、NRD REBLUR）で求める（既定オフ）。`rayTracedLighting` が必要 |
| `probeGi` | カメラの周りのプローブの格子からの間接光（各プローブが毎フレーム、シーンの三角形にレイを飛ばす）。`radianceCascades` がオフのときに使う |
| `lensFlare` | 太陽と明るいランプからのレンズフレア |
| `rayTracedLighting` | レイトレースの太陽の影と AO（NVIDIA NRD の SIGMA、REBLUR でノイズ除去）。オフ：シャドウマップと SSAO。状態は `get_scene_info` の `lightingDenoiser` |

### set_graphics_option のオプション

受け付けるのは次の 27 個です（`Api.cpp` の `kOptions`）。範囲の検証は設定と同じ（`src/Aether.Native/Core/Config.cpp`）です。

| オプション | 値 | 内容 |
|---|---|---|
| `skyMode` | `physical` / `scene` | 空と太陽：Aether の大気（`timeOfDay` の時刻）か、シーン自身の空のテクスチャーと太陽 |
| `timeOfDay` | 0〜24 | 物理的な空の時刻（時） |
| `sunHeading` | 0〜360 | 太陽の通り道を上向きの軸の周りに回す（度） |
| `cloudCoverage` | 0〜1 | 雲の量（0 で雲なし） |
| `cloudSpeed` | 0〜4 | 雲の流れる速さ |
| `shadowSoftness` | 0〜4 | 太陽の影の縁のぼかし（シャドウマップのテクセル数。0 で硬い影） |
| `indirectStrength` | 0〜3 | 照り返しの係数 |
| `fogStrength` | 0〜20 | 霧の濃さの係数（物理の空では空気の霞、シーンの空ではシーンの霧に掛かる） |
| `moonlight` | 0〜4 | 夜の月の光の係数 |
| `skinnedRayUpdates` | 0〜4 | レイトレースの反射のために、毎フレーム GPU から姿勢を読み戻すスキンメッシュの数（既定 0。毎フレームの読み戻しは、しばらくするとゲームがフレームを飛ばす原因になる） |
| `motionBlurStrength` | 0〜2 | モーションブラーの強さ（シーンの値への係数） |
| `bloomStrength` | 0〜2 | ブルームの強さ（シーンの値への係数） |
| `aoStrength` | 0〜2 | アンビエントオクルージョンの強さ |
| `aoRadius` | 0.2〜4 | アンビエントオクルージョンの半径（m） |
| `lensFlareStrength` | 0〜3 | レンズフレアの係数 |
| `smokeOpacity` | 0〜2 | タイヤスモークの不透明度の係数 |
| `lodBias` | 0.3〜8 | Unity の LOD バイアス（大きいほど遠くまで詳しいモデル） |
| `anisotropicLevel` | 1〜16（整数） | 異方性フィルタリングのレベル |
| `vsync` | 0〜2（整数） | 垂直同期：0 オフ、1 毎リフレッシュ、2 1 回おき |
| `exposureEV` | −10〜10 | シーンの露出に足す補正（段） |
| `tonemap` | `host` / `agx` / `aces` / `neutral` / `none` | トーンマッピングの曲線 |
| `pathTracing` | `off` / `freeCamera` / `always` | パストレーシング：オフ、フリーカメラ（F11）の間だけ（既定）、常に |
| `pathTracingBounces` | 1〜12（整数） | パストレーシングの経路のバウンス数（既定 4） |
| `pathTracingMaxSamples` | 1〜100000（整数） | パストレーシングを重ねる画素あたりのサンプル数の上限（既定 4096） |
| `freeCameraSpeed` | 0.2〜100 | フリーカメラの速さ（m/s） |
| `freeCameraSensitivity` | 0.01〜1 | フリーカメラのマウスの感度（度 / カウント） |
| `reflectionDenoiser` | `nrd` / `off` | 既定 `nrd`：反射の光線を表面のローブからサンプリングし、NVIDIA NRD（REBLUR）でノイズ除去する。`off`：鏡面方向の光線、ノイズ除去なし。状態は `get_scene_info` の `reflectionDenoiser` |

MCP から変えられない設定：`overlayHotkey`、`overlayVisibleAtStart`、`overlayScale`、`vramBudgetMB`、`vramWarnPercent`、`mcpControl`、`mcpApprovalTimeoutSec`、`logLevel`。`quality` は `set_render_quality`、`renderMode` と `debugView` は `set_render_mode` で変えます。

## 権限

操作は次の 3 段を通ります。どこで断られたかはエラーコードで分かります。

1. **MCP サーバー**：`--allow-control` が無ければ `E_CONTROL_DISABLED`。
2. **ゲームの設定 `mcpControl`**（オーバーレイの Settings の `Control requests`、または `aether.json`）：
   `deny` は `E_CONTROL_DENIED`。`ask`（既定）はゲーム内に承認ダイアログを出し、拒否か時間切れ（`mcpApprovalTimeoutSec`、5〜300 秒、既定 30 秒）で `E_NOT_APPROVED`。`allow` はそのまま通す。
3. **値の検証**：範囲外や未知の値は `E_INVALID_PARAMS`。引数の型と、プリセット / 機能 / オプション / モードの名前は承認の前に調べます（不正な要求で承認ダイアログは出ません）。`set_render_feature` で変えられるのは描画機能だけ、`set_graphics_option` で変えられるのは上のオプションだけで、`mcpControl` などほかの設定には届きません。

AI から届く操作は、オーバーレイと同じ検証済みの入口（設定の変更、シェーダーの再読み込みの要求、キャプチャとベンチマークの要求）だけです。
D3D12 のデバイス、リソースの寿命、任意のコードの実行に届くツールはありません。応答は IPC のスレッドがスナップショットから作り、描画スレッドを待たせません。

## エラーコード

ツールのエラーは `isError: true` と、`<コード>: <メッセージ>` のテキストで返ります。

| コード | 意味 |
|---|---|
| `E_NOT_RUNNING` | パイプにつながらない。ゲームが起動していない、Aether が入っていない / 起動に失敗した、ゲームが別ユーザー（管理者など）で動いている |
| `E_DISCONNECTED` | 応答の前にゲームが終了した / 接続が切れた（次の呼び出しで再接続する） |
| `E_TIMEOUT` | 時間内に応答が無い（ゲームが固まっている、最小化や背景で止まっている）。ベンチマークが終わらないときも |
| `E_NOT_READY` | `capture_frame` / `run_benchmark`：レンダラーがまだ 1 フレームも描いていない、または障害で止まっている |
| `E_CONTROL_DISABLED` / `E_CONTROL_DENIED` / `E_NOT_APPROVED` | 上の「権限」 |
| `E_INVALID_PARAMS` | 引数が不正（スキーマに合わない、範囲外、未知の値） |
| `E_CAPTURE_FAILED` / `E_BENCHMARK` / `E_WRITE_FAILED` | その操作の失敗（メッセージに理由） |
| `E_UNKNOWN_TOOL` | 無いツール名 |
| `E_PROTOCOL` / `E_BAD_REQUEST` / `E_UNKNOWN_METHOD` / `E_INTERNAL` | サーバーとランタイムの間のやり取りの異常 |

## 調べ方の例

- **VRAM を多く使っている原因**：`get_vram_usage`（プロセス全体と Aether / Unity の内訳）→ `get_resource_list`（`includeUnityRenderTextures: true` で Unity 側の大きなレンダーターゲット）→ `get_render_graph` → `get_logs`。
- **フレーム時間の悪化**：`get_performance`（`historyFrames` で推移、パスごとの時間、Unity の CPU / GPU）→ `get_render_graph`。
- **画面のちらつき**：`get_status` の `renderer.commandLists`。`noneFree` が増え続けていれば、Aether の処理を飛ばしたフレームがある。キャプチャは Aether が描けたフレームでしか成功しないので、撮影では分からない。
- **シェーダーエラー**：`get_shader_errors` → ファイルを直す → `reload_shader`。
- **前回落ちた**：`get_crash_report`。

## IPC（MCP サーバーとランタイムの間）

1 行 1 JSON です。

- 要求：`{"id":1,"method":"get_gpu_info","params":{},"control":false}`
- 応答：`{"id":1,"ok":true,"result":{...}}` または `{"id":1,"ok":false,"error":{"code":"...","message":"..."}}`

メソッド名はツール名と同じです。ほかに `ping`（`get_status` と同じ）、`get_capture`（`id` を渡してキャプチャの状態を読む）、`stop_benchmark`（操作）があります。`control` が true になるのは、`--allow-control` のサーバーが操作ツールを呼んだときだけです。
パイプのインスタンスは最大 4 本、1 要求は 1 MB までです。実装は `src/Aether.Native/Ipc/`（ランタイム側）と `src/Aether.MCP/IPC/RuntimeClient.cs`（サーバー側）。
