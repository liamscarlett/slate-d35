#include "slate/slate.hh"
#include "test.hh"
#include "blas_flops.hh"

#include "scalapack_wrappers.hh"
#include "scalapack_support_routines.hh"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

//------------------------------------------------------------------------------
template< typename scalar_t >
void test_syr2k_work(Params& params, bool run)
{
    using real_t = blas::real_type<scalar_t>;
    using slate::Op;
    using slate::Norm;

    // get & mark input values
    slate::Uplo uplo = params.uplo();
    slate::Op trans = params.trans();
    int64_t n = params.dim.n();
    int64_t k = params.dim.k();
    scalar_t alpha = params.alpha();
    scalar_t beta = params.beta();
    int64_t p = params.p();
    int64_t q = params.q();
    int64_t nb = params.nb();
    int64_t lookahead = params.lookahead();
    slate::Norm norm = params.norm();
    bool check = params.check() == 'y';
    bool ref = params.ref() == 'y';
    bool trace = params.trace() == 'y';
    slate::Target target = char2target(params.target());

    // mark non-standard output values
    params.time();
    params.gflops();
    params.ref_time();
    params.ref_gflops();

    if (! run)
        return;

    // Error analysis applies in these norms.
    slate_assert(norm == Norm::One || norm == Norm::Inf || norm == Norm::Fro);

    // setup so op(A) and op(B) are n-by-k
    int64_t Am = (trans == slate::Op::NoTrans ? n : k);
    int64_t An = (trans == slate::Op::NoTrans ? k : n);
    int64_t Bm = Am;
    int64_t Bn = An;
    int64_t Cm = n;
    int64_t Cn = n;

    // Local values
    const int izero = 0, ione = 1;

    // BLACS/MPI variables
    int ictxt, nprow, npcol, myrow, mycol, info;
    int descA_tst[9], descB_tst[9], descC_tst[9], descC_ref[9];
    int iam = 0, nprocs = 1;
    int iseed = 1;

    // initialize BLACS and ScaLAPACK
    Cblacs_pinfo(&iam, &nprocs);
    assert(p*q <= nprocs);
    Cblacs_get(-1, 0, &ictxt);
    Cblacs_gridinit(&ictxt, "Col", p, q);
    Cblacs_gridinfo(ictxt, &nprow, &npcol, &myrow, &mycol);

    // matrix A, figure out local size, allocate, create descriptor, initialize
    int64_t mlocA = scalapack_numroc(Am, nb, myrow, izero, nprow);
    int64_t nlocA = scalapack_numroc(An, nb, mycol, izero, npcol);
    scalapack_descinit(descA_tst, Am, An, nb, nb, izero, izero, ictxt, mlocA, &info);
    assert(info == 0);
    int64_t lldA = (int64_t)descA_tst[8];
    std::vector< scalar_t > A_tst(lldA*nlocA);
    scalapack_pplrnt(&A_tst[0], Am, An, nb, nb, myrow, mycol, nprow, npcol, mlocA, iseed + 1);

    // matrix B, figure out local size, allocate, create descriptor, initialize
    int64_t mlocB = scalapack_numroc(Bm, nb, myrow, izero, nprow);
    int64_t nlocB = scalapack_numroc(Bn, nb, mycol, izero, npcol);
    scalapack_descinit(descB_tst, Bm, Bn, nb, nb, izero, izero, ictxt, mlocB, &info);
    assert(info == 0);
    int64_t lldB = (int64_t)descB_tst[8];
    std::vector< scalar_t > B_tst(lldB*nlocB);
    scalapack_pplrnt(&B_tst[0], Bm, Bn, nb, nb, myrow, mycol, nprow, npcol, mlocB, iseed + 1);

    // matrix C, figure out local size, allocate, create descriptor, initialize
    int64_t mlocC = scalapack_numroc(Cm, nb, myrow, izero, nprow);
    int64_t nlocC = scalapack_numroc(Cn, nb, mycol, izero, npcol);
    scalapack_descinit(descC_tst, Cm, Cn, nb, nb, izero, izero, ictxt, mlocC, &info);
    assert(info == 0);
    int64_t lldC = (int64_t)descC_tst[8];
    std::vector< scalar_t > C_tst(lldC*nlocC);
    scalapack_pplrnt(&C_tst[0], Cm, Cn, nb, nb, myrow, mycol, nprow, npcol, mlocC, iseed + 1);

    // if check is required, copy test data and create a descriptor for it
    std::vector< scalar_t > C_ref;
    if (check || ref) {
        C_ref = C_tst;
        scalapack_descinit(descC_ref, Cm, Cn, nb, nb, izero, izero, ictxt, mlocC, &info);
        assert(info == 0);
    }

    // create SLATE matrices from the ScaLAPACK layouts
    auto A = slate::Matrix<scalar_t>::fromScaLAPACK(Am, An, &A_tst[0], lldA, nb, nprow, npcol, MPI_COMM_WORLD);
    auto B = slate::Matrix<scalar_t>::fromScaLAPACK(Bm, Bn, &B_tst[0], lldB, nb, nprow, npcol, MPI_COMM_WORLD);
    auto C = slate::SymmetricMatrix<scalar_t>::fromScaLAPACK(uplo, Cn, &C_tst[0], lldC, nb, nprow, npcol, MPI_COMM_WORLD);

    if (trans == slate::Op::Trans) {
        A = transpose(A);
        B = transpose(B);
    }
    else if (trans == slate::Op::ConjTrans) {
        A = conj_transpose(A);
        B = conj_transpose(B);
    }
    assert(A.mt() == C.mt());
    assert(B.mt() == C.mt());
    assert(A.nt() == B.nt());

    if (trace) slate::trace::Trace::on();
    else slate::trace::Trace::off();

    {
        slate::trace::Block trace_block("MPI_Barrier");
        MPI_Barrier(MPI_COMM_WORLD);
    }
    double time = libtest::get_wtime();

    //==================================================
    // Run SLATE test.
    // C = alpha A B^T + alpha B A^T + beta C.
    //==================================================
    slate::syr2k(alpha, A, B, beta, C, {
        {slate::Option::Lookahead, lookahead},
        {slate::Option::Target, target}
    });

    {
        slate::trace::Block trace_block("MPI_Barrier");
        MPI_Barrier(MPI_COMM_WORLD);
    }
    double time_tst = libtest::get_wtime() - time;

    if (trace) slate::trace::Trace::finish();

    // Compute and save timing/performance
    double gflop = blas::Gflop< scalar_t >::syr2k(n, n);
    params.time() = time_tst;
    params.gflops() = gflop / time_tst;

    if (check || ref) {
        // comparison with reference routine from ScaLAPACK

        // set MKL num threads appropriately for parallel BLAS
        int omp_num_threads;
        #pragma omp parallel
        { omp_num_threads = omp_get_num_threads(); }
        int saved_num_threads = slate_set_num_blas_threads(omp_num_threads);

        // allocate workspace for norms
        size_t ldw = nb*ceil(ceil(mlocC / (double) nb) / (scalapack_ilcm(&nprow, &npcol) / nprow));
        std::vector< real_t > worklansy(2*nlocC + mlocC + ldw);
        std::vector< real_t > worklange(std::max({mlocA, mlocB, nlocA, nlocB}));

        // get norms of the original data
        real_t A_norm = scalapack_plange(norm2str(norm), Am, An, &A_tst[0], ione, ione, descA_tst, &worklange[0]);
        real_t B_norm = scalapack_plange(norm2str(norm), Bm, Bn, &B_tst[0], ione, ione, descB_tst, &worklange[0]);
        real_t C_orig_norm = scalapack_plansy(norm2str(norm), uplo2str(uplo), Cn, &C_ref[0], ione, ione, descC_ref, &worklansy[0]);

        //==================================================
        // Run ScaLAPACK reference routine.
        //==================================================
        MPI_Barrier(MPI_COMM_WORLD);
        time = libtest::get_wtime();
        scalapack_psyr2k(uplo2str(uplo), op2str(trans), n, k, alpha,
                         &A_tst[0], ione, ione, descA_tst,
                         &B_tst[0], ione, ione, descB_tst, beta,
                         &C_ref[0], ione, ione, descC_ref);
        MPI_Barrier(MPI_COMM_WORLD);
        double time_ref = libtest::get_wtime() - time;

        // local operation: error = C_ref - C_tst
        blas::axpy(C_ref.size(), -1.0, &C_tst[0], 1, &C_ref[0], 1);

        // norm(C_ref - C_tst)
        real_t C_diff_norm = scalapack_plansy(norm2str(norm), uplo2str(uplo), Cn, &C_ref[0], ione, ione, descC_ref, &worklansy[0]);

        real_t error = C_diff_norm
                     / (sqrt(real_t(2*k) + 2) * std::abs(alpha) * A_norm * B_norm
                        + 2 * std::abs(beta) * C_orig_norm);

        params.ref_time() = time_ref;
        params.ref_gflops() = gflop / time_ref;
        params.error() = error;

        slate_set_num_blas_threads(saved_num_threads);

        // Allow 3*eps; complex needs 2*sqrt(2) factor; see Higham, 2002, sec. 3.6.
        real_t eps = std::numeric_limits<real_t>::epsilon();
        params.okay() = (params.error() <= 3*eps);
    }

    //Cblacs_exit(1) is commented out because it does not handle re-entering ... some unknown problem
    //Cblacs_exit(1); // 1 means that you can run Cblacs again
}

// -----------------------------------------------------------------------------
void test_syr2k(Params& params, bool run)
{
    switch (params.datatype()) {
        case libtest::DataType::Integer:
            throw std::exception();
            break;

        case libtest::DataType::Single:
            test_syr2k_work<float> (params, run);
            break;

        case libtest::DataType::Double:
            test_syr2k_work<double> (params, run);
            break;

        case libtest::DataType::SingleComplex:
            test_syr2k_work<std::complex<float>> (params, run);
            break;

        case libtest::DataType::DoubleComplex:
            test_syr2k_work<std::complex<double>> (params, run);
            break;
    }
}
