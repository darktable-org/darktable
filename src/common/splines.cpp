/*
    This file is part of darktable,
    copyright (c) 2019 Heiko Bauke.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "splines.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace interpol
{

template <typename T> struct point
{
  T x{ 0 };
  T y{ 0 };
};

template <typename T> struct base_point
{
  T x{ 0 };
  T y{ 0 };
  T dy{ 0 };
};

template <typename T> struct limits
{
  T min{ -std::numeric_limits<T>::infinity() };
  T max{ +std::numeric_limits<T>::infinity() };
};

template <typename T> constexpr limits<T> infinity()
{
  return limits<T>{};
}


template <typename T> class spline_base
{
protected:
  using size_type = typename std::vector<base_point<T> >::size_type;
  std::vector<base_point<T> > points;
  limits<T> x_lim;
  limits<T> y_lim;
  bool periodic{ false };

  template <typename iter> spline_base(iter i_begin, iter i_end)
  {
    for(iter i{ i_begin }; i != i_end; ++i) points.push_back({ i->x, i->y, 0 });
    if(points.empty()) throw std::invalid_argument("empty set of interpolation points");
    std::sort(points.begin(), points.end(),
              [](const base_point<T> &a, const base_point<T> &b) { return a.x < b.x; });
    x_lim = { points.front().x, points.back().x };
  }

  template <typename iter>
  spline_base(iter i_begin, iter i_end, const limits<T> &x_lim_, const limits<T> &y_lim_, bool periodic_ = false)
    : x_lim{ x_lim_ }, y_lim{ y_lim_ }, periodic{ periodic_ }
  {
    if(periodic)
    {
      const T period{ x_lim.max - x_lim.min };
      for(iter i{ i_begin }; i != i_end; ++i)
      {
        T x{ std::fmod(i->x, period) };
        if(x < 0) x += period;
        points.push_back({ x, i->y, 0 });
      }
    }
    else
    {
      for(iter i{ i_begin }; i != i_end; ++i)
      {
        if(x_lim.min <= i->x and i->x <= x_lim.max) points.push_back({ i->x, i->y, 0 });
      }
    }
    if(points.empty()) throw std::invalid_argument("empty set of interpolation points");
    std::sort(points.begin(), points.end(),
              [](const base_point<T> &a, const base_point<T> &b) { return a.x < b.x; });
  }

  spline_base(const std::initializer_list<point<T> > &I) : spline_base(I.begin(), I.end())
  {
  }

  spline_base(const std::initializer_list<point<T> > &I, const limits<T> &x_lim_, const limits<T> &y_lim_,
              bool periodic_ = false)
    : spline_base(I.begin(), I.end(), x_lim_, y_lim_, periodic_)
  {
  }

public:
  T operator()(T x) const
  {
    if(points.size() == 1) return points[0].y;
    size_type n0{ 0 };
    size_type n1{ 0 };
    T h{ 0 };
    if(periodic)
    {
      const T period{ x_lim.max - x_lim.min };
      x = std::fmod(x, period);
      if(x < points.front().x) x += period;
      n0 = std::upper_bound(points.begin(), points.end(), base_point<T>{ x, 0, 0 },
                            [](const base_point<T> &a, const base_point<T> &b) { return a.x < b.x; })
           - points.begin();
      n0 = n0 > 0 ? n0 - 1 : points.size() - 1;
      n1 = n0 + 1 < points.size() ? n0 + 1 : 0;
      if(n1 > n0)
        h = points[n1].x - points[n0].x;
      else
        h = points[n1].x - (points[n0].x - period);
    }
    else
    {
      x = std::max(x, x_lim.min);
      x = std::min(x, x_lim.max);
      if(x >= points.front().x)
      {
        n0 = std::upper_bound(points.begin(), points.end(), base_point<T>{ x, 0, 0 },
                              [](const base_point<T> &a, const base_point<T> &b) { return a.x < b.x; })
             - points.begin();
        if(n0 > 0) n0 = std::min(n0 - 1, points.size() - 1);
      }
      n1 = n0 + 1;
      h = points[n1].x - points[n0].x;
    }
    const T dx{ (x - points[n0].x) / h };
    const T dx2{ dx * dx };
    const T dx3{ dx2 * dx };
    const T h00{ 2 * dx3 - 3 * dx2 + 1 };
    const T h10{ dx3 - 2 * dx2 + dx };
    const T h01{ -2 * dx3 + 3 * dx2 };
    const T h11{ dx3 - dx2 };
    T y{ h00 * points[n0].y + h10 * h * points[n0].dy + h01 * points[n1].y + h11 * h * points[n1].dy };
    y = std::max(y, y_lim.min);
    y = std::min(y, y_lim.max);
    return y;
  }
};


// cubic hermite spline interpolation
// tangents at interpolation points given by with central difference formula, see
// https://en.wikipedia.org/wiki/Cubic_Hermite_spline
// https://de.wikipedia.org/wiki/Kubisch_Hermitescher_Spline
template <typename T> class Catmull_Rom_spline : public spline_base<T>
{
  using base = spline_base<T>;
  using base::periodic;
  using base::points;
  using base::x_lim;
  using base::y_lim;
  using typename base::size_type;

  void init()
  {
    if(points.size() == 1)
      points[0].dy = 0;
    else
    {
      const size_type N{ points.size() };
      if(periodic)
      {
        const T period{ x_lim.max - x_lim.min };
        points[0].dy = (points[1].y - points[N - 1].y) / (points[1].x - points[N - 1].x + period);
        for(size_type i{ 1 }; i < N - 1; ++i)
          points[i].dy = (points[i + 1].y - points[i - 1].y) / (points[i + 1].x - points[i - 1].x);
        points[N - 1].dy = (points[0].y - points[N - 2].y) / (points[0].x - points[N - 2].x + period);
      }
      else
      {
        points[0].dy = (points[1].y - points[0].y) / (points[1].x - points[0].x);
        for(size_type i{ 1 }; i < N - 1; ++i)
          points[i].dy = (points[i + 1].y - points[i - 1].y) / (points[i + 1].x - points[i - 1].x);
        points[N - 1].dy = (points[N - 1].y - points[N - 2].y) / (points[N - 1].x - points[N - 2].x);
      }
    }
  }

public:
  template <typename iter>
  Catmull_Rom_spline(iter i_begin, iter i_end) : spline_base<T>::spline_base(i_begin, i_end)
  {
    init();
  }

  template <typename iter>
  Catmull_Rom_spline(iter i_begin, iter i_end, const limits<T> &x_lim_, const limits<T> &y_lim_,
                     bool periodic_ = false)
    : spline_base<T>::spline_base(i_begin, i_end, x_lim_, y_lim_, periodic_)
  {
    init();
  }

  Catmull_Rom_spline(const std::initializer_list<point<T> > &I) : spline_base<T>::spline_base(I)
  {
    init();
  }

  Catmull_Rom_spline(const std::initializer_list<point<T> > &I, const limits<T> &x_lim_, const limits<T> &y_lim_,
                     bool periodic_ = false)
    : spline_base<T>::spline_base(I, x_lim_, y_lim_, periodic_)
  {
    init();
  }
};


// cubic hermite spline interpolation
// tangents at interpolation points are determined such that resulting interpolating function is monotonous between
// successive interpolation points, see https://en.wikipedia.org/wiki/Monotone_cubic_interpolation
template <typename T> class monotone_hermite_spline : public spline_base<T>
{
  using base = spline_base<T>;
  using base::periodic;
  using base::points;
  using base::x_lim;
  using base::y_lim;
  using typename base::size_type;

  void init()
  {
    if(points.size() == 1)
      points[0].dy = 0;
    else
    {
      const size_type N{ points.size() };
      if(periodic)
      {
        const T period{ x_lim.max - x_lim.min };
        std::vector<T> Delta;
        Delta.reserve(N);
        for(size_type i{ 0 }; i < N - 1; ++i)
          Delta.push_back((points[i + 1].y - points[i].y) / (points[i + 1].x - points[i].x));
        Delta.push_back((points[0].y - points[N - 1].y) / (points[0].x - points[N - 1].x + period));
        if(Delta[N - 1] * Delta[0] <= 0)
          points[0].dy = 0;
        else
          points[0].dy = (Delta[N - 1] + Delta[0]) / 2;
        for(size_type i{ 1 }; i < N; ++i)
          if(Delta[i - 1] * Delta[i] <= 0)
            points[i].dy = 0;
          else
            points[i].dy = (Delta[i - 1] + Delta[i]) / 2;
        for(size_type i{ 0 }; i < N; ++i)
        {
          const size_type i_1{ i + 1 < N ? i + 1 : 0 };
          if(std::abs(Delta[i]) < std::numeric_limits<T>::epsilon())
            points[i].dy = points[i_1].dy = 0;
          else
          {
            const T alpha{ points[i].dy / Delta[i] };
            const T beta{ points[i_1].dy / Delta[i] };
            const T tau{ alpha * alpha + beta * beta };
            if(tau > 9)
            {
              points[i].dy = 3 * alpha * Delta[i] / std::sqrt(tau);
              points[i_1].dy = 3 * beta * Delta[i] / std::sqrt(tau);
            }
          }
        }
      }
      else
      {
        std::vector<T> Delta;
        Delta.reserve(N - 1);
        for(size_type i{ 0 }; i < N - 1; ++i)
          Delta.push_back((points[i + 1].y - points[i].y) / (points[i + 1].x - points[i].x));
        points[0].dy = Delta[0];
        for(size_type i{ 1 }; i < N - 1; ++i)
          if(Delta[i - 1] * Delta[i] <= 0)
            points[i].dy = 0;
          else
            points[i].dy = (Delta[i - 1] + Delta[i]) / 2;
        if(N >= 2) points[N - 1].dy = Delta[N - 2];
        for(size_type i{ 0 }; i < N - 1; ++i)
          if(std::abs(Delta[i]) < std::numeric_limits<T>::epsilon())
            points[i].dy = points[i + 1].dy = 0;
          else
          {
            const T alpha{ points[i].dy / Delta[i] };
            const T beta{ points[i + 1].dy / Delta[i] };
            const T tau{ alpha * alpha + beta * beta };
            if(tau > 9)
            {
              points[i].dy = 3 * alpha * Delta[i] / std::sqrt(tau);
              points[i + 1].dy = 3 * beta * Delta[i] / std::sqrt(tau);
            }
          }
      }
    }
  }

public:
  template <typename iter>
  monotone_hermite_spline(iter i_begin, iter i_end) : spline_base<T>::spline_base(i_begin, i_end)
  {
    init();
  }

  template <typename iter>
  monotone_hermite_spline(iter i_begin, iter i_end, const limits<T> &x_lim_, const limits<T> &y_lim_,
                          bool periodic_ = false)
    : spline_base<T>::spline_base(i_begin, i_end, x_lim_, y_lim_, periodic_)
  {
    init();
  }

  monotone_hermite_spline(const std::initializer_list<point<T> > &I) : spline_base<T>::spline_base(I)
  {
    init();
  }

  monotone_hermite_spline(const std::initializer_list<point<T> > &I, const limits<T> &x_lim_,
                          const limits<T> &y_lim_, bool periodic_ = false)
    : spline_base<T>::spline_base(I, x_lim_, y_lim_, periodic_)
  {
    init();
  }
};


// cubic hermite spline interpolation
// tangents at interpolation points are determined such that resulting
// interpolating function is monotonous between successive interpolation points,
// see SIAM J. Sci. Stat. Comput., Vol. 5, pp. 300-304
// gives similar but sometimes more pleasing results than
// monotone_hermite_spline
template <typename T> class monotone_hermite_spline_variant : public spline_base<T>
{
  using base = spline_base<T>;
  using base::periodic;
  using base::points;
  using base::x_lim;
  using base::y_lim;
  using typename base::size_type;

  static T G(const T S1, const T S2, const T h1, const T h2)
  {
    if(S1 * S2 > 0)
    {
      const T alpha{ (h1 + 2 * h2) / (3 * (h1 + h2)) };
      return S1 * S2 / (alpha * S2 + (1 - alpha) * S1);
    }
    return 0;
  }

  void init()
  {
    if(points.size() == 1)
      points[0].dy = 0;
    else
    {
      const size_type N{ points.size() };
      if(periodic)
      {
        const T period{ x_lim.max - x_lim.min };
        std::vector<T> h, Delta;
        h.reserve(N);
        Delta.reserve(N);
        for(size_type i{ 0 }; i < N - 1; ++i)
        {
          h.push_back(points[i + 1].x - points[i].x);
          Delta.push_back((points[i + 1].y - points[i].y) / (points[i + 1].x - points[i].x));
        }
        h.push_back(points[0].x - points[N - 1].x + period);
        Delta.push_back((points[0].y - points[N - 1].y) / (points[0].x - points[N - 1].x + period));
        points[0].dy = G(Delta[N - 1], Delta[0], h[N - 1], h[0]);
        for(size_type i{ 1 }; i < N; ++i) points[i].dy = G(Delta[i - 1], Delta[i], h[i - 1], h[i]);
      }
      else
      {
        std::vector<T> h, Delta;
        h.reserve(N - 1);
        Delta.reserve(N - 1);
        for(size_type i{ 0 }; i < N - 1; ++i)
        {
          h.push_back(points[i + 1].x - points[i].x);
          Delta.push_back((points[i + 1].y - points[i].y) / (points[i + 1].x - points[i].x));
        }
        points[0].dy = Delta[0];
        for(size_type i{ 1 }; i < N - 1; ++i) points[i].dy = G(Delta[i - 1], Delta[i], h[i - 1], h[i]);
        if(N >= 2) points[N - 1].dy = Delta[N - 2];
      }
    }
  }

public:
  template <typename iter>
  monotone_hermite_spline_variant(iter i_begin, iter i_end) : spline_base<T>::spline_base(i_begin, i_end)
  {
    init();
  }

  template <typename iter>
  monotone_hermite_spline_variant(iter i_begin, iter i_end, const limits<T> &x_lim_, const limits<T> &y_lim_,
                                  bool periodic_ = false)
    : spline_base<T>::spline_base(i_begin, i_end, x_lim_, y_lim_, periodic_)
  {
    init();
  }

  monotone_hermite_spline_variant(const std::initializer_list<point<T> > &I) : spline_base<T>::spline_base(I)
  {
    init();
  }

  monotone_hermite_spline_variant(const std::initializer_list<point<T> > &I, const limits<T> &x_lim_,
                                  const limits<T> &y_lim_, bool periodic_ = false)
    : spline_base<T>::spline_base(I, x_lim_, y_lim_, periodic)
  {
    init();
  }
};


// cubic hermite spline interpolation
// tangents at interpolation points are determined such that resulting interpolating function has continuous 1st
// and 2nd derivatives over the whole interval, see https://de.wikipedia.org/wiki/Spline-Interpolation
template <typename T> class smooth_cubic_spline : public spline_base<T>
{
  using base = spline_base<T>;
  using base::periodic;
  using base::points;
  using base::x_lim;
  using base::y_lim;
  using typename base::size_type;

  class matrix
  {
    using size_type = typename std::vector<T>::size_type;
    const size_type N{ 0 };
    std::vector<T> A;

  public:
    explicit matrix(size_type N_) : N{ N_ }, A(N * N, 0)
    {
    }

    T &operator()(size_type i, size_type j)
    {
      return A[i + N * j];
    }

    const T &operator()(size_type i, size_type j) const
    {
      return A[i + N * j];
    }

    size_type size() const
    {
      return N;
    }
  };

  // Gaussian elimination with partial pivoting
  // after function call the square matrix A is triangular
  // vector p keeps track of row swaps
  // returns false if matrix A is singular
  static std::tuple<bool, std::vector<size_type> > gauss_make_triangular(matrix &A)
  {
    const size_type n{ A.size() };
    std::vector<size_type> p(n, 0);
    p[n - 1] = n - 1; // we never swap from the last row
    for(size_type k = 0; k < n; ++k)
    {
      // find pivot element for row swap
      size_type m = k;
      for(size_type i = k + 1; i < n; ++i)
        if(std::abs(A(k, i)) > std::abs(A(k, m))) m = i;
      p[k] = m; // rows k and m are swapped
      // eliminate elements and swap rows
      T t1 = A(k, m);
      A(k, m) = A(k, k);
      A(k, k) = t1; // new diagonal elements are (implicitly) one, store scaling factors on diagonal
      if(t1 != 0)
      {
        for(size_type i = k + 1; i < n; ++i) A(k, i) /= -t1;
        // swap rows
        if(k != m)
          for(size_type i = k + 1; i < n; ++i) std::swap(A(i, m), A(i, k));
        for(size_type j = k + 1; j < n; ++j)
          for(size_type i = k + 1; i < n; ++i) A(i, j) += A(k, j) * A(i, k);
      }
      else
        // the matrix is singular
        return { false, {} };
    }
    return { true, p };
  }

  // backward substitution after Gaussian elimination
  static void gauss_solve_triangular(const matrix &A, const std::vector<size_type> &p, std::vector<T> &b)
  {
    const size_type n{ A.size() };
    // permute and rescale elements of right-hand-side
    for(size_type k = 0; n > 0 and k < n - 1; ++k)
    {
      size_type m = p[k];
      std::swap(b[m], b[k]);
      for(size_type i = k + 1; i < n; ++i) b[i] += A(k, i) * b[k];
    }
    // perform backward substitution
    for(size_type k = n - 1;; --k)
    {
      b[k] /= A(k, k);
      T t = b[k];
      for(size_type i = 0; i < k; ++i) b[i] -= A(k, i) * t;
      if(k == 0) break;
    }
    b[0] /= A(0, 0);
  }

  // solve linear system
  static bool gauss_solve(matrix &A, std::vector<T> &b)
  {
    bool ok{ false };
    std::vector<size_type> p;
    std::tie(ok, p) = gauss_make_triangular(A);
    if(ok) gauss_solve_triangular(A, p, b);
    return ok;
  }

  void init()
  {
    if(points.size() == 1)
      points[0].dy = 0;
    else
    {
      const size_type N{ points.size() };
      std::vector<T> Delta_x, Delta_y;
      Delta_x.reserve(periodic ? N : N - 1);
      Delta_y.reserve(periodic ? N : N - 1);
      for(size_type i{ 0 }; i < N - 1; ++i)
      {
        Delta_x.push_back(points[i + 1].x - points[i].x);
        Delta_y.push_back(points[i + 1].y - points[i].y);
      }
      if(periodic)
      {
        const T period{ x_lim.max - x_lim.min };
        Delta_x.push_back(points[0].x - points[N - 1].x + period);
        Delta_y.push_back(points[0].y - points[N - 1].y);
      }
      matrix A(N);
      std::vector<T> b(N);
      for(size_type i{ 1 }; i < N - 1; ++i)
      {
        A(i, i - 1) = Delta_x[i - 1] / 6;
        A(i, i) = (Delta_x[i - 1] + Delta_x[i]) / 3;
        A(i, i + 1) = Delta_x[i] / 6;
        b[i] = Delta_y[i] / Delta_x[i] - Delta_y[i - 1] / Delta_x[i - 1];
      }
      if(periodic)
      {
        A(0, 0) = (Delta_x[N - 1] + Delta_x[0]) / 3;
        A(0, 1) = Delta_x[0] / 6;
        A(N - 1, N - 2) = Delta_x[N - 2] / 6;
        A(N - 1, N - 1) = (Delta_x[N - 2] + Delta_x[N - 1]) / 3;
        b[0] = Delta_y[0] / Delta_x[0] - Delta_y[N - 1] / Delta_x[N - 1];
        b[N - 1] = Delta_y[N - 1] / Delta_x[N - 1] - Delta_y[N - 2] / Delta_x[N - 2];
        A(0, N - 1) = A(N - 1, 0) = Delta_x[N - 1] / 6;
      }
      else
      {
        A(0, 0) = 1;
        A(N - 1, N - 1) = 1;
      }
      gauss_solve(A, b);
      T c_i{ 0 };
      for(size_type i{ 0 }; i < N - 1; ++i)
      {
        c_i = Delta_y[i] / Delta_x[i] - Delta_x[i] / 6 * (b[i + 1] - b[i]);
        points[i].dy = -Delta_x[i] * b[i] / 2 + c_i;
      }
      points[N - 1].dy = Delta_x[N - 1] * b[N - 1] / 2 + c_i;
    }
  }

public:
  template <typename iter>
  smooth_cubic_spline(iter i_begin, iter i_end) : spline_base<T>::spline_base(i_begin, i_end)
  {
    init();
  }

  template <typename iter>
  smooth_cubic_spline(iter i_begin, iter i_end, const limits<T> &x_lim_, const limits<T> &y_lim_,
                      bool periodic_ = false)
    : spline_base<T>::spline_base(i_begin, i_end, x_lim_, y_lim_, periodic_)
  {
    init();
  }

  smooth_cubic_spline(const std::initializer_list<point<T> > &I) : spline_base<T>::spline_base(I)
  {
    init();
  }

  smooth_cubic_spline(const std::initializer_list<point<T> > &I, const limits<T> &x_lim_, const limits<T> &y_lim_,
                      bool periodic_ = false)
    : spline_base<T>::spline_base(I, x_lim_, y_lim_, periodic_)
  {
    init();
  }
};

} // namespace interpol


float interpolate_val(int n, CurveAnchorPoint Points[], float x, unsigned int type)
{
  if(type == CUBIC_SPLINE)
  {
    interpol::smooth_cubic_spline<float> s(Points, Points + n);
    return s(x);
  }
  else if(type == CATMULL_ROM)
  {
    interpol::Catmull_Rom_spline<float> s(Points, Points + n);
    return s(x);
  }
  else if(type == MONOTONE_HERMITE)
  {
    interpol::monotone_hermite_spline<float> s(Points, Points + n);
    return s(x);
  }
  return NAN;
}


float interpolate_val_periodic(int n, CurveAnchorPoint Points[], float x, unsigned int type, float period)
{
  if(type == CUBIC_SPLINE)
  {
    interpol::smooth_cubic_spline<float> s(Points, Points + n, {0.f, period}, interpol::infinity<float>(), true);
    return s(x);
  }
  else if(type == CATMULL_ROM)
  {
    interpol::Catmull_Rom_spline<float> s(Points, Points + n, {0.f, period}, interpol::infinity<float>(), true);
    return s(x);
  }
  else if(type == MONOTONE_HERMITE)
  {
    interpol::monotone_hermite_spline<float> s(Points, Points + n, {0.f, period}, interpol::infinity<float>(), true);
    return s(x);
  }
  return NAN;
}


int CurveDataSample(CurveData *curve, CurveSample *sample)
{
  try
  {
    const float box_width = curve->m_max_x - curve->m_min_x;
    const float box_height = curve->m_max_y - curve->m_min_y;

    std::vector<interpol::point<float> > v;
    // build arrays for processing
    if(curve->m_numAnchors == 0)
    {
      // just a straight line using box coordinates
      v.push_back({ curve->m_min_x, curve->m_min_y });
      v.push_back({ curve->m_max_x, curve->m_max_y });
    }
    else
    {
      for(int i = 0; i < curve->m_numAnchors; i++)
        v.push_back({ curve->m_anchors[i].x * box_width + curve->m_min_x,
                      curve->m_anchors[i].y * box_height + curve->m_min_y });
    }

    const float res = 1.0f / (sample->m_samplingRes - 1);
    const int firstPointX = v.front().x * (sample->m_samplingRes - 1);
    const int firstPointY = v.front().y * (sample->m_outputRes - 1);
    const int lastPointX = v.back().x * (sample->m_samplingRes - 1);
    const int lastPointY = v.back().y * (sample->m_outputRes - 1);
    const int maxY = curve->m_max_y * (sample->m_outputRes - 1);
    const int minY = curve->m_min_y * (sample->m_outputRes - 1);
    const int n = sample->m_samplingRes;
    if(curve->m_spline_type == CUBIC_SPLINE)
    {
      interpol::smooth_cubic_spline<float> s(v.begin(), v.end(), { v.front().x, v.back().x },
                                             { curve->m_min_y, curve->m_max_y }, false);
      for(int i = 0; i < n; ++i)
      {
        if(i < firstPointX)
          sample->m_Samples[i] = firstPointY;
        else if(i > lastPointX)
          sample->m_Samples[i] = lastPointY;
        else
        {
          int val = static_cast<int>(std::round(s(i * res) * (sample->m_outputRes - 1)));
          if(val > maxY) val = maxY;
          if(val < minY) val = minY;
          sample->m_Samples[i] = val;
        }
      }
    }
    else if(curve->m_spline_type == CATMULL_ROM)
    {
      interpol::Catmull_Rom_spline<float> s(v.begin(), v.end(), { v.front().x, v.back().x },
                                            { curve->m_min_y, curve->m_max_y }, false);
      for(int i = 0; i < n; ++i)
      {
        if(i < firstPointX)
          sample->m_Samples[i] = firstPointY;
        else if(i > lastPointX)
          sample->m_Samples[i] = lastPointY;
        else
        {
          int val = static_cast<int>(std::round(s(i * res) * (sample->m_outputRes - 1)));
          if(val > maxY) val = maxY;
          if(val < minY) val = minY;
          sample->m_Samples[i] = val;
        }
      }
    }
    else if(curve->m_spline_type == MONOTONE_HERMITE)
    {
      interpol::monotone_hermite_spline<float> s(v.begin(), v.end(), { v.front().x, v.back().x },
                                                 { curve->m_min_y, curve->m_max_y }, false);
      for(int i = 0; i < n; ++i)
      {
        if(i < firstPointX)
          sample->m_Samples[i] = firstPointY;
        else if(i > lastPointX)
          sample->m_Samples[i] = lastPointY;
        else
        {
          int val = std::round(s(i * res) * (sample->m_outputRes - 1));
          if(val > maxY) val = maxY;
          if(val < minY) val = minY;
          sample->m_Samples[i] = val;
        }
      }
    }
    return CT_SUCCESS;
  }
  catch(...)
  {
    return CT_ERROR;
  }
}


int CurveDataSamplePeriodic(CurveData *curve, CurveSample *sample)
{
  try
  {
    const float box_width = curve->m_max_x - curve->m_min_x;
    const float box_height = curve->m_max_y - curve->m_min_y;

    std::vector<interpol::point<float> > v;
    // build arrays for processing
    if(curve->m_numAnchors == 0)
    {
      // just a straight line using box coordinates
      v.push_back({ curve->m_min_x, curve->m_min_y });
      v.push_back({ curve->m_max_x, curve->m_max_y });
    }
    else
    {
      for(int i = 0; i < curve->m_numAnchors; i++)
        v.push_back({ curve->m_anchors[i].x * box_width + curve->m_min_x,
                      curve->m_anchors[i].y * box_height + curve->m_min_y });
    }

    const float res = 1.0f / (sample->m_samplingRes - 1);
    if(curve->m_spline_type == CUBIC_SPLINE)
    {
      interpol::smooth_cubic_spline<float> s(v.begin(), v.end(), { curve->m_min_x, curve->m_max_x },
                                             { curve->m_min_y, curve->m_max_y }, true);
      for(unsigned int i = 0; i < sample->m_samplingRes; ++i)
        sample->m_Samples[i] = static_cast<unsigned short int>(std::round(s(i * res) * (sample->m_outputRes - 1)));
    }
    else if(curve->m_spline_type == CATMULL_ROM)
    {
      interpol::Catmull_Rom_spline<float> s(v.begin(), v.end(), { curve->m_min_x, curve->m_max_x },
                                            { curve->m_min_y, curve->m_max_y }, true);
      for(unsigned int i = 0; i < sample->m_samplingRes; ++i)
        sample->m_Samples[i] = static_cast<unsigned short int>(std::round(s(i * res) * (sample->m_outputRes - 1)));
    }
    else if(curve->m_spline_type == MONOTONE_HERMITE)
    {
      interpol::monotone_hermite_spline<float> s(v.begin(), v.end(), { curve->m_min_x, curve->m_max_x },
                                                 { curve->m_min_y, curve->m_max_y }, true);
      for(unsigned int i = 0; i < sample->m_samplingRes; ++i)
        sample->m_Samples[i] = static_cast<unsigned short int>(std::round(s(i * res) * (sample->m_outputRes - 1)));
    }
    return CT_SUCCESS;
  }
  catch(...)
  {
    return CT_ERROR;
  }
}
