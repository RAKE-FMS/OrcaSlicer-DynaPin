# DynaPin 設定ファイル仕様

DynaPinはプリンタのY軸側面に取り付けたピンアレイを使って、印刷途中にピンを引き出す機能です。  
本ドキュメントではプリンタごとに用意するJSON設定ファイルのスキーマと、各設定値がスライサ上でどう使われるかを説明します。

---

## 動作の概要

```
Y軸 (プリンタ手前 ←→ 奥)
  ↑
  │  [ピンアレイ]  row=0  row=1  row=2 ...
  │              ○      ○      ○       ← col=0  (Z=origin_z)
  │              ○      ○      ○       ← col=1  (Z=origin_z + col_pitch_z)
  │              ○      ○      ○       ← col=2
  │
  └─────────────────────────────────── Z軸 (印刷高さ)
```

- **行 (row)** … Y軸方向に並ぶピン。`row_pitch_y` ごとに配置。
- **列 (col)** … Z軸（高さ）方向に並ぶピン。`col_pitch_z` ごとに配置。

指定したレイヤー（ピンのZ高さ）に達したとき、ノズルがX軸を移動してピンを引き出す。

### ピン引き出しのGコード動作

```
x_hook  x_latch             x_front
  │       │                    │
  ↓       ↓                    ↓
─────── ───────         ─────────────── (X軸)

1. G1 Y(pin_y + y_offset + approach_y_offset)       ← ピンをよけてY接近（早送り）
2. G1 X(x_hook) Z(pin_z)                            ← XZ同時移動でフック高さへ（早送り）
3. G1 X(x_latch)                                    ← ラッチをかける（低速）
4. G1 Y(pin_y + y_offset)                           ← Yをピンに当てる（低速）
── ; DYNAPIN_PULL_MOVE ──
5. G1 X(x_front)                                    ← 前面まで引き出す（高速）
6. G1 X(x_front + disengage_x_offset) Y(pin_y + y_offset + approach_y_offset)
                                                    ← XY同時移動でピンを外す（低速）
7. G1 Z(pin_z + z_offset)                           ← Z退避（低速）
```

### サポート材の除外領域

ピンが引き出される経路にサポート材があると動作を妨げるため、スライサが自動でサポートをブロックします。

```
Z
↑
│  z_max = pin_z + z_above
│  ┌──────────────────────┐
│  │   サポート除外領域    │
│  │                      │
│  │  x_front ← ─ ─ → center_x ± width_x/2
│  │                      │
│  └──────────────────────┘
│  z_min = max(0, pin_z - pin_z_height)
│
└──────────────────────────── X
   x_front          center_x
```

- **X範囲**: `x_front` ～ `center_x + width_x/2`（引き出し経路の全域）
- **Y範囲**: `pin_y ± width_y/2`（ピン周囲の幅）
- **Z範囲**: `pin_z - pin_z_height`（ピン下面）から `pin_z + z_above`（ピン上面のクリアランス）まで。
  - `pin_z_height`（ピンの厚さ）が設定されていない、または0の場合は、底面（Z=0）から除外されます。これにより、ピンより下の安全な領域にはサポート材が正しく生成されます。

### 仮想サポート面（着地面）の生成

ピンの上面（`pin_z + z_above`）は「仮想ビルドプレート」として機能します。ピンの上にあるモデルのオーバーハングから生成されたサポート材は、ベッド（Z=0）まで降りる代わりにピンの上面で「着地」します。

- **仮想サポート面のXY範囲**: ピンが実際に配置されているX座標（`center_x - width_x/2` ～ `center_x + width_x/2`）の範囲のみに限定されます。
- **引き出し経路の真上**: 引き出し経路（`x_front` から `center_x - width_x/2` まで）の真上にあるサポート材は、この仮想サポート面で止まることなくベッド（または下のモデル表面）まで生成されます。これにより、引き出し経路の真上にあるサポートが宙に浮いてしまう問題を防ぎます。

---

## JSONスキーマ

### `grid` セクション（必須）

ピンアレイの物理配置を定義します。

```json
"grid": {
  "origin": {
    "row": 0,
    "col": 0,
    "y": 63.6,
    "z": 4.0
  },
  "row_count": 10,
  "col_count": 14,
  "pitch": {
    "row_y": 12.4,
    "col_z": 7.4
  }
}
```

| フィールド | 型 | 必須 | 説明 |
|---|---|---|---|
| `origin.row` | int | ○ | 原点ピンの行番号 |
| `origin.col` | int | ○ | 原点ピンの列番号 |
| `origin.y` | number | ○ | 原点ピンのY座標 [mm] |
| `origin.z` | number | ○ | 原点ピンのZ座標（印刷高さ）[mm] |
| `row_count` | int | ○ | `origin.row` から正方向に存在するピン行数 |
| `col_count` | int | ○ | `origin.col` から正方向に存在するピン列数 |
| `pitch.row_y` | number | ○ | 行間ピッチ（Y方向）[mm] |
| `pitch.col_z` | number | ○ | 列間ピッチ（Z方向）[mm] |

ピン `(row, col)` の実座標：
```
pin_y = origin.y + (row - origin.row) × pitch.row_y
pin_z = origin.z + (col - origin.col) × pitch.col_z
```

`dynapin_selected_pins` が空の場合は、この行数・列数で列挙したピンを自動選択候補として使用します。

---

### `support_exclusion` セクション（必須）

サポート材を除外する領域を定義します。

```json
"support_exclusion": {
  "center_x": 110,
  "width_x": 20,
  "width_y": 8,
  "z_above": 4,
  "pin_z_height": 5
}
```

| フィールド | 型 | 必須 | 説明 |
|---|---|---|---|
| `center_x` | number | △ | ピン本体のX中心座標 [mm]。`x_min`/`x_max` で代替可 |
| `width_x` | number | △ | ピン本体のX方向幅 [mm]。`x_min`/`x_max` で代替可 |
| `width_y` | number | ○ | ピン周囲のY方向幅 [mm] |
| `z_above` | number | – | ピン位置より上へ除外を延ばす量 [mm]（省略時 0） |
| `pin_z_height` | number | – | ピン本体の厚さ（Z高さ）[mm]。省略された場合は 0 となり、Z=0（ベッド面）から除外されます。 |

**代替記法**（`center_x`/`width_x` の代わりに使用可）：

```json
"support_exclusion": {
  "x_min": 100,
  "x_max": 120,
  ...
}
```

> **注意**: `x_min`/`x_max` が両方指定されている場合、`center_x`/`width_x` より優先されます。

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
| `z_offset` | number | – | 引き出し完了後の退避Z量 [mm]（省略時 0）。`pin_z + z_offset` の高さまでZを上げる |
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

## 後方互換性

旧フォーマットのフィールドも読み込み可能です。

| 旧フィールド | 現在の解釈 |
|---|---|
| `support_exclusion.z_range` | `z_above = z_range / 2` として扱う（`z_above` が未指定の場合のみ） |
| `support_exclusion.x_center` | `center_x` の代替名 |
| `support_exclusion.x_width` | `width_x` の代替名 |
| `support_exclusion.y_width` | `width_y` の代替名 |
| `pull_gcode.pull_feedrate` | `feed_rate` の代替名 |
| `pull_gcode.travel_feedrate` | `fast_feed_rate` の代替名 |

新フィールドを省略した場合はデフォルト値が使われます（後方互換あり）。

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
