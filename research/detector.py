"""Sibilance detector v3 (features2 + causal speaker adaptation + logistic model), shared by the evaluations."""
import numpy as np
from scipy.ndimage import uniform_filter1d

def adapt(F, tau=1500):
    out = F.copy(); base = None; a = 1.0 / tau; cols = slice(3, F.shape[1])
    for i in range(len(F)):
        if F[i, 0] < 0.2 and F[i, 2] > -25:
            base = F[i, cols].copy() if base is None else base + a * (F[i, cols] - base)
        if base is not None:
            out[i, cols] = F[i, cols] - base
    return out

def design(F, A):
    G = uniform_filter1d(F, 7, axis=0); H = uniform_filter1d(A, 7, axis=0)
    bands = H[:, 3:-1]; peak = bands.max(1); low_hi = np.maximum(bands[:, 1], bands[:, 2]) - bands[:, 0]
    return np.column_stack([G[:, :3], H[:, 3:], peak, low_hi, G[:, 0] * peak, G[:, 1] * peak, np.ones(len(G))])

def fit(X, y, l2=1e-2):
    mu, sd = X[:, :-1].mean(0), X[:, :-1].std(0) + 1e-9
    Z = X.copy(); Z[:, :-1] = (Z[:, :-1] - mu) / sd; w = np.zeros(Z.shape[1])
    for it in range(60):
        p = 1 / (1 + np.exp(-Z @ w)); W = p * (1 - p) + 1e-6
        w += np.linalg.solve(Z.T @ (Z * W[:, None]) + l2 * np.eye(len(w)), Z.T @ (y - p))
    return w, mu, sd

def prob(X, m):
    w, mu, sd = m; Z = X.copy(); Z[:, :-1] = (Z[:, :-1] - mu) / sd; return 1 / (1 + np.exp(-Z @ w))
