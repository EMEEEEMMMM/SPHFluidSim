#include "FluidSolver.hpp"
#include <cstdio>
#include <cmath>
int main() {
    FluidSolver s;
    s.GetParams().spacing = 0.025f;
    s.InitInflow(1.0f, 0.3f, 100, 0.08f, 1.0f, 0.5f, 8000);
    int lastN = 0;
    for (int k = 0; k < 1500; k++) {
        s.Step();
        const int n = s.FluidCount();
        if (n != lastN) {
            if (k % 100 == 0 || n >= 8000)
                printf("step %4d: N=%d  err=%.3f%%\n", k, n, 100.0 * s.AvgDensityError());
            lastN = n;
        }
        if (n >= 8000) break;
    }
    printf("最终 N=%d (目标 8000, 注入应停止)\n", s.FluidCount());
    // 再跑 300 步确认不再增长
    const int n0 = s.FluidCount();
    for (int k = 0; k < 300; k++) s.Step();
    printf("封顶后 300 步: N=%d (应保持 %d)\n", s.FluidCount(), n0);
    printf("%s\n", (s.FluidCount() == n0 && n0 <= 8000) ? "PASS" : "FAIL");
    return 0;
}
