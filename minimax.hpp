#include <boost/multiprecision/cpp_bin_float.hpp>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <boost/math/tools/remez.hpp>
#include <boost/math/tools/polynomial.hpp>
#include <vector>
#include <functional>
#include <cmath>
#include "openfhe.h"

using mp_type = boost::multiprecision::cpp_bin_float_100;
using namespace std;
using namespace lbcrypto;

inline double f(const double& x) {
    return 1.0 / sqrt(x + 1e-12);
}

inline std::vector<double> MinimaxCoefficients(const std::function<double(double)>& f, double a, double b, int degree) {
    bool rel_error = false;
    boost::math::tools::remez_minimax<double> remez(f, degree, 0, a, b, false, rel_error, 0, 100);

    for (int i = 0; i < 10; ++i) {
        remez.iterate();
    }

    boost::math::tools::polynomial<double> poly = remez.numerator();
    return std::vector<double>(poly.data().begin(), poly.data().end());
}

inline double EvaluatePolynomial(const std::vector<double>& coeffs, double x) {
    double result = 0.0;
    for (int i = (int)coeffs.size() - 1; i >= 0; --i) {
        result = result * x + coeffs[i];
    }
    return result;
}

inline Ciphertext<DCRTPoly> HomomorphicPolyEval(
        CryptoContext<DCRTPoly> cc,
        const Ciphertext<DCRTPoly>& ct,
        const std::vector<double>& coeffs) {

    std::vector<Plaintext> plainCoeffs;
    plainCoeffs.reserve(coeffs.size());
    for (auto c : coeffs) {
        plainCoeffs.push_back(cc->MakeCKKSPackedPlaintext(std::vector<double>{c}));
    }

    // Horner’s method
    Ciphertext<DCRTPoly> result = cc->EvalMult(ct, plainCoeffs.back());
    for (int i = (int)coeffs.size() - 2; i >= 0; --i) {
        result = cc->EvalAdd(cc->EvalMult(result, ct), plainCoeffs[i]);
    }

    return result;
}

inline Ciphertext<DCRTPoly> MinimaxEvaluation(
        const std::function<double(double)>& f,
        const Ciphertext<DCRTPoly>& ct,
        double a,
        double b,
        int degree) {

    auto cc = ct->GetCryptoContext();
    std::vector<double> coefficients = MinimaxCoefficients(f, a, b, degree);
    return HomomorphicPolyEval(cc, ct, coefficients);
}
