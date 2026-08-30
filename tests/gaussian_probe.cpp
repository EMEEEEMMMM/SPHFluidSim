// Diagnose splat covariance: eigen-value ranges, extreme splats.
#include "FluidSolver.hpp"
#include "GaussianRenderer.hpp"
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <omp.h>

int main() {
    FluidSolver s;
    const float waterVol = 0.512f;
    const float waterSize = powf(waterVol, 1.0f / 3.0f);
    const float spacing = powf(waterVol / 8000.0f, 1.0f / 3.0f);
    s.GetParams().spacing = spacing;
    s.InitDamOpen(2.0f, 1.2f, 1.0f, waterSize);
    for (int k = 0; k < 31; k++) s.Step();
    printf("solver: spacing=%.6f N=%d\n", s.Spacing(), s.FluidCount());

    GaussianRenderer g;
    g.Compute(s.Positions(), s.Velocities(), s.Densities(),
              s.Spacing(), s.GetParams().dt, Math::Vector3(2.9f, 2.3f, 2.9f));
    const auto& sp = g.Splats();
    printf("splats=%d\n", (int)sp.size());

    // per-splat covariance eigenvalues (sym 3x3) via Jacobi
    std::vector<double> lamMax, lamMin;
    auto jacobi = [](const double a[9], double ev[3]) {
        double A[9]; for (int i=0;i<9;i++) A[i]=a[i];
        for (int it=0; it<50; it++) {
            int p=0,q=1; double mx=std::fabs(A[1]);
            if (std::fabs(A[5])>mx){mx=std::fabs(A[5]);p=1;q=2;}
            if (std::fabs(A[2])>mx){mx=std::fabs(A[2]);p=0;q=2;}
            if (mx<1e-14) break;
            const double App=A[3*p+p], Aqq=A[3*q+q], Apq=A[3*p+q];
            const double theta=0.5*(Aqq-App)/Apq;
            const double t=(theta>=0?1.0:-1.0)/(std::fabs(theta)+std::sqrt(theta*theta+1.0));
            const double c=1.0/std::sqrt(t*t+1.0), s=t*c;
            for (int k=0;k<3;k++){ if(k==p||k==q) continue;
                double akp=A[3*k+p], akq=A[3*k+q];
                A[3*k+p]=c*akp-s*akq; A[3*k+q]=s*akp+c*akq;
                A[3*p+k]=A[3*k+p]; A[3*q+k]=A[3*k+q]; }
            double app2=c*c*App-2*s*c*Apq+s*s*Aqq;
            double aqq2=s*s*App+2*s*c*Apq+c*c*Aqq;
            A[3*p+p]=app2; A[3*q+q]=aqq2; A[3*p+q]=A[3*q+p]=0.0;
        }
        ev[0]=A[0]; ev[1]=A[4]; ev[2]=A[8];
    };
    int bad = 0;
    for (const auto& sp2 : sp) {
        double covm[9] = {sp2.cov[0], sp2.cov[3], sp2.cov[4],
                          sp2.cov[3], sp2.cov[1], sp2.cov[5],
                          sp2.cov[4], sp2.cov[5], sp2.cov[2]};
        double ev[3]; jacobi(covm, ev);
        double lo = std::min({ev[0],ev[1],ev[2]}), hi = std::max({ev[0],ev[1],ev[2]});
        lamMax.push_back(hi);
        lamMin.push_back(lo);
        if (!(std::isfinite(hi)) || hi > 10.0 || lo < -1e-9) bad++;
    }
    std::sort(lamMax.begin(), lamMax.end());
    std::sort(lamMin.begin(), lamMin.end());
    auto pct = [&](auto& v, float q){ return v[(size_t)(q*(v.size()-1))]; };
    printf("cov lambda_max: min=%.5f p50=%.5f p90=%.5f p99=%.5f max=%.5f\n",
           lamMax.front(), pct(lamMax,0.5f), pct(lamMax,0.9f), pct(lamMax,0.99f), lamMax.back());
    printf("cov lambda_min: min=%.7f p50=%.5f max=%.5f  (negatives=%d, bad=%d)\n",
           lamMin.front(), pct(lamMin,0.5f), lamMin.back(), (int)std::count_if(lamMin.begin(),lamMin.end(),[](double v){return v<0;}), bad);

    // 3 sigma in meters for the largest splats
    double max3s = 0; int argmax = -1;
    for (size_t i = 0; i < sp.size(); i++) {
        double s3 = 3.0*sqrt(lamMax[i]);
        if (s3 > max3s) { max3s = s3; argmax = (int)i; }
    }
    printf("largest 3-sigma = %.3f m at splat %d pos=(%.3f,%.3f,%.3f) cov=(%.5f %.5f %.5f | %.5f %.5f %.5f) a=%.2f\n",
           max3s, argmax, sp[argmax].center.x, sp[argmax].center.y, sp[argmax].center.z,
           sp[argmax].cov[0], sp[argmax].cov[1], sp[argmax].cov[2],
           sp[argmax].cov[3], sp[argmax].cov[4], sp[argmax].cov[5], sp[argmax].alpha);
    return 0;
}
