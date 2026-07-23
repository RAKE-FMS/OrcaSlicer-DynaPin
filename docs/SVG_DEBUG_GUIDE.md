# OrcaSlicer SVG デバッグ出力仕様・編集ガイド (SVG Debug Output Guide)

OrcaSlicer のサポート構造生成プロセスにおける **2D / Side View SVG デバッグ出力機能** の仕様、ファイル保存場所、およびソースコード編集ガイドです。

---

## 📂 1. SVG ファイルの保存先 (Output Location)

SVG デバッグファイルは、`src/libslic3r/Utils.cpp` 内の `debug_out_path()` 関数によって自動生成・保存されます。

- **macOS（GUI起動時）**:
  `<UserName>/Library/Application Support/OrcaSlicer/SVG/`
- **CLI / スタンドアロン実行時**:
  カレント作業ディレクトリ下の `./SVG/`

### 自動出力される主要ファイル

| ファイル名 | 視点 (投影軸) | 概要 |
| :--- | :--- | :--- |
| `support-SIDE-VIEW-XZ-run1.svg` | **X-Z 平面（正面/背面）** | 横軸=X, 縦軸=Z。3Dモデル本体（灰色）とサポート層の立面図 |
| `support-SIDE-VIEW-YZ-run1.svg` | **Y-Z 平面（側面）** | 横軸=Y, 縦軸=Z。側面から見た3Dモデルとサポート層の立面図 |
| `support-ALL-LAYERS-OVERLAY-run1.svg` | **X-Y 平面（上面）** | 上面から見下ろした全レイヤー重なる配置関係図 |
| `support-top-contacts-1-*.svg` | **X-Y 平面** | 各層ごとの Top Contact （サポート天面）の個別ポリゴン図 |

---

## ✏️ 2. ソースコードの編集箇所 (Customization Points)

すべての処理は **`src/libslic3r/Support/SupportMaterial.cpp`** 内に集約されています。

### ① SVG 出力処理の呼び出し位置
- **対象行**: [src/libslic3r/Support/SupportMaterial.cpp](../src/libslic3r/Support/SupportMaterial.cpp#L789-L806)
- サポート生成関数 `PrintObjectSupportMaterial::generate()` の最終処理段階で呼び出されます。

```cpp
#ifdef SLIC3R_DEBUG
    if (!layers_sorted.empty()) {
        static int iRunSide = 0;
        ++iRunSide;
        // 正面立面図 (X-Z)
        export_support_side_view_to_svg(
            debug_out_path("support-SIDE-VIEW-XZ-run%d.svg", iRunSide).c_str(),
            object, layers_sorted.data(), layers_sorted.size(), SideViewPlane::XZ);
        // 側面立面図 (Y-Z)
        export_support_side_view_to_svg(
            debug_out_path("support-SIDE-VIEW-YZ-run%d.svg", iRunSide).c_str(),
            object, layers_sorted.data(), layers_sorted.size(), SideViewPlane::YZ);
        // 上面統合図 (X-Y)
        export_print_z_polygons_with_object_to_svg(
            debug_out_path("support-ALL-LAYERS-OVERLAY-run%d.svg", iRunSide).c_str(),
            object, layers_sorted.data(), layers_sorted.size());
    }
#endif
```

---

### ② 配色（RGBカラーマッピング）の変更
- **対象行**: [src/libslic3r/Support/SupportMaterial.cpp](../src/libslic3r/Support/SupportMaterial.cpp#L74-L87)
- `support_surface_type_to_color_name()` 関数で各サーフェス種別の RGB 値を指定します。

```cpp
const char* support_surface_type_to_color_name(const SupporLayerType surface_type)
{
    switch (surface_type) {
        case SupporLayerType::TopContact:     return "rgb(0,128,0)";    // Support Interface (#008000)
        case SupporLayerType::TopInterface:   return "rgb(0,128,0)";    // Support Interface (#008000)
        case SupporLayerType::Base:           return "rgb(0,255,0)";    // Base (#0F0 / #00FF00)
        case SupporLayerType::BottomInterface:return "rgb(0,128,0)";    // Support Interface (#008000)
        case SupporLayerType::BottomContact:  return "rgb(0,128,0)";    // Support Interface (#008000)
        case SupporLayerType::Unknown:        return "rgb(255,125,56)"; // Unknown (#FF7D38)
        default:                              return "rgb(0,255,0)";
    };
}
```

---

### ③ 線の太さ（stroke-width）の変更
- **対象行**: [src/libslic3r/Support/SupportMaterial.cpp](../src/libslic3r/Support/SupportMaterial.cpp#L191-L213)
- `export_support_side_view_to_svg()` 関数内で `scale_(layer->height * 倍率)` の計算式で制御しています。

```cpp
// 3Dモデル本体の描画幅 (Line 191)
float model_line_width = (layer->height > 0.0) ? scale_(layer->height * 1.05) : scale_(0.2);

// サポート材の描画幅 (Line 213)
float support_line_width = (layers[i]->height > 0.0) ? scale_(layers[i]->height * 1.05) : scale_(0.2);
```

---

## 🛠 3. Debug / Release ビルド条件の制御フラグ

`SupportMaterial.cpp` のファイル先頭に制御マクロが定義されています：

- **ファイル先頭**: [src/libslic3r/Support/SupportMaterial.cpp](../src/libslic3r/Support/SupportMaterial.cpp#L35)

```cpp
#define SLIC3R_DEBUG
```

- **常時出力する場合**: 35行目の `#define SLIC3R_DEBUG` を残しておきます（Release ビルドでも SVG が出力されます）。
- **Debug ビルド時のみ出力する場合**: 35行目の `#define SLIC3R_DEBUG` をコメントアウトします（CMake の Debug ビルド時のみ動くようになります）。

---

## 🌳 4. 補足: Tree Support (ツリーサポート) の SVG デバッグ

Tree Support 側のデバッグ出力は別のファイルで管理されています：

- **対象ファイル**: `src/libslic3r/Support/TreeSupport.cpp` ([Line 39](../src/libslic3r/Support/TreeSupport.cpp#L39))
- **制御マクロ**: `#define SUPPORT_TREE_DEBUG_TO_SVG`
