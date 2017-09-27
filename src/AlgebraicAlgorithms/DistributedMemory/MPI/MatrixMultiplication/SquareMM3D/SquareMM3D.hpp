/* Author: Edward Hutter */


static std::tuple<MPI_Comm,
                  MPI_Comm,
                  MPI_Comm,
                  MPI_Comm,
		  int,
                  int,
                  int>
                      setUpCommunicators(MPI_Comm commWorld)
{
  int rank,size;
  MPI_Comm_rank(commWorld, &rank);
  MPI_Comm_size(commWorld, &size);

  int pGridDimensionSize = ceil(pow(size,1./3.));
  int helper = pGridDimensionSize;
  helper *= helper;
  int pGridCoordX = rank%pGridDimensionSize;
  int pGridCoordY = (rank%helper)/pGridDimensionSize;
  int pGridCoordZ = rank/helper;

  MPI_Comm rowComm, columnComm, sliceComm, depthComm;

  // First, split the 3D Cube processor grid communicator into groups based on what 2D slice they are located on.
  // Then, subdivide further into row groups and column groups
  MPI_Comm_split(commWorld, pGridCoordY*pGridDimensionSize+pGridCoordX, rank, &depthComm);
  MPI_Comm_split(commWorld, pGridCoordZ, rank, &sliceComm);
  MPI_Comm_split(sliceComm, pGridCoordY, pGridCoordX, &rowComm);
  MPI_Comm_split(sliceComm, pGridCoordX, pGridCoordY, &columnComm);

  return std::make_tuple(rowComm, columnComm, sliceComm, depthComm, pGridCoordX, pGridCoordY, pGridCoordZ);
}


// This algorithm with underlying gemm BLAS routine will allow any Matrix Structure.
//   Of course we will serialize into Square Structure if not in Square Structure already in order to be compatible
//   with BLAS-3 routines.

template<typename T, typename U,
  template<typename,typename, template<typename,typename,int> class> class StructureA,
  template<typename,typename, template<typename,typename,int> class> class StructureB,
  template<typename,typename, template<typename,typename,int> class> class StructureC,		// Defaulted to MatrixStructureSquare
  template<typename,typename> class blasEngine>							// Defaulted to cblasEngine
template<template<typename,typename,int> class Distribution>
void SquareMM3D<T,U,StructureA,StructureB,StructureC,blasEngine>::Multiply(
                                                              Matrix<T,U,StructureA,Distribution>& matrixA,
                                                              Matrix<T,U,StructureB,Distribution>& matrixB,
                                                              Matrix<T,U,StructureC,Distribution>& matrixC,
                                                              U dimensionX,
                                                              U dimensionY,
                                                              U dimensionZ,
                                                              MPI_Comm commWorld,
                                                              const blasEngineArgumentPackage_gemm<T>& srcPackage
                                                            )
{
  // Use tuples so we don't have to pass multiple things by reference.
  // Also this way, we can take advantage of the new pass-by-value move semantics that are efficient

  auto commInfo3D = setUpCommunicators(commWorld);

  // Simple asignments like these don't need pass-by-reference. Remember the new pass-by-value semantics are efficient anyways
  MPI_Comm rowComm = std::get<0>(commInfo3D);
  MPI_Comm columnComm = std::get<1>(commInfo3D);
  MPI_Comm sliceComm = std::get<2>(commInfo3D);
  MPI_Comm depthComm = std::get<3>(commInfo3D);
  int pGridCoordX = std::get<4>(commInfo3D);
  int pGridCoordY = std::get<5>(commInfo3D);
  int pGridCoordZ = std::get<6>(commInfo3D);

  std::vector<T>& dataA = matrixA.getVectorData(); 
  std::vector<T>& dataB = matrixB.getVectorData();
  U sizeA = matrixA.getNumElems();
  U sizeB = matrixB.getNumElems();
  std::vector<T> foreignA;
  std::vector<T> foreignB;
  bool isRootRow = ((pGridCoordX == pGridCoordZ) ? true : false);
  bool isRootColumn = ((pGridCoordY == pGridCoordZ) ? true : false);

  BroadcastPanels((isRootRow ? dataA : foreignA), sizeA, isRootRow, pGridCoordZ, rowComm);
  BroadcastPanels((isRootColumn ? dataB : foreignB), sizeB, isRootColumn, pGridCoordZ, columnComm);

  T* matrixAforEnginePtr = getEnginePtr(matrixA, (isRootRow ? dataA : foreignA), isRootRow);
  T* matrixBforEnginePtr = getEnginePtr(matrixB, (isRootColumn ? dataB : foreignB), isRootColumn);

  // Assume, for now, that matrixC has Square Structure. In the future, we can always do the same procedure as above, and add a Serialize after the AllReduce
  std::vector<T>& matrixCforEngine = matrixC.getVectorData();
  U numElems = matrixC.getNumElems();				// We assume that the user initialized matrixC correctly, even for TRMM

  blasEngine<T,U>::_gemm(matrixAforEnginePtr, matrixBforEnginePtr, &matrixCforEngine[0], dimensionX, dimensionY,
    dimensionX, dimensionZ, dimensionY, dimensionZ, dimensionY, dimensionX, dimensionY, srcPackage);

  MPI_Allreduce(MPI_IN_PLACE, &matrixCforEngine[0], numElems, MPI_DOUBLE, MPI_SUM, depthComm);

  // Unlike before when I had explicit new calls, the memory will get deleted automatically since the vectors will go out of scope
}

template<typename T, typename U,
  template<typename,typename, template<typename,typename,int> class> class StructureA,
  template<typename,typename, template<typename,typename,int> class> class StructureB,
  template<typename,typename, template<typename,typename,int> class> class StructureC,		// Defaulted to MatrixStructureSquare
  template<typename,typename> class blasEngine>							// Defaulted to cblasEngine
template<template<typename,typename,int> class Distribution>
void SquareMM3D<T,U,StructureA,StructureB,StructureC,blasEngine>::Multiply(
                                                              Matrix<T,U,StructureA,Distribution>& matrixA,
                                                              Matrix<T,U,StructureB,Distribution>& matrixB,
                                                              U dimensionX,
                                                              U dimensionY,
                                                              U dimensionZ,
                                                              MPI_Comm commWorld,
                                                              const blasEngineArgumentPackage_trmm<T>& srcPackage
                                                            )
{
  // Use tuples so we don't have to pass multiple things by reference.
  // Also this way, we can take advantage of the new pass-by-value move semantics that are efficient

  auto commInfo3D = setUpCommunicators(commWorld);

  // Simple asignments like these don't need pass-by-reference. Remember the new pass-by-value semantics are efficient anyways
  MPI_Comm rowComm = std::get<0>(commInfo3D);
  MPI_Comm columnComm = std::get<1>(commInfo3D);
  MPI_Comm sliceComm = std::get<2>(commInfo3D);
  MPI_Comm depthComm = std::get<3>(commInfo3D);
  int pGridCoordX = std::get<4>(commInfo3D);
  int pGridCoordY = std::get<5>(commInfo3D);
  int pGridCoordZ = std::get<6>(commInfo3D);

  std::vector<T>& dataA = matrixA.getVectorData(); 
  std::vector<T>& dataB = matrixB.getVectorData();
  U sizeA = matrixA.getNumElems();
  U sizeB = matrixB.getNumElems();
  std::vector<T> foreignA;
  std::vector<T> foreignB;
  bool isRootRow = ((pGridCoordX == pGridCoordZ) ? true : false);
  bool isRootColumn = ((pGridCoordY == pGridCoordZ) ? true : false);

  BroadcastPanels((isRootRow ? dataA : foreignA), sizeA, isRootRow, pGridCoordZ, rowComm);
  BroadcastPanels((isRootColumn ? dataB : foreignB), sizeB, isRootColumn, pGridCoordZ, columnComm);

  // Right now, foreignA and/or foreignB might be empty if this processor is the rowRoot or the columnRoot

  T* matrixAforEnginePtr = getEnginePtr(matrixA, (isRootRow ? dataA : foreignA), isRootRow);

  // We assume that matrixB is Square for now. No reason to believe otherwise
  std::vector<T>& matrixBforEngine = matrixB.getVectorData();
  U numElems = matrixB.getNumElems();				// We assume that the user initialized matrixC correctly, even for TRMM

  // Now at this point we have a choice. SquareMM3D template class accepts 3 template parameters for the Matrix Structures of matrixA, matrixB, and matrixC
  //   But, trmm only deals with 2 matrices matrixA and matrixB, and stores the result in matrixB.
  //   Should the user just deal with this? And how do we deal with a template parameter that doesn't need to exist?

  blasEngine<T,U>::_trmm(matrixAforEnginePtr, &matrixBforEngine[0], (srcPackage.side == blasEngineSide::AblasLeft ? dimensionX : dimensionY),
    (srcPackage.side == blasEngineSide::AblasLeft ? dimensionZ : dimensionX), (srcPackage.side == blasEngineSide::AblasLeft ? dimensionY : dimensionX),
    (srcPackage.side == blasEngineSide::AblasLeft ? dimensionX : dimensionY), srcPackage);

  MPI_Allreduce(MPI_IN_PLACE, &matrixBforEngine[0], sizeB, MPI_DOUBLE, MPI_SUM, depthComm);
}

template<typename T, typename U,
  template<typename,typename, template<typename,typename,int> class> class StructureA,
  template<typename,typename, template<typename,typename,int> class> class StructureB,
  template<typename,typename, template<typename,typename,int> class> class StructureC,		// Defaulted to MatrixStructureSquare
  template<typename,typename> class blasEngine>							// Defaulted to cblasEngine
template<template<typename,typename,int> class Distribution>
void SquareMM3D<T,U,StructureA,StructureB,StructureC,blasEngine>::Multiply(
                                                              Matrix<T,U,StructureA,Distribution>& matrixA,
                                                              Matrix<T,U,StructureB,Distribution>& matrixB,
                                                              U dimensionX,
                                                              U dimensionY,
                                                              U dimensionZ,
                                                              MPI_Comm commWorld,
                                                              const blasEngineArgumentPackage_syrk<T>& srcPackage
                                                            )
{
  // Use tuples so we don't have to pass multiple things by reference.
  // Also this way, we can take advantage of the new pass-by-value move semantics that are efficient

  auto commInfo3D = setUpCommunicators(commWorld);

  // Simple asignments like these don't need pass-by-reference. Remember the new pass-by-value semantics are efficient anyways
  MPI_Comm rowComm = std::get<0>(commInfo3D);
  MPI_Comm transComm = std::get<1>(commInfo3D);
  MPI_Comm sliceComm = std::get<2>(commInfo3D);
  MPI_Comm depthComm = std::get<3>(commInfo3D);
  int pGridCoordX = std::get<4>(commInfo3D);
  int pGridCoordY = std::get<5>(commInfo3D);
  int pGridCoordZ = std::get<6>(commInfo3D);

  std::vector<T>& dataA = matrixA.getVectorData(); 
  std::vector<T> dataAtrans = dataA;			// need to make a copy here I think
  U sizeA = matrixA.getNumElems();
  std::vector<T> foreignA;
  std::vector<T> foreignAtrans;
  bool isRootRow = ((pGridCoordX == pGridCoordZ) ? true : false);
  bool isRootTrans = ((pGridCoordY == pGridCoordZ) ? true : false);

  BroadcastPanels((isRootRow ? dataA : foreignA), sizeA, isRootRow, pGridCoordZ, rowComm);
  BroadcastPanels((isRootTrans ? dataAtrans : foreignAtrans), sizeA, isRootTrans, pGridCoordZ, transComm);

  // Right now, foreignA and/or foreignAtrans might be empty if this processor is the rowRoot or the transRoot
  T* matrixAforEnginePtr = getEnginePtr(matrixA, (isRootRow ? dataA : foreignA), isRootRow);

  // We assume that matrixB is Square for now. No reason to believe otherwise

  std::vector<T>& matrixBforEngine = matrixB.getVectorData();
  U numElems = matrixB.getNumElems();				// We assume that the user initialized matrixC correctly, even for TRMM

  blasEngine<T,U>::_syrk(matrixAforEnginePtr, &matrixBforEngine[0], dimensionX, dimensionY,
    dimensionX, dimensionY, srcPackage);

  MPI_Allreduce(MPI_IN_PLACE, &matrixBforEngine[0], numElems, MPI_DOUBLE, MPI_SUM, depthComm);
}

template<typename T, typename U,
  template<typename,typename, template<typename,typename,int> class> class StructureA,
  template<typename,typename, template<typename,typename,int> class> class StructureB,
  template<typename,typename, template<typename,typename,int> class> class StructureC,		// Defaulted to MatrixStructureSquare
  template<typename,typename> class blasEngine>							// Defaulted to cblasEngine
template<template<typename,typename,int> class Distribution>
void SquareMM3D<T,U,StructureA,StructureB,StructureC,blasEngine>::Multiply(
                                                              Matrix<T,U,StructureA,Distribution>& matrixA,
                                                              Matrix<T,U,StructureB,Distribution>& matrixB,
                                                              Matrix<T,U,StructureC,Distribution>& matrixC,
                                                              U matrixAcutXstart,
                                                              U matrixAcutXend,
                                                              U matrixAcutYstart,
                                                              U matrixAcutYend,
                                                              U matrixBcutZstart,
                                                              U matrixBcutZend,
                                                              U matrixBcutXstart,
                                                              U matrixBcutXend,
                                                              U matrixCcutZstart,
                                                              U matrixCcutZend,
                                                              U matrixCcutYstart,
                                                              U matrixCcutYend,
                                                              MPI_Comm commWorld,
                                                              const blasEngineArgumentPackage_gemm<T>& srcPackage,
                                                              bool cutA,
                                                              bool cutB,
                                                              bool cutC
                                                            )
{
  // We will set up 3 matrices and call the method above.

  U rangeA_x = matrixAcutXend-matrixAcutXstart;
  U rangeA_y = matrixAcutYend-matrixAcutYstart;
  U rangeB_z = matrixBcutZend-matrixBcutZstart;
  U rangeB_x = matrixBcutXend-matrixBcutXstart;
  U rangeC_z = matrixCcutZend - matrixCcutZstart;
  U rangeC_y = matrixCcutYend - matrixCcutYstart; 
  U globalDiffA = matrixA.getNumRowsGlobal() / matrixA.getNumRowsLocal();		// picked rows arbitrarily
  U globalDiffB = matrixB.getNumRowsGlobal() / matrixB.getNumRowsLocal();		// picked rows arbitrarily
  U globalDiffC = matrixC.getNumRowsGlobal() / matrixC.getNumRowsLocal();		// picked rows arbitrarily

  U sizeA = matrixA.getNumElems(rangeA_x, rangeA_y);
  U sizeB = matrixB.getNumElems(rangeB_z, rangeB_x);
  U sizeC = matrixC.getNumElems(rangeC_y, rangeC_z);

  // I cannot use a fast-pass-by-value via move constructor because I don't want to corrupt the true matrices A,B,C. Other reasons as well.
  Matrix<T,U,StructureA,Distribution> subMatrixA(std::vector<T>(), rangeA_x, rangeA_y, rangeA_x*globalDiffA, rangeA_y*globalDiffA);
  Matrix<T,U,StructureB,Distribution> subMatrixB(std::vector<T>(), rangeB_z, rangeB_x, rangeB_z*globalDiffB, rangeB_x*globalDiffB);
  Matrix<T,U,StructureC,Distribution> subMatrixC(std::vector<T>(), rangeC_z, rangeC_y, rangeC_z*globalDiffC, rangeC_y*globalDiffC);
  Matrix<T,U,StructureA,Distribution>& matA = getSubMatrix(matrixA, subMatrixA, matrixAcutXstart, matrixAcutXend, matrixAcutYstart, matrixAcutYend, globalDiffA, cutA);
  Matrix<T,U,StructureB,Distribution>& matB = getSubMatrix(matrixB, subMatrixB, matrixBcutZstart, matrixBcutZend, matrixBcutXstart, matrixBcutXend, globalDiffB, cutB);
  Matrix<T,U,StructureC,Distribution>& matC = getSubMatrix(matrixC, subMatrixC, matrixCcutZstart, matrixCcutZend, matrixCcutYstart, matrixCcutYend, globalDiffC, cutC);

  Multiply(matA, matB, matC, rangeA_y, rangeA_x, rangeB_x, commWorld, srcPackage);


  // for debugging
  int rank;
  MPI_Comm_rank(commWorld, &rank);
  if (rank == 0)
  {
    for (int i=0; i<sizeA; i++)
    {
      std::cout << matA.getRawData()[i] << " ";
    }
    std::cout << "\n\n";
    std::cout << "first 4 values - " << matC.getRawData()[0] << " " << matC.getRawData()[1] << " " << matC.getRawData()[2] << " " << matC.getRawData()[3] << std::endl;
    for (int i=0; i<sizeB; i++)
    {
      std::cout << matB.getRawData()[i] << " ";
    }
  }

  // reverse serialize, to put the solved piece of matrixC into where it should go.
  if (cutC)
  {
    Serializer<T,U,StructureC,StructureC>::Serialize(matrixC, matC,
      matrixCcutZstart, matrixCcutZend, matrixCcutYstart, matrixCcutYend, true);
  }
}


template<typename T, typename U,
  template<typename,typename, template<typename,typename,int> class> class StructureA,
  template<typename,typename, template<typename,typename,int> class> class StructureB,
  template<typename,typename, template<typename,typename,int> class> class StructureC,		// Defaulted to MatrixStructureSquare
  template<typename,typename> class blasEngine>							// Defaulted to cblasEngine
template<template<typename,typename,int> class Distribution>
void SquareMM3D<T,U,StructureA,StructureB,StructureC,blasEngine>::Multiply(
                                                              Matrix<T,U,StructureA,Distribution>& matrixA,
                                                              Matrix<T,U,StructureB,Distribution>& matrixB,
                                                              U matrixAcutXstart,
                                                              U matrixAcutXend,
                                                              U matrixAcutYstart,
                                                              U matrixAcutYend,
                                                              U matrixBcutZstart,
                                                              U matrixBcutZend,
                                                              U matrixBcutXstart,
                                                              U matrixBcutXend,
                                                              MPI_Comm commWorld,
                                                              const blasEngineArgumentPackage_trmm<T>& srcPackage,
                                                              bool cutA,
                                                              bool cutB
                                                            )
{
  // We will set up 3 matrices and call the method above.

  U rangeA_x = matrixAcutXend-matrixAcutXstart;
  U rangeA_y = matrixAcutYend-matrixAcutYstart;
  U rangeB_x = matrixBcutXend-matrixBcutXstart;
  U rangeB_z = matrixBcutZend-matrixBcutZstart;
  U globalDiffA = matrixA.getNumRowsGlobal() / matrixA.getNumRowsLocal();		// picked rows arbitrarily
  U globalDiffB = matrixB.getNumRowsGlobal() / matrixB.getNumRowsLocal();		// picked rows arbitrarily

  U sizeA = matrixA.getNumElems(rangeA_x, rangeA_y);
  U sizeB = matrixB.getNumElems(rangeB_z, rangeB_x);

  // I cannot use a fast-pass-by-value via move constructor because I don't want to corrupt the true matrices A,B,C. Other reasons as well.
  Matrix<T,U,StructureA,Distribution> subMatrixA(std::vector<T>(), rangeA_x, rangeA_y, rangeA_x*globalDiffA, rangeA_y*globalDiffA);
  Matrix<T,U,StructureB,Distribution> subMatrixB(std::vector<T>(), rangeB_z, rangeB_x, rangeB_z*globalDiffB, rangeB_x*globalDiffB);
  Matrix<T,U,StructureA,Distribution>& matA = getSubMatrix(matrixA, subMatrixA, matrixAcutXstart, matrixAcutXend, matrixAcutYstart, matrixAcutYend, globalDiffA, cutA);
  Matrix<T,U,StructureB,Distribution>& matB = getSubMatrix(matrixB, subMatrixB, matrixBcutZstart, matrixBcutZend, matrixBcutXstart, matrixBcutXend, globalDiffB, cutB);

  Multiply(matA, matB, rangeA_y, rangeA_x, rangeB_x, commWorld, srcPackage);

  // reverse serialize, to put the solved piece of matrixC into where it should go. Only if we need to
  if (cutB)
  {
    Serializer<T,U,StructureB,StructureB>::Serialize(matrixB, matB,
      matrixBcutZstart, matrixBcutZend, matrixBcutXstart, matrixBcutXend, true);
  }
}


template<typename T, typename U,
  template<typename,typename, template<typename,typename,int> class> class StructureA,
  template<typename,typename, template<typename,typename,int> class> class StructureB,
  template<typename,typename, template<typename,typename,int> class> class StructureC,		// Defaulted to MatrixStructureSquare
  template<typename,typename> class blasEngine>							// Defaulted to cblasEngine
template<template<typename,typename,int> class Distribution>
void SquareMM3D<T,U,StructureA,StructureB,StructureC,blasEngine>::Multiply(
                                                              Matrix<T,U,StructureA,Distribution>& matrixA,
                                                              Matrix<T,U,StructureB,Distribution>& matrixB,
                                                              U matrixAcutXstart,
                                                              U matrixAcutXend,
                                                              U matrixAcutYstart,
                                                              U matrixAcutYend,
                                                              U matrixBcutZstart,
                                                              U matrixBcutZend,
                                                              U matrixBcutXstart,
                                                              U matrixBcutXend,
                                                              MPI_Comm commWorld,
                                                              const blasEngineArgumentPackage_syrk<T>& srcPackage,
                                                              bool cutA,
                                                              bool cutB
                                                            )
{
  // We will set up 3 matrices and call the method above.

  U rangeA_x = matrixAcutXend-matrixAcutXstart;
  U rangeA_y = matrixAcutYend-matrixAcutYstart;
  U rangeB_z = matrixBcutZend-matrixBcutZstart;
  U rangeB_x = matrixBcutXend-matrixBcutXstart;
  U globalDiffA = matrixA.getNumRowsGlobal() / matrixA.getNumRowsLocal();		// picked rows arbitrarily
  U globalDiffB = matrixB.getNumRowsGlobal() / matrixB.getNumRowsLocal();		// picked rows arbitrarily

  U sizeA = matrixA.getNumElems(rangeA_x, rangeA_y);
  U sizeB = matrixB.getNumElems(rangeB_z, rangeB_x);

  // I cannot use a fast-pass-by-value via move constructor because I don't want to corrupt the true matrices A,B,C. Other reasons as well.
  Matrix<T,U,StructureA,Distribution> subMatrixA(std::vector<T>(), rangeA_x, rangeA_y, rangeA_x*globalDiffA, rangeA_y*globalDiffA);
  Matrix<T,U,StructureB,Distribution> subMatrixB(std::vector<T>(), rangeB_z, rangeB_x, rangeB_z*globalDiffB, rangeB_x*globalDiffB);
  Matrix<T,U,StructureA,Distribution>& matA = getSubMatrix(matrixA, subMatrixA, matrixAcutXstart, matrixAcutXend, matrixAcutYstart, matrixAcutYend, globalDiffA, cutA);
  Matrix<T,U,StructureB,Distribution>& matB = getSubMatrix(matrixB, subMatrixB, matrixBcutZstart, matrixBcutZend, matrixBcutXstart, matrixBcutXend, globalDiffB, cutB);

  Multiply(matA, matB, rangeA_y, rangeA_x, rangeB_x, commWorld, srcPackage);

  // reverse serialize, to put the solved piece of matrixC into where it should go. Only if we need to
  if (cutB)
  {
    Serializer<T,U,StructureB,StructureB>::Serialize(matrixB, matB,
      matrixBcutZstart, matrixBcutZend, matrixBcutXstart, matrixBcutXend, true);
  }
}


template<typename T, typename U,
  template<typename,typename, template<typename,typename,int> class> class StructureA,
  template<typename,typename, template<typename,typename,int> class> class StructureB,
  template<typename,typename, template<typename,typename,int> class> class StructureC,		// Defaulted to MatrixStructureSquare
  template<typename,typename> class blasEngine>							// Defaulted to cblasEngine
void SquareMM3D<T,U,StructureA, StructureB, StructureC, blasEngine>::BroadcastPanels(
											std::vector<T>& data,
											U size,
											bool isRoot,
											int pGridCoordZ,
											MPI_Comm panelComm
										    )
{
  if (isRoot)
  {
    MPI_Bcast(&data[0], size, MPI_DOUBLE, pGridCoordZ, panelComm);
  }
  else
  {
    data.resize(size);
    MPI_Bcast(&data[0], size, MPI_DOUBLE, pGridCoordZ, panelComm);
  }
}


template<typename T, typename U,
  template<typename,typename, template<typename,typename,int> class> class StructureA,
  template<typename,typename, template<typename,typename,int> class> class StructureB,
  template<typename,typename, template<typename,typename,int> class> class StructureC,		// Defaulted to MatrixStructureSquare
  template<typename,typename> class blasEngine>							// Defaulted to cblasEngine
template<template<typename,typename, template<typename,typename,int> class> class StructureArg,
  template<typename,typename,int> class Distribution>					// Added additional template parameters just for this method
T* SquareMM3D<T,U,StructureA, StructureB, StructureC, blasEngine>::getEnginePtr(
											Matrix<T,U,StructureArg, Distribution>& matrixArg,
											std::vector<T>& data,
											bool isRoot
									       )
{
  if (!std::is_same<StructureArg<T,U,Distribution>,MatrixStructureSquare<T,U,Distribution>>::value)		// compile time if statement. Branch prediction should be correct.
  {
    Matrix<T,U,MatrixStructureSquare,Distribution> matrixForEngine(std::vector<T>(), matrixArg.getNumColumnsLocal(), matrixArg.getNumRowsLocal(),
      matrixArg.getNumColumnsGlobal(), matrixArg.getNumRowsGlobal());
    if (!isRoot)
    {
      Matrix<T,U,StructureArg,Distribution> matrixToSerialize(std::vector<T>(matrixArg.getNumColumnsLocal()*matrixArg.getNumRowsLocal())
        , matrixArg.getNumColumnsLocal(), matrixArg.getNumRowsLocal(), matrixArg.getNumColumnsGlobal(), matrixArg.getNumRowsGlobal(), true);
      Serializer<T,U,StructureArg,MatrixStructureSquare>::Serialize(matrixToSerialize, matrixForEngine);
    }
    else
    {
      Serializer<T,U,StructureArg,MatrixStructureSquare>::Serialize(matrixArg, matrixForEngine);
    }
    return matrixForEngine.getRawData();
  }
  else
  {
    return &data[0];
  }
}


template<typename T, typename U,
  template<typename,typename, template<typename,typename,int> class> class StructureA,
  template<typename,typename, template<typename,typename,int> class> class StructureB,
  template<typename,typename, template<typename,typename,int> class> class StructureC,		// Defaulted to MatrixStructureSquare
  template<typename,typename> class blasEngine>							// Defaulted to cblasEngine
template<template<typename,typename, template<typename,typename,int> class> class StructureArg,
  template<typename,typename,int> class Distribution>					// Added additional template parameters just for this method
Matrix<T,U,StructureArg,Distribution>& SquareMM3D<T,U,StructureA, StructureB, StructureC, blasEngine>::getSubMatrix(
											Matrix<T,U,StructureArg, Distribution>& srcMatrix,	// pass by value via move constructor
											Matrix<T,U,StructureArg, Distribution>& fillMatrix,	// pass by value via move constructor
											U matrixArgColumnStart,
											U matrixArgColumnEnd,
											U matrixArgRowStart,
											U matrixArgRowEnd,
											U globalDiff,
											bool getSub
									       )
{
  if (getSub)
  {
    U rangeC_column = matrixArgColumnEnd - matrixArgColumnStart;
    U rangeC_row = matrixArgRowEnd - matrixArgRowStart;
    Serializer<T,U,StructureArg,StructureArg>::Serialize(srcMatrix, fillMatrix,
      matrixArgColumnStart, matrixArgColumnEnd, matrixArgRowStart, matrixArgRowEnd);
    return fillMatrix;			// I am returning a lvalue reference to a lvalue reference
  }
  else
  {
    return srcMatrix;
  }
}
