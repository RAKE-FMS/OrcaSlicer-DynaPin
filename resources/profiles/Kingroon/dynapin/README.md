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

1. G0 Z(pin_z + z_offset)           ← ピン高さへ移動（早送り）
2. G0 X(x_hook) Y(pin_y + y_offset) ← フック位置へ移動（早送り）
3. G1 X(x_latch)                    ← ラッチをかける（低速）
4. G1 X(x_front)                    ← 前面まで引き出す（低速）
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
│  z_min = 0（底面）
│
└──────────────────────────── X
   x_front          center_x
```

- **X範囲**: `x_front` ～ `center_x + width_x/2`（引き出し経路の全域）
- **Y範囲**: `pin_y ± width_y/2`（ピン周囲の幅）
- **Z範囲**: 底面（Z=0）から `pin_z + z_above` まで（ピン高さより下のすべてのレイヤー）

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
| `pitch.row_y` | number | ○ | 行間ピッチ（Y方向）[mm] |
| `pitch.col_z` | number | ○ | 列間ピッチ（Z方向）[mm] |

ピン `(row, col)` の実座標：
```
pin_y = origin.y + (row - origin.row) × pitch.row_y
pin_z = origin.z + (col - origin.col) × pitch.col_z
```

---

### `support_exclusion` セクション（必須）

サポート材を除外する領域を定義します。

```json
"support_exclusion": {
  "center_x": 110,
  "width_x": 20,
  "width_y": 8,
  "z_above": 4
}
```

| フィールド | 型 | 必須 | 説明 |
|---|---|---|---|
| `center_x` | number | △ | ピン本体のX中心座標 [mm]。`x_min`/`x_max` で代替可 |
| `width_x` | number | △ | ピン本体のX方向幅 [mm]。`x_min`/`x_max` で代替可 |
| `width_y` | number | ○ | ピン周囲のY方向幅 [mm] |
| `z_above` | number | – | ピン位置より上へ除外を延ばす量 [mm]（省略時 0） |

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
| `z_offset` | number | – | ピンZ座標へのオフセット [mm]（省略時 0） |
| `feed_rate` | number | – | 引き出し動作の送り速度 [mm/min]（省略時 1200） |
| `fast_feed_rate` | number | – | 移動動作の送り速度 [mm/min]（省略時 6000） |

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
