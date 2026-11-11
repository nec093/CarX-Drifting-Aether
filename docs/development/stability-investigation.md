# D3D12 リソース破損の調査（2026-11-11）

対象：D3D12 デバッグレイヤー付き（`launch-game.ps1 -DebugLayer`）でガレージに放置すると、Aether が動いているときに
Unity が落ちる問題（`docs/development/phase2-report.md` 1 章）。RTX 4070 Ti、1920×1080、各回 300 秒。

## 1. 結論

**確認できた事実**

1. 壊れるのは Unity 自身の**動画フレームのアップロード**。デバッグレイヤーの最初のメッセージ（3 回で採取）：
   `CopyTextureRegion: The destination region extends past ... destination format is A8_UNORM ... SrcBox width is 1080, height is 608`。
   A8・1080×608 は動画の輝度プレーン。アップロード先のテクスチャーが、Unity の想定と違うもの（差し替え済み / 解放済み）になっている。
   別の回では `pResource in the first parameter is corrupt`（解放済み）や、Unity の遅延解放リストの破損（`UnityPlayer+0x684302 / +0x684440`）として現れる。同じ系統。
2. **ゲームの動画と広告（ANZU の 2 コンポーネントと `VideoPlayer` 1 個）を無効にすると、落ちない。**
   compare 3/3、aether 3/3、hdrp 1/1 が 300 秒・D3D12 メッセージ 0 件（6 章。hdrp の残り 2 回は Mod が読み込まれず無効）。
3. 動画が有効でも、**Aether がシーンを扱っていなければ落ちない**（何も提出しない：5/5 生存）。動画だけでは起きない。
4. Aether の GPU への提出は必須条件ではない。リストを記録するだけで一度も提出しない条件でも 1/3 で落ち、1/3 で上の A8 のエラーが出た。
5. Aether が借用するテクスチャーも必須条件ではない（テクスチャーを一切借用しない：3/3 で落ちる）。
6. 提出の経路（Unity の `ExecuteCommandList` か、キューへ直接か）は無関係（どちらも落ちる）。
7. エンジン単体（テストホスト、デバッグレイヤー）は 20 分・70,828 フレーム、エラー 0、リーク 0。

**原因についての判断**

- 確認できたのは「**ゲームの動画再生（ANZU / VideoPlayer）のテクスチャー差し替えと、Aether のシーン処理が同時に動くと、Unity 内部で動画テクスチャーのアップロード先がずれる**」こと。
- Aether のどの操作が引き金かは**特定できていない**。残る候補（5 章）は、メインスレッドからの `Mesh.GetNative*BufferPtr`（描画スレッドとの同期を起こす）、
  フレーム途中のシーンイベント（`FlushCommandBuffers | SyncWorkerThreads`）のいずれか、または両方。
- Aether 自身の D3D12 の使い方（アロケーター、フェンス、解放、ディスクリプター）に違反は見つかっていない（4 章）。
- したがって現在の対処は**回避策**（DX12 では動画と広告を止める）であり、根本原因の修正ではない。プロジェクトの所有者が無効化を了承済み。

## 2. 実験（デバッグレイヤー有、ガレージ放置、300 秒）

| 記号 | 変えた点（基準は A） | モード | 生存 / 回数 | 落ちた時刻 (s)、備考 |
|---|---|---|---|---|
| A | 基準：Unity の `ExecuteCommandList`、全機能 | compare | 0 / 3 | 221、76、151 |
| B | キューへ直接 `ExecuteCommandLists` | compare | 1 / 3 | 118、257。生存回は A8 のエラー後に Aether 停止 |
| C | GPU 時間計測オフ | compare | 1 / 2 | 142（ほか 1 回は操作ミスで無効） |
| D | Aether は何も提出しない + 描画スレッドで 3 ms 待つ | hdrp | 3 / 3 | 提出 0、約 17,500 フレーム |
| （前日分） | Aether は何も提出しない | hdrp | 2 / 2 | |
| E | リストを記録するだけで提出しない | compare | 2 / 3 | 275。生存 1 回は A8 のエラーでデバイス喪失 |
| F | ネイティブポインタの問い合わせを描画ループの外（Update）へ | compare | 2 / 3 | 82。生存 1 回は A8 のエラーでデバイス喪失 |
| G1 | テクスチャーを一切借用しない | compare | 0 / 3 | 236、118、205 |
| **I** | **動画と ANZU を無効化**（F の変更込み） | compare | **3 / 3** | D3D12 メッセージ 0 |
| **J** | 同上 | aether | **3 / 3** | D3D12 メッセージ 0 |
| **K** | 同上 | hdrp | **1 / 1** | D3D12 メッセージ 0。2・3 回目は KSL が Mod を拒否（下の「KSL による拒否」）して Aether が動いておらず、無効 |

落ちた時刻とフレーム数に規則性はない（76〜278 秒、1,198〜13,286 フレーム）。フレーム数や提出回数に比例する故障ではない。
各回の記録（時間、フレーム数、提出回数、リスト再利用数、フェンス値、リソースの確保 / 解放数、最初と最後のデバッグメッセージ、クラッシュの位置）は
`kino\mods\Aether\benchmarks\soak-results.jsonl`。

## 3. この調査で直した Aether 側の誤り

破損の原因ではなかったが、Unity のヘッダーや D3D12 の規則に照らして誤りだったもの。

| 誤り | 修正 |
|---|---|
| キューへの提出が禁止の設定（`DontCare`：「no work must be submitted」）のイベントでリストを提出していた（フレーム先頭、Phase 1 から） | `Allow` + `SyncWorkerThreads` |
| バックバッファーの状態を毎フレーム `states` で宣言していた（デバッグレイヤー下、hdrp でも 161 秒で落ちた：2 回） | 宣言しない。RENDER_TARGET のまま描いて返す |
| Unity のフレームフェンスを完了判定に使っていた（アロケーターを実行中にリセット） | 完了マーカー + 自前フェンス |
| 起動直後、バックバッファーが RENDER_TARGET でないフレームに描いていた | 最初の 8 フレームは描かない |
| 描画ループ（`endContextRendering`）の中でネイティブポインタを問い合わせていた | Update へ移動 |

## 4. 寿命と所有の監査

- **COM の解放**：ネイティブコードに生の `Release()` 呼び出しは無い。すべて `ComPtr`。`Detach` は「GPU の停止を確認できないときに意図的に漏らす」終了経路だけ。`Attach` は `QueryInterface` の結果だけ。
- **所有の区分**：Aether 所有（`GpuResources` / `ResourceRegistry` が作ったもの）、借用（Unity のメッシュバッファーとテクスチャー。`QueryInterface` で自分の参照を取り、退役キュー経由で 1 回だけ手放す）、
  参照を持たない生ポインタ（Unity のデバイス、キュー、スワップチェーン。解放しない）。バックバッファーは `GetBuffer` の参照をそのフレーム内で手放す。
- **追加した検査**（違反は `E_LIFETIME` としてログに出る）：登録簿からの二重解除、退役の順序違反、Aether 所有テクスチャーの解放時の参照数（COM 1、所有者 1 であること）。
  テストホスト（リサイズ、シーンのアンロード / 再読み込み）とゲーム内の全実験で 0 件。
- **アロケーター / リスト**：再利用の条件は (a) 自前の完了マーカーと自前フェンスで GPU の完了を確認、(b) Unity のフレームフェンスが `ExecuteCommandList` の戻り値を超えた、(c) その後 24 回の提出。
  デバッグレイヤーのアロケーター関連のエラーは 0 件。
- **ディスクリプター / アップロード / フレームアリーナ**：すべて同じ完了通し番号で退役。使用中のテーブルは書き換えず、新しい範囲を取る。
- **終了時**：テストホストでデバイスの残り参照 0。ゲーム内は `Device objects released`、`stopped cleanly`、漏らしの警告なし。
  `ReportLiveDeviceObjects` はゲーム内では Unity のオブジェクトが大量に出るため使っていない（テストホストの参照数検査で代替）。

## 5. 未確定の点（仮説）

1. **引き金は `Mesh.GetNativeVertexBufferPtr / GetNativeIndexBufferPtr` の同期**：メインスレッドが描画スレッドを待つあいだに、動画テクスチャーの作り直しとフレームのアップロードの順序が入れ替わる。
   G1（テクスチャーなし）でも落ち、E（提出なし）でも落ちたことと矛盾しない。未検証。
2. **引き金はフレーム途中のシーンイベントのフラグ**（`FlushCommandBuffers | SyncWorkerThreads`）。E と矛盾しない。未検証。
3. ANZU と VideoPlayer の**どちらの動画**が壊れるのかは切り分けていない（両方まとめて無効化した）。
4. デバッグレイヤーなしでも同じずれが起きているか（検出されないだけか）は不明。回避策なしの compare / aether は、デバッグレイヤーなしで 168 秒・エラー 0 を 1 回確認しただけ。

次に行うなら：メッシュの問い合わせを止める条件、シーンイベントのフラグを 1 つずつ外す条件、VideoPlayer だけ / ANZU だけを止める条件を、動画有効のまま各 3 回。

## 6. 対処の内容と結果

`GameDx12Workarounds`（DX12 のときだけ）：2 秒ごとに、有効な `UnityEngine.Video.VideoPlayer` と、型名に `Anzu` を含むコンポーネントを無効にする。
ガレージで無効になったもの：`Services.Anzu.AnzuUnityWrapper`、`Services.Anzu.Obstruction.AnzuObstructionItem`、`VideoPlayer` 1 個。
`kino\config\Aether\keep-game-videos` というファイルを置くと無効化しない。状態は KSL のパネルと Aether のログ。

影響：ガレージの広告パネルと動画スクリーンが更新されなくなる。広告の配信が止まる。コースでの影響は未確認。

結果（デバッグレイヤー有、300 秒）：compare 3/3、aether 3/3、hdrp 1/1。有効な 7 回すべてで D3D12 のメッセージ 0、Aether のエラー 0。
hdrp は成功基準（3 回）に届いていません。終了は、ウィンドウを閉じてから 30 秒以内にプロセスが終わらない回が 4 回ありました（デバッグレイヤー下。次の起動までには終了）。
終了時のログ（`stopped cleanly`）は記録の取り方の問題で確認できていません。

### KSL による拒否（2026-11-11 22:49 以降）

hdrp の 2 回目から、KSL が Mod を読み込まなくなりました（`kino\output.log`）：

```
[Info :       KSL] User type: Regular, maxMods=30
[Error:  KSL.Mods] Unable to load mod, result: Mod 117596592274407424 is banned (BannedMod)
```

それまで `ModDeveloper` だったユーザー種別も `Regular` になっています。原因は不明です。時系列では、動画と広告（ANZU）を無効にするビルドを入れて約 45 分後、
この日 40 回を超える起動とクラッシュの後です。広告の無効化が検出された可能性と、短時間の大量起動・クラッシュが原因の可能性のどちらもあり、確認できていません。
この拒否を回避する操作（別の ID で署名し直す等）は行っていません。以後の実験は止めています。

## 7. 道具

- `scripts\soak-test.ps1`：条件を指定して起動、放置、結果を 1 行の JSON に記録。繰り返し可。
- `kino\config\Aether\dev-options.json`（`src/Aether.Native/Core/DevOptions.h`）：実験用のスイッチ。ファイルが無ければ何も変えない。
  `directSubmit`、`beginFlags` / `endFlags` / `sceneFlags`、`recordOnly`、`sleepMs`、`captureFlags`、`statsSeconds`（提出統計のログ）。
