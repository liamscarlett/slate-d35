//------------------------------------------------------------------------------
// Copyright (c) 2017, University of Tennessee
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the University of Tennessee nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL UNIVERSITY OF TENNESSEE BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//------------------------------------------------------------------------------
// This research was supported by the Exascale Computing Project (17-SC-20-SC),
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation's exascale computing imperative.
//------------------------------------------------------------------------------
// For assistance with SLATE, email <slate-user@icl.utk.edu>.
// You can also join the "SLATE User" Google group by going to
// https://groups.google.com/a/icl.utk.edu/forum/#!forum/slate-user,
// signing in with your Google credentials, and then clicking "Join group".
//------------------------------------------------------------------------------

#include "slate/slate.hh"
#include "aux/Debug.hh"
#include "slate/HermitianMatrix.hh"
#include "slate/Tile_blas.hh"
#include "slate/HermitianBandMatrix.hh"
#include "internal/internal.hh"

namespace slate {

//------------------------------------------------------------------------------
template <typename scalar_t>
void heev( HermitianMatrix<scalar_t>& A,
           std::vector< blas::real_type<scalar_t> >& W,
           const std::map<Option, Value>& opts)
{
    using real_t = blas::real_type<scalar_t>;

    int64_t n = A.n();

    // Scale matrix to allowable range, if necessary.
    // todo

    // 1. Reduce to band form.
    TriangularFactors<scalar_t> T;
    he2hb(A, T, opts);

    // Copy band.
    // Currently, gathers band matrix to rank 0.
    HermitianBandMatrix<scalar_t> Aband(A.uplo(), n, A.tileNb(0), A.tileNb(0),
                                        1, 1, A.mpiComm());
    Aband.insertLocalTiles();
    Aband.he2hbGather(A);

    // Currently, hb2st and sterf are run on a single node.
    W.resize(n);
    if (A.mpiRank() == 0) {
        // 2. Reduce band to symmetric tri-diagonal.
        hb2st(Aband, opts);

        // Copy diagonal and super-diagonal to vectors.
        std::vector<real_t> E(n - 1);
        internal::copyhb2st(Aband, W, E);

        // 3. Tri-diagonal eigenvalue solver.
        // QR iteration
        sterf<real_t>(W, E, opts);
    }

    // If matrix was scaled, then rescale eigenvalues appropriately.
    // todo

    // todo: bcast W.
}

//------------------------------------------------------------------------------
// Explicit instantiations.
template
void heev<float>(
     HermitianMatrix<float>& A,
     std::vector<float>& W,
     const std::map<Option, Value>& opts);

template
void heev<double>(
     HermitianMatrix<double>& A,
     std::vector<double>& W,
     const std::map<Option, Value>& opts);

template
void heev< std::complex<float> >(
     HermitianMatrix< std::complex<float> >& A,
     std::vector<float>& W,
     const std::map<Option, Value>& opts);

template
void heev< std::complex<double> >(
     HermitianMatrix< std::complex<double> >& A,
     std::vector<double>& W,
     const std::map<Option, Value>& opts);

} // namespace slate