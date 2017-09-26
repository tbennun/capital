/* Author: Edward Hutter */

// System includes
#include <iostream>
#include <cstdlib>
#include <utility>
#include <cmath>
#include <mpi.h>

// Local includes
#include "CFR3D.h"
#include "../CFvalidate/CFvalidate.h"

using namespace std;

int main(int argc, char** argv)
{
  using MatrixTypeA = Matrix<double,int,MatrixStructureSquare,MatrixDistributerCyclic>;
  using MatrixTypeL = Matrix<double,int,MatrixStructureSquare,MatrixDistributerCyclic>;

  // argv[1] - Matrix size x where x represents 2^x.
  // So in future, we might want t way to test non power of 2 dimension matrices

  int rank,size,provided;
  MPI_Init_thread(&argc, &argv, MPI_THREAD_SINGLE, &provided);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  // size -- total number of processors in the 3D grid

  int pGridDimensionSize = ceil(pow(size,1./3.));
  uint64_t globalMatrixSize = (1<<(atoi(argv[1])));
  uint64_t localMatrixSize = globalMatrixSize/pGridDimensionSize;
  
  cout << "global matrix size - " << globalMatrixSize << ", local Matrix size - " << localMatrixSize;
  cout << ", rank - " << rank << ", size - " << size << ", one dimension of the 3D grid's size - " << pGridDimensionSize << endl;

  MatrixTypeA matA(localMatrixSize,localMatrixSize,globalMatrixSize,globalMatrixSize);
  MatrixTypeL matL(localMatrixSize,localMatrixSize,globalMatrixSize,globalMatrixSize);
  MatrixTypeL matLI(localMatrixSize,localMatrixSize,globalMatrixSize,globalMatrixSize);

  int helper = pGridDimensionSize;
  helper *= helper;
  int pCoordX = rank%pGridDimensionSize;
  int pCoordY = (rank%helper)/pGridDimensionSize;
  int pCoordZ = rank/helper;

  matA.DistributeSymmetric(pCoordX, pCoordY, pGridDimensionSize, pGridDimensionSize, true);

  CFR3D<double,int,MatrixStructureSquare,MatrixStructureSquare,cblasEngine>::
    Factor(matA, matL, matLI, localMatrixSize, MPI_COMM_WORLD);

  MPI_Barrier(MPI_COMM_WORLD);		// for debugging

  std::pair<double,double> error = CFvalidate<double,int>::validateCF_Local(matL, matLI, localMatrixSize, globalMatrixSize, MPI_COMM_WORLD);

  //std::cout << "Rank " << rank << " has CF error " << error.first << " and TI error - " << error.second << std::endl;

  MPI_Finalize();

  return 0;
}
