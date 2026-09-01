# DynaPin 設定ファイル仕様

DynaPinはプリンタのY軸側面に取り付けたピンアレイを使って、印刷途中にピンを引き出す機能です。  
本ドキュメントではプリンタごとに用意するJSON設定ファイルのスキーマと、各設定値がスライサ上でどう使われるかを説明します。

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

### ピン引き出しのGコード動作

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
7. G1 Z(pull_z + z_offset)                           ← Z退避（低速）
```

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
pull_y = pull_origin.y + col × pitch.col_y + pull_gcode.y_offset
pull_z = pull_origin.z + row × pitch.row_z
```

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

X範囲は `pull_gcode.x_front` とプリンタの `printable_area` から自動計算されるため、X位置やX幅をここへ記述しません。

---

### `pull_gcode` セクション（必須）

ピン引き出し動作のGコードパラメータを定義します。

```json
"pull_gcode": {
  "x_hook": 160,
  "x_latch": 168,
  "x_front": 30,
  "y_offset": 4,
  "z_offset": 4,
  "approach_y_offset": -4,
  "pull_feedrate_fast": 3000,
  "disengage_x_offset": 1.5,
  "disengage_z_offset": 4,
  "feed_rate": 1500,
  "fast_feed_rate": 5000
}
```

| フィールド | 型 | 必須 | 説明 |
|---|---|---|---|
| `x_hook` | number | ○ | フック位置のX座標 [mm]（ピンに引っかかる位置） |
| `x_latch` | number | ○ | ラッチ位置のX座標 [mm]（ピンを捕捉する位置） |
| `x_front` | number | ○ | 最終引き出し位置のX座標 [mm] |
| `y_offset` | number | – | ピンY座標へのオフセット [mm]（省略時 0） |
| `z_offset` | number | – | 引き出し完了後の退避Z量 [mm]（省略時 0）。`pull_z + z_offset` の高さまでZを上げる |
| `approach_y_offset` | number | – | 接近時にY座標をさらにずらす量 [mm]（省略時 -4）。フック進入時にピンと干渉しないよう手前にずれる |
| `pull_feedrate_fast` | number | – | 引き出し（`x_front`への移動）時の送り速度 [mm/min]（省略時 3000） |
| `disengage_x_offset` | number | – | 外し動作でXを `x_front` から追加移動する量 [mm]（省略時 1.5） |
| `feed_rate` | number | – | ラッチ・係合・外し動作の送り速度 [mm/min]（省略時 1200） |
| `fast_feed_rate` | number | – | 接近・フック移動の送り速度 [mm/min]（省略時 6000） |

> `feed_rate` は `pull_feedrate`、`fast_feed_rate` は `travel_feedrate` としても記述できます。

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
