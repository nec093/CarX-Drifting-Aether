# MCP API リファレンス

`Aether.MCP.exe` は stdio の MCP サーバーです（JSON-RPC 2.0、1 行 1 メッセージ、プロトコル 2025-06-18 / 2025-03-26 / 2024-11-05）。
ゲームの中の Aether ランタイムとは、この PC の同じユーザーだけが開ける名前付きパイプ `\\.\pipe\Aether.Runtime` で話します。
各ツールの入力・出力スキーマは `tools/list` が返します（定義は `src/Aether.MCP/Tools/ToolCatalog.cs`）。

```
Aether.MCP.exe [--allow-control] [--pipe <name>] [--list-tools]
```

| 引数 / 環境変数 | 意味 |
|---|---|
| `--allow-control` / `AETHER_MCP_ALLOW_CONTROL=1` | 操作ツール（下の表の「操作」）を提供する。無ければ読み取り専用 |
| `--pipe <name>` / `AETHER_PIPE_NAME` | パイプ名（`\\.\pipe\` を除く）。既定 `Aether.Runtime` |

コマンドラインから 1 回呼ぶには `scripts\mcp-call.ps1 <tool> [key=value ...] [-AllowControl]`。

## 読み取りツール

| ツール | 入力 | 返すもの |
|---|---|---|
| `get_status` | なし | バージョン、レンダラーの状態、GPU、エラー数、前回の異常終了の有無、ゲーム側の情報（Unity、グラフィックス API、HDRP の設定、読み込まれている Mod、HDRP の DX12 修正の状態）。Phase 2：`renderMode`、`sceneRenderer`（可視インスタンス、ドローコール、三角形、ライトとクラスター、シャドウ）、`managed.sceneCapture`。最初に呼ぶ |
| `get_gpu_info` | なし | アダプター（ベンダー、名前、デバイス ID、ドライバー、専用 / 共有メモリ）と D3D12 の機能（シェーダーモデル、Resource Binding Tier、Resource Heap Tier、Wave Ops、レイトレーシング、メッシュシェーダー、VRS、Enhanced Barriers、BC 形式）、デバイスの状態 |
| `get_performance` | `historyFrames`（0〜2000） | FPS、フレーム時間（平均、最小、最大、p50 / p95 / p99）、1% low と 0.1% low、GPU フレーム時間、Aether の CPU / GPU 時間とドローコール、パスごとの時間、Unity の値（メイン / 描画スレッド、Present 待ち、ドローコール、バッチ、三角形） |
| `get_vram_usage` | なし | プロセスの専用 / 共有メモリと予算（DXGI）、Aether のカテゴリー別、Unity の申告値、Aether 以外の分。Phase 2：`aetherManager`（ディスクリプター、フレームメモリ、退役待ち、予算）、`shared`（Unity から借りているメッシュ / テクスチャーのバイト数。Aether の使用量には含めない） |
| `get_resource_list` | `limit`、`category`、`includeUnityRenderTextures` | Aether のリソース（大きい順）、作成 / 解放の回数と直近のイベント。指定すれば Unity のレンダーターゲットも |
| `get_render_graph` | なし | パス（順序、読み書き、直近の時間）とリソース。スキーマ `aether.rendergraph/2`：`sceneGraph`（シーンレンダラーのパスごとの読み書きと状態、ドローコール、三角形、CPU / GPU 時間、一時リソースとプール、検証エラー）、`sceneStats` |
| `get_shader_errors` | `includeAllShaders` | コンパイルに失敗したシェーダーとコンパイラの出力 |
| `get_material_info` | `refresh`（既定 true）、`detail`：`summary`（既定）/ `survey` / `aether` | `summary`：シェーダーごとのマテリアル数、レンダラー数、レンダーキュー、透明 / アルファテストの数、キーワード。`survey`：シェーダーのプロパティ配置、頂点レイアウト、ライト、HDRP のグローバルテクスチャー（マテリアル変換の設計資料。重い）。`aether`：Aether のマテリアル（クラス、フラグ、数値、束縛テクスチャーの形式とサイズ、未対応プロパティ数）とクラス別の集計 |
| `get_scene_info` | なし | シーン、カメラ（HDRP の設定つき）、ライト、レンダラー、反射プローブ、HDRP のボリュームとその中身、キャンバス、環境。Phase 2：`aetherScene`（ネイティブ側が持つメッシュ / テクスチャー / マテリアル / インスタンスの数とバイト数、契約違反の数と最後の内容）、`aetherRenderer` |
| `get_logs` | `limit`、`minSeverity`、`afterSeq`、`component` | 構造化ログ。`lastSeq` を次の `afterSeq` に渡すと新しい分だけ |
| `get_crash_report` | なし | 今回のセッションの障害レポート、前回のレポート、前回が正常終了でなかったか |
| `get_settings` | なし | 設定値とそのスキーマ、切り替えられる描画機能、画質プリセット |
| `get_benchmark` | なし | ベンチマークの状態（`idle` / `running` / `done` と結果） |

## 操作ツール（`--allow-control` が必要）

| ツール | 入力 | すること |
|---|---|---|
| `set_render_quality` | `quality`：`low` / `medium` / `high` / `ultra` | 画質プリセットを設定（シーンレンダラーのシャドウカスケードの解像度と距離） |
| `set_render_mode` | `mode`：`hdrp` / `aether` / `compare`、`debugView`（省略可）：`none` / `albedo` / `normal` / `roughness` / `metallic` / `depth` / `lighting` / `shadow` / `materialClass` | 描画モードとデバッグ表示を切り替える。`hdrp`：ゲームのレンダラーのみ（シーンキャプチャ停止）。`aether`：Aether の絵で置き換える（UI は残る）。`compare`：左半分 HDRP、右半分 Aether |
| `set_render_feature` | `feature`、`enabled` | 描画機能の切り替え（`get_settings` の `renderFeatures`：`testTriangle`、`gpuTiming`、`shadows`、`sky`） |
| `reload_shader` | なし | シェーダーをディスクから読み直してパイプラインを作り直し、コンパイラの出力を返す |
| `capture_frame` | `withOverlay`、`includeImage` | 次のフレームを PNG に保存してパスを返す（`includeImage` で画像も） |
| `capture_render_graph` | なし | レンダーグラフを JSON ファイルに保存 |
| `run_benchmark` | `seconds`（1〜600）、`label`、`wait` | 指定秒数だけ計測して JSON に保存し、結果を返す（平均 FPS、1% low、フレーム時間、GPU 時間、VRAM の開始 / 終了 / 最大、リソース統計、レンダーグラフ） |

## 権限

操作は次の 3 段を通ります。どこで断られたかはエラーコードで分かります。

1. **MCP サーバー**：`--allow-control` が無ければ `E_CONTROL_DISABLED`。
2. **ゲームの設定 `mcpControl`**（オーバーレイの Settings、または `kino\config\Aether\aether.json`）：
   `deny` は `E_CONTROL_DENIED`。`ask`（既定）はゲーム内に承認ダイアログを出し、拒否か時間切れ（`mcpApprovalTimeoutSec`、既定 30 秒）で `E_NOT_APPROVED`。`allow` はそのまま通す。
3. **値の検証**：範囲外や未知の値は `E_INVALID_PARAMS`。`set_render_feature` で変えられるのは描画機能だけで、`mcpControl` などほかの設定には届きません。

AI から届く操作は、オーバーレイと同じ検証済みの入口（設定の変更、シェーダーの再読み込みの要求、キャプチャとベンチマークの要求）だけです。
D3D12 のデバイス、リソースの寿命、任意のコードの実行に届くツールはありません。応答は IPC のスレッドがスナップショットから作り、描画スレッドを待たせません。

## エラーコード

| コード | 意味 |
|---|---|
| `E_NOT_RUNNING` | ゲームが起動していない、Aether が入っていない / 起動に失敗した、別ユーザーで動いている |
| `E_DISCONNECTED` | 応答の前にゲームが終了した / 接続が切れた（次の呼び出しで再接続） |
| `E_TIMEOUT` | 時間内に応答が無い（ゲームが固まっている、最小化で止まっている） |
| `E_NOT_READY` | レンダラーがまだ 1 フレームも描いていない、または障害で止まっている |
| `E_CONTROL_DISABLED` / `E_CONTROL_DENIED` / `E_NOT_APPROVED` | 上の「権限」 |
| `E_INVALID_PARAMS` | 引数が不正 |
| `E_CAPTURE_FAILED` / `E_BENCHMARK` / `E_WRITE_FAILED` | その操作の失敗（メッセージに理由） |

## 調べ方の例

- **VRAM を多く使っている原因**：`get_vram_usage`（プロセス全体と Aether / Unity の内訳）→ `get_resource_list`（`includeUnityRenderTextures: true` で HDRP 側の大きなレンダーターゲット）→ `get_render_graph` → `get_logs`。
- **フレーム時間の悪化**：`get_performance`（`historyFrames` で推移、パスごとの時間、Unity の CPU / GPU）→ `get_render_graph`。
- **シェーダーエラー**：`get_shader_errors` → ファイルを直す → `reload_shader`。
- **前回落ちた**：`get_crash_report`。

## IPC（MCP サーバーとランタイムの間）

1 行 1 JSON。要求 `{"id":1,"method":"get_gpu_info","params":{},"control":false}`、
応答 `{"id":1,"ok":true,"result":{...}}` または `{"id":1,"ok":false,"error":{"code":"...","message":"..."}}`。
メソッド名はツール名と同じです（ほかに `get_capture`、`stop_benchmark`）。実装は `src/Aether.Native/Ipc/`。
