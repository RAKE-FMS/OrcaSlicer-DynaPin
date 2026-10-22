# DynaPin 設定ファイル仕様

DynaPinはプリンタのY軸側面に取り付けたピンアレイを使って、印刷途中にピンを引き出す機能です。  
本ドキュメントではプリンタごとに用意するJSON設定ファイルのスキーマと、各設定値がどう使われるかを説明します。

`kp3s.json` はOrcaSlicerとPython検証コードで共用します。`normal`・`alternating`・`reverted`を動作モードごとに分け、各モードのX座標・速度・移動量を独立して設定します。alternatingでは`alternating.normal`と`alternating.reverted`をrowごとに切り替えます。旧形式のJSONは読み込みエラーになります。

## Pythonで引き出し動作を検証する

リポジトリのルートで実行します。Python標準ライブラリだけを使用します。

```bash
# JSONのtypeに従う（現在はalternating）
python3 scripts/generate_dynapin_pull_gcode.py --pins "0,0; 1,0" --output /tmp/alternating.gcode

# 移植前C++のピン単体動作を比較用に再現する
python3 scripts/generate_dynapin_pull_gcode.py --pins "0,0; 1,0" --current --output /tmp/current.gcode

# typeを一時的に上書きし、接近Yと係合Yを入れ替える
python3 scripts/generate_dynapin_pull_gcode.py --pins "0,0; 1,0" --type reverted --output /tmp/reverted.gcode
```

`--config` でJSONパスを指定できます。省略時は、このREADMEと同じディレクトリの `kp3s.json` を読みます。
まずリポジトリ内の `old-models/min/min.gcode` を読み込み、内容を変更せず出力の先頭にコピーして、その後に引き出しG-codeを追記します。`--base-file /path/to/min.gcode` で別の読み込み元を指定できます。元ファイルは上書きしません。
元の改行・バイト列を保持し、末尾に改行がない場合だけ追記との境界に改行を1つ加えます。読み込み元が存在しない場合はエラーにして、出力ファイルを変更しません。
`--output` 省略時は標準出力へ出力します。ピンは0始まりで、指定したピンだけを高さ・列順に並べ、重複を取り除きます。
`--type normal` は全段にnormalのYオフセットを適用します。`--current` は`normal`のX座標・速度・動作値を使い、向き別オフセットを適用せず、旧 `z_offset` に相当する値として `normal.retreat_z_lift` を使います。

出力は `min.gcode` の内容に続いて `G21`（mm）・`G90`（絶対座標）と引き出し動作を含みます。ホーミング・加熱・押出などの初期化は、読み込んだ `min.gcode` の内容に従います。Pythonはプリンタへの送信やスライサのレイヤー判定・Z-hopは行いません。
`--return-position X Y Z` を指定した場合だけ、全ピン処理後に一度、XY→Zの順で指定座標へ戻ります。復帰速度は選択モードの設定値を使います（alternatingでは`alternating.normal`の速度）。

```bash
python3 -m unittest discover -s tests/scripts -p test_generate_dynapin_pull_gcode.py -v
```

---

## 動作の概要

```
Z軸 (印刷高さ)
  ↑
  │  row=2  ○      ○      ○
  │  row=1  ○      ○      ○
  │  row=0  ○      ○      ○
  │         col=0  col=1  col=2 ...
  └──────────────────────────────────→ Y軸 (プリンタ手前 ←→ 奥)
```

- **行 (row)** … Z軸（高さ）方向に並ぶピン。`row_pitch_z` ごとに配置。
- **列 (col)** … Y軸方向に並ぶピン。`col_pitch_y` ごとに配置。

指定したレイヤー（ピンのZ高さ）に達したとき、ノズルがX軸を移動してピンを引き出す。

### 現行比較モードのピン引き出し動作（`--current`）

```
x_hook  x_latch             x_front
  │       │                    │
  ↓       ↓                    ↓
─────── ───────         ─────────────── (X軸)

1. G1 Y(pull_y + approach_y_offset)                 ← ピンをよけてY接近（早送り）
2. G1 X(x_hook) Z(pull_z)                           ← XZ同時移動でフック高さへ（早送り）
3. G1 X(x_latch)                                    ← ラッチをかける（低速）
4. G1 Y(pull_y)                                      ← Yをピンに当てる（低速）
── ; DYNAPIN_PULL_MOVE ──
5. G1 X(x_front)                                    ← 前面まで引き出す（高速）
6. G1 X(x_front + disengage_x_offset) Y(pull_y + approach_y_offset)
                                                    ← XY同時移動でピンを外す（低速）
7. G1 Z(pull_z + retreat_z_lift)                     ← Z退避（低速）
```

`reverted` は上記の `pull_y + approach_y_offset` と `pull_y` を入れ替えます。Y位置を反転して新しい位置を作るのではなく、接近位置と係合位置を交換します。Z動作は現行比較モードと同じです。

`alternating` は `row % 2 == alternating.row_parity` の段をreverted、それ以外をnormalにします。両者とも次の6動作で、現在の設定ではnormalの開始Yがrevertedより8.8mm大きくなります。

```text
1. Y = pull_y + approach_y_offset
2. X = x_hook, Z = pull_z + approach_z_offset       （XZ同時移動）
3. X = x_latch
4. Z = 2のZ - engage_z_drop                        （Zで係合）
5. X = x_front                                    （引き出し）
6. X = x_front + disengage_x_offset,
   Z = 4のZ + disengage_z_lift                     （XZ同時移動で離脱）
```

alternatingでは手順1のYを最後まで維持し、離脱後に `retreat_z_lift` を追加しません。
現在のrow=0,col=0（reverted）はY=3.7、Z=11→6→11。row=1,col=0（normal）はY=12.5、Z=18.4→13.4→18.4です。

### サポート材の除外領域

ピンが引き出される経路にサポート材があると動作を妨げるため、スライサが自動でサポートをブロックします。

```
Z
↑
│  z_max = support_origin.z + row × row_pitch_z
│  ┌──────────────────────┐
│  │   サポート除外領域    │
│  │                      │
│  │  x_front ─────────────────── bed_max_x
│  │                      │
│  └──────────────────────┘
│  z_min = z_max - blocker_height_z
│
└──────────────────────────── X
   x_front          bed_max_x
```

- **X範囲**: `x_front` ～ `bed_max_x`（引き出し経路およびサポートブロック全域）
  - `bed_max_x` はプリンタの `printable_area` に記載された最大X座標から自動取得します。KP3Sでは `x_front=20`、`bed_max_x=180` なので `20..180` です。
- **Y範囲**: `blocker_y ± blocker_width_y/2`（ブロッカーのY方向幅）
- **Z範囲**: `z_max - blocker_height_z`（ピン下面）から `z_max`（ピン上面）まで。
  - `blocker_height_z` が設定されていない、または0の場合は、Z方向の下側延長はありません。

### 仮想サポート面（着地面）の生成

ピンの上面は「仮想ビルドプレート」として機能します。`landing_slope`を省略した場合は`z_max`の水平面を使います。傾斜を設定した場合はX方向の水平な帯ごとに上面を近似します。ピンの上にあるモデルのオーバーハングから生成されたサポート材は、ベッド（Z=0）まで降りる代わりにこの上面で「着地」します。

- **仮想サポート面のXY範囲**: サポートブロックと同じ `x_front..bed_max_x`、および `blocker_y ± blocker_width_y/2` です。
- **引き出し経路の真上**: 引き出し経路全体を仮想支持面として扱い、上のサポート材がピン上面で着地できるようにします。

### ピン上の定着用サポート初層（2026-10-22）

最初の実サポート層の下端をピン上面に合わせ、その1層だけを通常サポートの初層として造形します。2層目以降は通常のサポート生成・冷却へ戻します。ピン専用インターフェースは生成せず、`support_interface_bottom_layers`との連動もありません。造形物との接触に必要な通常インターフェースは維持します。

| 条件 | 既存の設定・動作 |
| --- | --- |
| 密度・拡張 | `raft_first_layer_density`・`raft_first_layer_expansion`。オブジェクトでの上書きも反映 |
| 線幅 | 既存の初層線幅を解決する`support_material_1st_layer_flow` |
| 層厚・押出量 | ピン上面からの実層厚を使用し、ベッド初層の層厚には置換しない |
| 速度・材料 | `initial_layer_speed`・`support_filament`。種別は通常サポート |
| 経路 | 機械X方向への一方向走査。通常初層の外周経路は追加しない |
| 冷却 | 対象経路だけ造形物冷却ファン0%。退出後は通常制御。先行起動・キックスタートも停止区間を保護 |

赤いボックスはサポート除外領域であり、定着用初層の外縁余白を設定しても縮小しません。実際に材料を置ける範囲は、下記の`landing_edge_inset_y`と押出線幅で別に制限します。

インターフェース方式から変更した理由、Z整合、連続ピンの扱いは[現行仕様と変更理由](../../../../docs/DYNAPIN_PIN_BASE.md)、測定値とテスト結果は[検証記録](../../../../docs/DYNAPIN_PIN_BASE_VALIDATION.md)に記載しています。

---

## JSONスキーマ

### `grid` セクション（必須）

ピンアレイの物理配置を定義します。

```json
"grid": {
  "pull_origin": {
    "y": 14.0,
    "z": 5.0
  },
  "support_origin": {
    "y": 18.0,
    "z": 7.55
  },
  "row_count": 10,
  "col_count": 14,
  "pitch": {
    "row_z": 7.4,
    "col_y": 12.4
  }
}
```

| フィールド | 型 | 必須 | 説明 |
|---|---|---|---|
| `pull_origin.y` | number | ○ | 引き抜きG-codeのY基準座標 [mm] |
| `pull_origin.z` | number | ○ | 引き抜きG-codeのZ基準座標 [mm] |
| `support_origin.y` | number | ○ | 実ピン配列・サポート形状のY基準座標 [mm] |
| `support_origin.z` | number | ○ | row=0 のサポートブロッカー上端のZ座標 [mm] |
| `row_count` | int | ○ | `row=0` から存在するピン行数（Z方向） |
| `col_count` | int | ○ | `col=0` から存在するピン列数（Y方向） |
| `pitch.row_z` | number | ○ | 行間ピッチ（Z方向）[mm] |
| `pitch.col_y` | number | ○ | 列間ピッチ（Y方向）[mm] |

ピン `(row, col)` の座標は、論理的な行・列ともに0始まりです。

引き抜きG-codeの座標：
```
pull_y = pull_origin.y + col × pitch.col_y + 選ばれた向きのy_offset
pull_z = pull_origin.z + row × pitch.row_z
```

`--current` 以外では `pull_y` に、選ばれた向きの `y_origin_offset` を加えます。`alternating` の場合だけ、さらに選ばれた向きの `alternating_y_offset`（現在は両方-1mm）を加えます。単一向きモードと現行比較には適用しません。`support_origin` はこれらの動作オフセットとは独立です。

サポートブロッカー上端の座標：
```
blocker_y   = support_origin.y + col × pitch.col_y − 7.2
blocker_z_max = support_origin.z + row × pitch.row_z
blocker_z_min = blocker_z_max - blocker_height_z
```

現行実装では支持領域のY中心に固定補正`support_block_y_offset = −7.2 mm`を加えています。この補正は引き出しモードのY補正とは独立です。

`dynapin_selected_pins` が空の場合は、この行数・列数で列挙したピンを自動選択候補として使用します。`row` はZ方向の高さ、`col` はY方向の位置を表します。

---

### `support_exclusion` セクション（必須）

サポート材を除外する領域を定義します。

```json
"support_exclusion": {
  "blocker_width_y": 12.4,
  "blocker_height_z": 5,
  "landing_edge_inset_y": 1.2
}
```

| フィールド | 型 | 必須 | 説明 |
|---|---|---|---|
| `blocker_width_y` | number | ○ | サポートブロッカーのY方向幅 [mm] |
| `blocker_height_z` | number | – | サポートブロッカーの上端から下方向への高さ [mm]（省略時 0） |
| `landing_edge_inset_y` | number | – | 定着用初層の造形範囲をY方向の外縁から縮める片側の余白 [mm]（省略時 0） |

`kp3s.json`の`landing_edge_inset_y`は1.2 mmです。同じ高さで連続して選択・使用するピンはつながった面として扱い、ピン間の共有境界には余白を設けません。単独ピンや連続領域の両端には、それぞれ1.2 mmの余白を設けます。未使用ピンや高さの異なるピンとの間は共有境界として扱いません。初層の拡張はこの範囲内に制限し、押出経路はさらに線幅の半分だけ内側に配置します。

KP3Sのピッチ12.4 mmに対して両端を1.2 mmずつ縮めるため、単独ピンの造形可能幅は10.0 mmです。実幅10.4 mmに対して片側0.2 mmの余裕を残します。連続するN本の造形可能幅は`12.4 × N − 2.4` mmになります。この余白は定着用初層の範囲に適用します。

サポート除外領域・仮想支持面・プレビューのX範囲は、選択中の動作モードの `x_front` とプリンタの `printable_area` から自動計算されます。alternatingの場合はnested normal/revertedの小さい方の`x_front`を使い、両方の引き出し経路を含めます。引き出し用のY・Z補正はサポート座標に適用しません。

---

### `support_speed` セクション（任意）

ピン上の接触面を造形した次のサポート層から、大きなX方向の押出し移動を減速します。

```json
"support_speed": {
  "layers": 3,
  "min_x_distance": 5,
  "max_x_speed": 0
}
```

| フィールド | 省略時 | 説明 |
|---|---|---|
| `layers` | 3 | 接触面の次から数える実サポート層数。0で減速を無効化 |
| `min_x_distance` | 5 | 1本の直線押出し区間で減速対象となるX方向の最小移動量 [mm] |
| `max_x_speed` | 0 | X方向の速度成分の上限 [mm/s]。0はプロセス設定の`initial_layer_speed`を使用 |

各値は非負で、`layers`は整数です。各ピンの実際の定着用初層を基準に、直上につながるサポート層を数えます。同じZの層を重複して数えず、層間が途切れたサポートには引き継ぎません。対象領域は接触面のXY範囲を直上へ投影した範囲です。同じ高さに異なる層厚の経路が混在する場合も、最終的な各押出し経路の層厚・範囲から接続を判定します。層全体の代表層厚やサポート島だけには依存せず、接触面より下の経路は対象層数に含めません。接触面自体の既存の初層条件は維持します。

通常サポート・インターフェースの押出しで、その領域に重なる直線区間だけに適用します。斜め移動では`経路速度 × |ΔX| / 経路長`を上限以下に抑えます。Y方向だけの区間、しきい値未満の区間、空走、モデルの押出し、ピン引き出し動作は変更しません。既に遅い押出しを加速することはありません。減速対象の区間を含むサポート経路は元の直線区間からG1で出力し、円弧フィッティングによる制限の迂回を防ぎます。

対象経路では、流量平滑化（`max_volumetric_extrusion_rate_slope`）によって短いY方向の区間まで連動して減速されないように、方向別の速度指定を優先します。通常の冷却による減速は維持します。

JSON変更後はG-codeを再生成してください。DynaPin無効時には適用しません。

---

### `pull_gcode` セクション（必須）

OrcaSlicerとPythonで共通のピン引き出し動作を定義します。次の形で、単独normal・alternating・単独revertedの設定を分離します。各プロファイルはX座標・速度・移動量を個別に持つため、あるモードの値を変えても別モードには影響しません。完全な値は[`kp3s.json`](kp3s.json)を参照してください。

```json
"pull_gcode": {
  "type": "alternating",
  "normal": {
    "x_hook": 169,
    "x_latch": 174,
    "x_front": 20,
    "fast_feed_rate": 5000,
    "feed_rate": 1500,
    "pull_feedrate_fast": 3000,
    "y_origin_offset": 7,
    "y_offset": -3.5,
    "approach_y_offset": -4,
    "approach_z_offset": 5,
    "engage_z_drop": 5,
    "disengage_x_offset": 1.5,
    "disengage_z_lift": 5,
    "retreat_z_lift": 4
  },
  "alternating": {
    "row_parity": 0,
    "normal": {
      "x_hook": 169,
      "x_latch": 174,
      "x_front": 20,
      "fast_feed_rate": 5000,
      "feed_rate": 1500,
      "pull_feedrate_fast": 3000,
      "y_origin_offset": 7,
      "alternating_y_offset": -1,
      "y_offset": -3.5,
      "approach_y_offset": -4,
      "approach_z_offset": 5,
      "engage_z_drop": 5,
      "disengage_x_offset": 1.5,
      "disengage_z_lift": 5,
      "retreat_z_lift": 4
    },
    "reverted": {
      "x_hook": 169,
      "x_latch": 174,
      "x_front": 20,
      "fast_feed_rate": 5000,
      "feed_rate": 1500,
      "pull_feedrate_fast": 3000,
      "y_origin_offset": -1.8,
      "alternating_y_offset": -1,
      "y_offset": -3.5,
      "approach_y_offset": -4,
      "approach_z_offset": 5,
      "engage_z_drop": 5,
      "disengage_x_offset": 1.5,
      "disengage_z_lift": 5,
      "retreat_z_lift": 4
    }
  },
  "reverted": {
    "x_hook": 169,
    "x_latch": 174,
    "x_front": 20,
    "fast_feed_rate": 5000,
    "feed_rate": 1500,
    "pull_feedrate_fast": 3000,
    "y_origin_offset": 0,
    "y_offset": -3.5,
    "approach_y_offset": -4,
    "approach_z_offset": 5,
    "engage_z_drop": 5,
    "disengage_x_offset": 1.5,
    "disengage_z_lift": 5,
    "retreat_z_lift": 4
  }
}
```

`alternating.normal`と`alternating.reverted`は`normal`・`reverted`と同じ項目を持ち、さらに`alternating_y_offset`が必須です。`row_parity`はalternatingでrevertedにする段で、0は偶数、1は奇数です。座標・移動量はmm、送り速度はmm/minです。

| フィールド | 説明 |
|---|---|
| `type` | `normal` / `alternating` / `reverted` |
| `normal` / `reverted` | 各単独モードの設定。すべてのX座標・速度・移動量を含む |
| `alternating.normal` / `alternating.reverted` | alternating内のrow別設定。単独モードとは独立 |
| 各プロファイルの `x_hook` / `x_latch` / `x_front` | フック進入・ラッチ・引き出し先の絶対X座標 |
| 各プロファイルの `fast_feed_rate` / `feed_rate` / `pull_feedrate_fast` | 接近・ラッチ等・引き出しの送り速度 |
| 各プロファイルの `y_origin_offset` / `y_offset` | pull_originおよび列ごとのY基準への加算量 |
| 各プロファイルの `approach_y_offset` | 接近Yと係合Yの差。単独revertedでは両者を交換 |
| alternatingプロファイルの `alternating_y_offset` | alternatingだけに加えるY補正（現在は両方-1mm） |
| `alternating.row_parity` | alternatingでrevertedにする段。0は偶数、1は奇数 |
| 各プロファイルのZ・離脱項目 | approach/engage/disengage/retreatで使うZ量とX離脱量 |

---

## スライサ設定との対応

プリンタプロファイル（`.json`）に以下のプロパティを設定します。

| プロパティ | 説明 |
|---|---|
| `dynapin_config_path` | 本JSONファイルへの相対パス（`profiles/` 以降） |
| `enable_dynapin_support_optimization` | サポート除外を有効にするか（bool） |
| `dynapin_selected_pins` | 引き出すピンの一覧（`row,col` を空白・セミコロン・改行で区切る） |

**`dynapin_selected_pins` の記述例：**
```
0,0 1,0 2,0
```
または
```
0,0; 1,0; 2,0
```

---

## 旧設定からの変更点

旧形式の `pull_gcode` は読み込みエラーになります。現在の `kp3s.json` を元に移行してください。

- `type`、`normal`、`alternating`、`reverted` を明示し、各プロファイルに必要な全項目を設定します。
- X座標と速度も各プロファイルに置きます。`alternating.normal` / `alternating.reverted` は単独モードとは独立した設定です。
- 旧 `z_offset` は `retreat_z_lift` に置き換えます。
- サポートのX範囲は選択中プロファイルの`x_front..bed_max_x`です。alternatingでは2つの`x_front`のうち小さい値を使います。`support_exclusion` にX関連フィールドは追加しません。
- `grid` は `pull_origin` / `support_origin`、`pitch.row_z` / `pitch.col_y` を使います。行・列番号は0始まりです。

---

## 設定ファイルの検索順

`dynapin_config_path` が相対パスの場合、以下の順で検索されます。

1. `{data_dir}/user/{path}`
2. `{data_dir}/system/{path}`
3. `{resources_dir}/profiles/{path}`
4. `{resources_dir}/{path}`
5. カレントディレクトリからの相対パス

上位に同じパスのファイルがあると、後続の検索先にあるJSONは使われません。macOSでは通常、`{data_dir}` は `~/Library/Application Support/OrcaSlicer` です。そのため、たとえば `dynapin_config_path` が `Kingroon/dynapin/kp3s.json` の場合、次のユーザー設定またはシステム設定がアプリに同梱された `Resources/profiles/Kingroon/dynapin/kp3s.json` より優先されます。

- `~/Library/Application Support/OrcaSlicer/user/Kingroon/dynapin/kp3s.json`
- `~/Library/Application Support/OrcaSlicer/system/Kingroon/dynapin/kp3s.json`

ソース側やアプリ内のJSONを更新しても、これらの優先ファイルが古いままだと、スライス結果には古い設定値（例: `pull_gcode` の `y_origin_offset`）が使われます。設定ファイルと実際の結果が食い違う場合は、まずこの検索順で実際に読まれるファイルを確認してください。設定をそろえるには、優先ファイルも同じ内容に更新するか、不要な優先ファイルを退避してから再スライスします。

### 開発環境のシンボリックリンク

本家版とこのDynaPin版は、通常起動では同じ`~/Library/Application Support/OrcaSlicer`を使用します。`dynapin/kp3s.json`はDynaPin版が使う追加ファイルですが、プリンタ・材料・プロセス等の設定保存先は共有されます。データフォルダ全体を分ける場合は、DynaPin版を専用の`--datadir`で起動します。

2026-10-22のGUIスライスでは、`system/Kingroon/dynapin/kp3s.json`の古いコピーが優先され、リポジトリに追加した`landing_edge_inset_y`が未設定（既定0 mm）になっていました。CLI検証で新しい設定が使われていても、GUIが同じファイルを使うとは限りません。

この開発環境では元の通常ファイルを`kp3s.json.backup-<日時>`へバックアップし、次のシンボリックリンクに置き換えました。

```text
~/Library/Application Support/OrcaSlicer/system/Kingroon/dynapin/kp3s.json
  → <リポジトリ>/resources/profiles/Kingroon/dynapin/kp3s.json
```

リンク自体はローカル環境の設定であり、Gitには保存されません。別の環境では同じ参照関係を設定するか、`dynapin_config_path`をリポジトリのJSONへの絶対パスにします。`user/`に同名のファイルがある場合は、そちらがさらに優先されます。

確認には`readlink`で参照先を調べ、ログの`[DynaPin] config loaded`の`resolved`で実際の読み込み先を確認します。リンク先のJSONを変更すると、次に設定を読み込むスライスに反映されます。既に生成済みのG-codeは変わらず、JSONの外部編集だけで自動的に再スライスされることも保証しません。設定を変更した後は、サポートを再生成する再スライスを実行し、押出線幅を含めた経路を確認してください。

---

## 完全な設定例（KP3S）

[kp3s.json](kp3s.json)

### `landing_slope` セクション（任意）

ピン上面のX方向の傾斜を、水平な帯と追加サポート層で補正します。設定例と生成・引き出しの仕様は[ピン上の定着用初層の仕様](../../../../docs/DYNAPIN_PIN_BASE.md#x方向に傾いたピンへの水平層による補正)を参照してください。

| フィールド | 省略値 | 意味 |
|---|---|---|
| `reference_x` | 25 | `support_origin.z`が表す上面高さの機械X座標 [mm] |
| `slope_x` | 0 | 上面の傾き dz/dx。0で補正なし |
| `max_band_width` | 7.5 | 水平近似する帯の最大X幅 [mm] |
| `travel_clearance` | 2 | ピン引き出し、ピンと干渉する通常移動、ピン上初層の別領域への移動で使う退避量 [mm] |

例として、X25の先端からX175の根元まで1 mm下がる場合は`slope_x: -0.006666666666666667`です。現在の`kp3s.json`にはユーザー指定のこの傾きを設定済みです。項目を省略するか`slope_x`を0にすると補正を無効にできます。JSONの読み込み優先順位と、変更後の再スライスも確認してください。

同じ高さの隣の初層走査線へ短距離でつなぐ場合は、毎回Z退避しません。初層への進入・退出と離れた場所への移動では垂直退避します。連続走査の判定、衝突判定との整合、および検証状況は[定着用初層の仕様](../../../../docs/DYNAPIN_PIN_BASE.md)を参照してください。
