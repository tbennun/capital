/* Author: Edward Hutter */

#include "../../alg/qr/cacqr/cacqr.h"
#include "../../test/qr/validate.h"

using namespace std;

int main(int argc, char** argv){
  using T = double; using U = int64_t;
  using MatrixTypeS = matrix<T,U,rect>;
  using MatrixTypeR = matrix<T,U,rect>;

  int rank,size,provided;
  MPI_Init_thread(&argc, &argv, MPI_THREAD_SINGLE, &provided);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  size_t num_iter = atoi(argv[1]);
  U globalMatrixDimensionM = atoi(argv[2]);
  U globalMatrixDimensionN = atoi(argv[3]);
  U dimensionC = atoi(argv[4]);
  bool complete_inv = atoi(argv[5]);
  U split = atoi(argv[6]);
  U bcMultiplier = atoi(argv[7]);
  size_t num_chunks        = atoi(argv[8]);
  size_t numIterations=atoi(argv[9]);
  size_t id = atoi(argv[10]);	// 0 for critter-only, 1 for critter+production, 2 for critter+production+numerical

  using qr_type = typename qr::cacqr<>;
  {
    double iterTimeGlobal = 0; double iterTimeLocal = 0;
    T residualErrorGlobal,orthogonalityErrorGlobal;
    auto mpi_dtype = mpi_type<T>::type;
    auto RectTopo = topo::rect(MPI_COMM_WORLD,dimensionC, num_chunks);
    MatrixTypeR A(globalMatrixDimensionN,globalMatrixDimensionM, RectTopo.c, RectTopo.d);
    MatrixTypeS R(globalMatrixDimensionN,globalMatrixDimensionN, RectTopo.c, RectTopo.c);
    MatrixTypeR saveA = A;

    for (size_t i=0; i<numIterations; i++){
      // Generate algorithmic structure via instantiating packs
      cholesky::cholinv<>::info<T,U> ci_pack(complete_inv,split,bcMultiplier,'U');
      qr_type::info<T,U,decltype(ci_pack)::alg_type> pack(num_iter,ci_pack);
      // reset the matrix before timer starts
      A.distribute_random(RectTopo.x, RectTopo.y, RectTopo.c, RectTopo.d, rank/RectTopo.c);
      MPI_Barrier(MPI_COMM_WORLD);	// make sure each process starts together
      critter::start();
      qr_type::factor(A, R, pack, RectTopo);
      critter::stop();
      if (id>0){
        A.distribute_random(RectTopo.x, RectTopo.y, RectTopo.c, RectTopo.d, rank/RectTopo.c);
        volatile double startTime=MPI_Wtime();
        qr_type::factor(A, R, pack, RectTopo);
        iterTimeLocal = MPI_Wtime() - startTime;
        MPI_Reduce(&iterTimeLocal, &iterTimeGlobal, 1, mpi_dtype, MPI_MAX, 0, MPI_COMM_WORLD);
        if (id>1){
          saveA.distribute_random(RectTopo.x, RectTopo.y, RectTopo.c, RectTopo.d, rank/RectTopo.c);
          auto error = qr::validate<qr_type>::invoke(saveA, A, R, RectTopo);
          MPI_Reduce(&error.first, &residualErrorGlobal, 1, mpi_dtype, MPI_MAX, 0, MPI_COMM_WORLD);
          MPI_Reduce(&error.second, &orthogonalityErrorGlobal, 1, mpi_dtype, MPI_MAX, 0, MPI_COMM_WORLD);
        }
      }
      if (rank==0){
        std::cout << iterTimeGlobal << " " << residualErrorGlobal << " " << orthogonalityErrorGlobal << std::endl;
      }
    }
  }

  MPI_Finalize();
  return 0;
}
