# アーキテクチャの概要

## 全体

```
CarX Drift Racing Online（Unity 2023.2.22f1, HDRP, -force-d3d12）
      │
   KSL Loader ── Aether.ksm（C#, src/Aether.Managed）
      │             ・HDRP の DX12 修正（GameIntegration/HdrpDx12Fix.cs）
      │             ・AetherNative.dll を Unity のプラグインとして読み込ませる（Native.cs）
      │             ・描画イベントの発行（FRAME_BEGIN / FRAME_END / SHUTDOWN）
      │             ・Unity 側の統計・シーン・マテリアル・レンダーターゲットの報告
      ▼
AetherNative.dll（C++, src/Aether.Native）
      ├─ Device/UnityBridge   IUnityGraphicsD3D12v7 → HostGfx
      ├─ Renderer             フレーム、テスト三角形、キャプチャ、GPU タイムスタンプ
      ├─ ImGui/Overlay        ゲーム内 GUI
      ├─ Resources            GPU リソースの登録簿
      ├─ Profiler             フレーム統計、ベンチマーク
      ├─ Shaders              HLSL のコンパイルとエラー
      ├─ Core                 構造化ログ、設定
      └─ Ipc                  名前付きパイプ \\.\pipe\Aether.Runtime（JSON 1 行 = 1 要求）
                                   ▲
Aether.MCP.exe（C#, src/Aether.MCP）──┘   stdio の MCP ⇔ パイプ。ゲームとは別プロセス
      ▲
   AI エージェント
```

- Unity は物理、ゲームロジック、シーン管理、入力を担当し続けます。Aether が受け持つのは描画と、その計測・診断です。
- MCP サーバーはゲームと別プロセスで、ゲームが居なければ各ツールが `E_NOT_RUNNING` を返し、次の呼び出しで再接続します。

## スレッド

| スレッド | 何をするか | 規則 |
|---|---|---|
| Unity のメインスレッド | C# Mod。描画イベントの発行、統計の送信、ウィンドウプロシージャ（入力をキューに積むだけ） | 描画スレッドを待たない |
| Unity の描画スレッド | `Renderer`、`Overlay`（ImGui のフレーム構築と記録） | D3D12 のオブジェクトに触るのはここだけ。入口は `Session.cpp` で例外を受け止める |
| IPC スレッド（受付 1 + クライアントごと） | API の応答。スナップショットを読むだけ | 描画スレッドを待たせない、待たない。承認待ちや C# 側の収集待ちはここで行う |
| ワーカー 1 本 | PNG のエンコード、ファイル書き込み | |

C# とネイティブの間は、毎フレームの数値（double の配列）と、要求があったときだけの JSON（シーン、マテリアル、レンダーターゲット）です。
ネイティブ側が「欲しいもの」のビットを立て、C# 側が次のフレームで収集して渡します（`Aether_TakeManagedRequests` / `Aether_SetManagedState`）。

## ホストの境界（`Device/Host.h`）

レンダラーは `HostGfx`（デバイス、キュー、スワップチェーンの取得、コマンドリストの実行、フェンスの完了値、アイドル待ち）だけを相手にします。
ゲームでは Unity（`UnityBridge.cpp`）、テストではテストホスト（`tests/host/HostMain.cpp`、自前のデバイスとウィンドウ）がこれを埋めます。
グラフィックス API の抽象化ではありません（中身はすべて D3D12）。ゲームを起動せずに、デバッグレイヤー付きで統合の規則を検査するための境界です。

## フェーズと現状

| フェーズ | 内容 | 状態 |
|---|---|---|
| 1 | 基盤、DX12 統合、GPU 情報、Profiler / VRAM、Dear ImGui、MCP | 実装済み。確認状況は README の表 |
| 2 | 独自レンダラー（Forward+ / Clustered Forward を第一候補）：シーンキャプチャ、レンダーグラフ、GPU リソースマネージャー、ジオメトリ、PBR、影、反射、ポスト、テクスチャ管理、TAA | 未着手 |
| 3 | HDRP の完全置換、VRAM 最適化、3 状態（元の HDRP / 最適化した HDRP / Aether）の比較 | 未着手 |
| 4 | GI、高度な反射、DXR（オプション）、AI 診断の強化、自動ベンチマーク | 未着手（ベンチマークの記録と JSON 出力は Phase 1 に入っている） |

### Phase 2 に向けてゲームから分かっていること（garage、`get_scene_info` / `get_material_info`）

- HDRP の設定：Lit シェーダーモード Both、カラーバッファー R11G11B10、MSAA なし、SSR・SSAO・SSGI・ボリュメトリクス・ボリュメトリッククラウド・モーションベクター有効、デカール・SSS・レイトレーシング無効、動的解像度無効、シャドウアトラス 2048。
- garage のマテリアル：レンダラー 748、マテリアル 172、シェーダー 18 種。`HDRP/Lit` が最多（100 マテリアル）、車は Shader Graph（`CarPaint`、`CarGeneric`、`CarGlass`、`CarPaintRim`、`Colormask` ほか）。透明は車のガラスとステッチなど少数。
- UI は Screen Space - Overlay のキャンバス（バックバッファーへ直接）。メインカメラは 1 台。
- Unity のレンダーテクスチャーはガレージで約 245 個・約 628 MB（HDRP が DX12 で動いている状態）。大口はプロモーション用の 2560x1440、カスケードシャドウアトラス、ボリュメトリクスの 3D テクスチャー、SSR。
- ゲーム独自の HDRP カスタムパス（GPU カリング、パーティクル）がある。DX12 では動かないので無効化している（`dx12-integration.md`）。
- Phase 1 の確認結果と基準値は `docs/development/phase1-report.md`。
