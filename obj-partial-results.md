# OBJ 网格流式/中途失败后的部分结果语义分析

## 1. 结论摘要

**失败后已读入的几何与材质不能安全复用。** 各加载路径的提交点不同，导致同一个“失败”在调用方看来可能有三种完全不同的状态：

| 状态 | 含义 | 是否可复用 |
|---|---|---|
| A. 输出被清空 | 失败后容器为空 | 可复用（重新导入） |
| B. 输出保留“已提交的前缀” | `shapes` 里有 shape，但 `attrib` 已清空，索引悬空 | **不可复用，按索引访问会越界/崩溃** |
| C. 输出保留“已提交的材质” | `materials` 里有失败解析留下的条目（可能是完整的 MTL，也可能是截断的前缀），`material_ids` 相对它取值 | **不可复用**；截断时 id 会指向错误条目/`-1` |

核心事实：

1. **面顶点属性（vertices/normals/texcoords）只在 EOF 才提交**（`tiny_obj_loader.h:9397-9403` 的 `swap`）。中途失败时它们停留在局部变量 `v/vn/vt` 并被丢弃，`attrib` 保持空。
2. **面（shape）却在 `g`/`o`/EOF 处提前提交**（`tiny_obj_loader.h:9200-9206`、`9253-9260`、`9390-9394`）。失败发生在提交之后时，`shapes` 里的 `index_t` 仍然指向**已被丢弃**的顶点数组 → 悬空索引。
3. **材质在 `newmtl` 处逐个追加**（`tiny_obj_loader.h:8319-8325`、`8700-8704`），失败时**从不回滚**，且 legacy API **不清空**调用方的 `materials`。
4. 因此 legacy `LoadObj` / `LoadObjOpt` / `LoadObjOptTyped` 的 `false` 返回值**并不代表输出为空**；`ObjReader` 还会在多次解析间累积材质并保留过期 `Error()` 文本。
5. **回调 API 与纯流式 handler 天生非原子**：失败前已经交付的顶点/面/材质无法撤回；更糟的是它会把畸形角标静默替换成 `0`，把畸形 `vn`/`vt` 静默替换成 `0`，仍返回 `true`。
6. **C API 的 `TOBJ_OK` 不等于“结果自洽”**：正数越界面索引会原样出现在输出里，`f 1 2 x` 会产生 `-1` 角标，而 `result` 仍是 `TOBJ_OK`；实测会破坏它自己测试里的 `mesh_consistent` 不变量（见 §9.5）。

**隔离责任应落在 loader/API 边界，而不是调用方。** 原子化 API（`LoadObj`、`ObjReader`、`LoadObjOpt`、`LoadObjOptTyped`、C `tobj_load_obj_from_*`）必须在 `false`/非 `OK` 时保证“输出为空或保持调用前的值”；流式 API（callback、`StreamHandler`）必须把语义改成“明确的部分交付 + 每条诊断”，并禁止合成假数据。

---

## 2. 复现材料

以下模型保存于临时目录 `/tmp/objpartial_final/`（不在仓库内提交，仅用于复现）。

**正常模型 `valid.obj` / `valid.mtl`**

```text
# valid.obj
mtllib valid.mtl
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
g quad
usemtl red
f 1/1 2/2 3/3 4/4
o tri
usemtl blue
f 1/1 2/2 3/3
```

```mtl
# valid.mtl
newmtl red
Kd 1 0 0
newmtl blue
Kd 0 0 1
```

**损坏模型 A：几何中途语法失败（`bad_v.obj`）** —— 在第 10 行出现非法数值，此前已有 `g quad` 组被提交。

```text
mtllib valid.mtl
v 0 0 0
v 1 0 0
v 1 1 0
g quad
usemtl red
f 1 2 3
o tri
usemtl blue
v OOPS 0 0
f 1 2 3
```

**损坏模型 B：材质数值失败（`bad_mtl.obj` + `bad.mtl`）** —— 第 5 行 `Ks BAD 0 0` 触发 MTL 硬错误。

```text
# bad_mtl.obj
mtllib bad.mtl
v 0 0 0
v 1 0 0
v 1 1 0
g quad
usemtl red
f 1 2 3
o tri
usemtl blue
f 1 2 3
```

```mtl
# bad.mtl
newmtl red
Kd 1 0 0
newmtl blue
Kd 0 0 1
Ks BAD 0 0
newmtl green
Kd 0 1 0
```

**损坏模型 C：多边形中途截断（`trunc_f.obj`）**

```text
v 0 0 0
v 1 0 0
v 1 1 0
f 1 2
```

**探针程序**（编译器：`Apple clang 17`，`-std=c++11 -O0`）

```cpp
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#include "stream_obj_loader.h"      // 需加 -I experimental/stream
#include <fstream>
#include <sstream>
```

完整探针脚本（8 种 API × 4 个模型的矩阵表）见 §9；C API 探针单独编译 `tiny_obj_c.c + tobj_tess.c`。

---

## 3. 各 API 在“同一失败”下的输出状态矩阵

下表的列：`verts` = `vertices.size()/3`，`faces` = 面数，`idx` = 索引条目数，`mats` = 材质数，`warn/err` = 诊断字节数。数据来自 §9 的探针输出。

### 3.1 损坏模型 A（`bad_v.obj`，顶点语法错误，失败前已提交 1 个 shape）

| API | ret | verts | shapes | faces | idx | mats | warn | err |
|---|---|---|---|---|---|---|---|---|
| `LoadObj(file)` | **0** | 0 | **1** | **1** | **3** | 2 | 0 | 88 |
| `LoadObj(istream)` | **0** | 0 | **1** | **1** | **3** | 2 | 0 | 65 |
| `ObjReader(ParseFromFile)` | **0** | 0 | **1** | 1 | — | 2 | 0 | 88 |
| `ObjReader(ParseFromString)` | **0** | 0 | **1** | 1 | — | 2 | 0 | 65 |
| `LoadObjWithCallback` | **0** | 3（已交付） | — | 1 次回调 | 3 角 | 1 次 mtllib | 0 | 57 |
| `LoadObjOpt(file)` | **0** | 0 | 0 | 0 | 0 | **2** | 0 | 53 |
| `LoadObjOptTyped` | valid=**0** | 0 | 0 | 0 | 0 | **2** | — | 53 |
| `StreamExperimental` | **0** | 0 | 0 | 0 | 0 | 0 | 0 | 38 |

关键观察：

* legacy 家族的 `shapes=1, verts=0` 就是状态 **B**。`shape.mesh.indices` 里仍是 `{0,1,2}`，而 `attrib.vertices` 为空 → 按 README 示例的消费循环 `attrib.vertices[3*idx.vertex_index+...]` 直接读空指针。**用 ASan 复现为 SEGV**（§8.1）。
* opt/typed 清了几何，**但 `materials` 里仍留着 `red/blue`**（状态 C：该 MTL 本身解析完整，但它是“失败的这次加载”留下的已提交状态，与清零的几何混在一起，见 §5.2）。
* 回调路径已经把 3 个顶点、1 个面和 2 次 `usemtl` 交付给调用方，`ret=0` 时无法撤回。

### 3.2 损坏模型 B（`bad_mtl.obj`，MTL 第 5 行数值错误）

| API | ret | verts | shapes | faces | mats | warn | err |
|---|---|---|---|---|---|---|---|
| `LoadObj(file)` | **1** | 3 | 2 | 2 | **1（red）** | 93 | **85** |
| `LoadObj(istream)` | **1** | 3 | 2 | 2 | 1 | 93 | 85 |
| `ObjReader(file)` | **1** (valid=1) | 3 | 2 | 2 | 1 | 93 | 85 |
| `ObjReader(string)` | **1** | 3 | 2 | 2 | 1 | 93 | 64 |
| `LoadObjWithCallback` | **1** | 3 | — | 2 次回调 | **mtllib 回调 0 次** | 91 | 85 |
| `LoadObjOpt(file)` | **1** | 3 | 2 | 2 | 1 | 93 | 85 |
| `LoadObjOptTyped` | valid=1 | 3 | 2 | 2 | 1 | 93 | 85 |
| `StreamExperimental` | **1** | 3 | 2 | 2 | 1 | 93 | 85 |

关键观察：

* `ret/valid = 1`，但 **`err` 非空**。文档只说“失败时填充 err”，没定义“成功但 `err` 非空”的组合；调用方若只判断返回值，会把**截断的材质表**当有效结果。
* `blue` 被截断：`Ks BAD 0 0` 让 `sr_parseReal3` 失败并在写回 `specular` 之前 `return false`（`tiny_obj_loader.h:8373-8376`）；`blue` 这个“进行中”的材质只有在下一次 `newmtl` 或文件尾才 flush（`8319-8325`、`8700-8704`），而函数已提前返回 → `blue` 完全未入表。于是 `usemtl blue` 解析为 `material_id = -1`，并附警告 `material [ 'blue' ] not found in .mtl`。
* 之后的 `newmtl green` 永远读不到 → `materials` 就是“失败前已 flush 的前缀”（此处只有 `red`）。
* 回调路径 `mtllib_cb` **一次都没触发**（`tiny_obj_loader.h:9692-9696` 要求本次 `mtllib` 解析成功且 `!materials.empty()`），但 `usemtl_cb` 已经用 `id=0`/`id=-1` 交付过 → 调用方如果按 `usemtl_cb` 注册材质，前两次引用有效、第三次无效，且没有任何“材质表被截断”的信号。

### 3.3 损坏模型 C（`trunc_f.obj`，多边形少于 3 个角）

| API | ret | verts | shapes | faces | warn |
|---|---|---|---|---|---|
| `LoadObj` / `ObjReader` / `LoadObjOpt` | **1** | 3 | **1** | **0** | 24（`Degenerated face found`） |
| `LoadObjOptTyped` | valid=1 | 3 | **0** | 0 | 24 |
| `LoadObjWithCallback` | **1** | 3 | — | **1 次回调，n=2** | 0 |
| `StreamExperimental` | 1 | 3 | 1 | 0 | 24 |

关键观察：

* **同一个“成功”在 opt 与 typed 之间给出不同的 `shapes` 数量**（1 vs 0），因为 typed 的 `emit_shape` 会跳过空 range（`tiny_obj_loader.h:13712-13714` 的 `if (f_end <= f_start) return;`）。这已经是一个现存的不一致。
* legacy 保留一个 `faces=0` 的空 shape，这是被测试 `test_invalid_face_definition` 固定的行为（`tests/tester.cc:1200-1224` 断言 `1 == shapes.size()` 且 `0 == indices.size()`）。
* 回调把只有 2 个角的面也交付了。`callback_t` 的注释只说 `num_indices` 是“每面的索引数（3=三角形、4=四边形）”（`tiny_obj_loader.h:529-532`），**未规定最小值**，实现却允许 `num_indices >= 1`（`9605` 的条件是 `indices.size() > 0`）——契约模糊，调用方必须自行拒绝 `<3`。

### 3.4 正常模型（对照组）

8 条路径全部 `ret/valid = 1`、`verts=4`、`shapes=2`、`faces=3`、`idx=9`、`mats=2`，`warn=err=0`（`LoadObj(istream)` 显式传 `MaterialFileReader`）。回调路径 `v=4, face_cb=2, corners=7, usemtl=2, mtllib=1`。

（表中 `LoadObj(string)` 的 `idx` 列显示为 `—`，是因为探针只统计了 `num_face_vertices`；`ObjReader` 没有直接暴露扁平 `indices`，须遍历 shape。回调行的 `shapes/faces` 列分别是 `index_cb` 次数与角标数。）

### 3.5 小结：失败语义完全由“失败点相对提交点”决定

| 失败点 | `shapes` | `attrib` | `materials` |
|---|---|---|---|
| 第一个 `g`/`o` 之前 | 空 | 空 | 已解析的 MTL 条目 |
| 两个 `g`/`o` 之间 | 前面所有 shape | 空 | 已解析的 MTL 条目 |
| MTL 内部（前 N 个 `newmtl`） | 全部（若几何已完成） | 全部 | 前 N 个 |
| 只有退化面（不算硬失败） | 空 shape（legacy/opt）或 0（typed） | 全部 | 全部 |

---

## 4. 逐层提交/丢弃点（代码定位）

### 4.1 Legacy `LoadObjInternal`（`tiny_obj_loader.h:8844-9406`）

| 状态 | 存放位置 | 提交时机 | 失败时 |
|---|---|---|---|
| 顶点/法线/纹理坐标/权重/颜色 | 局部 `v`、`vn`、`vt`、`vt_w`、`vc` | **仅 EOF**：`9397-9403` `swap` 进 `attrib` | 丢弃（`attrib` 保持入口清空态） |
| 面 | 局部 `prim_group` + `shape` | `g`（`9200-9206`）、`o`（`9253-9260`）、EOF（`9390-9394`） | 在飞组丢弃；**此前已 push 的 shape 保留** |
| 当前材质 id / 平滑组 | 局部 `material`、`current_smoothing_id` | 随面写入 `material_ids` | 已写入的 shape 保留旧 id |
| 材质表 | **直接写调用方的 `materials`** | 每个 `newmtl`：`8319-8325`；文件尾：`8700-8704` | **不回滚，调用方容器也不清空** |
| `material_map` | 局部（但通过 `MaterialReader::operator()` 传出） | 同上 | 不回滚 |

入口只清几何（`9412-9419` / `9486-9493`）：

```cpp
attrib->vertices.clear(); ... shapes->clear();
// 注意：没有 materials->clear()
```

硬失败点：`8910`（`v`）、`8933`（`vn`）、`8945`（`vt`）、`8963`（`vw`）、`9025`（`l`）、`9056`（`p`）、`9091`（`f`）。

**为什么 B 状态是危险的**：`shapes` 里的索引只在 `attrib` 提交后才有意义。`attrib` 与 `shapes` 是两个独立的提交点，失败时只有前者被保证清空，后者没有 → 结构性撕裂。

另外两个细节：

* **越界诊断在 EOF 才统计**（`9366-9388`），失败时提前 return，所以“已经有了越界面索引”的警告**永远不会发出**。实测 `oob_then_bad.obj`：`ret=0`、`shapes` 含 `f 1 2 99` 的悬空面、`warn` 为空。
* `usemtl` 切换时会把上一组 flush 到局部 `shape`（`9126-9131`），但只有后续 `g`/`o`/EOF 才 push。因此“失败发生在 `usemtl` 之后、`g/o` 之前”时，这一组是干净丢弃的——提交粒度是组/对象边界。

### 4.2 Legacy MTL（`LoadMtlInternal`，`8278-8711`）

* `newmtl` 处 flush 上一材质（`8319-8325`），数值解析失败直接 `return false`（如 `8351`/`8363`/`8376`）。
* **当前正在解析的材质不会 flush**，之前 flush 的保留。
* `material_map->insert` 对同名材质是“先到先得”（`8322-8324`）；配合多 `mtllib` 搜索路径会产出**伪装成功**的错配（§8.3）。
* 公开的 `LoadMtl` 返回 `void`（`8713-8718`），调用方只能靠 `err` 文本判断；实测部分材质已留在向量里（`mats=1 red`）。

### 4.3 `ObjReader`（v2 API，`9798-9838`）

```cpp
valid_ = LoadObj(&attrib_, &shapes_, &materials_, &warning_, &error_, ...);   // 9815 / 9833
```

* `warning_` / `error_` **每次 Parse 前都不清空**；`materials_` 也不清空。
* 实测：先失败后成功，`Valid()==1` 但 `Error()` 仍保留上一次的 89 字节错误；连续成功两次 `materials` 2→4→6，而 `shapes[0].material_ids` 从 `0` 变成 `2`、`4`（指向新追加的那份拷贝）。
* 对 Python 绑定（`python/bindings.cc:29-36`）尤为致命：`sample.py` 先 `ParseFromFile` 再 `GetMaterials()` 是安全的，但**同一个 `ObjReader` 解析两次**会得到偏移的 `material_ids`。

### 4.4 回调 API（`LoadObjWithCallbackInternal`，`9501-9786`）

* 无任何暂存结构：`vertex_cb`/`normal_cb`/`texcoord_cb`/`index_cb`/`usemtl_cb`/`mtllib_cb` 逐条直发。
* `vn`/`vt` 使用**不带 err 的旧重载**（`9563`、`9575`），解析失败返回 0 且不报错；实测 `vn 0 0 X` / `vt 0 Y` 被交付为 `vn=(0,0,0)`、`vt=(0,0,0)`，`ret=1`、`err` 为空。
* 面角标使用 `sr_parseRawTriple`（`7409-7435`）→ `atoi`，`f 1 2 x` 交付 `(1, 2, 0)`，`ret=1`。
* 只对 `v` 保留硬错误（`9544-9547`）。
* 不统计 `greatest_*` → **越界正索引完全无警告**：`f 1 2 99` 交付 `99`，`warn` 为空（legacy 至少会警告）。
* `mtllib_cb` 仅在“本次 `mtllib` 解析成功且 `materials` 非空”时触发（`9692-9696`）；部分失败的 MTL 会**既消耗了 `usemtl_cb` 的 id 又不回调材质表**。

### 4.5 `LoadObjOpt` / `LoadObjOptTyped`

| 阶段 | 行为 |
|---|---|
| 入口 | opt 清 `attrib/shapes/materials`（`11946-11957`）；typed 用全新 `OptResult`（`12935` 置 `valid=false`） |
| buffer 版 | `enable_mtllib_loading=false`（`12837-12839`、`13780-13781`）→ **`mtllib` 被静默跳过**，只对 `usemtl` 产生 `material not found` 警告；实测 `mats=0`、`err` 为空 |
| file 版 | 传 `true`（`12917-12919`、`13854-13856`），走 MTL |
| phase 3 错误 | `12343`（opt）/`13304`、`13313`（typed）直接 `return false`，**不清 `materials`**；实测 `comb` 模型 `ret=0` 而 `mats=1（red）` |
| merge 错误 | opt 清全部（`12595-12606`）；**typed 的对应块 `13610-13617` 只 return，不清 `materials`** |
| 文件打开失败 | opt `12895-12898` **不清调用方容器**（实测预置的 `1` 个顶点 + 1 shape + 1 material 全部残留）；typed `13833-13836` 返回的是新建的空 `OptResult`，天然为空 |
| 读取失败 | opt `12910-12913` 同样不清（实测同上）；typed `13848-13851` 返回空 `OptResult` |
| 几何 vs 材质 | typed 在 phase 3 早退时 phase 4 的 arena 分配尚未发生（`13347` 起才 `allocate`），所以 `attrib` 天然为空；**但 `materials` 在 phase 3 已经就地写入 `result->materials`（`13153`）** → 几何原子、材质不原子 |
| 退化面 | opt 保留空 shape，typed 跳过 → `shapes` 数量分歧（§3.3） |
| 行号 | `scalar_find_line_infos` 跳过空行（`tiny_obj_loader.h:11308-11333` 的 `line_len > 0` 判断），**空行会被吞掉**：实测 `f` 在第 5/7 行时，opt 报 `line 4`；legacy 报正确行号。且错误文本没有列号/源码片段，比 legacy 的诊断弱 |

### 4.6 experimental stream（`experimental/stream/stream_obj_loader.cc`）

* `MeshBuilderHandler` 构造时清 `attrib/shapes/materials`（`508-516`）；失败时 `LoadObjStreamExperimental` 再清一次（`1241-1249`、`1299-1307`）。
* **文件打开失败不清**（`1269-1274`）；实测预置的 `verts=1, shapes=1, mats=1` 全部残留。
* `ParseObjStream` 在遇到错误 chunk 时会**先把错误 chunk 内“错误发生之前”已解析好的事件回放给 handler**（`1199-1209`，`error_chunk_index + 1` 个 chunk），随后才 `return false`（`1211-1216`）。实测：6 行输入中第 6 行是坏的 `v OOPS 0 0`，`ParseObjStream` 返回 `false`，但 handler 已经收到 **5 个 `OnVertex`**。因此直接使用 `StreamHandler` 的调用方**必然看到部分事件**（无法回滚）；`LoadObjStreamExperimental` 之所以“干净”，只是因为它事后清空了容器。
* 面角标 `f 1 2 x` → `ParseRawTripleToken` 失败 → 硬错误（`905-908`），与 C++ 回调的“静默变 0”相反 → 同一份坏输入在不同 API 上一个成功一个失败。
* 截断（`f 1 2`）：交付 `n=2` 事件，handler 记 `OnDegenerateFace`，整体 `ret=1`。

### 4.7 C API（`tiny_obj_c_impl.inc`）

| 场景 | 行为 |
|---|---|
| 入口/失败 | `tobj_memset(out, 0, ...)`（`1523`、`1625-1629`；`1638-1643` 的 io 版同样）→ **非 `OK` 时输出天然为空**，这是全部路径中唯一严格遵守“失败即空”的 |
| `TOBJ_OK` 定义 | `tiny_obj_c.h:60-61` 注释：“`TOBJ_OK` 即使有警告也返回……任何非零码是硬失败，输出被清零” |
| `v` 分量不足 | 补 0 并警告（`205-208`），继续；截断的 `v 1 0` 变成 `(1,0,0)` |
| 角标语法非法 | `parse_corner_n` 返回 false（`135-164`）；`parse_corner` 只发警告并保留 `-1`（`165-171`）→ **`TOBJ_OK` 但角标含 `-1`** |
| 越界正索引 | **完全不校验**（`parse_corner_n` 只检查相对索引 `<0`）→ 输出 `98`，无警告，`TOBJ_OK` |
| 三角形化遇到坏索引 | 警告 + fan fallback，**写入索引 0 和 `-1`**（`300-332`） |
| 回调版 | 用 `parse_corner_n` 直接填槽（`1911`），**不发任何警告**；`f 1 2 x`/`f 1 2 99` 都是 `TOBJ_OK` + `warn` 为空 |
| `from_io` | `got == 0` 视为 EOF（`1680`）；无法区分“正常结束”和“传输中断” |
| `from_file` | `fread` 短读不报错，`fb->len = rd`（`tiny_obj_c.c:842-846`） |
| `mtllib` | **无去重**：同一文件名会被 resolver 两次（`814-839`）→ 两份同名材质、`id` 与 C++ 不一致（实测 `mats=2`；C++ 为 1） |
| MTL 数值非法 | `read_reals` 静默跳过（`547-553`）→ `Ks BAD` 保留 `(0,0,0)`，**材质完整、`TOBJ_OK`**（与 C++ 的“截断材质表”相反） |
| `TOBJ_ERR_PARSE` | 枚举存在（`tiny_obj_c.h:67`）、字符串存在（`tiny_obj_c.c:719`），但**全库 0 处使用** |

**C API 的自洽性实测**：用 `tests/tester_c.c:19-33` 的 `mesh_consistent_f` 检查 `f 1 2 99` 的返回场景：

```text
result=ok verts=3 consistent=0
```

即 `TOBJ_OK` 的结果违反了它自己测试套件里的不变量。另见 §9.4 的 `from_io` 截断实验：`result=TOBJ_OK, shapes=1`，但输入只给了 2/3。

---

## 5. 材质引用到底“保留到哪里”

### 5.1 三种不同的失败点，三种不同结果

以 §3.2 的 `bad_mtl.obj` 为例：

```
materials = [red]                 // blue 被截断丢弃，green 从未读到
shapes[0].material_ids = [0]      // quad → red
shapes[1].material_ids = [-1]     // tri → usemtl blue 落空
```

而调用方看到的是 `ret == true`。如果调用方把 `material_ids[i]` 直接当 `materials` 下标使用（README 的写法），`-1` 会越界；如果它在后续再加载第二个 MTL，“有效 id”还可能指向另一个列表。

### 5.2 opt/typed 的“假清空”

`bad_v.obj`（几何硬失败）：

```
LoadObjOpt(file)   ret=0, shapes=0, vertices=0, materials=[red, blue]
LoadObjOptTyped    valid=0, shapes=0, vertices=0, materials=[red, blue]
```

几何被清，材质没被清。原因：`material_dst` 直接指向调用方向量（`12127`、`13153`），phase 3 的 MTL 在面校验之前/之中完成；错误返回点（`12343`/`13304`）不回滚材质。

因此**“部分结果”在这里分裂成两半**：一半被清零，一半被保留，且没有任何标志说明保留的那一半属于哪个“旧事务”。

### 5.3 同名材质的静默错配（最隐蔽的一类）

`shadow.obj`：第一个 MTL 里有一个被截断的 `blue`，第二个 MTL 里有完整的 `blue`。

```text
mtllib shadow1.mtl valid.mtl
v 0 0 0
v 1 0 0
v 1 1 0
usemtl blue
f 1 2 3
```

```mtl
# shadow1.mtl
newmtl blue
newmtl red
Kd 1 0 0
Ks BAD 0 0
```

实测：

```
ret=1 mats=3 [blue, red, blue]
face matid=0 -> 'blue' Kd=(0,0,0)     // 指向被截断的那份，全黑
err='... shadow1.mtl:4:4: error: expected number, got 'BAD''
```

`material_map` 用 `insert` 保留第一次（`8319-8325`、`8322-8324`），所以面引用的是**残缺定义的材质**，而完整的 `blue` 在 `materials[2]` 无人使用。同理，同一 `mtllib` 行写两次会重复解析、重复追加（实测 `mats=[red, red]`、`err` 两份拷贝），因为 `material_filenames` 只在 `operator()` **成功**后才插入（`9176-9179`）。

---

## 6. 调用方能否安全复用输出 / 重新导入

### 6.1 复用判定

| API | `false`/非 OK 后的输出 | 现在可复用？ | 需要做什么 |
|---|---|---|---|
| legacy `LoadObj(file/istream)` | 几何空、`shapes` 可能是状态 B、`materials` 可能部分追加 | **否** | 必须先手动 `shapes.clear()`，而 `materials.clear()` 还要看是否希望保留预置项 |
| `ObjReader` | `valid_==0`，但 `GetShapes()` 可能有 B 状态；`Warning/Error` 可能是旧文本；`materials_` 累积 | **否** | 必须新建一个 `ObjReader`，或至少 `Error()` 不能作为“本次失败”证据 |
| callback | 已交付的部分数据 | **否** | 必须自己实现两阶段提交（先写临时缓冲，成功后再合并） |
| `LoadObjOpt(file)` | phase3 失败留材质；打开/读取失败连几何都留 | **否**（部分场景） | 用 typed（`valid` 是结果内字段）或先清空 |
| `LoadObjOptTyped` | `valid==false` 但 `materials`/`attrib` 可能残留 | **否**（必须看 `valid`，不能只看容器） | 只信 `valid`，失败即丢弃整个 `OptResult` |
| experimental stream | parse 失败已清，打开失败未清 | **否**（打开失败） | 同上 |
| C API | 非 `OK` 已全部清空 | **是** | — |

### 6.2 重新导入（使用同一容器 / 同一 reader）

* **legacy 容器**：`LoadObj` 每次入口清 `attrib/shapes`，所以几何不会累积；但 `materials` 会（2→4→6，`material_ids` 同步偏移，§4.3 同款问题）。因此“失败后在同一容器重新导入”虽然几何正确，**材质 id 全错**。
* **`ObjReader`**：材质累积 + 过期错误文本，**任何“重试同一对象”的用法都不安全**。
* **`MaterialStreamReader` 是一次性的**：第二次 `LoadObj` 调用时流已耗尽，实测第二次 `mats=0`、`usemtl id=-1`，且 `ret=1`。若调用方把它当作可复用的 material resolver 就是静默丢材质（`tiny_obj_loader.h:8826-8842` 没有重置流）。

```cpp
MaterialStreamReader msr(mtl);        // 一个流
for (...) LoadObj(..., &msr, ...);    // 第 2 次开始就静默丢材质
```

* 重新导入**新文件但复用同一 `istream`** 更危险：失败时 `StreamReader` 可能已消费到错误行，重试会从中间开始。实测第二次在同一 `istringstream` 上重试 → `ret=1, verts=0, shapes=0`（成功但空）。

---

## 7. 哪一层应负责隔离部分结果

**结论：loader/API 边界负责隔离，调用方不应承担。**

理由（按重要性）：

1. **索引与顶点数组必须原子**。`index_t` 只有在 `attrib` 数组完整提交后才有意义；任何“只保留一半”的语义都会让调用方在无边界检查的循环里越界（§8.1 已证 SEGV）。这个约束只有 loader 知道，调用方无法从 `ret` 推断。
2. **`material_ids` 是相对下标**，与 `materials` 向量互为契约。部分 MTL + 部分几何时 id 仍“看起来合法”，但指向错误条目（§5.3）。清空/回滚必须发生在拥有这两个容器的层。
3. **同一失败在不同 API 上语义不同**（§3），调用方无法写出一套通用防御逻辑；只能在 API 边界统一。
4. `LoadObjInternal` 已经对 `attrib` 使用“本地暂存 + EOF swap”的模式（`9397-9403`），把同样模式扩展到 `shapes` 与 `materials` 是最小改动、且可复现既有成功路径。

分层职责建议：

| 层 | 负责 | 不该负责 |
|---|---|---|
| **MTL 解析层**（`LoadMtlInternal` / `parse_mtl_buffer`） | 一次 MTL 解析要么完整产出、要么不产出（本地 staging + 成功时 append）；返回 bool（C++ `LoadMtl` 也应改签名或加 bool 变体） | 不决定跨文件的 `material_map` 语义 |
| **原子化加载器**（`LoadObj`、`ObjReader`、`LoadObjOpt(Typed)`、C `tobj_load_obj_*`） | `false` ⇒ 输出为空或等于调用前的值；`valid` 是唯一权威；清空/不累积 `warn/err`；`materials` 也要原子 | 不替调用方做“部分结果”决策（提供显式选项即可） |
| **流式 API**（callback、`StreamHandler`） | 明确“已交付即不可撤回”；不合成假值（禁止 `0` 代替坏角标）；对每次丢弃/降级发诊断；文档写明回调顺序与失败时的回滚边界 | 不承诺原子性 |
| **调用方** | 只在拿到 `true/valid/TOBJ_OK` **且** 诊断可接受时消费；可选地 `keep_partial` | 不负责清理 |

---

## 8. 建议与兼容性权衡

### 8.1 P0：失败时清除“已提交的 shapes”（legacy / ObjReader）

现状：`bad_v.obj` 下 `ret=0, shapes=1, verts=0`，索引悬空。

```cpp
// 复现：ASan 直接 SEGV
bool r = LoadObj(&a, &s, &m, &w, &e, "/tmp/objpartial_final/bad_v.obj", "...");
int vi = s[0].mesh.indices[0].vertex_index;     // 0
volatile real_t x = a.vertices[3*vi];           // a.vertices.data() == nullptr
// => AddressSanitizer: SEGV on unknown address 0x000000000000
```

建议：在 `LoadObjInternal` 的每次失败返回处（`8910/8933/8945/8963/9025/9056/9091`）统一 `shapes->clear()`（或改为先在局部 `staged_shapes` 累积，EOF 时一次性 `swap`）。

兼容性：
* 无现有测试在 `false` 分支断言输出内容（扫描 `tests/tester.cc`：`test_malformed_*`、`test_invalid_*`、`test_objreader_api_error` 只断言 `ret==false` / `!err.empty()`；`test_numeric_overflow_preserves_default` 的 fast_float 分支同样只断言 `!err.empty()`，另一分支是 `ret==true` 且断言 `inf`）。
* README 的消费示例只在 `ret == true` 后读数据，符合新语义。
* 若有客户依赖“best effort”，提供 `LoadObjOptions{bool keep_partial}` 或新函数 `LoadObjPartial`，默认 `false`，可完全保留旧行为。

### 8.2 P0：材质原子化

建议：MTL 解析到本地 `std::vector<material_t>`，成功后 append/assign；失败则丢弃。入口清空 `materials`（文档写明“输出被覆盖”）。

兼容性风险：现有 legacy 语义允许调用方预置材质（不 clear）。但预置本身已经不自洽（`material_ids` 从 0 开始，会与预置项错位；实测 `PREMAT` 在 `[0]` 时面的 id 变成 `1/2`）。`examples/viewer/viewer.cc:619` 是在 `LoadObj` 之后 `push_back` 默认材质，不受影响。为降低风险，可先用“失败时不追加 + 成功时 append”，并把“清空”作为显式文档变更。

### 8.3 P1：同名材质不得被残缺定义“占位”

建议：`material_map` 用 `insert`（先到先得）改为“只在该 MTL **完整解析成功**后注册”；或在 `LoadMtlInternal` 内先收集到局部 map，成功后再合并；失败则整个文件不生效并报 warning（而不仅是 err）。

理由：§5.3 的 `shadow.obj` 会静默地把面指到全黑材质。修完后应保证“失败文件零贡献”，与“partially successful file 不应改写 material_map”一致。

兼容性：多 `mtllib` 中“前一个坏、后一个好”的正常场景（`two_mtllib` 实测 `mats=3` 且 `blue -> [2]`）在修复后会变成 `mats=2`，更符合直觉；没有测试固定 `mats=3`。

### 8.4 P0：C API 的 `TOBJ_OK` 必须意味着自洽

建议：
* `parse_corner_n` 对正数索引也做上界校验（`n` 参数已经在手上），越界 → `error_line` + `TOBJ_ERR_PARSE`，与 C++ 的 `f 1 2 -99` 行为对齐。
* 数组不足 3 分量、`f 1 2 x` 这类“可恢复降级”保留 `TOBJ_OK`，但**必须**保证降级后的索引不为 `-1`（改为跳过该面或 `TOBJ_ERR_PARSE`）；至少在 `mesh_consistent` 意义上不变量成立。
* 触发 `TOBJ_ERR_PARSE`（目前 0 使用），让“任何非零码 ⇒ 输出清零”的契约真正可用。
* `from_io`：`got == 0` 无法区分 EOF/中断。建议新增回调语义“返回非 `OK` 即中断”，文档要求传输层在短读时返回错误；或在 `read` 返回 `TOBJ_OK` 且 `got==0` 后做一次“协议结束”确认。
* `from_file`：`fread` 短读（`tiny_obj_c.c:842-846`）应比对 `rd == sz`，否则返回 `TOBJ_ERR_IO`。

兼容性：`tests/tester_c.c` 的 `kCorpus` 全是合法模型，`mesh_consistent_f` 只在 `TOBJ_OK` 时调用；严格化不会破坏现有 C 测试。C++ 侧 `test_out_of_range_face_index`（`tests/tester.cc:3310-3327`）断言 `f 1 2 999` → `ret==true`，所以 **C++ 的越界策略不能直接改成硬错误**，否则破坏既有测试。

### 8.5 P1：越界正索引（C++ 四条路径）——保留成功但要可判定

现状：`fixIndex` 只做 `idx-1`，不校验（`tiny_obj_loader.h:6404-6406`），`f 1 2 99` 在 legacy/opt/typed/exp 全部 `ret=1`，输出 `98`；警告只在 EOF 汇总且行号是 EOF（`9366-9388`）。

建议（不破坏 `test_out_of_range_face_index`）：
* 保留 `true`，但把警告改为“逐面 + 精确行号”，并在 `warn` 中给出 `attrib.vertices.size()` 边界；
* 新增 `ObjReaderConfig`/`OptLoadConfig` 的 `strict_indices`（默认 false）：开启后越界正索引 → `false` + 错误，与 `-99` 一致；
* 文档明确规定“消费方必须做上界检查”，因为 `fixIndex` 的契约只保证“相对索引在范围内”。

### 8.6 P1：`ObjReader` 每次 Parse 清空 `warning_`/`error_`/`materials_`

建议：`ParseFromFile/ParseFromString` 开头清三个成员；或让 `LoadObj` 填充局部再赋值（原子）。

兼容性：所有测试一次解析一个新 `ObjReader`；Python `sample.py` 也是单次解析。风险极低，收益是“错误文本可作本次结果证据”。

### 8.7 P1：流式 API 的诊断与不造假

建议：
* C++ callback：`vn`/`vt` 改用带 err 的重载（`sr_parseReal3(&x,&y,&z,sr,err,filename)`），畸形值 → `false`；`index_cb` 前校验角标，坏角标不得以 `0` 交付（至少发 warning 并跳过该面）；补做 `greatest_*` 汇总（哪怕是 EOF/失败时）。
* C callback：`parse_corner_n` 失败时发 warning（目前完全静默，`1905-1922`）。
* 文档写明“回调是部分交付，失败不撤回已交付数据”，并对 `num_indices < 3` 的行为给出明确契约。
* experimental `ParseObjStream`：明确“错误 chunk 会被回放后再返回 false”（`1199-1216`）；对直接使用 `StreamHandler` 的调用方给出 `OnError` 回调或“最后一条事件即失败边界”的约定。

### 8.8 P2：文档与 API 注释

统一写清楚：

* 原子化 API：`false` 后输出“未定义/为空”；**不得**消费。
* “成功但 `err` 非空”是合法组合（MTL 部分失败），把 `err` 非空视为“结果可降级”。
* `materials` 与 `material_ids` 的相对下标契约，以及 `-1` 的含义。
* buffer 版 `LoadObjOpt`/`LoadObjOptTyped` **不加载 `mtllib`**（`enable_mtllib_loading=false`），需要材质时必须用 file 版或自行解析 MTL。
* `MaterialStreamReader` 一次性。
* 截断检测在库内不可实现（流边界即 EOF）；要检测必须在 IO 层。

### 8.9 兼容性总表

| 修改 | 破坏的既有行为 | 风险 | 建议 |
|---|---|---|---|
| 失败清 `shapes` | 无测试；可能有客户依赖 best-effort | 低 | 默认开启，另加 `keep_partial` |
| 失败清/不追加 `materials` | 预置材质的客户（本就 id 错位） | 中 | 文档变更 + staging append |
| `ObjReader` 每调用清诊断/材质 | 无测试 | 低 | 直接修 |
| MTL 失败文件零贡献 | `two_mtllib` 结果从 3 变 2（无测试） | 低 | 直接修 |
| C 越界改 `TOBJ_ERR_PARSE` | C 测试全合法，`mesh_consistent` 仅 OK 时用 | 低 | 直接修 |
| C++ 越界改硬错误 | `test_out_of_range_face_index` 断言 `ret==true` | 高 | 只加 `strict_indices` 选项 |
| callback 畸形 `vn/vt` 改硬错误 | 无测试固定该行为 | 中 | 至少加 warning；硬错误放 strict |
| 保留空 shape（退化面） | `test_invalid_face_definition` 断言 1 个空 shape | 高 | 保持；只修 typed 的分歧（记录在案） |
| opt 行号修正（不吞空行） | 无测试断言错误文本行号 | 低 | 直接修 |

---

## 9. 探针与输出

### 9.1 编译

```bash
CL=/Users/dengquan/Downloads/job/code-annotation/fb2-workspace/frank-3991/0038-tinyobjloader-5-e5e8032b
clang++ -std=c++11 -O0 -g -I"$CL" -I"$CL/experimental/stream" \
  probe.cc "$CL/experimental/stream/stream_obj_loader.cc" -pthread -o probe
```

`probe.cc` 的结构（矩阵探针）：

```cpp
static void run(const char *path) {
  std::string text = slurp(path);
  // legacy file / legacy istream（显式 MaterialFileReader）
  // ObjReader::ParseFromFile / ParseFromString（string 版传对应 mtl）
  // LoadObjWithCallback（统计 v/vn/vt/face_cb/corners/usemtl/mtllib）
  // LoadObjOpt(file) / LoadObjOptTyped(file)
  // experimental_stream::LoadObjStreamExperimental(istream)
}
```

C 侧探针：

```bash
clang -std=c11 -O0 -g -I"$CL" probe.c "$CL/tiny_obj_c.c" "$CL/tobj_tess.c" -lm -o cprobe
```

### 9.2 `bad_v.obj` 的 legacy 诊断（精确输出）

```text
ret=0
attrib.vertices = 0
shapes[0] name='quad' faces=1 idx=3 matids=0  indices=[0 1 2]
ERR: /tmp/objpartial_final/bad_v.obj:10:3: error: expected number, got 'OOPS'
v OOPS 0 0
  ^
```

对照表：同一输入若把 `v OOPS` 移到第一个 `g` 之前（`bad_v2.obj`），则 `ret=0, verts=0, shapes=0` —— 说明**“部分结果”是否存在，只取决于失败点落在哪个组边界之间**。

### 9.3 `trunc_f.obj` 的跨 API 分歧

```text
legacy        ret=1 verts=3 shapes=1 faces=0 warn='Degenerated face found'
opt           ret=1 verts=3 shapes=1 faces=0
typed         valid=1 verts=3 shapes=0 faces=0
callback      ret=1 verts=3 face_cb=1 corners=2 warn=''
experimental  ret=1 verts=3 shapes=1 faces=0
```

### 9.4 C `from_io` 截断

```c
const char *full = "v 0 0 0\nv 1 0 0\nv 1 1 0\ng a\nf 1 2 3\ng b\nusemtl x\nf 1 2 3\n";
/* 只提供前 2/3 字节，io->read 在 EOF 返回 TOBJ_OK + got=0 */
```

```text
C io truncated at 38/57: result=TOBJ_OK verts=3 shapes=1
  shape[0] faces=1 idx=3
  warn='' err=''
```

即：截断被当作正常结束，无诊断，`TOBJ_OK`。

### 9.5 C 不变量破坏

```text
input: "v 0 0 0\nv 1 0 0\nv 1 1 0\nf 1 2 99\n"
result=ok verts=3 consistent=0
```

（`consistent` 是 `tests/tester_c.c:19-33` 的 `mesh_consistent_f` 的等价实现：要求每面顶点数之和等于索引数，且正索引不超过顶点数。）

### 9.6 直接使用 `StreamHandler` 时的不可撤回交付

```cpp
struct H : tinyobj::experimental_stream::StreamHandler { /* 只统计 */ };
// 输入：5 行正常 v，第 6 行 "v OOPS 0 0"
bool r = ParseObjStream(&iss, &h, &w, &e, "<mem>", StreamLoadConfig());
```

```text
ParseObjStream ret=0 delivered_vertices=5 faces=0
err='line 6: malformed vertex coordinates'
```

`ret == false` 时调用方已收到 5 个顶点；这是 `ParseObjStream` 的 `error_chunk_index + 1` 回放策略（`stream_obj_loader.cc:1199-1209`）的直接后果，**不是 bug 而是语义**——必须在文档中写明，或加 `OnError` 回调。

---

## 10. 一句话回答

* **顶点属性**：失败后不保留（`attrib` 被清空，局部 staging 丢弃）。
* **面**：失败前已过 `g`/`o`/EOF 提交的形状**保留在 `shapes`**，但其索引指向已丢弃的顶点数组 → 悬空；未过边界的那一组干净丢弃。
* **材质引用**：已写入 `material_ids` 的 id 保留，可能指向被截断/被同名残缺定义占位的条目；`usemtl` 找不到时是 `-1`。
* **材质结果**：每个已 flush 的 `newmtl` 都保留在调用方向量里（legacy 甚至不清空入口），不随失败回滚；`materials` 与 `shapes`/`attrib` 的清空策略不一致。
* **可复用性**：所有返回 `false`/`valid==false` 的原子化 API 的**输出都不可安全复用**；`ObjReader`/`MaterialStreamReader` 的“重试”也不安全。只有 C API 在非 `OK` 时保证输出清零，但其 `TOBJ_OK` 本身不保证自洽。
* **隔离层**：应由 loader（原子化 API）与 MTL 解析层负责回滚/不追加，流式 API 负责“不造假 + 完整诊断 + 明确的非原子契约”。
* **兼容性**：清除失败时的 `shapes`/`materials`、修 `ObjReader` 状态、让 C 使用 `TOBJ_ERR_PARSE` 风险低；越界正索引的硬化与“去掉空 shape”会破坏既有测试，应通过 opt-in 的 strict 模式或保持现状 + 文档。
