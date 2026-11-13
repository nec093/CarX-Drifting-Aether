# マテリアルの変換

`src/Aether.Managed/SceneCapture/MaterialTranslator.cs` は、Unity のマテリアルを Aether 自身のマテリアル `AeMaterialDesc`（[scene-data-contract.md](scene-data-contract.md)）に変える。HDRP のマテリアルの評価、キーワード、パスは使わない。シェーダーが実際に持つプロパティを読み、Aether の前方シェーダー（`shaders/scene_forward.hlsl`）が扱う少数のクラスに当てはめる。

ここで扱うのは「何を読み、どこに入れるか」まで。各クラスの陰影（塗装、クローム、ランプなど）は [rendering-features.md](rendering-features.md) にある。

## クラスの判定

シェーダーが持つプロパティの組で決める。上から順に、最初に当てはまったもの。シェーダー名を使うのはブロブシャドウだけ。

| クラス（`AeMaterialClass` / シェーダーの定数） | 条件 | ガレージで該当したシェーダー |
|---|---|---|
| `skip`（13） | `_MotionVectorAngular` がある | HDRP/Custom/PhysicsMotionVectors（ホイールのモーションベクター用の形状） |
| `vehiclePaint`（2、`CLASS_VEHICLE_PAINT`） | `_Roughness`、`_Metalness`、`_Clearcoat`（以下「塗装の系統」）と `_Spectrum` | CarPaint |
| `vehicleStitches`（6、`CLASS_VEHICLE_STITCHES`） | 塗装の系統で、`Texture2D_67B10B5E`（継ぎ目のパックテクスチャー）がある | CarPaintStitches |
| `vehicleRim`（5、`CLASS_VEHICLE_RIM`） | 塗装の系統で、上の 2 つでない | CarPaintRim |
| `vehicleGlass`（4、`CLASS_VEHICLE_GLASS`） | `_AlphaClippingThreshold` と `_Spectrum` | CarGlass、CarGlassNoVinyls |
| `vehicleTire`（12、`CLASS_VEHICLE_TIRE`） | `_TireBaseMap` | CarTireCustomization |
| `vehicleLights`（11、`CLASS_VEHICLE_LIGHTS`） | `_BaseMap`、`_PackMap`、`_EmissionMasks` | CarLights |
| `vehicleGeneric`（3、`CLASS_VEHICLE_GENERIC`） | `_BaseMap`、`_PackMap` | CarGeneric、CarGenericAlpha |
| `standard`（1、`CLASS_STANDARD`） | `_BASE_COLOR_MAP` と `_METALNESS` | HDRP の 3ds Max 物理マテリアル |
| `standard` / `transparent`（9、`CLASS_TRANSPARENT`） | `_BaseColorMap`、`_Metallic`、`_Smoothness` | HDRP/Lit |
| `colorMask`（7、`CLASS_COLOR_MASK`） | `_BaseColorMap`、`_MaskMap`、`_BaseColor` | Colormask |
| `vegetation`（8、`CLASS_VEGETATION`） | `_BaseColorMap`、`_DiffusionProfileHash` | CarXFoliageSimple |
| `blobShadow`（14、`CLASS_BLOB_SHADOW`） | シェーダー名に `Blobshadow` を含む | CarBlobshadow |
| `skip` | `_BaseColorMap` があり、透明で、`_MainTex` が無い（中身の分からない無照明の透明グラフ） | |
| `unlit`（10、`CLASS_UNLIT`） | `_MainTex` | Anzu の動画パネル、Mobile/Particles/Additive |
| `unsupported`（0、`CLASS_UNSUPPORTED`） | どれにも当てはまらない | Hidden/InternalErrorShader |

右の列は、ガレージでの調査（`get_material_info detail=survey`）で見つかったシェーダー。判定はシェーダー名に依らない。

- `skip` のサブメッシュは描かず、影にもレイにも入れない。
- `blobShadow` は車の下の箱で、ネイティブ側はデカールとして描く（後ろの物を暗くする）。影を落とさず、レイにも入らない。
- `unsupported` は診断用の市松模様（マゼンタと黒）で描き、数える。消えずに見えるようにするため。レイや反射プローブには入らない。
- マテリアルの無いサブメッシュも同じ診断用の模様になる。
- `vegetation` と `transparent` は、前方シェーダーでは特別な扱いが無い（`standard` と同じ式で照らされる）。

## 共通のフラグ

`skip` と `unsupported` 以外に付く。

| フラグ | 条件 |
|---|---|
| `kAeMaterialBlend` | 透明：`_SurfaceType` > 0.5、またはレンダーキュー ≥ 3000。ガラスとブロブシャドウは常に透明。透明な `standard` は `transparent` クラスになる |
| `kAeMaterialAlphaTest` | `_AlphaCutoffEnable` > 0.5。しきい値は `alphaCutoff`（既定 0.5） |
| `kAeMaterialDoubleSided` | `_DoubleSidedEnable` > 0.5。ガラスは常に |
| `kAeMaterialAdditive` | `unlit` で、透明で、`_ShouldFlipX` が無い |
| `kAeMaterialClearCoat` | 塗装の系統で、`_ClearCoatEnabled` > 0.5 かつ `_Clearcoat` > 0 |
| `kAeMaterialNoShadow` | ブロブシャドウ |

透明なマテリアルは影を落とさない。

## テクスチャースロットとチャンネルの配置

スロットは base、normal、mask、emissive、extra の 5 つ。無いスロットはネイティブ側で、白（normal は平らな法線）になる。mask スロットの共通の処理（金属度・粗さ・遮蔽）は、マスクの配置が `none` 以外のときだけ行う。

| クラス | base | normal | mask（配置） | emissive | extra |
|---|---|---|---|---|---|
| `vehiclePaint` | `_BaseMap`（リバリー） | `_NormalMap` | | | |
| `vehicleStitches` | `_BaseMap` | `_NormalMap` | 継ぎ目のパック（配置 `none`。シェーダーが直接読む。B が継ぎ目の不透明度） | | |
| `vehicleRim` | `_BaseMap` | `_NormalMap` | | | |
| `vehicleGlass` | `_BaseMap` | | | | |
| `vehicleTire` | `_TireBaseMap` | `_TireNormalMap` | `_TirePackMap`（`rmo`） | | |
| `vehicleGeneric` | `_BaseMap` | `_NormalMap` | `_PackMap`（`rmo`） | `_EmissionMap` | `_ColormaskMap` |
| `vehicleLights` | `_BaseMap` | `_NormalMap` | `_PackMap`（`rmo`） | `_EmissionMasks` | |
| `standard`（3ds Max） | `_BASE_COLOR_MAP` | | | | |
| `standard`（Lit） | `_BaseColorMap` | `_NormalMap` | `_MaskMap`（`hdrp`） | `_EmissiveColorMap` | |
| `colorMask` | `_BaseColorMap` | `_NormalMap` | `_MaskMap`（`hdrp`） | | |
| `vegetation`、`blobShadow` | `_BaseColorMap` | | | | |
| `unlit` | `_MainTex` | | | | |

マスクの配置（`AeMaskLayout`）：

- `hdrp`（HDRP の MaskMap）：R 金属度、G 遮蔽、A 滑らかさ。`colorMask` では B が `_BaseColor` で色を付ける範囲。
- `rmo`（ゲームの「パック」テクスチャー）：R 粗さ、G 金属度、B 遮蔽。チャンネルの意味は名前とテクスチャーの見た目から決めたもので、ゲームのシェーダーグラフで確かめたものではない。
- マスクがあるとき、`metallic` はマスクの金属度に掛ける係数になる。`roughness` は、`rmo` では粗さに掛ける係数、`hdrp` では滑らかさの上限（1 − `roughness`、これにマスクの A を掛ける）になる。`rmo` のクラスはどちらも 1。`colorMask` は金属度 1、粗さ 0（滑らかさはマスクの A そのまま）。
- `occlusion` は遮蔽のチャンネルの強さ（常に 1）。

法線マップは RG（BC5）、AG（DXT5nm）、RGB のどれでも読めるように復号し、Z を求める。

UV の拡大とずらし（`uvScale`、`uvOffset`）は、クラスの主テクスチャー（上の表の base の列のプロパティ）の `GetTextureScale / GetTextureOffset`。全スロットに共通。ガラスとブロブシャドウは拡大 1、ずらし 0。

## クラスごとの値

| クラス | 値 |
|---|---|
| 塗装の系統 | `baseColor` ← `_BaseColor`、`secondaryColor` ← `_SecondaryColor`（見る角度で変わる 2 色目）、`metallic` ← `_Metalness`、`roughness` ← `_Roughness`。クリアコートのフラグが付くとき `clearCoat` ← `_Clearcoat`（0〜1 に制限）、`clearCoatRoughness` = 0.04 |
| `vehiclePaint` | `baseColor` のアルファを「リバリーのテクスチャーが base にある」の意味に使う（1 = ある、0 = ない）。ゲームはこのアルファを使っていない |
| `vehicleGlass` | `baseColor` ← `_BaseColor`、`roughness` = 0.03、`alphaCutoff` ← `_AlphaClippingThreshold`（アルファテストのフラグがあるときだけ効く） |
| `vehicleTire` | `baseColor` は白、`metallic` = `roughness` = 1（パックの係数） |
| `vehicleGeneric` | `metallic` = `roughness` = 1。`secondaryColor` ← `_SecondaryColor`（あれば）。カラーマスクの R が `baseColor`、G が `secondaryColor` で色を付け、量はそれぞれの色のアルファ。`_EmissionMap` があり `_EmissionIntensity` > 0 なら `emissive` = その強さ |
| `vehicleLights` | `metallic` = `roughness` = 1。`_EmissionMasks` は 1 チャンネル 1 系統のランプのマスク。`_EmissionIntensity`（ベクトル）の 4 つの強さを `secondaryColor` に入れる。マスクがあれば `emissive` = 1。発光の色は base テクスチャーの色 |
| `standard`（3ds Max） | `baseColor` ← `_BASE_COLOR`、`roughness` ← `max(_REFLECTIONS_ROUGHNESS, 0.05)`、`metallic` ← `_METALNESS` |
| `standard`（Lit） | `baseColor` ← `_BaseColor`、`normalScale` ← `_NormalScale`。マスクありなら `metallic` ← `_MetallicRemapMax`、`roughness` ← 1 − `_SmoothnessRemapMax`。マスクなしなら `metallic` ← `_Metallic`、`roughness` ← 1 − `_Smoothness`。`emissive` ← `_EmissiveColor`（HDR の色をそのまま）。`alphaCutoff` ← `_AlphaCutoff` |
| `colorMask` | `baseColor` ← `_BaseColor` |
| `vegetation` | `roughness` = 0.8 |
| その他の既定値 | `baseColor` 白、`metallic` 0、`roughness` 0.6、`normalScale` 1、`alphaCutoff` 0.5 |

`shaderId`（シェーダーのインスタンス id）と `renderQueue` は診断用に入れる。

## MaterialPropertyBlock

ゲームは、プレイヤーが変えるもの（塗装の色、粗さ、金属度、クリアコート、リバリーのテクスチャー、ランプの点灯）をレンダラーごとの `MaterialPropertyBlock` で渡す。ランプはマテリアルスロットごとのブロック。共有マテリアルの値は既定値でしかない。

- 変換では、テクスチャー、数値、色、ベクトルのどれも、ブロックに値があればそれを使い、無ければマテリアルの値を使う。ブロックの色は線形として読む。マテリアルの色は線形に直して読む。
- ブロックに `_BaseColor` があれば、どのクラスでも `baseColor` はその値になる。`vehicleTire`、`vehicleGeneric`、`vehicleLights`、`vegetation`、`unlit`、`blobShadow` は、マテリアル自身の `_BaseColor` は読まない。
- ブロックを持つスロットは専用のネイティブマテリアルになる（ハンドルの作り方と、どのブロックが優先されるかは [scene-capture.md](scene-capture.md)）。

## 更新

- ブロックの値の変化：毎フレーム、`_BaseColor`、`_SecondaryColor`、`_EmissionIntensity`、`_Roughness`、`_Metalness`、`_Clearcoat`、`_BaseMap` を比べ、違えば変換し直す。それ以外の値は、レンダラーの見直し（約 30 フレームで一巡）で変換し直す。
- 共有マテリアルの書き換え：2 秒ごとの `ComputeCRC()` で見つけ、変換し直す。
- 変換の結果は、前に送ったものとバイト単位で違うときだけ送る。
- テクスチャーはハンドルで共有する。GPU リソースが替わったテクスチャー（ストリーミング）は、ネイティブ側でそのテクスチャーを使うマテリアルのデスクリプター表だけを作り直す。

## 診断

- `get_material_info detail=aether`：ネイティブ側のマテリアルごとのクラス、フラグ、数値、マスクの配置、束縛したテクスチャー（スロット、形式、大きさ、バインドできたか）、変換が使わなかったテクスチャーの数（`unsupportedProperties`）と、クラス別の集計。
- `get_material_info detail=survey`：シェーダーのプロパティの配置、レンダラーごとのプロパティブロックなど、変換を考えるための資料。
- デバッグ表示（`debugView`）：`albedo`、`normal`、`roughness`、`metallic`、`materialClass`。

## 制限

- `_Clearcoat` が塗装グラフの中で何を意味するかは分からない。クリアコートの強さとして読んでいる。
- `_Spectrum` はクラスの判定にしか使わない。
- 読まないもの：タイヤのカスタマイズの層、HDRP のディテールマップ、HDRP のリマップの最小値（`_MetallicRemapMin`、`_SmoothnessRemapMin`、`_AORemapMin`）、遮蔽の強さのプロパティ、発光の強さの単位（`_EmissiveColor` はそのまま）。
- 植物に透過と風は無い。
- `rmo` のチャンネルの意味は推定。
- 判定の条件はガレージのシェーダーから作った。コースのシェーダーは調べていない。
