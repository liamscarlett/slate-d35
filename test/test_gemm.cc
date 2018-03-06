#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#ifdef SLATE_WITH_MPI
#include <mpi.h>
#else
#include "slate_NoMpi.hh"
#endif

#include "slate.hh"
#include "slate_Debug.hh"
#include "test.hh"
#include "error.hh"
#include "blas_flops.hh"

#include "scalapack_wrappers.hh"

//#include "mkl.h"
extern "C" int MKL_Set_Num_Threads( int nt );

//------------------------------------------------------------------------------
template< typename scalar_t >
void test_gemm_work( Params& params, bool run )
{
    typedef typename blas::traits< scalar_t >::real_t real_t;

    // get & mark input values
    int64_t n = params.dim.n();
    int64_t nb = params.nb.value();
    int64_t p = params.p.value();
    int64_t q = params.q.value();
    bool check = params.check.value()=='y';
    bool ref = params.ref.value()=='y';
    bool trace = params.trace.value()=='y';
    const char *transa = "n";
    const char *transb = "n";
    // int64_t align = params.align.value();
    int64_t lookahead = params.lookahead.value();

    // mark non-standard output values
    params.time.value();
    params.gflops.value();
    params.ref_time.value();
    params.ref_gflops.value();

    if (! run)
        return;

    // Local values
    static int i0=0, i1=1;
    static scalar_t alpha = 1.234;
    static scalar_t beta = 4.321;

    // BLACS/MPI variables
    int ictxt, nprow, npcol, myrow, mycol, info, mloc, nloc;
    int descA_tst[9], descB_tst[9], descC_tst[9], descC_ref[9];
    int iam=0, nprocs=1;
    int n_ = n; 
    int nb_ = nb;

    // Initialize BLACS and ScaLAPACK
    Cblacs_pinfo( &iam, &nprocs );  assert(p*q <= nprocs);
    Cblacs_get( -1, 0, &ictxt );
    Cblacs_gridinit( &ictxt, "Row", p, q );
    Cblacs_gridinfo( ictxt, &nprow, &npcol, &myrow, &mycol );
    mloc = scalapack_numroc( &n_, &nb_, &myrow, &i0, &nprow );
    nloc = scalapack_numroc( &n_, &nb_, &mycol, &i0, &npcol );

    // typedef long long lld;

    // Allocate space
    size_t size_A = (size_t)( mloc*nloc );
    std::vector< scalar_t > A_tst( size_A );
    std::vector< scalar_t > B_tst( size_A );
    std::vector< scalar_t > C_tst( size_A );
    std::vector< scalar_t > C_ref;

    // Initialize the matrix
    int iseed = 0;
    scalapack_pdplrnt( &A_tst[0], n_, n_, nb_, nb_, myrow, mycol, nprow, npcol, mloc, iseed+1 );
    scalapack_pdplrnt( &B_tst[0], n_, n_, nb_, nb_, myrow, mycol, nprow, npcol, mloc, iseed+2 );
    scalapack_pdplrnt( &C_tst[0], n_, n_, nb_, nb_, myrow, mycol, nprow, npcol, mloc, iseed+3 );

    // Create ScaLAPACK descriptors
    scalapack_descinit( descA_tst, &n_, &n_, &nb_, &nb_, &i0, &i0, &ictxt, &mloc, &info ); assert(info==0);
    scalapack_descinit( descB_tst, &n_, &n_, &nb_, &nb_, &i0, &i0, &ictxt, &mloc, &info ); assert(info==0);
    scalapack_descinit( descC_tst, &n_, &n_, &nb_, &nb_, &i0, &i0, &ictxt, &mloc, &info ); assert(info==0);

    // If check is required, save data and create a descriptor for it
    if ( check || ref ) {
        C_ref.resize( size_A );
        C_ref = C_tst;
        scalapack_descinit( descC_ref, &n_, &n_, &nb_, &nb_, &i0, &i0, &ictxt, &mloc, &info ); assert(info==0);
    }

    // Create SLATE matrices from the ScaLAPACK layouts
    int64_t local_lda = (int64_t)descA_tst[8];
    slate::Matrix<double> A( n_, n_, &A_tst[0], local_lda, nb_, nb_, nprow, npcol, local_lda, MPI_COMM_WORLD );
    slate::Matrix<double> B( n_, n_, &B_tst[0], local_lda, nb_, nb_, nprow, npcol, local_lda, MPI_COMM_WORLD );
    slate::Matrix<double> C( n_, n_, &C_tst[0], local_lda, nb_, nb_, nprow, npcol, local_lda, MPI_COMM_WORLD );

    if (trace) slate::trace::Trace::on();
    else slate::trace::Trace::off();

    // Call the routine using ScaLAPACK layout
    MPI_Barrier(MPI_COMM_WORLD);
    double time = libtest::get_wtime();
    slate::gemm<slate::Target::HostTask>(
        alpha, A, B, beta, C, {{slate::Option::Lookahead, lookahead}});
    // sla_pgemm( transa, transb, &n_, &n_, &n_, &alpha,
    //            &A_tst[0], &i1, &i1, descA_tst,
    //            &B_tst[0], &i1, &i1, descB_tst, &beta,
    //            &C_tst[0], &i1, &i1, descC_tst );
    MPI_Barrier(MPI_COMM_WORLD);
    double time_tst = libtest::get_wtime() - time;
    
    if (trace) slate::trace::Trace::finish();

    // Compute and save timing/performance
    double gflop = blas::Gflop< scalar_t >::gemm( n, n, n );
    params.time.value() = time_tst;
    params.gflops.value() = gflop / time_tst;

    real_t tol = params.tol.value();

    // if ( 0==1 )
    if ( check || ref ) {
        // Comparison with reference routine from ScaLAPACK

        // Set MKL num threads appropriately for parallel BLAS
        int omp_num_threads = 1;
        #pragma omp parallel
        { omp_num_threads = omp_get_num_threads(); }
        int saved_mkl_num_threads = MKL_Set_Num_Threads(omp_num_threads);

        // Run the reference routine
        MPI_Barrier(MPI_COMM_WORLD);        
        double time = libtest::get_wtime();
        scalapack_pgemm( transa, transb, &n_, &n_, &n_, &alpha,
            &A_tst[0], &i1, &i1, descA_tst,
            &B_tst[0], &i1, &i1, descB_tst, &beta,
            &C_ref[0], &i1, &i1, descC_ref );
        MPI_Barrier(MPI_COMM_WORLD);
        double time_ref = libtest::get_wtime() - time;

        // Allocate work space
        std::vector< scalar_t > worklange( mloc );

        // blas::axpy((size_t)lda*n, -1.0, C_tst, 1, C_ref, 1);
        // Local operation: error = C_ref - C_tst
        for(size_t i = 0; i < C_ref.size(); i++)
            C_ref[i] = C_ref[i] - C_tst[i];

        // norm(C_tst)
        real_t C_tst_norm = scalapack_plange( "I", &n_, &n_, &C_tst[0], &i1, &i1, descC_tst, &worklange[0]);

        // norm(C_ref - C_tst)
        real_t error_norm = scalapack_plange( "I", &n_, &n_, &C_ref[0], &i1, &i1, descC_tst, &worklange[0] );
        if ( C_tst_norm != 0 )
            error_norm /=  C_tst_norm;

        params.ref_time.value() = time_ref;
        params.ref_gflops.value() = gflop / time_ref;
        params.error.value() = error_norm;

        MKL_Set_Num_Threads(saved_mkl_num_threads);
    }

    params.okay.value() = (params.error.value() <= tol);

    //Cblacs_exit(1) is commented out because it does not handle re-entering ... some unknown problem
    //Cblacs_exit(1); // 1 means that you can run Cblacs again
}

// -----------------------------------------------------------------------------
void test_gemm( Params& params, bool run )
{
    switch (params.datatype.value()) {
        case libtest::DataType::Integer:
            throw std::exception();
            break;

        case libtest::DataType::Single:
            throw std::exception();// test_gemm_work< float >( params, run );
            break;

        case libtest::DataType::Double:
            test_gemm_work< double >( params, run );
            break;

        case libtest::DataType::SingleComplex:
            throw std::exception();// test_gemm_work< std::complex<float> >( params, run );
            break;

        case libtest::DataType::DoubleComplex:
            throw std::exception();// test_gemm_work< std::complex<double> >( params, run );
            break;
    }
}