# DynaPin 設定ファイル仕様

DynaPinはプリンタのY軸側面に取り付けたピンアレイを使って、印刷途中にピンを引き出す機能です。  
本ドキュメントではプリンタごとに用意するJSON設定ファイルのスキーマと、各設定値がどう使われるかを説明します。

現在の `kp3s.json` はPython検証用の `pull_gcode.normal` / `pull_gcode.reverted` 構造です。C++の設定読み込みはまだ旧構造を要求するため、このJSONを現行スライサに指定すると設定読み込みが失敗します。以下のサポート形状・スライサ連携の説明はC++側の既存の仕組みを示しています。今回サポート座標やC++は変更していません。

## Pythonで引き出し動作を検証する

リポジトリのルートで実行します。Python標準ライブラリだけを使用します。

```bash
# JSONのtypeに従う（現在はalternating）
python3 scripts/generate_dynapin_pull_gcode.py --pins "0,0; 1,0" --output /tmp/alternating.gcode

# 現行C++のピン単体動作を比較用に再現する
python3 scripts/generate_dynapin_pull_gcode.py --pins "0,0; 1,0" --current --output /tmp/current.gcode

# typeを一時的に上書きし、接近Yと係合Yを入れ替える
python3 scripts/generate_dynapin_pull_gcode.py --pins "0,0; 1,0" --type revertedonly --output /tmp/revertedonly.gcode
```

`--config` でJSONパスを指定できます。省略時は、このREADMEと同じディレクトリの `kp3s.json` を読みます。
まずリポジトリ内の `old-models/min/min.gcode` を読み込み、内容を変更せず出力の先頭にコピーして、その後に引き出しG-codeを追記します。`--base-file /path/to/min.gcode` で別の読み込み元を指定できます。元ファイルは上書きしません。
元の改行・バイト列を保持し、末尾に改行がない場合だけ追記との境界に改行を1つ加えます。読み込み元が存在しない場合はエラーにして、出力ファイルを変更しません。
`--output` 省略時は標準出力へ出力します。ピンは0始まりで、指定したピンだけを高さ・列順に並べ、重複を取り除きます。
`--type normalonly` は全段にnormalのYオフセットを適用します。`--current` はnormalの動作値を使い、向き別オフセットを適用せず、旧 `z_offset` に相当する値として `normal.retreat_z_lift` を使います。

出力は `min.gcode` の内容に続いて `G21`（mm）・`G90`（絶対座標）と引き出し動作を含みます。ホーミング・加熱・押出などの初期化は、読み込んだ `min.gcode` の内容に従います。Pythonはプリンタへの送信やスライサのレイヤー判定・Z-hopは行いません。
`--return-position X Y Z` を指定した場合だけ、全ピン処理後に一度、XY→Zの順で指定座標へ戻ります。復帰速度は最後に引き出したピンの向きの設定を使います。

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

`revertedonly` は上記の `pull_y + approach_y_offset` と `pull_y` を入れ替えます。Y位置を反転して新しい位置を作るのではなく、接近位置と係合位置を交換します。Z動作は現行比較モードと同じです。

`alternating` は `row % 2 == reverted.row_parity` の段をreverted、それ以外をnormalにします。両者とも次の6動作で、normalはY基準が7mm大きくなります。

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
現在のrow=0,col=0（reverted）はY=5.5、Z=11→6→11。row=1,col=0（normal）はY=12.5、Z=18.4→13.4→18.4です。

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
- **Y範囲**: `pin_y ± blocker_width_y/2`（ブロッカーのY方向幅）
- **Z範囲**: `z_max - blocker_height_z`（ピン下面）から `z_max`（ピン上面）まで。
  - `blocker_height_z` が設定されていない、または0の場合は、Z方向の下側延長はありません。

### 仮想サポート面（着地面）の生成

ピンの上面（`z_max`）は「仮想ビルドプレート」として機能します。ピンの上にあるモデルのオーバーハングから生成されたサポート材は、ベッド（Z=0）まで降りる代わりにピンの上面で「着地」します。

- **仮想サポート面のXY範囲**: サポートブロックと同じ `x_front..bed_max_x`、および `pin_y ± blocker_width_y/2` です。
- **引き出し経路の真上**: 引き出し経路全体を仮想支持面として扱い、上のサポート材がピン上面で着地できるようにします。

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
blocker_y   = support_origin.y + col × pitch.col_y
blocker_z_max = support_origin.z + row × pitch.row_z
blocker_z_min = blocker_z_max - blocker_height_z
```

`dynapin_selected_pins` が空の場合は、この行数・列数で列挙したピンを自動選択候補として使用します。`row` はZ方向の高さ、`col` はY方向の位置を表します。

---

### `support_exclusion` セクション（必須）

サポート材を除外する領域を定義します。

```json
"support_exclusion": {
  "blocker_width_y": 8,
  "blocker_height_z": 5
}
```

| フィールド | 型 | 必須 | 説明 |
|---|---|---|---|
| `blocker_width_y` | number | ○ | サポートブロッカーのY方向幅 [mm] |
| `blocker_height_z` | number | – | サポートブロッカーの上端から下方向への高さ [mm]（省略時 0） |

既存C++でのX範囲は旧構造の `pull_gcode.x_front` とプリンタの `printable_area` から自動計算されます。新構造へのC++対応時には `選ばれた向きのx_front` を参照する必要があります。

---

### `pull_gcode` セクション（必須）

Python検証用のピン引き出し動作を定義します。以下は現在のKP3S設定です。

```json
"pull_gcode": {
  "type": "alternating",
  "normal": {
    "y_origin_offset": 7,
    "alternating_y_offset": -1,
    "x_hook": 169,
    "x_latch": 174,
    "x_front": 20,
    "y_offset": -3.5,
    "approach_y_offset": -4,
    "approach_z_offset": 5,
    "engage_z_drop": 5,
    "disengage_x_offset": 1.5,
    "disengage_z_lift": 5,
    "retreat_z_lift": 4,
    "pull_feedrate_fast": 3000,
    "feed_rate": 1500,
    "fast_feed_rate": 5000
  },
  "reverted": {
    "row_parity": 0,
    "y_origin_offset": 0,
    "alternating_y_offset": -1,
    "x_hook": 169,
    "x_latch": 174,
    "x_front": 20,
    "y_offset": -3.5,
    "approach_y_offset": -4,
    "approach_z_offset": 5,
    "engage_z_drop": 5,
    "disengage_x_offset": 1.5,
    "disengage_z_lift": 5,
    "retreat_z_lift": 4,
    "pull_feedrate_fast": 3000,
    "feed_rate": 1500,
    "fast_feed_rate": 5000
  }
}
```

全項目を明示します。座標・移動量はmm、送り速度はmm/minです。

| フィールド | 説明 |
|---|---|
| `type` | `normalonly` / `revertedonly` / `alternating` |
| `normal.y_origin_offset` | normalのY基準への加算量 |
| `reverted.y_origin_offset` | revertedのY基準への加算量 |
| `normal.alternating_y_offset` / `reverted.alternating_y_offset` | alternatingだけに加えるY補正（現在は-1mm） |
| `reverted.row_parity` | alternatingでrevertedにする段。0は偶数、1は奇数 |
| `normal` / `reverted` の `x_hook` / `x_latch` / `x_front` | フック進入・ラッチ・引き出し先の絶対X座標 |
| `normal` / `reverted` の `y_offset` | gridのY基準への加算量 |
| `normal` / `reverted` の `approach_y_offset` | 接近Yと係合Yの差。revertedonlyでは両者を交換 |
| `normal` / `reverted` の `approach_z_offset` | alternatingの進入Zへの加算量 |
| `normal` / `reverted` の `engage_z_drop` | alternatingで進入Zから下げる量 |
| `normal` / `reverted` の `disengage_x_offset` | 離脱時にx_frontへ加算する量 |
| `normal` / `reverted` の `disengage_z_lift` | alternatingで係合Zから上げる量 |
| `normal` / `reverted` の `retreat_z_lift` | 単一向きモード・現行比較でpull_zから上げる量。alternatingでは使わない |
| `normal` / `reverted` の `pull_feedrate_fast` | 引き出し時の送り速度 |
| `normal` / `reverted` の `feed_rate` | ラッチ・係合・離脱・退避時の送り速度 |
| `normal` / `reverted` の `fast_feed_rate` | 接近・フック進入・復帰XYの送り速度 |

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

`center_x`、`width_x`、`x_min`、`x_max` は使用しません。X範囲は必ず `pull_gcode.x_front..bed_max_x` になります。

`grid.origin`、`grid.physical_origin`、`origin_row`、`origin_col`、`origin_y`、`origin_z` は読み込まれません。新しい設定では必ず `pull_origin` と `support_origin` を使用してください。

旧 `pitch.row_y` と `pitch.col_z` も読み込まれません。新しい設定では `pitch.row_z` と `pitch.col_y` を使用してください。

| 旧フィールド | 現在の解釈 |
|---|---|
| `pull_gcode.pull_feedrate` | `feed_rate` の代替名 |
| `pull_gcode.travel_feedrate` | `fast_feed_rate` の代替名 |

旧形式のJSONを使う場合は、`support_exclusion` からX関連フィールドを削除し、プリンタの `printable_area` が正しく設定されていることを確認してください。

| フィールド | 省略時のデフォルト |
|---|---|
| `pull_gcode.approach_y_offset` | `-4.0` mm |
| `pull_gcode.pull_feedrate_fast` | `3000` mm/min |
| `pull_gcode.disengage_x_offset` | `1.5` mm |

---

## 設定ファイルの検索順

`dynapin_config_path` が相対パスの場合、以下の順で検索されます。

1. `{data_dir}/user/{path}`
2. `{data_dir}/system/{path}`
3. `{resources_dir}/profiles/{path}`
4. `{resources_dir}/{path}`
5. カレントディレクトリからの相対パス

---

## 完全な設定例（KP3S）

[kp3s.json](kp3s.json)
