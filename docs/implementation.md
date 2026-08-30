# 3D Gaussian Splatting 流体渲染实现总结

> 本文档对照 `docs/theory.md` 的理论推导,逐节说明本项目中 3D Gaussian Splatting 渲染的**实际实现**、**工程参数**以及与理论公式的**偏差及原因**。实现以 `GaussianRenderer`(CPU 几何/外观) + `splat_vertex/fragment.glsl`(GPU 光栅化)为核心,完全替代了原来的 Marching Cubes 表面重建路径。

---

## 1. 总体架构

```
FluidSolver (SPH 状态: x, v, rhoN, spacing, dt)
        │
        ▼
GaussianRenderer::Compute()          ← CPU, 每 N 帧(默认 3)后台线程
  ├─ 均匀网格邻居搜索(flat SoA, cell = 2·spacing)
  ├─ color field C_i / 邻居数        (theory §4.1)
  ├─ 法线 n_i + 法线平滑 ×3          (theory §4.3, 工程增强)
  ├─ 应变率张量 D_i + Jacobi 特征分解 (theory §5)
  ├─ 曲率 κ_i                        (theory §7.3)
  ├─ 旋转 R / 尺度 S / 协方差 Σ      (theory §6-8)
  ├─ 渲染色 c_i / 透明度 α           (theory §10-12)
  └─ 颜色空间平滑(工程增强)
        │
        ▼  Splat { center, cov[6], color[3], alpha }
OpenGLLayer::DrawGaussianSplats()    ← 每帧
  ├─ CPU 按相机深度远→近排序         (theory §17)
  ├─ instanced quad(每 splat 4 顶点) (theory §15-16)
  └─ fragment: 硬边缘椭圆 + premultiplied alpha 混合
```

对应 theory.md Part V 的 11 步管线:步骤 1-6(color field、法线、应变率、曲率、R/S 与 Σ)在 `ComputeColorField` / `ComputeSurfaceAniso` / `BuildSplats`(Pass C)中完成;步骤 7-9(Σ 组装、颜色、透明度)在 `BuildSplats`(Pass A/C)中完成;步骤 10-11(投影、屏幕椭圆、排序混合)在 GPU shader + `OpenGLLayer::DrawGaussianSplats` 中完成。

---

## 2. 逐节对照 theory.md

### Part II — 几何(§4-8)

#### §4 Color Field 与表面法线

| 理论 | 实现 |
|---|---|
| $C_i = \sum_j \frac{m_j}{\rho_j} W_{ij}$,Poly6 核 | `ComputeColorField`: 用归一化密度 $\rho_N$,体积 $V_j = \text{spacing}^3/\rho_{N,j}$;核用 Poly6(附录 B) |
| $\mathbf n_i = -\nabla C_i / \lVert\nabla C_i\rVert$ | `ComputeSurfaceAniso` pass A + **法线平滑 3 次迭代**(等权平均可见邻居法线) |

**偏差 1 — surfaceThreshold 取值**:theory 建议 $C_i < 0.5$ 为表面。实测有限分辨率 SPH 核下,立方体最外层(顶面)粒子的 $C \approx 0.93$(而非连续体理论的 0.5),次表层 $\approx 1.04$。因此阈值取 **0.95**,使整个最外层(顶面/侧壁)进入表面 pass;否则顶面被判为内部、以低透明度深蓝渲染,整个立方体顶面变暗(初版实测顶面仅 44 个表面粒子,应为 400+)。

**偏差 2 — 法线平滑**:SPH 离散法线含噪声,直接用于 Blinn-Phong/Fresnel 会在平坦表面产生亮暗条纹(相邻像素亮度差 ~5.7 vs MC 1.3)。迭代平滑 3 次后降为可接受水平。

#### §5 Velocity Gradient 与主拉伸方向

| 理论 | 实现 |
|---|---|
| $\nabla\mathbf v_i = \sum_j V_j(\mathbf v_j-\mathbf v_i)\nabla_i W_{ij}^T$ | `ComputeSurfaceAniso` pass A,同公式 |
| $\mathbf D_i = \tfrac12(\nabla\mathbf v + \nabla\mathbf v^\mathsf T)$,谱分解得 $\lambda_1 \ge \lambda_2 \ge \lambda_3$、$\mathbf t_1$ | `SymEigen3`: 自实现 **Jacobi 旋转**(3×3 对称,迭代 ≤32,特征值降序) |

#### §6 旋转矩阵 R

| 理论 | 实现 |
|---|---|
| $\mathbf r_3 = \mathbf n_i$;$\mathbf r_1 = \text{normalize}(\mathbf t_{\max} - (\mathbf t_{\max}\cdot\mathbf r_3)\mathbf r_3)$;$\mathbf r_2 = \mathbf r_3\times\mathbf r_1$ | 完全按公式;退化情形($\mathbf t_{\max}\parallel\mathbf r_3$)回退到任意切向量 Gram-Schmidt |

**偏差 3 — 孤立粒子 isotropic 回退**:theory §7.5 只对内部粒子各向同性。实测发现**飞溅/立方体棱角**等邻居稀少的表面粒子($\text{邻居}<12$)应变率方向是噪声,各向异性拉伸会渲染成"长方形碎片"。故对 $\text{邻居}< \text{isoNbThreshold}=12$ 的表面粒子强制 isotropic(球)。这同时让飞溅渲染为圆润水滴、立方体棱角不再尖锐。

#### §7 尺度 S(体积守恒)

| 理论 | 实现 |
|---|---|
| $s_1^* = r(1+\alpha\max(0,\lambda_1)\Delta t)$,cap $r_{\max}$ | `stretchAlpha=1.2`, `rMaxFactor=2.0`(弱化拉伸,避免长条) |
| $\varepsilon = \text{clamp}(\varepsilon_{\min} + \frac{\kappa_i}{\kappa_{\max}}(\varepsilon_{\max}-\varepsilon_{\min}))$, $s_3 = r\varepsilon$ | `epsMin=0.4`, `epsMax=0.8`, `kappaMaxFactor=2.0`;另有 **s3MinFactor=0.45** 下限 |
| $s_2 = r^3/(s_1 s_3)$,floor | `s2MaxFactor=2.5`, `s2MinFactor=0.35`(双向 clamp) |

**偏差 4 — 各参数下限/上限**(工程需要,理论为"建议起始值"):

- `epsMin=0.4`(理论 0.05):太扁的椭圆在斜视角下投影退化为 ~1px 细线,侧面消失、顶面行间露缝。0.4 保证掠射角下仍有 ~3px 屏幕厚度。
- `s3MinFactor=0.45`:垂直棱/角的 splat(法线沿视线方向)若 $s_3$ 过薄,投影为发丝线,棱处露出暗内部 → 立方体"棱深"。下限保证棱处有厚度。
- `s2MaxFactor=2.5`:理论 $s_2 = r^3/(s_1 s_3)$ 在 $\varepsilon\to0.05$ 时爆炸到 $20r$,产生比 tank 还大的巨盘;cap 到 2.5r。
- `s2MinFactor=0.35`:防止拉伸后 $s_2$ 过细成针。
- **splatScale=1.6**:整体放大椭圆,使相邻表面粒子(间距 1r)的椭圆重叠 ~3 倍,保证硬边缘无缝隙、无逐粒子斑点。透明度用**未放大**体积计算,故 α 保持校准值。

#### §8 协方差 Σ

$\Sigma = \sum_k s_k^2 \mathbf r_k \mathbf r_k^\mathsf T$(外积和),完全按理论。

### Part III — 外观(§10-12)

#### §10 漫反射基色

| 理论 | 实现 |
|---|---|
| $\mathbf d^{\text{foam}} = (1-u)\mathbf d_{\text{deep}} + u\mathbf d_{\text{foam}}$, $u = v/v_{\max}$ | 保留 foam 叠加,但 **$u^2$ 缓化** + `foamVMax=4.0` |
| $\mathbf d^{\text{thick}} = (1-C)\mathbf d_{\text{shallow}} + C\mathbf d_{\text{deep}}$ | **放弃**(见偏差 5) |
| $\mathbf d = \mathbf d^{\text{foam}} \odot \mathbf d^{\text{thick}}$ | 放弃元素乘 |

**偏差 5 — 统一基色替代 C 驱动厚度色**:理论用 $C$ 驱动"浅→深"厚度色。实测 $C$ 在立方体角(≈0.3)/棱/面(≈0.93)差异极大,导致**角发白、面偏暗**、颜色跳跃。改为**统一深蓝基色** $\mathbf d_{\text{base}} = 0.8\,\mathbf d_{\text{deep}} + 0.2\,\mathbf d_{\text{shallow}}$,明暗完全由(平滑)法线驱动光照产生,类似 MC mesh 的单色+光照。

#### §11 高光

Blinn-Phong $\mathbf s = \mathbf k_s \max(0,\mathbf n\cdot\mathbf h)^\gamma$:`specGamma=96`, `specStrength=0.25`。`specStrength` 偏高会把法线噪声放大成白色条纹,故取低值。

#### §12 透明度

$\alpha_i = \min(1, \frac{r^3}{\sqrt{\det\Sigma}}\alpha_{\text{base}})$:`alphaBase=0.95`(接近不透明,消除雾感)。

**偏差 6 — Fresnel 天空反射 + 内部填充**(视觉增强,理论未含):

- **Fresnel 天空反射**:$\mathbf c = \mathbf c\,(1 - f_{\text{refl}} f_{\text{res}}) + \mathbf c_{\text{sky}}\,f_{\text{refl}} f_{\text{res}}$,$f_{\text{res}}=(1-\mathbf n\cdot\mathbf v)^2$。掠射角反射天空蓝(对应旧 MC surface shader 的 sky/water fresnel mix),`fresnelReflect=0.45`, `skyColor=(0.40,0.62,0.92)`。强度过高会整面泛白(雾感)。
- **内部粒子填充**:theory §7.5 的内部 isotropic 球**实际渲染**(`interiorAlpha=0.25`,颜色深蓝)。否则水体是空壳,半透明表面透出背景 → "空洞/空"。

### Part IV — 光栅化(§13-17)

#### §14-15 视图变换与透视投影

完全按理论:相机空间 $\Sigma_c = R\Sigma R^\mathsf T$($R=\text{mat3}(view)$),屏幕协方差 $\Sigma' = JW\Sigma W^\mathsf T J^\mathsf T$,焦距 $f = \frac{H/2}{\tan(\text{fov}/2)}$,像素中心 $(f x_c/z_c + W/2,\; H/2 - f y_c/z_c)$。

**偏差 7 — 像素坐标翻转(关键 bug 修复)**:vertex shader 中屏幕坐标 $y$ 是"从上往下",而 `gl_FragCoord.y` 是"从下往上"。若不翻转 `vCenterPx.y = viewport.y - py`,fragment 的 Mahalanobis 距离在**除屏幕中心行外全部错误**(被 discard)→ 只有 y≈H/2 一行可见(即早期"水只显示中间一条"的现象)。

#### §16 屏幕空间高斯权重

**偏差 8 — 硬边缘椭圆替代高斯(决定性改进)**:

理论是 $\exp(-\tfrac12 \mathbf d^\mathsf T\Sigma'^{-1}\mathbf d)$ 的软高斯。实测即使 $\exp(-3m^2)$ 仍有 5-12px 羽化边缘,整面"像被雾蒙住"——因为 MC 是不透明硬边 mesh。最终 fragment 改为:

```glsl
float w = (m2 < 9.0f) ? 1.0f : 0.0f;   // 3σ 内实心,硬切
```

配合 `splatScale=1.6` 保证硬边无缝隙。量化效果:亮度分布 82.6% 落在 64-96(MC 90.3%),相邻像素差 1.0-2.8(MC 1.2),顶面背景像素 1.9-4.2%。

#### §17 Alpha 混合

按理论远→近排序 + over 算子。实现用 premultiplied alpha:`FragColor = vec4(vColor * a, a)`,`glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`。depth test 关闭(透明排序代替)。

---

## 3. 参数表(默认值)

| 参数 | 值 | 对应 theory |
|---|---|---|
| `surfaceThreshold` | 0.95 | §4.3(实测 C 校准) |
| `isoNbThreshold` | 12 | §7.5 延伸(孤立粒子 isotropic) |
| `stretchAlpha` | 1.2 | §7.2 |
| `rMaxFactor` | 2.0 | §7.2 |
| `epsMin` / `epsMax` | 0.40 / 0.80 | §7.3(工程下限) |
| `kappaMaxFactor` | 2.0 | §7.3 |
| `s3MinFactor` | 0.45 | §7.3(棱厚度) |
| `s2MaxFactor` / `s2MinFactor` | 2.5 / 0.35 | §7.4(防巨盘/细针) |
| `splatScale` | 1.6 | §7(覆盖,硬边无缝隙) |
| `isoBlendLo/Hi` | 0.85 / 0.95 | §7.5 |
| `alphaBase` | 0.95 | §12 |
| `interiorAlpha` | 0.25 | §7.5(体积填充) |
| `deepColor` / `shallowColor` | (0.06,0.26,0.48) / (0.50,0.70,0.95) | §10(MC 风格深蓝) |
| `ambient` | 0.20 | —(增强) |
| `fresnelRim` | 0.20 | —(增强) |
| `fresnelReflect` / `skyColor` | 0.45 / (0.40,0.62,0.92) | —(增强) |
| `foamVMax` | 4.0 | §10(防过曝) |
| `specGamma` / `specStrength` | 96 / 0.25 | §11 |

---

## 4. 调试过程中发现的关键问题(供后续维护参考)

1. **rhoN 未初始化**:求解器首步 `Step()` 前 `Densities()` 全 0 → $V_j=\text{spacing}^3/\rho_{N,j}$ 除零 → color field 全 NaN → 0 个 splats(GUI 切到 surface 模式"看不见水立方体",只剩 tank 线框"三条棱")。`Compute()` 增加防御:检测到 rhoN 无效时回退单位密度。
2. **像素坐标翻转**(见偏差 7)。
3. **s2 无上限**(见偏差 4):曾出现 3σ=2.4m 的巨盘覆盖全画面(垂直条)。
4. **泡沫过曝**:`foamVMax=1.5` 时 dam break 波速 1-2 m/s 全部 $u=1$ → 基色纯白 → 流动帧 74% 像素 >176 亮度。`foamVMax=4.0` + $u^2$ 后峰值回到 64-96。
5. **颜色跳跃**:C 驱动的厚度色导致角白面暗(见偏差 5),统一基色 + 颜色空间平滑(表面粒子颜色邻居平均)解决。

---

## 5. 性能

| 场景 | 时间 |
|---|---|
| `GaussianRenderer::Compute`(8k 粒子,含邻居搜索/特征分解/平滑) | ~4-5 ms / 次(每 3 帧) |
| `GaussianRenderer::Compute`(33k 粒子) | ~15 ms / 次 |
| OpenGL 排序 + 上传 + 绘制(8k splats) | ~1.5 ms / 帧 |
| OpenGL 排序 + 上传 + 绘制(33k splats) | ~8.5 ms / 帧 |

对比原 MC 路径:`surface_reconstruct` 33k 粒子重建 189 ms / 次,而 Gaussian CPU 计算仅 ~15 ms,约 **12 倍**加速;GPU 光栅化也远快于 MC mesh 上传与绘制。
