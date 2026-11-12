#pragma once
// Dense and sparse linear algebra on row-major double/float matrices: LU (partial pivoting), Cholesky,
// Householder QR, triangular solves, CSR SpMV and conjugate gradients.
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace forge::linalg {
template <class T> struct mat {
    std::size_t r = 0, c = 0; std::vector<T> a;
    mat() = default; mat(std::size_t r_, std::size_t c_) : r(r_), c(c_), a(r_ * c_) {}
    T& operator()(std::size_t i, std::size_t j) { return a[i * c + j]; }
    const T& operator()(std::size_t i, std::size_t j) const { return a[i * c + j]; }
};
template <class T> mat<T> matmul(const mat<T>& A, const mat<T>& B) {
    mat<T> C(A.r, B.c);
    for (std::size_t i = 0; i < A.r; ++i) for (std::size_t k = 0; k < A.c; ++k) { T v = A(i, k); for (std::size_t j = 0; j < B.c; ++j) C(i, j) += v * B(k, j); }
    return C;
}
// LU with partial pivoting, in place: A = P^T L U. Returns the permutation; throws on a singular matrix.
template <class T> std::vector<std::size_t> lu(mat<T>& A) {
    std::size_t n = A.r; std::vector<std::size_t> p(n); for (std::size_t i = 0; i < n; ++i) p[i] = i;
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t m = k; for (std::size_t i = k + 1; i < n; ++i) if (std::fabs(A(i, k)) > std::fabs(A(m, k))) m = i;
        if (A(m, k) == T(0)) throw std::runtime_error("lu: singular");
        if (m != k) { for (std::size_t j = 0; j < n; ++j) std::swap(A(k, j), A(m, j)); std::swap(p[k], p[m]); }
        for (std::size_t i = k + 1; i < n; ++i) { A(i, k) /= A(k, k); T f = A(i, k); for (std::size_t j = k + 1; j < n; ++j) A(i, j) -= f * A(k, j); }
    }
    return p;
}
template <class T> std::vector<T> lu_solve(const mat<T>& LU, const std::vector<std::size_t>& p, const std::vector<T>& b) {
    std::size_t n = LU.r; std::vector<T> x(n);
    for (std::size_t i = 0; i < n; ++i) { T s = b[p[i]]; for (std::size_t j = 0; j < i; ++j) s -= LU(i, j) * x[j]; x[i] = s; }
    for (std::size_t i = n; i-- > 0;) { T s = x[i]; for (std::size_t j = i + 1; j < n; ++j) s -= LU(i, j) * x[j]; x[i] = s / LU(i, i); }
    return x;
}
// Cholesky A = L L^T (lower). Throws if A is not positive definite.
template <class T> mat<T> cholesky(const mat<T>& A) {
    std::size_t n = A.r; mat<T> L(n, n);
    for (std::size_t j = 0; j < n; ++j) {
        T d = A(j, j); for (std::size_t k = 0; k < j; ++k) d -= L(j, k) * L(j, k);
        if (!(d > T(0))) throw std::runtime_error("cholesky: not positive definite");
        L(j, j) = std::sqrt(d);
        for (std::size_t i = j + 1; i < n; ++i) { T s = A(i, j); for (std::size_t k = 0; k < j; ++k) s -= L(i, k) * L(j, k); L(i, j) = s / L(j, j); }
    }
    return L;
}
// Householder QR of an m x n matrix (m >= n): Q is m x m orthogonal, R is m x n upper triangular.
template <class T> void qr(const mat<T>& A, mat<T>& Q, mat<T>& R) {
    std::size_t m = A.r, n = A.c; R = A; Q = mat<T>(m, m); for (std::size_t i = 0; i < m; ++i) Q(i, i) = 1;
    std::vector<T> v(m);
    for (std::size_t k = 0; k < n && k + 1 < m + (m > n); ++k) {
        T nrm = 0; for (std::size_t i = k; i < m; ++i) nrm += R(i, k) * R(i, k); nrm = std::sqrt(nrm);
        if (nrm == T(0)) continue;
        T alpha = R(k, k) > 0 ? -nrm : nrm;
        for (std::size_t i = 0; i < m; ++i) v[i] = i < k ? 0 : R(i, k); v[k] -= alpha;
        T vv = 0; for (std::size_t i = k; i < m; ++i) vv += v[i] * v[i]; if (vv == T(0)) continue;
        for (std::size_t j = 0; j < n; ++j) { T s = 0; for (std::size_t i = k; i < m; ++i) s += v[i] * R(i, j); s = 2 * s / vv; for (std::size_t i = k; i < m; ++i) R(i, j) -= s * v[i]; }
        for (std::size_t j = 0; j < m; ++j) { T s = 0; for (std::size_t i = k; i < m; ++i) s += Q(j, i) * v[i]; s = 2 * s / vv; for (std::size_t i = k; i < m; ++i) Q(j, i) -= s * v[i]; }
    }
}
template <class T> struct csr { std::size_t rows = 0, cols = 0; std::vector<std::size_t> ptr{0}, idx; std::vector<T> val; };
template <class T> csr<T> to_csr(const mat<T>& A) {
    csr<T> s; s.rows = A.r; s.cols = A.c;
    for (std::size_t i = 0; i < A.r; ++i) { for (std::size_t j = 0; j < A.c; ++j) if (A(i, j) != T(0)) { s.idx.push_back(j); s.val.push_back(A(i, j)); } s.ptr.push_back(s.idx.size()); }
    return s;
}
template <class T> void spmv(const csr<T>& A, const T* x, T* y) {
    for (std::size_t i = 0; i < A.rows; ++i) { T s = 0; for (std::size_t k = A.ptr[i]; k < A.ptr[i + 1]; ++k) s += A.val[k] * x[A.idx[k]]; y[i] = s; }
}
// Conjugate gradients for SPD A; returns the iteration count.
template <class T> int cg(const csr<T>& A, const std::vector<T>& b, std::vector<T>& x, T tol = T(1e-10), int maxit = 1000) {
    std::size_t n = b.size(); x.assign(n, T(0)); std::vector<T> r = b, p = b, Ap(n);
    auto dot = [&](const std::vector<T>& u, const std::vector<T>& v) { T s = 0; for (std::size_t i = 0; i < n; ++i) s += u[i] * v[i]; return s; };
    T rr = dot(r, r), bb = rr;
    for (int it = 0; it < maxit; ++it) {
        if (std::sqrt(rr) <= tol * std::sqrt(bb)) return it;
        spmv(A, p.data(), Ap.data()); T al = rr / dot(p, Ap);
        for (std::size_t i = 0; i < n; ++i) { x[i] += al * p[i]; r[i] -= al * Ap[i]; }
        T rn = dot(r, r); for (std::size_t i = 0; i < n; ++i) p[i] = r[i] + (rn / rr) * p[i]; rr = rn;
    }
    return maxit;
}
// Symmetric eigendecomposition by cyclic Jacobi: A = V diag(w) V^T, eigenvalues ascending.
template <class T> void sym_eig(mat<T> A, std::vector<T>& w, mat<T>& V, int sweeps = 100) {
    std::size_t n = A.r; V = mat<T>(n, n); for (std::size_t i = 0; i < n; ++i) V(i, i) = 1;
    for (int s = 0; s < sweeps; ++s) {
        T off = 0; for (std::size_t i = 0; i < n; ++i) for (std::size_t j = i + 1; j < n; ++j) off += A(i, j) * A(i, j);
        if (off < T(1e-30)) break;
        for (std::size_t p = 0; p < n; ++p) for (std::size_t q = p + 1; q < n; ++q) {
            if (std::fabs(A(p, q)) < T(1e-300)) continue;
            T th = (A(q, q) - A(p, p)) / (2 * A(p, q));
            T t = (th >= 0 ? T(1) : T(-1)) / (std::fabs(th) + std::sqrt(th * th + 1)), c = 1 / std::sqrt(t * t + 1), sn = t * c;
            for (std::size_t k = 0; k < n; ++k) { T akp = A(k, p), akq = A(k, q); A(k, p) = c * akp - sn * akq; A(k, q) = sn * akp + c * akq; }
            for (std::size_t k = 0; k < n; ++k) { T apk = A(p, k), aqk = A(q, k); A(p, k) = c * apk - sn * aqk; A(q, k) = sn * apk + c * aqk; }
            for (std::size_t k = 0; k < n; ++k) { T vkp = V(k, p), vkq = V(k, q); V(k, p) = c * vkp - sn * vkq; V(k, q) = sn * vkp + c * vkq; }
        }
    }
    std::vector<std::size_t> o(n); for (std::size_t i = 0; i < n; ++i) o[i] = i;
    for (std::size_t i = 0; i < n; ++i) for (std::size_t j = i + 1; j < n; ++j) if (A(o[j], o[j]) < A(o[i], o[i])) std::swap(o[i], o[j]);
    mat<T> V2(n, n); w.assign(n, 0);
    for (std::size_t j = 0; j < n; ++j) { w[j] = A(o[j], o[j]); for (std::size_t i = 0; i < n; ++i) V2(i, j) = V(i, o[j]); }
    V = V2;
}
// Thin SVD by one-sided Jacobi (m >= n): A = U diag(s) V^T, singular values descending.
template <class T> void svd(const mat<T>& A, mat<T>& U, std::vector<T>& s, mat<T>& V, int sweeps = 100) {
    std::size_t m = A.r, n = A.c; if (m < n) throw std::invalid_argument("svd: needs rows >= cols");
    U = A; V = mat<T>(n, n); for (std::size_t i = 0; i < n; ++i) V(i, i) = 1;
    for (int sw = 0; sw < sweeps; ++sw) {
        bool rot = false;
        for (std::size_t p = 0; p < n; ++p) for (std::size_t q = p + 1; q < n; ++q) {
            T a = 0, b = 0, g = 0;
            for (std::size_t i = 0; i < m; ++i) { a += U(i, p) * U(i, p); b += U(i, q) * U(i, q); g += U(i, p) * U(i, q); }
            if (std::fabs(g) <= T(1e-15) * std::sqrt(a * b) || g == 0) continue;
            rot = true;
            T z = (b - a) / (2 * g), t = (z >= 0 ? T(1) : T(-1)) / (std::fabs(z) + std::sqrt(1 + z * z)), c = 1 / std::sqrt(1 + t * t), sn = c * t;
            for (std::size_t i = 0; i < m; ++i) { T up = U(i, p), uq = U(i, q); U(i, p) = c * up - sn * uq; U(i, q) = sn * up + c * uq; }
            for (std::size_t i = 0; i < n; ++i) { T vp = V(i, p), vq = V(i, q); V(i, p) = c * vp - sn * vq; V(i, q) = sn * vp + c * vq; }
        }
        if (!rot) break;
    }
    s.assign(n, 0);
    for (std::size_t j = 0; j < n; ++j) { T nn = 0; for (std::size_t i = 0; i < m; ++i) nn += U(i, j) * U(i, j); s[j] = std::sqrt(nn);
        if (s[j] > 0) for (std::size_t i = 0; i < m; ++i) U(i, j) /= s[j]; }
    std::vector<std::size_t> o(n); for (std::size_t i = 0; i < n; ++i) o[i] = i;
    for (std::size_t i = 0; i < n; ++i) for (std::size_t j = i + 1; j < n; ++j) if (s[o[j]] > s[o[i]]) std::swap(o[i], o[j]);
    mat<T> U2(m, n), V2(n, n); std::vector<T> s2(n);
    for (std::size_t j = 0; j < n; ++j) { s2[j] = s[o[j]]; for (std::size_t i = 0; i < m; ++i) U2(i, j) = U(i, o[j]); for (std::size_t i = 0; i < n; ++i) V2(i, j) = V(i, o[j]); }
    U = U2; V = V2; s = s2;
}
// Restarted GMRES(k) for general (non-symmetric) CSR systems. Returns iterations used, -1 if not converged.
template <class T> int gmres(const csr<T>& A, const std::vector<T>& b, std::vector<T>& x, int restart = 30, T tol = T(1e-10), int maxit = 1000) {
    std::size_t n = b.size(); if (x.size() != n) x.assign(n, 0);
    auto nrm = [](const std::vector<T>& v) { T s = 0; for (T e : v) s += e * e; return std::sqrt(s); };
    T bn = nrm(b); if (bn == 0) bn = 1;
    int it = 0;
    while (it < maxit) {
        std::vector<T> r(n); spmv(A, x.data(), r.data()); for (std::size_t i = 0; i < n; ++i) r[i] = b[i] - r[i];
        T beta = nrm(r); if (beta / bn < tol) return it;
        std::size_t k = std::size_t(restart);
        std::vector<std::vector<T>> Vb(k + 1, std::vector<T>(n)); mat<T> H(k + 1, k);
        std::vector<T> cs(k), sn(k), g(k + 1); g[0] = beta;
        for (std::size_t i = 0; i < n; ++i) Vb[0][i] = r[i] / beta;
        std::size_t j = 0;
        for (; j < k && it < maxit; ++j, ++it) {
            spmv(A, Vb[j].data(), Vb[j + 1].data());
            for (std::size_t i = 0; i <= j; ++i) { T h = 0; for (std::size_t q = 0; q < n; ++q) h += Vb[j + 1][q] * Vb[i][q]; H(i, j) = h;
                for (std::size_t q = 0; q < n; ++q) Vb[j + 1][q] -= h * Vb[i][q]; }
            H(j + 1, j) = nrm(Vb[j + 1]);
            if (H(j + 1, j) > 0) for (auto& e : Vb[j + 1]) e /= H(j + 1, j);
            for (std::size_t i = 0; i < j; ++i) { T t = cs[i] * H(i, j) + sn[i] * H(i + 1, j); H(i + 1, j) = -sn[i] * H(i, j) + cs[i] * H(i + 1, j); H(i, j) = t; }
            T d = std::sqrt(H(j, j) * H(j, j) + H(j + 1, j) * H(j + 1, j)); cs[j] = H(j, j) / d; sn[j] = H(j + 1, j) / d;
            H(j, j) = d; H(j + 1, j) = 0; g[j + 1] = -sn[j] * g[j]; g[j] = cs[j] * g[j];
            if (std::fabs(g[j + 1]) / bn < tol) { ++j; ++it; break; }
        }
        std::vector<T> y(j);
        for (std::size_t i = j; i-- > 0;) { T sum = g[i]; for (std::size_t q = i + 1; q < j; ++q) sum -= H(i, q) * y[q]; y[i] = sum / H(i, i); }
        for (std::size_t i = 0; i < j; ++i) for (std::size_t q = 0; q < n; ++q) x[q] += y[i] * Vb[i][q];
        std::vector<T> r2(n); spmv(A, x.data(), r2.data()); for (std::size_t i = 0; i < n; ++i) r2[i] = b[i] - r2[i];
        if (nrm(r2) / bn < tol) return it;
    }
    return -1;
}
}
