# From SPH Particles to Pixels: A Complete Theoretical Handbook for 3D Gaussian Splatting Fluid Rendering

> Scope. This handbook builds, **from the bottom up and without skipping a step**, the full chain that turns a set of SPH fluid particles into the pixels of a rendered water surface. The chain has four independent legs:

> 1. **Geometry.** A *color field* locates the free surface; the surface normal and the strain-rate tensor are combined into an oriented, volume-conserving spheroid per particle (a 3D anisotropic Gaussian).
> 2. **Appearance.** The *render color* of a particle is derived from physical quantities (speed, thickness) plus a Blinn–Phong specular term; it is **completely decoupled** from the color field, which is a historical naming collision and never draws a pixel.
> 3. **Projection.** Each 3D Gaussian is moved into camera space by the view transform and squeezed into a 2D ellipse by the linearized perspective projection (Jacobian).
> 4. **Rasterization.** Ellipses are alpha-blended front-to-back / back-to-front to produce the final RGB.

> Notation. Vectors are columns, $\mathbf{x} \in \mathbb{R}^3$. $\mathbf{a}\cdot\mathbf{b}$ is the dot product, $\mathbf{a}\otimes\mathbf{b} = \mathbf{a}\mathbf{b}^T$ the outer product, $\mathbf{a}\times\mathbf{b}$ the cross product. $A^T$ is the transpose, $\det A$ the determinant, $\lVert\mathbf{x}\rVert = \sqrt{\mathbf{x}\cdot\mathbf{x}}$. $W(\mathbf{x}, h)$ is an SPH kernel of support radius $h$. $W_{ij} := W(\mathbf{x}_i - \mathbf{x}_j, h)$ and $\nabla_i W_{ij}$ is the gradient with respect to $\mathbf{x}_i$. Particle $i$ has position $\mathbf{x}_i$, velocity $\mathbf{v}_i$, mass $m_i$, density $\rho_i$, and volume $V_i = m_i/\rho_i$.

---

# Part I — Foundations

## 1. Why Gaussians Instead of Marching Cubes

SPH simulates a fluid with particles: every quantity (density, velocity, pressure) is carried by the particles and interpolated in space through a kernel. To *display* the fluid one must reconstruct a visible surface from the particle soup.

- **Marching Cubes.** One rasterizes a scalar field $\phi$ (e.g. the distance to the particle-weighted centroid) on a grid, then extracts an explicit triangle mesh by looking up the 256 configurations of how an isosurface crosses a cube. Problems: it needs a grid (resolution/size trade-off), produces a mesh that must be smoothed, and the topology (merging/splitting water) can change abruptly between frames.
- **3D Gaussian Splatting (3DGS).** Instead of a mesh, every *surface* particle is represented by a **3D anisotropic Gaussian** — an ellipsoid. Rendering simply **squeezes** each 3D ellipsoid onto the image plane (a 2D Gaussian = an ellipse) and alpha-composites the ellipses. No grid, no mesh, fully continuous, and each particle's shape can adapt frame-to-frame to the local strain and curvature of the fluid.

The rest of this handbook derives every formula used by this pipeline.

## 2. The Multivariate Gaussian Distribution, Built from Scratch

### 2.1 The one-dimensional Gaussian

We want the *densest* distribution with a fixed variance, but for this handbook we only need the normalization constant, and it is worth deriving because the same trick is used in every dimension.

Let

$$
I = \int_{-\infty}^{\infty} e^{-x^2/2}\,dx .
$$

Square it and switch to polar coordinates ($x = r\cos\theta$, $y = r\sin\theta$, $dx\,dy = r\,dr\,d\theta$):

$$
I^2 = \int_{-\infty}^{\infty}\int_{-\infty}^{\infty} e^{-(x^2+y^2)/2}\,dx\,dy
    = \int_0^{2\pi}\int_0^{\infty} e^{-r^2/2}\, r\, dr\, d\theta .
$$

The inner integral is exact:

$$
\int_0^{\infty} e^{-r^2/2}\,r\,dr = \Big[-e^{-r^2/2}\Big]_0^{\infty} = 0 - (-1) = 1 ,
$$

so $I^2 = 2\pi \cdot 1 = 2\pi$ and $I = \sqrt{2\pi}$. Substituting $u = x/\sigma$ gives

$$
\int_{-\infty}^{\infty} e^{-x^2/(2\sigma^2)}\,dx = \sigma\sqrt{2\pi},
$$

hence the 1D Gaussian with mean $\mu$ and variance $\sigma^2$,

$$
f(x) = \frac{1}{\sqrt{2\pi}\,\sigma}\, e^{-\frac{(x-\mu)^2}{2\sigma^2}},
$$

is normalized: $\int f\,dx = 1$.

### 2.2 The multivariate Gaussian in $d$ dimensions

Start with the **diagonal** case: $d$ *independent* 1D Gaussians with variances $\sigma_1^2,\dots,\sigma_d^2$. Their joint density is the product of the marginals,

$$
f(\mathbf{x}) = \prod_{k=1}^{d} \frac{1}{\sqrt{2\pi}\,\sigma_k} e^{-x_k^2/(2\sigma_k^2)}
             = \frac{1}{(2\pi)^{d/2}\sqrt{\sigma_1^2\cdots\sigma_d^2}} \,
               e^{-\frac12 \mathbf{x}^T \Lambda^{-1} \mathbf{x}},
\qquad \Lambda = \operatorname{diag}(\sigma_1^2,\dots,\sigma_d^2).
$$

**General case.** Let $\Sigma \in \mathbb{R}^{d\times d}$ be symmetric positive definite (SPD). By the spectral theorem (Appendix A) it decomposes as

$$
\Sigma = Q \Lambda Q^T,
$$

with $Q$ orthogonal ($Q^T Q = I$, columns = orthonormal eigenvectors) and $\Lambda = \operatorname{diag}(\lambda_1,\dots,\lambda_d)$, $\lambda_k > 0$. Define the change of variables

$$
\mathbf{y} = Q^T (\mathbf{x} - \boldsymbol\mu) \quad\Longleftrightarrow\quad \mathbf{x} - \boldsymbol\mu = Q\mathbf{y}.
$$

The Jacobian of $\mathbf{x}\mapsto\mathbf{y}$ is $\det Q^T = \det Q = \pm 1$ (orthogonal), so $d\mathbf{x} = d\mathbf{y}$. The quadratic form becomes

$$
(\mathbf{x}-\boldsymbol\mu)^T \Sigma^{-1} (\mathbf{x}-\boldsymbol\mu)
= \mathbf{y}^T Q^T \Sigma^{-1} Q \mathbf{y}
= \mathbf{y}^T \Lambda^{-1} \mathbf{y},
$$

because $Q^T \Sigma^{-1} Q = Q^T (Q\Lambda Q^T)^{-1} Q = Q^T Q \Lambda^{-1} Q^T Q = \Lambda^{-1}$. Therefore

$$
\int_{\mathbb{R}^d} e^{-\frac12(\mathbf{x}-\boldsymbol\mu)^T\Sigma^{-1}(\mathbf{x}-\boldsymbol\mu)}\,d\mathbf{x}
= \int_{\mathbb{R}^d} e^{-\frac12 \mathbf{y}^T\Lambda^{-1}\mathbf{y}}\,d\mathbf{y}
= \prod_{k=1}^d \sqrt{2\pi\lambda_k}
= (2\pi)^{d/2}\sqrt{\det\Lambda}
= (2\pi)^{d/2}\sqrt{\det\Sigma},
$$

using $\det\Sigma = \det(Q\Lambda Q^T) = (\det Q)^2 \det\Lambda = \det\Lambda$. The normalized density is

$$
\boxed{\,f(\mathbf{x}) = \frac{1}{(2\pi)^{d/2}\,\lvert\Sigma\rvert^{1/2}}\,
e^{-\frac12(\mathbf{x}-\boldsymbol\mu)^T\Sigma^{-1}(\mathbf{x}-\boldsymbol\mu)}\,}
$$

which for our fluid is $d = 3$, $\boldsymbol\mu = \mathbf{x}_i$ (the particle position), and $\Sigma = \Sigma_i$ (the particle's covariance). We write $\lvert\Sigma\rvert$ for $\det\Sigma$ from now on.

### 2.3 Level sets are ellipsoids

The exponent is a *Mahalanobis distance*: the level set $f = \text{const}$ satisfies

$$
(\mathbf{x}-\boldsymbol\mu)^T \Sigma^{-1} (\mathbf{x}-\boldsymbol\mu) = c^2 .
$$

With $\mathbf{y} = Q^T(\mathbf{x}-\boldsymbol\mu)$ this reads $\sum_k y_k^2/\lambda_k = c^2$: an **ellipsoid** whose principal axes point along the columns of $Q$ and whose half-axis lengths are $c\sqrt{\lambda_1}, c\sqrt{\lambda_2}, c\sqrt{\lambda_3}$. **The Gaussian is therefore a soft spheroid**, and $\Sigma$ fully determines its orientation and shape. Section 8 shows how we *build* $\Sigma$ from physics.

## 3. SPH Interpolation, Built from Scratch

### 3.1 From a Dirac delta to a kernel

A continuum field $A(\mathbf{x})$ (density, velocity, color...) can be recovered from its own values by the sifting property of the Dirac delta:

$$
A(\mathbf{x}) = \int A(\mathbf{x}') \,\delta(\mathbf{x}-\mathbf{x}')\, d\mathbf{x}' .
$$

The delta is not a function we can evaluate numerically. Replace it by a **smoothing kernel** $W(\mathbf{x}-\mathbf{x}', h)$ that approximates the delta as $h\to 0$:

$$
A(\mathbf{x}) \approx \int A(\mathbf{x}')\, W(\mathbf{x}-\mathbf{x}', h)\, d\mathbf{x}' .
$$

$W$ is chosen with three properties: (i) **normalized**, $\int W(\mathbf{x},h)\,d\mathbf{x} = 1$; (ii) **radially symmetric**, $W(\mathbf{x},h) = W(\lVert\mathbf{x}\rVert,h)$; (iii) **compact support**, $W = 0$ for $\lVert\mathbf{x}\rVert > h$ (so each particle only sees a bounded neighborhood). The most common choice is the *Poly6* kernel; see Appendix B.

### 3.2 Particle discretization

The fluid is sampled by particles that partition space: particle $j$ "owns" the volume $V_j = m_j/\rho_j$. Replace the integral by a Riemann sum over the particles inside the support:

$$
A_i := A(\mathbf{x}_i) \;\approx\; \sum_j A_j \, V_j \, W_{ij}
      = \sum_j \frac{m_j}{\rho_j}\, A_j \, W_{ij},
\qquad W_{ij} := W(\mathbf{x}_i - \mathbf{x}_j, h).
$$

Setting $A = \rho$ (and using $\rho_j$ for particle $j$) recovers the standard **density estimate**:

$$
\rho_i = \sum_j m_j \, W_{ij},
$$

because $A_j = \rho_j$ cancels the $\rho_j$ in the denominator: $\sum_j (m_j/\rho_j)\,\rho_j W_{ij} = \sum_j m_j W_{ij}$.

### 3.3 The gradient operator

Differentiating the interpolation formula with respect to $\mathbf{x}_i$ (only $W_{ij}$ depends on $\mathbf{x}_i$, and it depends on it through $r_{ij} = \lVert\mathbf{x}_i - \mathbf{x}_j\rVert$):

$$
\nabla A_i \;\approx\; \sum_j \frac{m_j}{\rho_j}\, A_j\, \nabla_i W_{ij},
\qquad
\nabla_i W_{ij} = \frac{\partial W}{\partial r}\Big|_{r_{ij}} \frac{\mathbf{x}_i - \mathbf{x}_j}{r_{ij}},
$$

where the last identity is the chain rule for a radial function, $\nabla_i r_{ij} = (\mathbf{x}_i - \mathbf{x}_j)/r_{ij}$.

**Antisymmetric (difference) form.** The plain form above does not vanish for a *constant* field $A_j = A_0$: it gives $A_0 \sum_j V_j \nabla_i W_{ij}$, which is only approximately zero. To make the gradient of a constant field exactly zero (crucial for stability), subtract the exact zero

$$
\sum_j V_j\, \nabla_i W_{ij} \;\approx\; \int \nabla W(\mathbf{x}_i-\mathbf{x}',h)\,d\mathbf{x}'
      = \nabla\!\!\int W\,d\mathbf{x}' = \nabla 1 = \mathbf{0},
$$

from the plain form:

$$
\nabla A_i \;\approx\; \sum_j \frac{m_j}{\rho_j}\,(A_j - A_i)\, \nabla_i W_{ij}.
$$

We will use the antisymmetric form for all field gradients below, and the symmetric combination $\frac12\sum_j V_j (A_i + A_j)\nabla_i W_{ij}$ when a symmetric version is required (it is exact for linear fields; we do not need it here).

---

# Part II — Geometry: Building the Anisotropic Gaussian from Physics

Every surface particle must become an ellipsoid. The pipeline:

$$
\text{color field } C_i \;\rightarrow\; \text{normal } \mathbf{n}_i \;\rightarrow\; \mathbf{r}_3
\qquad
\text{velocity gradient } \nabla\mathbf{v}_i \;\rightarrow\; \mathbf{t}_{\max} \;\rightarrow\; \mathbf{r}_1, \mathbf{r}_2
$$

$$
R = [\mathbf{r}_1\ \mathbf{r}_2\ \mathbf{r}_3], \quad S = \operatorname{diag}(s_1,s_2,s_3) \quad\Rightarrow\quad \Sigma_i = R\,S\,S^T R^T .
$$

Each step is derived below.

## 4. The Color Field (Surface Detection)

### 4.1 Definition and physical meaning

Define the scalar field

$$
C_i = \sum_j \frac{m_j}{\rho_j}\, W_{ij} = \sum_j V_j\, W_{ij}.
$$

$C_i$ counts, at the location of particle $i$, how much of the surrounding space is occupied by fluid: each neighbor contributes its volume $V_j$ weighted by the kernel, and the kernel is normalized ($\int W\,d\mathbf{x}=1$). Hence

- **Deep inside** the fluid the neighbors completely fill the kernel support, and the sum approximates the integral $\int W\,d\mathbf{x} = 1$: $\;C_i \to 1$.
- **At the surface** half of the support is empty (air), the sum loses about half its weight: $C_i \approx 0.5$ (and falls to 0 outside the fluid).

$C$ is therefore a **volume-occupancy fraction**, a smooth indicator of "how inside the fluid am I". It is used only for *geometry* (finding the surface); it never draws a pixel (see Part III).

### 4.2 Gradient

Differentiating with the antisymmetric form (Section 3.3),

$$
\nabla C_i = \sum_j \frac{m_j}{\rho_j}\,\nabla_i W_{ij},
$$

because the term $(C_j - C_i)$ with a constant field vanishes by construction. Since $C$ rises from 0 (air) to 1 (interior), $\nabla C_i$ points **from the surface toward the interior**, perpendicular to the surface, and its magnitude is large exactly where $C$ drops sharply — i.e. at the surface.

### 4.3 Surface normal

The outward normal (pointing into the air) is the *negated, normalized* gradient:

$$
\mathbf{n}_i = -\,\frac{\nabla C_i}{\lVert\nabla C_i\rVert},
\qquad \lVert\mathbf{n}_i\rVert = 1 .
$$

Normalization is needed because only the *direction* of the gradient is geometric information; its magnitude depends on the sampling density. The minus sign flips "toward interior" into "toward air". This $\mathbf{n}_i$ is exactly the third column $\mathbf{r}_3$ of the rotation matrix (Section 6) — the direction in which the ellipsoid is squeezed flat against the surface.

**Surface test.** A particle is a surface particle iff $C_i$ is below a threshold (e.g. $C_i < 0.5$) or, equivalently, iff $\lVert\nabla C_i\rVert$ is above a threshold. Interior particles ($C_i > 0.95$) will fall back to isotropic spheres (Section 7.5).

## 5. The Velocity Gradient Tensor and the Principal Stretch Direction

### 5.1 SPH discretization of the velocity gradient

Applying the antisymmetric gradient formula component-wise to the vector field $\mathbf{v}$ gives a $3\times 3$ tensor:

$$
\nabla\mathbf{v}_i = \sum_j \frac{m_j}{\rho_j}\,(\mathbf{v}_j - \mathbf{v}_i)\,\nabla_i W_{ij}^T .
$$

Using the difference $\mathbf{v}_j - \mathbf{v}_i$ (rather than $\mathbf{v}_j$) makes the discretization exact for a rigid-body velocity field $\mathbf{v}_j = \mathbf{v}_i + \boldsymbol\omega\times(\mathbf{x}_j-\mathbf{x}_i)$ *after* symmetrization (below), and removes the self-contribution.

### 5.2 Symmetrization: the strain-rate tensor

The velocity gradient decomposes uniquely as

$$
\nabla\mathbf{v} = \underbrace{\tfrac12\big(\nabla\mathbf{v} + (\nabla\mathbf{v})^T\big)}_{\mathbf{D}\text{ (strain rate)}} \;+\; \underbrace{\tfrac12\big(\nabla\mathbf{v} - (\nabla\mathbf{v})^T\big)}_{\mathbf{\Omega}\text{ (spin)}} .
$$

$\mathbf{\Omega}$ is antisymmetric and represents a local rigid rotation; it stretches nothing. **$\mathbf{D}$ is symmetric and represents pure deformation** — it is the physically meaningful part for shaping an ellipsoid. We use

$$
\mathbf{D}_i = \frac{\nabla\mathbf{v}_i + \nabla\mathbf{v}_i^T}{2}.
$$

### 5.3 Eigen-decomposition and the principal stretch direction

$\mathbf{D}_i$ is symmetric, so the spectral theorem applies: it has three real eigenvalues and three mutually orthonormal eigenvectors,

$$
\mathbf{D}_i = \sum_{k=1}^{3} \lambda_k\, \mathbf{t}_k \mathbf{t}_k^T,
\qquad \lambda_1 \ge \lambda_2 \ge \lambda_3 .
$$

Why is $\lambda_1$ the *stretch* rate? Linearize the velocity field around particle $i$: for a nearby point $\mathbf{x} = \mathbf{x}_i + \boldsymbol\delta$,

$$
\mathbf{v}(\mathbf{x}) \approx \mathbf{v}_i + \mathbf{D}_i\,\boldsymbol\delta + \mathbf{\Omega}_i\,\boldsymbol\delta .
$$

The spin part $\mathbf{\Omega}\boldsymbol\delta$ rotates the separation vector but preserves its length ($\boldsymbol\delta^T \mathbf{\Omega}\boldsymbol\delta = 0$ since $\mathbf{\Omega}$ is antisymmetric). The length of $\boldsymbol\delta$ changes only through the symmetric part:

$$
\frac{d}{dt}\lVert\boldsymbol\delta\rVert^2 = 2\,\boldsymbol\delta^T \mathbf{D}_i\, \boldsymbol\delta + O(\lVert\boldsymbol\delta\rVert^3),
$$

so per unit length the fastest growth rate in any direction is

$$
\max_{\lVert\boldsymbol\delta\rVert=1} \boldsymbol\delta^T \mathbf{D}_i\, \boldsymbol\delta
= \max_{\lVert\boldsymbol\delta\rVert=1} \sum_k \lambda_k (\boldsymbol\delta\cdot\mathbf{t}_k)^2 = \lambda_1,
$$

attained at $\boldsymbol\delta = \mathbf{t}_1$ (the last equality is the Rayleigh quotient argument: writing $\boldsymbol\delta = \sum_k a_k \mathbf{t}_k$ with $\sum_k a_k^2 = 1$ gives $\sum_k \lambda_k a_k^2 \le \lambda_1$). Define

$$
\boxed{\;\mathbf{t}_{\max} := \mathbf{t}_1\;}
$$

the eigenvector of the largest eigenvalue of $\mathbf{D}_i$ — the direction along which the fluid is locally being pulled into a filament. This is the input for the first column of $R$.

## 6. The Rotation Matrix $R$

We want an orthonormal right-handed frame $\{\mathbf{r}_1, \mathbf{r}_2, \mathbf{r}_3\}$ such that:

- $\mathbf{r}_3$ = surface normal $\mathbf{n}_i$ — the **squeeze axis** (the ellipsoid is thinnest along it, Section 7.3);
- $\mathbf{r}_1$ = the projection of $\mathbf{t}_{\max}$ onto the tangent plane — the **stretch axis** (the ellipsoid is longest along it, Section 7.2);
- $\mathbf{r}_2$ completes the frame.

**Step 1.** Set $\mathbf{r}_3 = \mathbf{n}_i$ (already a unit vector, Section 4.3).

**Step 2.** Remove from $\mathbf{t}_{\max}$ its component along $\mathbf{r}_3$ (orthogonal projection onto the tangent plane). The projection formula follows from writing $\mathbf{t}_{\max} = \mathbf{t}_{\max}^{\parallel} + \mathbf{t}_{\max}^{\perp}$ with $\mathbf{t}_{\max}^{\parallel} = (\mathbf{t}_{\max}\cdot\mathbf{r}_3)\,\mathbf{r}_3$ parallel to $\mathbf{r}_3$ and $\mathbf{t}_{\max}^{\perp}$ perpendicular to it; solving gives $\mathbf{t}_{\max}^{\perp} = \mathbf{t}_{\max} - (\mathbf{t}_{\max}\cdot\mathbf{r}_3)\,\mathbf{r}_3$, which is indeed perpendicular:

$$
\big(\mathbf{t}_{\max} - (\mathbf{t}_{\max}\cdot\mathbf{r}_3)\mathbf{r}_3\big)\cdot\mathbf{r}_3
= \mathbf{t}_{\max}\cdot\mathbf{r}_3 - (\mathbf{t}_{\max}\cdot\mathbf{r}_3)(\mathbf{r}_3\cdot\mathbf{r}_3) = 0,
$$

since $\mathbf{r}_3\cdot\mathbf{r}_3 = 1$. Normalize:

$$
\mathbf{r}_1 = \frac{\mathbf{t}_{\max} - (\mathbf{t}_{\max}\cdot\mathbf{r}_3)\,\mathbf{r}_3}
                   {\big\lVert \mathbf{t}_{\max} - (\mathbf{t}_{\max}\cdot\mathbf{r}_3)\,\mathbf{r}_3 \big\rVert}.
$$

*Degenerate case:* if $\mathbf{t}_{\max}\parallel\mathbf{r}_3$ the numerator vanishes; fall back to any fixed unit vector not parallel to $\mathbf{r}_3$ and Gram–Schmidt it the same way (the exact choice is irrelevant for an isotropic particle).

**Step 3.** The cross product of two perpendicular unit vectors is again a unit vector (since $\lVert\mathbf{a}\times\mathbf{b}\rVert = \lVert\mathbf{a}\rVert\lVert\mathbf{b}\rVert\sin\angle(\mathbf{a},\mathbf{b}) = 1$) and is perpendicular to both. Set

$$
\mathbf{r}_2 = \mathbf{r}_3 \times \mathbf{r}_1 .
$$

This also fixes the orientation: from the cyclic identity $\mathbf{a}\times(\mathbf{b}\times\mathbf{c}) = \mathbf{b}(\mathbf{a}\cdot\mathbf{c}) - \mathbf{c}(\mathbf{a}\cdot\mathbf{b})$ one checks $\mathbf{r}_1 \times \mathbf{r}_2 = \mathbf{r}_3$, i.e. $\{\mathbf{r}_1,\mathbf{r}_2,\mathbf{r}_3\}$ is a **right-handed** orthonormal basis.

**Step 4.** Assemble

$$
R = \big[\,\mathbf{r}_1 \;\; \mathbf{r}_2 \;\; \mathbf{r}_3\,\big].
$$

Because the columns are orthonormal, $R^T R = I$; because the frame is right-handed, $\det R = \mathbf{r}_1\cdot(\mathbf{r}_2\times\mathbf{r}_3) = \mathbf{r}_1\cdot\mathbf{r}_1 = +1$. Hence $R \in \mathrm{SO}(3)$: a genuine rotation.

## 7. The Scale Matrix $S$ (Volume-Conserving Anisotropy)

$S = \operatorname{diag}(s_1, s_2, s_3)$ assigns a half-axis length along each frame axis: $s_1$ along $\mathbf{r}_1$ (stretch), $s_2$ along $\mathbf{r}_2$ (secondary tangent), $s_3$ along $\mathbf{r}_3$ (squeeze). The golden rule: **the fluid is incompressible, so the ellipsoid's volume must stay equal to the rest-sphere volume** $\tfrac43\pi r^3$, where $r$ is the particle rest radius.

### 7.1 Ellipsoid volume

A unit ball $B = \{\mathbf{y} : \lVert\mathbf{y}\rVert \le 1\}$ is mapped to the ellipsoid by the linear map $\mathbf{x} = RS\,\mathbf{y}$. By the change-of-variables theorem,

$$
\operatorname{Vol}(\text{ellipsoid}) = \int_{B} \lvert\det(RS)\rvert\, d\mathbf{y}
= \lvert\det R\rvert \det S \cdot \frac43\pi
= \frac43\pi\, s_1 s_2 s_3 ,
$$

using $\det R = 1$ and $\det S = s_1 s_2 s_3$. The volume constraint is therefore

$$
s_1\, s_2\, s_3 = r^3 .
$$

### 7.2 $s_1$ — the stretch axis (from the strain rate)

Physically, a filament segment of current length $L$ aligned with the principal stretch direction grows as $\dot L = \lambda_1 L$. A forward-Euler step over $\Delta t$ gives $L \mapsto L\,(1 + \lambda_1\Delta t)$. Starting from the rest radius $r$:

$$
s_1^{*} = r\,\big(1 + \alpha\,\max(0,\lambda_1)\,\Delta t\big),
$$

with $\alpha \approx 1.5\text{–}3$ a visual amplification factor (SPH strain rates are underestimated by the smoothing, so without $\alpha$ the stretching is invisible) and $\max(0,\cdot)$ ensuring we only *lengthen*, never shrink below $r$ on compression. A hard cap prevents infinite filaments:

$$
s_1 = \min\big(s_1^{*},\; r_{\max}\big), \qquad r_{\max} = 3r \ldots 5r .
$$

### 7.3 $s_3$ — the squeeze axis (from the surface curvature)

At the free surface the ellipsoid must be pancake-flat along the normal so that only the outermost layer is visible. But squeezing *too* hard on strongly curved regions (wave crests, droplets) opens holes between ellipsoids. The squeeze is therefore made curvature-adaptive.

**Curvature of the surface.** Take the unit normal field $\mathbf{n}$ (pointing outward, Section 4.3) and extend it off the surface so that it is constant along its own direction. At a point with principal curvature directions $\mathbf{e}_1, \mathbf{e}_2$ and (signed) principal curvatures $\kappa_1, \kappa_2$ defined by $\partial\mathbf{n}/\partial s_a = -\kappa_a \mathbf{e}_a$ for arc-length $s_a$ along $\mathbf{e}_a$, the divergence of the extended field is

$$
\nabla\cdot\mathbf{n} = \frac{\partial\mathbf{n}}{\partial s_1}\cdot\mathbf{e}_1
                    + \frac{\partial\mathbf{n}}{\partial s_2}\cdot\mathbf{e}_2
                    + \underbrace{\frac{\partial\mathbf{n}}{\partial s_3}\cdot\mathbf{e}_3}_{0}
                    = -\kappa_1 - \kappa_2,
$$

(the third term vanishes because $\mathbf{n}$ is constant along its own direction). So $\nabla\cdot\mathbf{n} = -(\kappa_1+\kappa_2) = -2H$, twice the signed mean curvature with a sign. **Check on a sphere** of radius $R$ with outward normal $\mathbf{n}(\mathbf{x}) = (\mathbf{x}-\mathbf{x}_c)/R$: $\nabla\cdot\mathbf{n} = 3/R - (\mathbf{x}-\mathbf{x}_c)\cdot(\mathbf{x}-\mathbf{x}_c)/R^3 = 2/R$, i.e. $-\kappa_1-\kappa_2 = -2(-1/R) = 2/R$. ✓

Define the curvature measure

$$
\kappa_i := \nabla\cdot\mathbf{n}_i,
$$

which is positive on convex bumps (wave crests, droplets), zero on flat regions, negative in concavities. Its **SPH discretization** follows from applying the antisymmetric gradient formula (Section 3.3) to each component of $\mathbf{n}$ and summing:

$$
\kappa_i = \sum_j \frac{m_j}{\rho_j}\,\big(\mathbf{n}_j - \mathbf{n}_i\big)\cdot\nabla_i W_{ij},
$$

because $\nabla\cdot\mathbf{n} = \sum_k \partial n_k/\partial x_k$ and each term uses the same difference form.

**Curvature-adaptive squeeze factor.** Map the convex curvature into the flattening coefficient $\epsilon$:

$$
\epsilon_i = \operatorname{clamp}\!\Big(
  \epsilon_{\min} + \frac{\kappa_i}{\kappa_{\max}}\,(\epsilon_{\max} - \epsilon_{\min}),
  \; \epsilon_{\min},\; \epsilon_{\max}
\Big),
\qquad
\epsilon_{\min} = 0.05,\ \ \epsilon_{\max} = 0.8,
$$

with $\kappa_{\max}$ a normalizing curvature (a sensible scale is $\sim 2/r$, the curvature of the smallest resolved droplet) and $\operatorname{clamp}(x,a,b) = \min(\max(x,a),b)$. Using $\kappa_i$ directly (or $\max(0,\kappa_i)$ if one only wants to react to convexity) gives: flat surface → $\epsilon \to \epsilon_{\min}$ (very flat pancake), sharp crest → $\epsilon \to \epsilon_{\max}$ (nearly round, no holes). Finally

$$
s_3 = r\,\epsilon_i .
$$

### 7.4 $s_2$ — volume conservation

Solve the volume constraint $s_1 s_2 s_3 = r^3$ for the remaining axis:

$$
s_2 = \frac{r^3}{s_1\, s_3}.
$$

Intuition: if the particle is stretched ($s_1 > r$) and squeezed ($s_3 < r$) in *balanced* amounts, $s_2 \approx r$; if it is stretched a lot but squeezed little, $s_2 < r$ — the ellipsoid becomes a thin strand, exactly how real fluid filaments look. A floor prevents division blow-ups:

$$
s_2 = \max\!\Big(\frac{r^3}{s_1 s_3},\; 0.1\,r\Big).
$$

### 7.5 Isotropic fallback (interior / quiescent fluid)

Inside the fluid ($C_i > 0.95$) or when there is no meaningful deformation ($\lvert\lambda_1\rvert$ below a small threshold), anisotropy would only create holes between interior particles. In that case return to the isotropic sphere:

$$
s_1 = s_2 = s_3 = r, \qquad \Sigma_i = r^2 I .
$$

(For smooth transitions one can blend between the anisotropic and isotropic values over a band of $C_i$; the formulas are linear interpolations of $s_1, s_2, s_3$.)

## 8. The Covariance Matrix $\Sigma = R\,S\,S^T R^T$

Now expand the product step by step. First note that $S = S^T$ (diagonal) and $S^2 = \operatorname{diag}(s_1^2, s_2^2, s_3^2)$:

$$
\Sigma = R\,S\,S^T R^T = (RS)(RS)^T .
$$

Write the product $RS$ explicitly column by column: column $k$ of $RS$ is $R$ times the $k$-th standard basis vector scaled by $s_k$, i.e. $s_k \mathbf{r}_k$. Hence

$$
RS = \big[\,s_1\mathbf{r}_1 \;\; s_2\mathbf{r}_2 \;\; s_3\mathbf{r}_3\,\big],
$$

and the outer product of a matrix with itself is the sum of outer products of its columns:

$$
\Sigma = \sum_{k=1}^{3} (s_k\mathbf{r}_k)(s_k\mathbf{r}_k)^T
      = s_1^2\,\mathbf{r}_1\mathbf{r}_1^T
      + s_2^2\,\mathbf{r}_2\mathbf{r}_2^T
      + s_3^2\,\mathbf{r}_3\mathbf{r}_3^T .
$$

Each term $s_k^2 \mathbf{r}_k\mathbf{r}_k^T$ is the (unnormalized) projector onto $\mathbf{r}_k$. Three checks:

1. **Symmetric & positive definite.** For any $\mathbf{x}\ne\mathbf{0}$,
   $\mathbf{x}^T\Sigma\mathbf{x} = \sum_k s_k^2 (\mathbf{x}\cdot\mathbf{r}_k)^2 > 0$, since the $\mathbf{r}_k$ form a basis. ✓
2. **Spectral form.** This is exactly $\Sigma = Q\Lambda Q^T$ from Section 2.2 with $Q = R$ and $\Lambda = S^2$: the *eigenvectors are the frame axes* and the *eigenvalues are the squared half-axis lengths*. The Gaussian's level sets are ellipsoids with half-axes $s_1,s_2,s_3$ along $\mathbf{r}_1,\mathbf{r}_2,\mathbf{r}_3$. ✓
3. **Determinant.** $\det\Sigma = (s_1 s_2 s_3)^2$ (as in Section 7.1), so $\sqrt{\det\Sigma} = s_1 s_2 s_3$ is proportional to the ellipsoid *volume*. This identity is used twice later: for the opacity (Section 12) and for nothing else in the projection (which needs $\Sigma$ itself).

---

# Part III — Appearance: Render Color and Opacity

## 9. The Color Field vs. the Render Color

Two concepts share the word "color" but are physically unrelated:

| | Color field $C_i$ (Part II) | Render color $\mathbf{c}_i$ (this part) |
|---|---|---|
| Role | geometry: locate the surface | optics: what the pixel sees |
| Derived from | $C_i = \sum_j V_j W_{ij}$ | speed, thickness, normal, light |
| Output | normal $\mathbf{n}_i$, squeeze axis | RGB triple + opacity |

The color field is a historical term and **never contributes RGB values**. Everything below concerns the render color only.

## 10. Diffuse Base Color

Two heuristic but stable mappings from physical quantities to a base color.

**Speed → foam.** Fast fluid (splashes, foam) is white, slow fluid is deep blue. With $v_i = \lVert\mathbf{v}_i\rVert$ and normalized speed $u_i = \min(v_i/v_{\max}, 1)$,

$$
\mathbf{d}^{\text{foam}}_i = (1-u_i)\,\mathbf{d}_{\text{deep}} + u_i\,\mathbf{d}_{\text{foam}},
\qquad
\mathbf{d}_{\text{deep}} = (0.1, 0.2, 0.5),\ \ \mathbf{d}_{\text{foam}} = (1,1,1).
$$

**Thickness → translucency.** Thin sheets of water transmit light and look pale; deep water looks dark. The color field value $C_i \in [0,1]$ is a convenient occupancy/thickness proxy (Section 4.1):

$$
\mathbf{d}^{\text{thick}}_i = (1 - C_i)\,\mathbf{d}_{\text{shallow}} + C_i\,\mathbf{d}_{\text{deep}} .
$$

**Combine.** Element-wise multiplication (color overlay):

$$
\mathbf{d}_i = \mathbf{d}^{\text{foam}}_i \odot \mathbf{d}^{\text{thick}}_i .
$$

## 11. Specular Highlight (Blinn–Phong)

Water is glossy; the highlight must be computed from the *geometric* normal $\mathbf{n}_i$ in real time.

**Reflected direction.** Decompose the light direction $\mathbf{l}$ (pointing *toward* the light) into parallel and perpendicular parts w.r.t. $\mathbf{n}_i$: $\mathbf{l} = \mathbf{l}_\parallel + \mathbf{l}_\perp$ with $\mathbf{l}_\parallel = (\mathbf{l}\cdot\mathbf{n}_i)\mathbf{n}_i$. Reflection keeps the parallel part and flips the perpendicular part, so the mirror direction is

$$
\mathbf{r} = \mathbf{l}_\parallel - \mathbf{l}_\perp
           = 2(\mathbf{l}\cdot\mathbf{n}_i)\,\mathbf{n}_i - \mathbf{l}.
$$

Phong shading evaluates $(\mathbf{r}\cdot\mathbf{v})^{\gamma}$, where $\mathbf{v}$ points toward the camera. Blinn–Phong replaces $\mathbf{r}\cdot\mathbf{v}$ with $\mathbf{n}_i\cdot\mathbf{h}$ where

$$
\mathbf{h} = \frac{\mathbf{l} + \mathbf{v}}{\lVert\mathbf{l}+\mathbf{v}\rVert}
$$

is the *halfway vector*. The two agree in the ideal case ($\mathbf{v} = \mathbf{r}$ forces $\mathbf{h}\parallel\mathbf{n}_i$, both dots equal 1) and are monotonically related nearby, so the exponent $\gamma$ absorbs the difference; Blinn–Phong is cheaper (no reflection computation) and is the standard choice for water:

$$
\mathbf{s}_i = \mathbf{k}_s\, \max\big(0,\ \mathbf{n}_i\cdot\mathbf{h}\big)^{\gamma},
\qquad
\mathbf{k}_s = \text{white-ish},\ \ \gamma \approx 64\text{–}128 .
$$

**Final render color** (stored per Gaussian and fed to the rasterizer):

$$
\mathbf{c}_i = \mathbf{d}_i + \mathbf{s}_i .
$$

## 12. Opacity

If an ellipsoid is stretched, it covers more screen area; stacking stretched ellipsoids over-exposes the image. The opacity should therefore decrease with the ellipsoid's 3D size. Recall from Section 8 that $\sqrt{\det\Sigma_i} = s_1 s_2 s_3$, which is proportional to the ellipsoid **volume** ($\tfrac43\pi s_1s_2s_3$). The rest sphere has volume proportional to $r^3$. Set

$$
\alpha_i = \min\!\Big(1,\; \frac{r^3}{\sqrt{\det\Sigma_i}}\,\alpha_{\text{base}}\Big),
\qquad \alpha_{\text{base}} \approx 0.5\text{–}0.8 .
$$

Interpretation: $\alpha_i$ is the *rest-volume over current-volume* ratio times a calibration constant. When the ellipsoid is exactly volume-conserving ($s_1s_2s_3 = r^3$) the ratio is 1 and $\alpha_i = \alpha_{\text{base}}$; when the cap in Section 7.2 inflates the volume, the ratio drops and the opacity compensates.

---

# Part IV — Rasterization: From 3D Gaussian to Pixel

## 13. Linear Transformation of a Gaussian

**Theorem.** If $\mathbf{x}\sim\mathcal N(\boldsymbol\mu,\Sigma)$ and $\mathbf{y} = A\mathbf{x} + \mathbf{b}$ ($A$ any matrix, $\mathbf{b}$ any vector), then $\mathbf{y}\sim\mathcal N(A\boldsymbol\mu+\mathbf{b},\; A\Sigma A^T)$.

*Proof.* The mean is $\mathbb E[\mathbf{y}] = A\mathbb E[\mathbf{x}] + \mathbf{b} = A\boldsymbol\mu + \mathbf{b}$ by linearity. The covariance is a centered second moment:

$$
\operatorname{Cov}(\mathbf{y}) = \mathbb E\big[(\mathbf{y}-\mathbb E[\mathbf{y}])(\mathbf{y}-\mathbb E[\mathbf{y}])^T\big]
= \mathbb E\big[A(\mathbf{x}-\boldsymbol\mu)(\mathbf{x}-\boldsymbol\mu)^T A^T\big]
= A\,\mathbb E\big[(\mathbf{x}-\boldsymbol\mu)(\mathbf{x}-\boldsymbol\mu)^T\big] A^T
= A\Sigma A^T,
$$

because $A$ is constant. ∎

This single theorem drives both the view transform and the projection below.

## 14. View Transformation (World → Camera)

The world-to-camera map is a rigid motion: $\mathbf{x}_c = W\,\mathbf{x}_w + \mathbf{t}$, with $W \in \mathrm{SO}(3)$ the camera rotation and $\mathbf{t}$ the translation. By Section 13, a particle's Gaussian center moves to $\boldsymbol\mu_c = W\mathbf{x}_i + \mathbf{t}$, and its covariance becomes

$$
\Sigma_c = W\,\Sigma_i\, W^T .
$$

The translation drops out of the covariance because covariance is translation-invariant (it is a *centered* moment — see the proof above). Only the rotation $W$ matters for the shape.

## 15. Perspective Projection and the Jacobian

**Camera model.** The pinhole projection of a camera-space point $\mathbf{x}_c = (x_c, y_c, z_c)^T$ onto the image plane is

$$
\mathbf{u} = \begin{pmatrix} u \\ v \end{pmatrix}
= \begin{pmatrix} f\, x_c / z_c \\ f\, y_c / z_c \end{pmatrix},
$$

with focal length $f$. This map is **nonlinear** (division by $z_c$), so a Gaussian does not stay Gaussian under it. 3DGS linearizes: since an ellipsoid is small relative to its distance, approximate the projection by its first-order Taylor expansion about the Gaussian center $\boldsymbol\mu_c = (x^*, y^*, z^*)^T$:

$$
\mathbf{u}(\mathbf{x}_c) \approx \mathbf{u}(\boldsymbol\mu_c) + J\,(\mathbf{x}_c - \boldsymbol\mu_c),
\qquad J = \frac{\partial \mathbf{u}}{\partial \mathbf{x}_c}\Big|_{\boldsymbol\mu_c} \in \mathbb{R}^{2\times 3}.
$$

**Jacobian entries** (all evaluated at $\boldsymbol\mu_c$; $f$, $z^*$ constants at the evaluation point):

$$
\frac{\partial u}{\partial x_c} = \frac{f}{z_c},\quad
\frac{\partial u}{\partial y_c} = 0,\quad
\frac{\partial u}{\partial z_c} = -\frac{f\,x_c}{z_c^2},
\qquad
\frac{\partial v}{\partial x_c} = 0,\quad
\frac{\partial v}{\partial y_c} = \frac{f}{z_c},\quad
\frac{\partial v}{\partial z_c} = -\frac{f\,y_c}{z_c^2},
$$

so

$$
J = \frac{f}{z^*}
\begin{pmatrix}
1 & 0 & -x^*/z^* \\
0 & 1 & -y^*/z^*
\end{pmatrix}.
$$

**Projected covariance.** The composite map $\mathbf{x}_w \mapsto \mathbf{u}$ is (approximately) affine: $\mathbf{u} \approx J W \mathbf{x}_w + \text{const}$. Applying Section 13 to the two-stage linear map $A = JW$:

$$
\boxed{\;\Sigma' = J\,W\,\Sigma_i\, W^T J^T\;}
$$

a $2\times 2$ SPD matrix describing the screen-space ellipse (its size, orientation, eccentricity). This is formula (6) of the original 3DGS paper.

## 16. Screen-Space Gaussian Weight

The projected 2D Gaussian has density (Section 2.2 with $d=2$, center $\mathbf{u}_i = \mathbf{u}(\boldsymbol\mu_c)$, covariance $\Sigma'$):

$$
G_i(\mathbf{u}) = \frac{1}{2\pi\sqrt{\det\Sigma'}}\,
\exp\!\Big(-\tfrac12 (\mathbf{u}-\mathbf{u}_i)^T \Sigma'^{-1} (\mathbf{u}-\mathbf{u}_i)\Big).
$$

The normalization constant is the $d=2$ case of the integral derived in Section 2.2: $\int_{\mathbb{R}^2} e^{-\frac12(\mathbf{u}-\mathbf{u}_i)^T\Sigma'^{-1}(\mathbf{u}-\mathbf{u}_i)}d\mathbf{u} = 2\pi\sqrt{\det\Sigma'}$. In practice the Gaussian is evaluated only inside a bounding rectangle of radius $3\sqrt{\lambda_{\max}(\Sigma')}$ (three standard deviations along the major axis) and clamped to the viewport.

## 17. Alpha Blending

Each pixel receives contributions from every Gaussian whose screen ellipse covers it. Let the *effective opacity* of Gaussian $i$ at pixel $\mathbf{u}$ be

$$
a_i(\mathbf{u}) = \alpha_i\, G_i(\mathbf{u}),
$$

the product of the particle's opacity (Section 12) and the screen-space weight (Section 16).

**The over operator.** Compositing a foreground color $C_f$ with effective opacity $a$ over a background $C_{bg}$ is

$$
C_{\text{out}} = a\, C_f + (1-a)\, C_{bg}.
$$

**Ordering.** Sort all covering Gaussians by depth (view-space $z_c$), far to near; index $i=1$ = farthest. Apply over repeatedly: after $i=1$, $C = a_1 c_1 + (1-a_1)C_{bg}$; after $i=2$, $C = a_2 c_2 + (1-a_2)\big[a_1 c_1 + (1-a_1)C_{bg}\big]$. Expanding and using induction,

$$
C_{\text{pixel}}(\mathbf{u}) = \sum_{i=1}^{N} \Big(\mathbf{c}_i\, a_i(\mathbf{u})\Big)
  \prod_{j=1}^{i-1}\Big(1 - a_j(\mathbf{u})\Big)
  \;+\; \Big(\prod_{j=1}^{N}\big(1-a_j(\mathbf{u})\big)\Big) C_{bg}.
$$

The product $\prod_{j<i}(1-a_j)$ is the **accumulated transmittance**. Recall the ordering: index 1 is the *farthest* Gaussian and the index grows as we move toward the camera, so $j < i$ means "Gaussian $j$ is farther than Gaussian $i$". When we composite in far-to-near order we start from the farthest splat and build up; by the time Gaussian $i$ is composited, all $j < i$ (which the view ray hits *before* $i$) have already deposited their color, and the fraction of light that still arrives from behind them is exactly $\prod_{j<i}(1-a_j)$. The trailing product $\prod_{j=1}^N (1-a_j)$ is the background visibility. Dropping the (constant) background term gives the standard 3DGS blending formula,

$$
\boxed{\;C_{\text{pixel}}(\mathbf{u}) = \sum_{i=1}^{N} \mathbf{c}_i\, \alpha_i\, G_i(\mathbf{u})
  \prod_{j=1}^{i-1}\big(1 - \alpha_j G_j(\mathbf{u})\big)\;}
$$

**Why sorting matters.** The over operator is not commutative; rendering in the wrong order changes the result. In practice, because the squeeze axis $s_3 \ll r$ makes only the outermost layer of ellipsoids carry significant weight, the visual sensitivity to exact ordering is low, but the formula assumes sorted, far-to-near evaluation.

---

# Part V — Pipeline Summary

Input: the DFSPH/IISPH solver's per-particle state $(\mathbf{x}_j, \mathbf{v}_j, m_j, \rho_j)$ and the kernel $W$.

| Step | Input | Formula | Output |
|---|---|---|---|
| 1 | $\mathbf{x}_j, m_j, \rho_j$ | $C_i = \sum_j \frac{m_j}{\rho_j} W_{ij}$ | occupancy field $C_i$ |
| 2 | $C_j$ | $\mathbf{n}_i = -\nabla C_i/\lVert\nabla C_i\rVert$ | surface normal $\mathbf{n}_i = \mathbf{r}_3$ |
| 3 | $\mathbf{v}_j$ | $\nabla\mathbf{v}_i = \sum_j \frac{m_j}{\rho_j}(\mathbf{v}_j-\mathbf{v}_i)\nabla_i W_{ij}^T$ | velocity gradient |
| 4 | $\nabla\mathbf{v}_i$ | $\mathbf{D}_i = \frac12(\nabla\mathbf{v}_i + \nabla\mathbf{v}_i^T)$, eigendecomposition | $\mathbf{t}_{\max}$, $\lambda_1$ |
| 5 | $\mathbf{t}_{\max}, \mathbf{n}_i$ | Gram–Schmidt (Section 6) | $R = [\mathbf{r}_1\ \mathbf{r}_2\ \mathbf{r}_3]$ |
| 6 | $\lambda_1, \kappa_i, C_i$ | Section 7 | $S = \operatorname{diag}(s_1,s_2,s_3)$ |
| 7 | $R, S$ | $\Sigma_i = R S S^T R^T = \sum_k s_k^2 \mathbf{r}_k\mathbf{r}_k^T$ | covariance $\Sigma_i$ |
| 8 | $v_i, C_i, \mathbf{n}_i$, light | Sections 10–11 | render color $\mathbf{c}_i$ |
| 9 | $\Sigma_i$ | $\alpha_i = \min(1, \frac{r^3}{\sqrt{\det\Sigma_i}}\alpha_{\text{base}})$ | opacity $\alpha_i$ |
| 10 | $\mathbf{x}_i, \Sigma_i$, camera | $\Sigma' = J W \Sigma_i W^T J^T$ (Sections 14–15) | screen ellipse $\Sigma'$ |
| 11 | $\Sigma'$, $\mathbf{c}_i$, $\alpha_i$ | Sections 16–17 | **pixel color** |

**Default parameters** (suggested starting values, all tunable):

| Symbol | Meaning | Default |
|---|---|---|
| $r$ | particle rest radius (kernel scale) | scene-dependent |
| $\alpha$ | stretch amplification | 1.5–3.0 |
| $r_{\max}$ | stretch cap | $3r$–$5r$ |
| $\epsilon_{\min}, \epsilon_{\max}$ | flattening range | 0.05, 0.8 |
| $\kappa_{\max}$ | curvature normalization | $\sim 2/r$ |
| $v_{\max}$ | foam speed normalization | scene-dependent |
| $\mathbf{k}_s, \gamma$ | specular color, shininess | $(1,1,1)$, 64–128 |
| $\alpha_{\text{base}}$ | opacity calibration | 0.5–0.8 |

**Known limitations and future work.**

1. **Flicker at surface transitions.** As particles enter/leave the surface, $s_3$ jumps between $r\epsilon$ and $r$ (Section 7.5). Smoothing the $s_k$ over a band of $C_i$ (or over time) removes the popping.
2. **Planar projection.** The Jacobian linearization (Section 15) is exact only for Gaussians centered on the optical axis; strongly off-center or very large ellipsoids are slightly distorted. The original 3DGS has the same limitation.
3. **Curved splats.** A future refinement replaces the flat ellipse with a *curved* splat (a section of the spheroid's surface) to better match the fluid's curved surface; the derivations in Part II are unchanged, only the screen-space footprint changes.

---

# Appendix A — Spectral Theorem (used in §2, §5, §8)

For a real symmetric matrix $M = M^T \in \mathbb{R}^{n\times n}$: all eigenvalues are real; eigenvectors of distinct eigenvalues are orthogonal; there exists an orthonormal eigenbasis. Hence $M = Q\Lambda Q^T$ with $Q^TQ = I$ and $\Lambda = \operatorname{diag}(\lambda_1,\dots,\lambda_n)$. If additionally $M$ is positive definite ($\mathbf{x}^T M\mathbf{x} > 0$ for all $\mathbf{x}\ne 0$), all $\lambda_k > 0$.

**Rayleigh quotient** (used in §5.3): for symmetric $M$,

$$
\max_{\lVert\mathbf{x}\rVert = 1} \mathbf{x}^T M \mathbf{x} = \lambda_{\max},
$$

attained at the corresponding eigenvector. Proof: expand $\mathbf{x} = \sum_k a_k \mathbf{q}_k$ with $\sum_k a_k^2 = 1$; then $\mathbf{x}^T M\mathbf{x} = \sum_k \lambda_k a_k^2 \le \lambda_{\max}\sum_k a_k^2 = \lambda_{\max}$.

# Appendix B — Common SPH kernels

**Poly6** (standard for density/color interpolation):

$$
W_{\text{poly6}}(\mathbf{r}, h) = \frac{315}{64\pi h^9}
\begin{cases}
(h^2 - r^2)^3, & 0 \le r \le h \\
0, & r > h
\end{cases}
$$

**Spiky** (standard for gradients/pressure forces, non-singular at $r=0$ in the gradient):

$$
W_{\text{spiky}}(\mathbf{r}, h) = \frac{15}{\pi h^6}
\begin{cases}
(h - r)^3, & 0 \le r \le h \\
0, & r > h
\end{cases}
$$

Both satisfy the three kernel requirements of Section 3.1 (check $\int W\,d\mathbf{x} = 1$ by direct integration in spherical coordinates).

# Appendix C — Symbol table

| Symbol | Meaning |
|---|---|
| $\mathbf{x}_i, \mathbf{v}_i, m_i, \rho_i, V_i$ | position, velocity, mass, density, volume ($V_i = m_i/\rho_i$) of particle $i$ |
| $W, h, r$ | kernel, support radius, particle rest radius |
| $C_i$ | color-field occupancy (geometry only) |
| $\mathbf{n}_i$ | outward surface normal |
| $\mathbf{D}_i, \lambda_k, \mathbf{t}_k$ | strain-rate tensor, its eigenvalues/eigenvectors, $\mathbf{t}_{\max} = \mathbf{t}_1$ |
| $R, \mathbf{r}_1, \mathbf{r}_2, \mathbf{r}_3$ | rotation matrix, frame axes ($\mathbf{r}_3 = \mathbf{n}_i$) |
| $S, s_1, s_2, s_3$ | scale matrix, half-axis lengths (volume-constrained) |
| $\Sigma_i$ | 3D covariance of particle $i$ |
| $\kappa_i$ | curvature measure $\nabla\cdot\mathbf{n}_i$ |
| $\mathbf{c}_i, \mathbf{d}_i, \mathbf{s}_i, \alpha_i$ | render color, diffuse, specular, opacity |
| $\mathbf{l}, \mathbf{v}, \mathbf{h}$ | light, view, halfway directions |
| $W$ (view), $J$, $f$ | view rotation, projection Jacobian, focal length |
| $\Sigma', G_i(\mathbf{u})$ | projected 2D covariance, screen-space weight |

