/* Author: Edward Hutter */

// System includes
#include <iostream>
#include <cstdlib>
#include <utility>
#include <cmath>
#include <string>
#include <mpi.h>

// Local includes
#include "CFR3D.h"
#include "../CFvalidate/CFvalidate.h"
#include "../../../Timer/Timer.h"

using namespace std;

int main(int argc, char** argv)
{
  using MatrixTypeA = Matrix<double,int,MatrixStructureSquare,MatrixDistributerCyclic>;
  using MatrixTypeL = Matrix<double,int,MatrixStructureSquare,MatrixDistributerCyclic>;
  using MatrixTypeR = Matrix<double,int,MatrixStructureSquare,MatrixDistributerCyclic>;

#ifdef PROFILE
  TAU_PROFILE_SET_CONTEXT(0)
#endif /*PROFILE*/

  // argv[1] - Matrix size x where x represents 2^x.
  // So in future, we might want t way to test non power of 2 dimension matrices

  int rank,size,provided;
  MPI_Init_thread(&argc, &argv, MPI_THREAD_SINGLE, &provided);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  // size -- total number of processors in the 3D grid
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  int pGridDimensionSize = std::nearbyint(std::pow(size,1./3.));
  int helper = pGridDimensionSize;
  helper *= helper;
  int pCoordX = rank%pGridDimensionSize;
  int pCoordY = (rank%helper)/pGridDimensionSize;
  int pCoordZ = rank/helper;

  /*
    methodKey1 -> 0) Lower
		              1) Upper
  */
  int methodKey1 = atoi(argv[1]);
  /*
    methodKey2 -> 0) Sequential Validation
		              1) Performance
                  2) Distributed validation
  */
  int methodKey2 = atoi(argv[2]);
  /*
    methodKey3 -> 0) Non power of 2 dimenson
		              1) Power of 2 dimension
  */
  int methodKey3 = atoi(argv[3]);
  /*
    methodKey4: -> 0) Broadcast + Allreduce
			             1) Allgather + Allreduce
  */
  int methodKey4 = atoi(argv[4]);

  uint64_t globalMatrixSize = (methodKey3 ? (1<<(atoi(argv[5]))) : atoi(argv[5]));
  int blockSizeMultiplier = atoi(argv[6]);
  int inverseCutOffMultiplier = atoi(argv[7]); // multiplies baseCase dimension by sucessive 2

  pTimer myTimer;
  int numIterations = 1;

  if (methodKey1 == 0)
  {
    MatrixTypeA matA(globalMatrixSize,globalMatrixSize, pGridDimensionSize, pGridDimensionSize);
    MatrixTypeR matLI(globalMatrixSize,globalMatrixSize, pGridDimensionSize, pGridDimensionSize);

    matA.DistributeSymmetric(pCoordX, pCoordY, pGridDimensionSize, pGridDimensionSize, pCoordX*pGridDimensionSize+pCoordY, true);
    // Save matrixA for correctness checking
    MatrixTypeA saveA = matA;

    // Perform a "cold run" first before keeping tracking of times
    std::tuple<MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,int,int,int> commInfo3D = setUpCommunicators(
      MPI_COMM_WORLD);
    CFR3D<double,int,cblasEngine>::Factor(
      matA, matLI, inverseCutOffMultiplier, 'L', blockSizeMultiplier, MPI_COMM_WORLD, commInfo3D, methodKey4);
    myTimer.clear();
    MPI_Comm_free(&std::get<0>(commInfo3D));
    MPI_Comm_free(&std::get<1>(commInfo3D));
    MPI_Comm_free(&std::get<2>(commInfo3D));
    MPI_Comm_free(&std::get<3>(commInfo3D));

    if (methodKey2 == 1) {numIterations = atoi(argv[8]);}
    for (int i=0; i<numIterations; i++)
    {
      // Reset matrixA
      matA.DistributeSymmetric(pCoordX, pCoordY, pGridDimensionSize, pGridDimensionSize, pCoordX*pGridDimensionSize+pCoordY, true);
#ifdef CRITTER
      Critter_Clear();
#endif
      std::tuple<MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,int,int,int> commInfo3D = setUpCommunicators(
        MPI_COMM_WORLD);
      CFR3D<double,int,cblasEngine>::Factor(
        matA, matLI, inverseCutOffMultiplier, 'L', blockSizeMultiplier, MPI_COMM_WORLD, commInfo3D, methodKey4);
#ifdef CRITTER
      Critter_Print();
#endif
      MPI_Comm_free(&std::get<0>(commInfo3D));
      MPI_Comm_free(&std::get<1>(commInfo3D));
      MPI_Comm_free(&std::get<2>(commInfo3D));
      MPI_Comm_free(&std::get<3>(commInfo3D));
    }
    if (methodKey2 == 0)
    {
      CFvalidate<double,int>::validateLocal(
        saveA, matA, 'L', MPI_COMM_WORLD);
    }
    else if (methodKey2 == 2)
    {
      std::tuple<MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,int,int,int> commInfo3D = setUpCommunicators(
        MPI_COMM_WORLD);
      CFvalidate<double,int>::validateParallel(
        saveA, matA, 'L', MPI_COMM_WORLD, commInfo3D);
      MPI_Comm_free(&std::get<0>(commInfo3D));
      MPI_Comm_free(&std::get<1>(commInfo3D));
      MPI_Comm_free(&std::get<2>(commInfo3D));
      MPI_Comm_free(&std::get<3>(commInfo3D));
    }
    else
    {
    }
  }
  else
  {
    MatrixTypeA matA(globalMatrixSize,globalMatrixSize, pGridDimensionSize, pGridDimensionSize);
    MatrixTypeR matRI(globalMatrixSize,globalMatrixSize, pGridDimensionSize, pGridDimensionSize);

    matA.DistributeSymmetric(pCoordX, pCoordY, pGridDimensionSize, pGridDimensionSize, pCoordX*pGridDimensionSize+pCoordY, true);
    // Save matrixA for correctness checking
    MatrixTypeA saveA = matA;

    // Perform a "cold run" first before keeping tracking of times
    std::tuple<MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,int,int,int> commInfo3D = setUpCommunicators(
      MPI_COMM_WORLD);
    CFR3D<double,int,cblasEngine>::Factor(
      matA, matRI, inverseCutOffMultiplier, 'U', blockSizeMultiplier, MPI_COMM_WORLD, commInfo3D, methodKey4);
    myTimer.clear();
    MPI_Comm_free(&std::get<0>(commInfo3D));
    MPI_Comm_free(&std::get<1>(commInfo3D));
    MPI_Comm_free(&std::get<2>(commInfo3D));
    MPI_Comm_free(&std::get<3>(commInfo3D));

    if (methodKey2 == 1) {numIterations = atoi(argv[8]);}
    for (int i=0; i<numIterations; i++)
    {
      // Reset matrixA
      matA.DistributeSymmetric(pCoordX, pCoordY, pGridDimensionSize, pGridDimensionSize, pCoordX*pGridDimensionSize+pCoordY, true);
#ifdef CRITTER
      Critter_Clear();
#endif
      std::tuple<MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,int,int,int> commInfo3D = setUpCommunicators(
        MPI_COMM_WORLD);
      CFR3D<double,int,cblasEngine>::Factor(
        matA, matRI, inverseCutOffMultiplier, 'U', blockSizeMultiplier, MPI_COMM_WORLD, commInfo3D, methodKey4);
#ifdef CRITTER
      Critter_Print();
#endif
      MPI_Comm_free(&std::get<0>(commInfo3D));
      MPI_Comm_free(&std::get<1>(commInfo3D));
      MPI_Comm_free(&std::get<2>(commInfo3D));
      MPI_Comm_free(&std::get<3>(commInfo3D));
    }
    if (methodKey2 == 0)
    {
      CFvalidate<double,int>::validateLocal(
        saveA, matA, 'U', MPI_COMM_WORLD);
    }
    else if (methodKey2 == 2)
    {
      std::tuple<MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,int,int,int> commInfo3D = setUpCommunicators(
        MPI_COMM_WORLD);
      CFvalidate<double,int>::validateParallel(
        saveA, matA, 'U', MPI_COMM_WORLD, commInfo3D);
      MPI_Comm_free(&std::get<0>(commInfo3D));
      MPI_Comm_free(&std::get<1>(commInfo3D));
      MPI_Comm_free(&std::get<2>(commInfo3D));
      MPI_Comm_free(&std::get<3>(commInfo3D));
    }
    else
    {
    }
  }  

  MPI_Finalize();
  return 0;
}
