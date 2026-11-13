# Scene Data Contract（版 7）

マネージド側（Unity、`src/Aether.Managed/SceneCapture`）と Aether のネイティブレンダラー（`src/Aether.Native/Scene/SceneStore`、レイトレース用の形状は `Renderer/Bvh`）の間の取り決め。

- 構造体の配置は `src/Aether.Native/include/SceneContract.h`。`SceneCapture/Contract.cs` がフィールド単位で同じ配置を持つ。
- 関数は `src/Aether.Native/include/AetherApi.h`（API 版 7、`AETHER_API_VERSION` と `Native.ApiVersion`）。
- 契約の版は `AE_SCENE_CONTRACT_VERSION`（ネイティブ）と `AeConst.ContractVersion`（マネージド）。現在はどちらも 7。

## 規則

- **使う前に確かめる。** 起動時に `SceneContract.Verify()` が契約の版（`Aether_SceneContractVersion`）と、8 つの構造体（`AeMeshDesc`、`AeTextureDesc`、`AeMaterialDesc`、`AeInstanceDesc`、`AeFrameInstance`、`AeLight`、`AeFrame`、`AeSubMesh`）の大きさ（`Aether_SceneStructSize`）を比べる。違えばシーンレンダラーは動かず、ログに `Scene contract mismatch` が出る。
- **呼び出しごとにも確かめる。** 各構造体は先頭に自分の大きさ（`structSize`）を持つ。`AeFrame` は `contractVersion` も持つ。プローブとパーティクルは大きさを引数で渡す。大きさやフィールドが不正な呼び出しは拒否し、`E_SCENE_CONTRACT` として数える（ログは最初の 20 件）。途中までの適用はしない。
- **ポインターでなくハンドル。** オブジェクトは `AeHandle`（32 ビット、0 = なし）で呼ぶ。マネージド側は Unity のインスタンス id か、id を組み合わせたハッシュを使う（[scene-capture.md](scene-capture.md)）。ハンドルは種類ごとに一意。既知のハンドルへの upsert は置き換え。
- **複製しない。** メッシュとテクスチャーは Unity 自身の `ID3D12Resource`（`Mesh.GetNativeVertexBufferPtr / GetNativeIndexBufferPtr`、`SkinnedMeshRenderer.GetVertexBuffer`、`Texture.GetNativeTexturePtr`）。ネイティブ側はポインターを SEH の保護下で `QueryInterface` して確かめ、COM の参照を取る。参照は廃棄キューを通してだけ手放す。生のポインターは保存しない。
- **バッファーに収まること。** 記述がバッファーの大きさより多くの頂点やインデックスを要求するメッシュは拒否する（描かない）。描けない形式のテクスチャーはバインドしない。

## オブジェクト

| 種類 | 構造体 | 内容 | 関数 |
|---|---|---|---|
| メッシュ（0） | `AeMeshDesc` + `AeSubMesh[]` | 頂点ストリーム最大 4（バッファー、ストライド）、インデックスバッファー（16 / 32 ビット）、頂点属性最大 6（位置、法線、接線、色、uv0、uv1。ストリーム、オフセット、DXGI 形式）、ローカルの境界、サブメッシュ（開始、数、基準頂点） | `Aether_SceneUpsertMesh` |
| テクスチャー（1） | `AeTextureDesc` | リソース、大きさ、ミップ数、次元（2D、キューブ、キューブ配列、2D 配列、3D） | `Aether_SceneUpsertTexture` |
| マテリアル（2） | `AeMaterialDesc` | Aether のマテリアル：クラス、フラグ、マスクの配置、色、数値、UV の拡大とずらし、テクスチャースロット 5 つ（[material-translation.md](material-translation.md)） | `Aether_SceneUpsertMaterial` |
| インスタンス（3） | `AeInstanceDesc` + `AeHandle[]` | メッシュ、サブメッシュごとのマテリアル、フラグ（影を落とす、影を受ける、静的）、レイヤー、`renderingLayerMask` | `Aether_SceneUpsertInstance` |
| 反射プローブ | `AeProbe[]` | キューブマップ、影響範囲（箱か球、ワールドから範囲への 3×4 行列、半径・半分の大きさ、ブレンド距離）、撮影位置、強さ、箱投影 | `Aether_SceneSetProbes`（一覧ごと。次の呼び出しまで保持。最大 256） |
| パーティクル | `AeParticleBatch[]` + `AeParticle[]` | そのフレームの粒子 | `Aether_SceneSubmitParticles` |
| フレーム | `AeFrame` + `AeFrameInstance[]` + `AeLight[]` | カメラ、インスタンスごとの行列とフラグ、ライト、環境 | `Aether_SceneSubmitFrame` |

消す：`Aether_SceneDestroy(kind, handle)`（kind 0〜3。メッシュを消すとそのレイトレースの木も消える）。すべて消す：`Aether_SceneClear`（レイトレースの木も消える）。

### フレーム（`AeFrame`）

毎フレーム、インスタンスの行列とライト以外に変わるもの。

| まとまり | フィールド |
|---|---|
| 識別 | `flags`（`kAeFrameCameraValid`：偽なら描く物が無い）、`sceneId`（読み込まれたシーンの集合が変わると変わる）、`cameraId`、`frameIndex` |
| カメラ | `width`、`height`（カメラの出力先の画素数）、`view`、`proj`、`cameraPosition`、`cameraForward`、`nearPlane`、`farPlane`、`deltaTime`、`time` |
| 環境光 | `ambientIntensity`、`ambientSH`（Unity の 7 つの SH ベクトル `unity_SHAr` .. `unity_SHC`） |
| 空 | `skyTexture`（ホストの HDRI のキューブマップ、通常のミップチェーン。0 = なし、環境光プローブが空になる）、`skyIntensity`（空のテクスチャーへの乗数。0 は 1 とみなす）、`skyRotation`（上軸まわりのラジアン） |
| 露出 | `exposureMode`（固定 / 自動）、`exposureFixedEV`、`exposureMinEV`、`exposureMaxEV`、`exposureCompensation`。自動のときは Aether がこの範囲で自分で測光する。`exposure` は露出モードが不明なときの乗数（マネージド側は 1）。`exposureTexture` は現在常に 0 で、ネイティブ側も使わない |
| トーンマップ | `tonemapMode`（`AeTonemap`）、`tonemapCurve`（Custom の 6 つの値：toe strength、toe length、shoulder strength、shoulder length、shoulder angle、gamma）、`postExposure`（露出の後に足す段数） |
| フォグ | `fogDensity`（平均自由行程の逆数、0 = なし）、`fogColor`、`fogBaseHeight`、`fogMaxHeight`（この間で薄くなる）、`fogMaxDistance`（0 = 制限なし）、`fogTint`、`fogFlags`（`kAeFogColorFromSky`） |
| 影 | `shadowDistance`（0 = 不明。Aether の既定値を使う）、`shadowCascades`（1〜4）、`shadowSplits`（カスケード 0〜2 の終わり。`shadowDistance` に対する割合） |
| AO | `aoIntensity`（0 = なし）、`aoRadius`（m）、`aoDirectStrength`（直接光も暗くする割合） |
| グレーディング | `smhShadows`、`smhMidtones`、`smhHighlights`（rgb の乗数。1 = そのまま）、`smhLimits`（シャドウの始まりと終わり、ハイライトの始まりと終わり。すべて 0 = なし） |
| ポストプロセス | `bloomIntensity`（ホストの 0〜1 の値）、`bloomScatter`、`motionBlurIntensity`、`antialiasing`（`AeAntialiasing`。参考情報。0 = オフ） |
| SSR | `ssrEnabled`（0 = オフ）、`ssrMinSmoothness`、`ssrSmoothnessFadeStart`、`ssrScreenFadeDistance`（画面の端で薄れる幅、0〜1） |

ホストの値は「ゲームがどう設定しているか」を伝えるもの。Aether がそれをどう使うかは [rendering-features.md](rendering-features.md)。

### インスタンスごとの値（`AeFrameInstance`）

ハンドル、フラグ、`localToWorld`。生きているインスタンスすべてについて毎フレーム送る。

- `kAeFrameInstanceVisible`：このフレームに存在する。視界、影のカスケード、反射に対するカリングはネイティブ側が行う。
- `kAeFrameInstanceShadowOnly`：視界には描かず、影だけ落とす（カメラが映さないレイヤーの物、影専用のレンダラー）。
- フレームに含まれないインスタンスは、そのフレームでは描かない。

### ライト（`AeLight`）

種類（平行、点、スポット）、フラグ（`kAeLightCastsShadows`、`kAeLightHeadlamp`：車のヘッドライト）、位置、向き（光の進む方向、正規化）、範囲、スポットの内側・外側の角度の余弦、色、`layerMask`。

- ライトは `(light.layerMask & instance.renderingLayerMask) != 0` のインスタンスだけを照らす。ホストがライトレイヤーを使わないときは `0xFFFFFFFF`。

### パーティクル（`AeParticle`、`AeParticleBatch`）

- `AeParticle` はカメラを向く四角形 1 枚：中心（ワールド）、大きさ（m）、色（線形 rgb、a = 不透明度。光を受けないバッチでは画面の明るさ、光を受けるバッチでは反射率）、回転（視線まわりのラジアン）、引き伸ばし（中心から四角形の先までのワールドのベクトル、すべて 0 = 正方形）、テクスチャーの範囲（オフセットと大きさ）。
- `AeParticleBatch` は同じテクスチャーと合成方法を持つ並び：テクスチャー（0 か未登録 = テクスチャーなし）、フラグ、最初の粒子の番号、数。
  - `kAeParticleAdditive`：絵に足す（炎、火花）。
  - `kAeParticleLit`：太陽（影つき）と空で照らす（煙、砂ぼこり）。
  - `kAeParticleDisc`：テクスチャーなしの丸い煙。
- 毎フレーム、そのフレームの `Aether_SceneSubmitFrame` の前に呼ぶ。粒子が無いときも 0 個で呼ぶ。前のフレームの粒子は置き換わる。
- 加算でないバッチは、ネイティブ側が奥から手前へ並べ替える。
- バッチの範囲が粒子の配列を越える呼び出しは拒否する。`AeParticleBatch` 自体の大きさは確かめない。

## レイトレース用の形状

メッシュの形状（位置、uv、インデックス）は、呼び出し側が GPU から読み戻して CPU のメモリで渡す。ネイティブ側はそれから BVH を作る（[rendering-features.md](rendering-features.md) の「反射」）。

| 関数 | 内容 |
|---|---|
| `Aether_SceneMeshVertices(mesh, data, bytes, stride, positionOffset, positionFormat, uvMode, uvOffset, uvFormat)` | 頂点。`positionFormat` は位置の DXGI 形式。`uvMode`：0 = uv なし、1 = 同じストリームにある（`uvOffset`、`uvFormat`）、2 = 別のストリームにあり `Aether_SceneMeshUvs` で送る（木はそれを待つ） |
| `Aether_SceneMeshUvs(mesh, data, bytes, stride, uvOffset, uvFormat)` | 別のストリームの uv |
| `Aether_SceneMeshIndices(mesh, data, bytes, indexStride, subMeshes, subMeshCount)` | インデックスとサブメッシュ |
| `Aether_SceneMeshChanging(mesh)` | 頂点が変わるメッシュ（スキンメッシュ）。形状を送る前に呼ぶ。その後に送った頂点は木の当てはめ直しになる |
| `Aether_SceneMeshAlias(alias, mesh)` | `alias` のインスタンスを `mesh` の木でたどる（スキンメッシュの交互に使われるバッファーが 1 つの木を共有する） |
| `Aether_SceneMeshForget(mesh)` | シーンのメッシュでないハンドル（上の木のハンドル）の木を消す |
| `Aether_SceneBvhReset()` | すべての木を消す。現在のマネージド側は呼ばない |

- 頂点・uv・インデックスの順は問わない。そろった時に木を作る。
- ハンドルはシーンのメッシュと同じ空間。インスタンスはそのメッシュハンドル（または別名）で木を引く。木がまだ GPU に無いインスタンスは、レイには現れない。

## その他の関数

- `Aether_SetSceneNight(night)`：ゲームの昼夜の切り替えの状態（1 = 夜、0 = 昼か不明）。
- `Aether_SmokeStep(...)`：タイヤスモークの 1 ステップをネイティブ側で計算し、結果を `AeParticle` として返す。契約のオブジェクトではない。
- `Aether_GetRenderMode()`：0 `hdrp`、1 `aether`、2 `compare`、3 `standalone`（`aether` と同じ）。
- `Aether_DevCaptureFlags()`、`Aether_SettingNumber(key, fallback)`：キャプチャの実験用スイッチと、Aether の設定値の読み出し。

## 約束事

- ワールド空間は Unity と同じ：左手系、+Y が上、単位はメートル。
- 行列は Unity の `Matrix4x4` のメモリ配置：16 個の float、列優先（`m[col*4+row]`）、列ベクトル（HLSL で `mul(M, v)`）。
- `view` = `Camera.worldToCameraMatrix`（右手系のビュー空間、カメラは −Z を向く）。
- `proj` = `GL.GetGPUProjectionMatrix(camera.nonJitteredProjectionMatrix, false)`：D3D のクリップ空間、y が上、逆 Z（ニア = 1、ファー = 0）。深度テストは `GREATER_EQUAL`、深度は 0 でクリアする。TAA のずらしは含まない。
- 表面は画面上で時計回り。行列の行列式が負のインスタンスは、カリングと表裏が反転する。
- 色は線形。ライトの色は HDRP の単位の強さを掛けたもの（平行光源は lux、点・スポットは candela）。
- テクスチャー座標は Unity のもの（元画像の下が v = 0）。テクスチャーは Unity 自身の GPU リソースなので、メッシュの uv で反転なしにサンプリングできる。

これらはホストテストの検証シーン（`tests/host/ValidationScene.cpp`）が画素の値で確かめる：位置、深度の順、カリング、鏡像のインスタンス、影、ブレンド。

## スレッドと寿命

- **メインスレッド**が関数を呼ぶ。関数は確かめ、参照を取り、保留の一覧に足す（ミューテックス）だけ。フレーム、パーティクル、プローブは最新のものが保留を置き換える。
- **描画スレッド**（`Renderer::SceneFrame`、プラグインイベント 4 = `AETHER_EVENT_SCENE`）が保留の一覧を取り込んで順に適用し、それから描く。変更が効くのは次のイベントから。どちらも相手を待たない。同じフレームを 2 回は描かない。
- **消す・置き換える**：古いリソースの参照とデスクリプターは、記録中のリストの番号とともに廃棄キューに入り、GPU がその番号を終えてから解放される（[dx12-integration.md](dx12-integration.md)）。描画中のリストが読むかもしれないマテリアルのデスクリプター表は書き換えず、新しい表を作る。
- **シーンの破棄**：マネージド側は次の走査で消えた物を消す。
- **描画モード `hdrp`**：キャプチャが止まり、`Aether_SceneClear` でストアが空になり、Unity の参照はすべて廃棄される。
- **終了**：GPU が止まったと確認できてからすべて解放する。確認できないときは、GPU が使用中のものを解放しないよう、参照をわざと手放さない。
- レイトレースの木は、メインスレッドの呼び出しを受けてワーカースレッドで作られ、描画スレッドが GPU に上げる。

## 拡張するとき

- 新しいフィールドは、まず `reserved` の場所に入れる（`AeMeshDesc`、`AeMaterialDesc`、`AeInstanceDesc`、`AeLight` にある）。
- 配置を変えるときは、`SceneContract.h` と `Contract.cs` の両方を直し、`AE_SCENE_CONTRACT_VERSION` と `AeConst.ContractVersion` を上げる。古い組み合わせは大きさと版の確認で止まる。
- 関数を変えるときは `AETHER_API_VERSION` と `Native.ApiVersion` を一緒に上げる。
- 新しい構造体を `Verify` で確かめるときは、`Aether_SceneStructSize` の番号と `Verify` の一覧の両方に足す。
