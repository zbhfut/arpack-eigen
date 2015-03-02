#ifndef SYMEIGSSOLVER_H
#define SYMEIGSSOLVER_H

#include <Eigen/Dense>
#include <vector>
#include <algorithm>
#include <cmath>
#include <utility>

#include "MatOp.h"
#include "SelectionRule.h"


template <typename Scalar = double, int SelectionRule = LARGEST_MAGN>
class SymEigsSolver
{
protected:
    typedef Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic> Matrix;
    typedef Eigen::Matrix<Scalar, Eigen::Dynamic, 1> Vector;
    typedef Eigen::Array<Scalar, Eigen::Dynamic, 1> Array;
    typedef Eigen::Array<bool, Eigen::Dynamic, 1> BoolArray;
    typedef Eigen::Map<const Matrix> MapMat;
    typedef Eigen::Map<const Vector> MapVec;
    typedef Eigen::SelfAdjointEigenSolver<Matrix> EigenSolver;
    typedef Eigen::HouseholderQR<Matrix> QRdecomp;
    typedef Eigen::HouseholderSequence<Matrix, Vector> QRQ;
    typedef std::pair<Scalar, int> SortPair;

    MatOp<Scalar> *op;   // object to conduct matrix operation,
                         // e.g. matrix-vector product
    int dim_n;           // dimension of matrix A
    int nev;             // number of eigenvalues requested
    int ncv;             // number of ritz values

    Matrix fac_V;        // V matrix in the Arnoldi factorization
    Matrix fac_H;        // H matrix in the Arnoldi factorization
    Vector fac_f;        // residual in the Arnoldi factorization

    Vector ritz_val;     // ritz values
    Matrix ritz_vec;     // ritz vectors
    BoolArray ritz_conv; // indicator of the convergence of ritz values

    const Scalar prec;   // precision parameter used to test convergence
                         // prec = epsilon^(2/3)
                         // epsilon is the machine precision,
                         // e.g. ~= 1e-16 for the "double" type

    // Matrix product in this case, and shift solve for SymEigsShiftSolver
    virtual void matrix_operation(Scalar *x_in, Scalar *y_out)
    {
        op->prod(x_in, y_out);
    }

    // Arnoldi factorization starting from step-k
    void factorize_from(int from_k, int to_m, const Vector &fk)
    {
        if(to_m <= from_k) return;

        fac_f = fk;

        Vector v(dim_n);
        Scalar beta = 0.0;
        for(int i = from_k; i <= to_m - 1; i++)
        {
            beta = fac_f.norm();
            v.noalias() = fac_f / beta;
            fac_V.col(i) = v; // The (i+1)-th column
            fac_H.block(i, 0, 1, i).setZero();
            fac_H(i, i - 1) = beta;

            Vector w(dim_n);
            matrix_operation(v.data(), w.data());
            Vector h = fac_V.leftCols(i + 1).adjoint() * w;
            fac_f = w - fac_V.leftCols(i + 1) * h;
            fac_H.block(0, i, i + 1, 1) = h;
        }
    }

    // Implicitly restarted Arnoldi factorization
    void restart(int k)
    {
        if(k >= ncv)
            return;

        QRdecomp qr;
        Vector em(ncv);
        em.setZero();
        em[ncv - 1] = 1;

        for(int i = k; i < ncv; i++)
        {
            qr.compute(fac_H - ritz_val[i] * Matrix::Identity(ncv, ncv));
            QRQ Q = qr.householderQ();
            // V -> VQ
            fac_V.applyOnTheRight(Q);
            // H -> Q'HQ
            fac_H.applyOnTheRight(Q);
            fac_H.applyOnTheLeft(Q.adjoint());
            // em -> Q'em
            em.applyOnTheLeft(Q.adjoint());
        }

        Vector fk = fac_f * em[k - 1];
        factorize_from(k, ncv, fk);
        retrieve_ritzpair();
    }

    // Test convergence
    bool converged(Scalar tol)
    {
        // bound = tol * max(prec, abs(theta)), theta for ritz value
        Array bound = tol * ritz_val.head(nev).array().abs().max(prec);
        Array resid =  ritz_vec.bottomRows(1).transpose().array().abs() * fac_f.norm();
        ritz_conv = (resid < bound);

        return ritz_conv.all();
    }

    // Retrieve and sort ritz values and ritz vectors
    void retrieve_ritzpair()
    {
        EigenSolver eig(fac_H);
        Vector evals = eig.eigenvalues();
        Matrix evecs = eig.eigenvectors();

        std::vector<SortPair> pairs(ncv);
        EigenvalueComparator<Scalar, SelectionRule> comp;
        for(int i = 0; i < ncv; i++)
        {
            pairs[i].first = evals[i];
            pairs[i].second = i;
        }
        std::sort(pairs.begin(), pairs.end(), comp);
        // For BOTH_ENDS, the eigenvalues are sorted according
        // to the LARGEST_ALGE rule, so we need to move those smallest
        // values to the left
        if(SelectionRule == BOTH_ENDS)
        {
            int offset = (nev + 1) / 2;
            for(int i = 0; i < nev - offset; i++)
            {
                std::swap(pairs[offset + i], pairs[ncv - 1 - i]);
            }
        }

        for(int i = 0; i < ncv; i++)
        {
            ritz_val[i] = pairs[i].first;
        }
        for(int i = 0; i < nev; i++)
        {
            ritz_vec.col(i) = evecs.col(pairs[i].second);
        }
    }

    // Sort the first nev Ritz pairs in decreasing magnitude order
    // This is used to return the final results
    virtual void sort_ritzpair()
    {
        std::vector<SortPair> pairs(nev);
        EigenvalueComparator<Scalar, LARGEST_MAGN> comp;
        for(int i = 0; i < nev; i++)
        {
            pairs[i].first = ritz_val[i];
            pairs[i].second = i;
        }
        std::sort(pairs.begin(), pairs.end(), comp);

        Matrix new_ritz_vec(ncv, nev);
        BoolArray new_ritz_conv(nev);

        for(int i = 0; i < nev; i++)
        {
            ritz_val[i] = pairs[i].first;
            new_ritz_vec.col(i) = ritz_vec.col(pairs[i].second);
            new_ritz_conv[i] = ritz_conv[pairs[i].second];
        }

        ritz_vec.swap(new_ritz_vec);
        ritz_conv.swap(new_ritz_conv);
    }

public:
    SymEigsSolver(MatOp<Scalar> *op_, int nev_, int ncv_) :
        op(op_),
        dim_n(op->rows()),
        nev(nev_),
        ncv(ncv_),
        fac_V(dim_n, ncv),
        fac_H(ncv, ncv),
        fac_f(dim_n),
        ritz_val(ncv),
        ritz_vec(ncv, nev),
        ritz_conv(nev),
        prec(std::pow(std::numeric_limits<Scalar>::epsilon(), Scalar(2.0 / 3)))
    {}

    // Initialization and clean-up
    virtual void init(Scalar *init_coef)
    {
        fac_V.setZero();
        fac_H.setZero();
        fac_f.setZero();
        ritz_val.setZero();
        ritz_vec.setZero();
        ritz_conv.setZero();

        Vector v(dim_n);
        matrix_operation(init_coef, v.data());
        v.normalize();
        Vector w(dim_n);
        matrix_operation(v.data(), w.data());

        fac_H(0, 0) = v.dot(w);
        fac_f = w - v * fac_H(0, 0);
        fac_V.col(0) = v;
    }
    // Initialization with random initial coefficients
    virtual void init()
    {
        Vector init_coef = Vector::Random(dim_n);
        init(init_coef.data());
    }

    // Compute Ritz pairs and return the number of iteration
    int compute(int maxit = 1000, Scalar tol = 1e-10)
    {
        // The m-step Arnoldi factorization
        factorize_from(1, ncv, fac_f);
        retrieve_ritzpair();
        // Restarting
        int i = 0;
        for(i = 0; i < maxit; i++)
        {
            if(converged(tol))
                break;

            restart(nev);
        }
        // Sorting results
        sort_ritzpair();

        return i + 1;
    }

    // Return converged eigenvalues
    Vector eigenvalues()
    {
        int nconv = ritz_conv.cast<int>().sum();
        Vector res(nconv);

        if(!nconv)
            return res;

        int j = 0;
        for(int i = 0; i < nev; i++)
        {
            if(ritz_conv[i])
            {
                res[j] = ritz_val[i];
                j++;
            }
        }

        return res;
    }

    // Return converged eigenvectors
    Matrix eigenvectors()
    {
        int nconv = ritz_conv.cast<int>().sum();
        Matrix res(dim_n, nconv);

        if(!nconv)
            return res;

        Matrix ritz_vec_conv(ncv, nconv);
        int j = 0;
        for(int i = 0; i < nev; i++)
        {
            if(ritz_conv[i])
            {
                ritz_vec_conv.col(j) = ritz_vec.col(i);
                j++;
            }
        }

        return fac_V * ritz_vec_conv;
    }
};





template <typename Scalar = double, int SelectionRule = LARGEST_MAGN>
class SymEigsShiftSolver: public SymEigsSolver<Scalar, SelectionRule>
{
private:
    typedef Eigen::Array<Scalar, Eigen::Dynamic, 1> Array;

    Scalar sigma;
    MatOpWithRealShiftSolve<Scalar> *op_shift;

    // Shift solve in this case
    virtual void matrix_operation(Scalar *x_in, Scalar *y_out)
    {
        op_shift->shift_solve(x_in, y_out);
    }

    // First transform back the ritz values, and then sort
    virtual void sort_ritzpair()
    {
        Array ritz_val_org = Scalar(1.0) / this->ritz_val.head(this->nev).array() + sigma;
        this->ritz_val.head(this->nev) = ritz_val_org;
        SymEigsSolver<Scalar, SelectionRule>::sort_ritzpair();
    }
public:
    SymEigsShiftSolver(MatOpWithRealShiftSolve<Scalar> *op_,
                       int nev_, int ncv_, Scalar sigma_) :
        SymEigsSolver<Scalar, SelectionRule>(op_, nev_, ncv_),
        sigma(sigma_),
        op_shift(op_)
    {
        op_shift->set_shift(sigma);
    }
};

#endif // SYMEIGSSOLVER_H
