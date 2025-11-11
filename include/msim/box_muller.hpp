// box_muller.hpp
#pragma once
#include <cmath>
struct NormalBM {
  bool has_spare = false;
  double spare = 0.0;
  template <class URNG>
  double operator()(URNG& rng, double mean, double sigma) {
    if (has_spare) {
      has_spare = false;
      return mean + sigma * spare;
    }
    double u, v, s;
    do {
      u = 2.0 * rng.next_uniform01() - 1.0;
      v = 2.0 * rng.next_uniform01() - 1.0;
      s = u * u + v * v;
    } while (s == 0.0 || s >= 1.0);
    const double m = std::sqrt(-2.0 * std::log(s) / s);
    spare = v * m;
    has_spare = true;
    return mean + sigma * (u * m);
  }
};
