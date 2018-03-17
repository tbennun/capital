/* Author: Edward Hutter */

static std::tuple<
			MPI_Comm,
			MPI_Comm,
			MPI_Comm,
			MPI_Comm,
			MPI_Comm,
			MPI_Comm
		 >
		getTunableCommunicators(
      MPI_Comm commWorld, int pGridDimensionD, int pGridDimensionC)
{
  TAU_FSTART(getTunableCommunicators);
  int worldRank, worldSize, sliceRank, columnRank;
  MPI_Comm_rank(commWorld, &worldRank);
  MPI_Comm_size(commWorld, &worldSize);

  int sliceSize = pGridDimensionD*pGridDimensionC;
  MPI_Comm sliceComm, rowComm, columnComm, columnContigComm, columnAltComm, depthComm, miniCubeComm;
  MPI_Comm_split(commWorld, worldRank/sliceSize, worldRank, &sliceComm);
  MPI_Comm_rank(sliceComm, &sliceRank);
  MPI_Comm_split(sliceComm, sliceRank/pGridDimensionC, sliceRank, &rowComm);
  int cubeInt = worldRank%sliceSize;
  MPI_Comm_split(commWorld, cubeInt/(pGridDimensionC*pGridDimensionC), worldRank, &miniCubeComm);
  MPI_Comm_split(commWorld, cubeInt, worldRank, &depthComm);
  MPI_Comm_split(sliceComm, sliceRank%pGridDimensionC, sliceRank, &columnComm);
  MPI_Comm_rank(columnComm, &columnRank);
  MPI_Comm_split(columnComm, columnRank/pGridDimensionC, columnRank, &columnContigComm);
  MPI_Comm_split(columnComm, columnRank%pGridDimensionC, columnRank, &columnAltComm); 
  
  MPI_Comm_free(&columnComm);
  TAU_FSTOP(getTunableCommunicators);
  return std::make_tuple(rowComm, columnContigComm, columnAltComm, depthComm, sliceComm, miniCubeComm);
}


template<typename T,typename U,template<typename,typename> class blasEngine>
template<template<typename,typename,template<typename,typename,int> class> class StructureA, template<typename,typename,int> class Distribution>
void CholeskyQR2<T,U,blasEngine>::Factor1D(
    Matrix<T,U,StructureA,Distribution>& matrixA, Matrix<T,U,MatrixStructureSquare,Distribution>& matrixR, MPI_Comm commWorld)
{
  TAU_FSTART(Factor1D);
  // We assume data is owned relative to a 1D processor grid, so every processor owns a chunk of data consisting of
  //   all columns and a block of rows.

  U globalDimensionM = matrixA.getNumRowsGlobal();
  U globalDimensionN = matrixA.getNumColumnsGlobal();
  U localDimensionM = matrixA.getNumRowsLocal();

  Matrix<T,U,MatrixStructureSquare,Distribution> matrixR2(std::vector<T>(globalDimensionN*globalDimensionN), globalDimensionN, globalDimensionN, globalDimensionN,
    globalDimensionN, true);

  Factor1D_cqr(
    matrixA, matrixR, commWorld);
  Factor1D_cqr(
    matrixA, matrixR2, commWorld);

  // Try gemm first, then try trmm later.
  blasEngineArgumentPackage_trmm<T> trmmPack1;
  trmmPack1.order = blasEngineOrder::AblasColumnMajor;
  trmmPack1.side = blasEngineSide::AblasLeft;
  trmmPack1.uplo = blasEngineUpLo::AblasUpper;
  trmmPack1.diag = blasEngineDiag::AblasNonUnit;
  trmmPack1.transposeA = blasEngineTranspose::AblasNoTrans;
  trmmPack1.alpha = 1.;
  blasEngine<T,U>::_trmm(matrixR2.getRawData(), matrixR.getRawData(), globalDimensionN, globalDimensionN,
    globalDimensionN, globalDimensionN, trmmPack1);
  TAU_FSTOP(Factor1D);
}

template<typename T,typename U,template<typename,typename> class blasEngine>
template<template<typename,typename,template<typename,typename,int> class> class StructureA, template<typename,typename,int> class Distribution>
void CholeskyQR2<T,U,blasEngine>::Factor3D(
    Matrix<T,U,StructureA,Distribution>& matrixA, Matrix<T,U,MatrixStructureSquare,Distribution>& matrixR,MPI_Comm commWorld, std::tuple<MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,int,int,int>& commInfo3D, 
      int inverseCutOffMultiplier, int baseCaseMultiplier)
{
  TAU_FSTART(Factor3D);
  // We assume data is owned relative to a 3D processor grid

  U globalDimensionM = matrixA.getNumRowsGlobal();
  U globalDimensionN = matrixA.getNumColumnsGlobal();
  U localDimensionN = matrixA.getNumColumnsLocal();		// no error check here, but hopefully 
  U localDimensionM = matrixA.getNumRowsLocal();		// no error check here, but hopefully 

  Matrix<T,U,MatrixStructureSquare,Distribution> matrixR2(std::vector<T>(localDimensionN*localDimensionN,0), localDimensionN, localDimensionN, globalDimensionN,
    globalDimensionN, true);
  Factor3D_cqr(
    matrixA, matrixR, commWorld, commInfo3D, inverseCutOffMultiplier, baseCaseMultiplier);
  Factor3D_cqr(
    matrixA, matrixR2, commWorld, commInfo3D, inverseCutOffMultiplier, baseCaseMultiplier);

  // Try gemm first, then try trmm later.
  blasEngineArgumentPackage_trmm<T> trmmPack1;
  trmmPack1.order = blasEngineOrder::AblasColumnMajor;
  trmmPack1.side = blasEngineSide::AblasLeft;
  trmmPack1.uplo = blasEngineUpLo::AblasUpper;
  trmmPack1.diag = blasEngineDiag::AblasNonUnit;
  trmmPack1.transposeA = blasEngineTranspose::AblasNoTrans;
  trmmPack1.alpha = 1.;

  // Later optimization - Serialize all 3 matrices into UpperTriangular first, then call this with those matrices, so we don't have to
  //   send half of the data!
  MM3D<T,U,blasEngine>::Multiply(
    matrixR2, matrixR, commWorld, commInfo3D, trmmPack1);
  TAU_FSTOP(Factor3D);
}

template<typename T,typename U,template<typename,typename> class blasEngine>
template<template<typename,typename,template<typename,typename,int> class> class StructureA, template<typename,typename,int> class Distribution>
void CholeskyQR2<T,U,blasEngine>::FactorTunable(
    Matrix<T,U,StructureA,Distribution>& matrixA, Matrix<T,U,MatrixStructureSquare,Distribution>& matrixR, int gridDimensionD, int gridDimensionC, MPI_Comm commWorld,
      std::tuple<MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm>& commInfoTunable, int inverseCutOffMultiplier,
      int baseCaseMultiplier)
{
  TAU_FSTART(FactorTunable);
  MPI_Comm miniCubeComm = std::get<5>(commInfoTunable);
  std::tuple<MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,int,int,int> commInfo3D = setUpCommunicators(
    miniCubeComm);

  U globalDimensionM = matrixA.getNumRowsGlobal();
  U globalDimensionN = matrixA.getNumColumnsGlobal();
  U localDimensionM = matrixA.getNumRowsLocal();//globalDimensionM/gridDimensionD;
  U localDimensionN = matrixA.getNumColumnsLocal();//globalDimensionN/gridDimensionC;

  // Need to get the right global dimensions here, use a tunable package struct or something
  Matrix<T,U,MatrixStructureSquare,Distribution> matrixR2(std::vector<T>(localDimensionN*localDimensionN,0), localDimensionN, localDimensionN, globalDimensionN,
    globalDimensionN, true);
  FactorTunable_cqr(
    matrixA, matrixR, gridDimensionD, gridDimensionC, commWorld, commInfoTunable, commInfo3D, inverseCutOffMultiplier, baseCaseMultiplier);
  FactorTunable_cqr(
    matrixA, matrixR2, gridDimensionD, gridDimensionC, commWorld, commInfoTunable, commInfo3D, inverseCutOffMultiplier, baseCaseMultiplier);

  // Try gemm first, then try trmm later.
  blasEngineArgumentPackage_trmm<T> trmmPack1;
  trmmPack1.order = blasEngineOrder::AblasColumnMajor;
  trmmPack1.side = blasEngineSide::AblasLeft;
  trmmPack1.uplo = blasEngineUpLo::AblasUpper;
  trmmPack1.diag = blasEngineDiag::AblasNonUnit;
  trmmPack1.transposeA = blasEngineTranspose::AblasNoTrans;
  trmmPack1.alpha = 1.;

  // Later optimization - Serialize all 3 matrices into UpperTriangular first, then call this with those matrices, so we don't have to
  //   send half of the data!
  MM3D<T,U,blasEngine>::Multiply(
    matrixR2, matrixR, miniCubeComm, commInfo3D, trmmPack1);

  TAU_FSTOP(FactorTunable);
}


template<typename T,typename U,template<typename,typename> class blasEngine>
template<template<typename,typename,template<typename,typename,int> class> class StructureA, template<typename,typename,int> class Distribution>
void CholeskyQR2<T,U,blasEngine>::Factor1D_cqr(
    Matrix<T,U,StructureA,Distribution>& matrixA, Matrix<T,U,MatrixStructureSquare,Distribution>& matrixR, MPI_Comm commWorld)
{
  TAU_FSTART(Factor1D_cqr);
  // Changed from syrk to gemm
  U localDimensionM = matrixA.getNumRowsLocal();
  U localDimensionN = matrixA.getNumColumnsGlobal();
  std::vector<T> localMMvec(localDimensionN*localDimensionN);
  blasEngineArgumentPackage_syrk<T> syrkPack;
  syrkPack.order = blasEngineOrder::AblasColumnMajor;
  syrkPack.uplo = blasEngineUpLo::AblasUpper;
  syrkPack.transposeA = blasEngineTranspose::AblasTrans;
  syrkPack.alpha = 1.;
  syrkPack.beta = 0.;

  blasEngine<T,U>::_syrk(matrixA.getRawData(), matrixR.getRawData(), localDimensionN, localDimensionM,
    localDimensionM, localDimensionN, syrkPack);

  // MPI_Allreduce to replicate the dimensionY x dimensionY matrix on each processor
  // Optimization potential: only Allreduce half of this matrix because its symmetric
  //   but only try this later to see if it actually helps, because to do this, I will have to serialize and re-serialize. Would only make sense if dimensionX is huge.
  MPI_Allreduce(MPI_IN_PLACE, matrixR.getRawData(), localDimensionN*localDimensionN, MPI_DOUBLE, MPI_SUM, commWorld);

  // Future optimization: remove this loop somehow, its just nasty and does not really need to be there
  // For correctness, because we are storing R and RI as square matrices AND using GEMM later on for Q=A*RI, lets manually set the lower-triangular portion of R to zeros
  //   Note that we could also do this before the AllReduce, but it wouldnt affect the cost
  for (U i=0; i<localDimensionN; i++)
  {
    for (U j=i+1; j<localDimensionN; j++)
    {
      matrixR.getRawData()[i*localDimensionN+j] = 0;
    }
  }

  // Now, localMMvec is replicated on every processor in commWorld
  LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', localDimensionN, matrixR.getRawData(), localDimensionN);

  // Need a true copy to avoid corrupting R-inverse.
  std::vector<T> RI = matrixR.getVectorData();

  // Next: sequential triangular inverse. Question: does DTRTRI require packed storage or square storage? I think square, so that it can use BLAS-3.
  LAPACKE_dtrtri(LAPACK_COL_MAJOR, 'U', 'N', localDimensionN, &RI[0], localDimensionN);

  // Finish by performing local matrix multiplication Q = A*R^{-1}
  blasEngineArgumentPackage_trmm<T> trmmPack1;
  trmmPack1.order = blasEngineOrder::AblasColumnMajor;
  trmmPack1.side = blasEngineSide::AblasRight;
  trmmPack1.uplo = blasEngineUpLo::AblasUpper;
  trmmPack1.diag = blasEngineDiag::AblasNonUnit;
  trmmPack1.transposeA = blasEngineTranspose::AblasNoTrans;
  trmmPack1.alpha = 1.;
  blasEngine<T,U>::_trmm(&RI[0], matrixA.getRawData(), localDimensionM, localDimensionN,
    localDimensionN, localDimensionM, trmmPack1);
  TAU_FSTOP(Factor1D_cqr);
}

template<typename T,typename U,template<typename,typename> class blasEngine>
template<template<typename,typename,template<typename,typename,int> class> class StructureA, template<typename,typename,int> class Distribution>
void CholeskyQR2<T,U,blasEngine>::Factor3D_cqr(
    Matrix<T,U,StructureA,Distribution>& matrixA, Matrix<T,U,MatrixStructureSquare,Distribution>& matrixR, MPI_Comm commWorld, std::tuple<MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,int,int,int>& commInfo3D,
      int inverseCutOffMultiplier, int baseCaseMultiplier)
{
  TAU_FSTART(Factor3D_cqr);

  MPI_Comm rowComm = std::get<0>(commInfo3D);
  MPI_Comm columnComm = std::get<1>(commInfo3D);
  MPI_Comm sliceComm = std::get<2>(commInfo3D);
  MPI_Comm depthComm = std::get<3>(commInfo3D);
  int pGridCoordX = std::get<4>(commInfo3D);
  int pGridCoordY = std::get<5>(commInfo3D);
  int pGridCoordZ = std::get<6>(commInfo3D);

  // Need to perform the multiple steps to get our partition of matrixA
  U localDimensionN = matrixA.getNumColumnsLocal();		// no error check here, but hopefully 
  U localDimensionM = matrixA.getNumRowsLocal();		// no error check here, but hopefully 
  U globalDimensionN = matrixA.getNumColumnsGlobal();		// no error check here, but hopefully 
  U globalDimensionM = matrixA.getNumRowsGlobal();		// no error check here, but hopefully 
  std::vector<T>& dataA = matrixA.getVectorData();
  U sizeA = matrixA.getNumElems();
  std::vector<T> foreignA;	// dont fill with data first, because if root its a waste,
                                //   but need it to outside to get outside scope
  bool isRootRow = ((pGridCoordX == pGridCoordZ) ? true : false);
  bool isRootColumn = ((pGridCoordY == pGridCoordZ) ? true : false);

  // No optimization here I am pretty sure due to final result being symmetric, as it is cyclic and transpose isnt true as I have painfully found out before.
  BroadcastPanels(
    (isRootRow ? dataA : foreignA), sizeA, isRootRow, pGridCoordZ, rowComm);

  blasEngineArgumentPackage_gemm<T> gemmPack1;
  gemmPack1.order = blasEngineOrder::AblasColumnMajor;
  gemmPack1.transposeA = blasEngineTranspose::AblasTrans;
  gemmPack1.transposeB = blasEngineTranspose::AblasNoTrans;
  gemmPack1.alpha = 1.;
  gemmPack1.beta = 0.;

  // I don't think I can run syrk here, so I will use gemm. Maybe in the future after I have it correct I can experiment.
  blasEngine<T,U>::_gemm((isRootRow ? &dataA[0] : &foreignA[0]), &dataA[0], matrixR.getRawData(), localDimensionN, localDimensionN,
    localDimensionM, localDimensionM, localDimensionM, localDimensionN, gemmPack1);

  MPI_Reduce((isRootColumn ? MPI_IN_PLACE : matrixR.getRawData()), matrixR.getRawData(), localDimensionN*localDimensionN, MPI_DOUBLE,
    MPI_SUM, pGridCoordZ, columnComm);

  MPI_Bcast(matrixR.getRawData(), localDimensionN*localDimensionN, MPI_DOUBLE, pGridCoordY, depthComm);

  // Create an extra matrix for R-inverse
  Matrix<T,U,MatrixStructureSquare,Distribution> matrixRI(std::vector<T>(localDimensionN*localDimensionN,0), localDimensionN, localDimensionN,
    matrixA.getNumColumnsGlobal(), matrixA.getNumColumnsGlobal(), true);

  std::pair<bool,std::vector<U>> baseCaseDimList = CFR3D<T,U,blasEngine>::Factor(
    matrixR, matrixRI, inverseCutOffMultiplier, 'U', baseCaseMultiplier, commWorld, commInfo3D);


// For now, comment this out, because I am experimenting with using TriangularSolve TRSM instead of MM3D
//   But later on once it works, use an integer or something to have both available, important when benchmarking
  // Need to be careful here. matrixRI must be truly upper-triangular for this to be correct as I found out in 1D case.

  if (baseCaseDimList.first)
  {
    blasEngineArgumentPackage_trmm<T> trmmPack1;
    trmmPack1.order = blasEngineOrder::AblasColumnMajor;
    trmmPack1.side = blasEngineSide::AblasRight;
    trmmPack1.uplo = blasEngineUpLo::AblasUpper;
    trmmPack1.diag = blasEngineDiag::AblasNonUnit;
    trmmPack1.transposeA = blasEngineTranspose::AblasNoTrans;
    trmmPack1.alpha = 1.;
    MM3D<T,U,blasEngine>::Multiply(
      matrixRI, matrixA, commWorld, commInfo3D, trmmPack1);
  }
  else
  {
    // Note: there are issues with serializing a square matrix into a rectangular. To bypass that,
    //        and also to communicate only the nonzeros, I will serialize into packed triangular buffers before calling TRSM
    Matrix<T,U,MatrixStructureUpperTriangular,Distribution> rectR(std::vector<T>(), localDimensionN, localDimensionN, globalDimensionN, globalDimensionN);
    Matrix<T,U,MatrixStructureUpperTriangular,Distribution> rectRI(std::vector<T>(), localDimensionN, localDimensionN, globalDimensionN, globalDimensionN);
    // Note: packedMatrix has no data right now. It will modify its buffers when serialized below
    Serializer<T,U,MatrixStructureSquare,MatrixStructureUpperTriangular>::Serialize(matrixR, rectR);
    Serializer<T,U,MatrixStructureSquare,MatrixStructureUpperTriangular>::Serialize(matrixRI, rectRI);
    // alpha and beta fields don't matter. All I need from this struct are whether or not transposes are used.
    gemmPack1.transposeA = blasEngineTranspose::AblasNoTrans;
    gemmPack1.transposeB = blasEngineTranspose::AblasNoTrans;
    blasEngineArgumentPackage_trmm<T> trmmPackage;
    trmmPackage.order = blasEngineOrder::AblasColumnMajor;
    trmmPackage.side = blasEngineSide::AblasRight;
    trmmPackage.uplo = blasEngineUpLo::AblasUpper;
    trmmPackage.diag = blasEngineDiag::AblasNonUnit;
    trmmPackage.transposeA = blasEngineTranspose::AblasNoTrans;
    trmmPackage.alpha = 1.;
    TRSM3D<T,U,blasEngine>::iSolveUpperLeft(
      matrixA, rectR, rectRI,
      baseCaseDimList.second, gemmPack1, trmmPackage, commWorld, commInfo3D);
  }
  TAU_FSTOP(Factor3D_cqr);
}

template<typename T,typename U,template<typename,typename> class blasEngine>
template<template<typename,typename,template<typename,typename,int> class> class StructureA, template<typename,typename,int> class Distribution>
void CholeskyQR2<T,U,blasEngine>::FactorTunable_cqr(
    Matrix<T,U,StructureA,Distribution>& matrixA, Matrix<T,U,MatrixStructureSquare,Distribution>& matrixR, int gridDimensionD, int gridDimensionC, MPI_Comm commWorld,
      std::tuple<MPI_Comm, MPI_Comm, MPI_Comm, MPI_Comm, MPI_Comm, MPI_Comm>& tunableCommunicators,
      std::tuple<MPI_Comm,MPI_Comm,MPI_Comm,MPI_Comm,int,int,int>& commInfo3D, int inverseCutOffMultiplier,
        int baseCaseMultiplier)
{
  TAU_FSTART(FactorTunable_cqr);
  MPI_Comm rowComm = std::get<0>(tunableCommunicators);
  MPI_Comm columnContigComm = std::get<1>(tunableCommunicators);
  MPI_Comm columnAltComm = std::get<2>(tunableCommunicators);
  MPI_Comm depthComm = std::get<3>(tunableCommunicators);
  MPI_Comm miniCubeComm = std::get<5>(tunableCommunicators);

  int worldRank;
  MPI_Comm_rank(commWorld, &worldRank);
  int sliceSize = gridDimensionD*gridDimensionC;
  int pCoordX = worldRank%gridDimensionC;
  int pCoordY = (worldRank%sliceSize)/gridDimensionC;
  int pCoordZ = worldRank/sliceSize;

  int columnContigRank;
  MPI_Comm_rank(columnContigComm, &columnContigRank);

  // Need to perform the multiple steps to get our partition of matrixA
  U globalDimensionM = matrixA.getNumRowsGlobal();
  U globalDimensionN = matrixA.getNumColumnsGlobal();
  U localDimensionM = matrixA.getNumRowsLocal();//globalDimensionM/gridDimensionD;
  U localDimensionN = matrixA.getNumColumnsLocal();//globalDimensionN/gridDimensionC;
  std::vector<T>& dataA = matrixA.getVectorData();
  U sizeA = matrixA.getNumElems();
  std::vector<T> foreignA;	// dont fill with data first, because if root its a waste,
                                //   but need it to outside to get outside scope
  bool isRootRow = ((pCoordX == pCoordZ) ? true : false);
  bool isRootColumn = ((columnContigRank == pCoordZ) ? true : false);

  // No optimization here I am pretty sure due to final result being symmetric, as it is cyclic and transpose isnt true as I have painfully found out before.
  BroadcastPanels(
    (isRootRow ? dataA : foreignA), sizeA, isRootRow, pCoordZ, rowComm);

  blasEngineArgumentPackage_gemm<T> gemmPack1;
  gemmPack1.order = blasEngineOrder::AblasColumnMajor;
  gemmPack1.transposeA = blasEngineTranspose::AblasTrans;
  gemmPack1.transposeB = blasEngineTranspose::AblasNoTrans;
  gemmPack1.alpha = 1.;
  gemmPack1.beta = 0.;

  // I don't think I can run syrk here, so I will use gemm. Maybe in the future after I have it correct I can experiment.
  blasEngine<T,U>::_gemm((isRootRow ? &dataA[0] : &foreignA[0]), &dataA[0], matrixR.getRawData(), localDimensionN, localDimensionN,
    localDimensionM, localDimensionM, localDimensionM, localDimensionN, gemmPack1);

  MPI_Reduce((isRootColumn ? MPI_IN_PLACE : matrixR.getRawData()), matrixR.getRawData(), localDimensionN*localDimensionN, MPI_DOUBLE,
    MPI_SUM, pCoordZ, columnContigComm);
  MPI_Allreduce(MPI_IN_PLACE, matrixR.getRawData(), localDimensionN*localDimensionN, MPI_DOUBLE,
    MPI_SUM, columnAltComm);

  MPI_Bcast(matrixR.getRawData(), localDimensionN*localDimensionN, MPI_DOUBLE, columnContigRank, depthComm);

  // Create an extra matrix for R-inverse
  Matrix<T,U,MatrixStructureSquare,Distribution> matrixRI(std::vector<T>(localDimensionN*localDimensionN,0), localDimensionN, localDimensionN,
    globalDimensionN, globalDimensionN, true);

  std::pair<bool,std::vector<U>> baseCaseDimList = CFR3D<T,U,blasEngine>::Factor(
    matrixR, matrixRI, inverseCutOffMultiplier, 'U', baseCaseMultiplier, miniCubeComm, commInfo3D);

  if (baseCaseDimList.first)
  {
    blasEngineArgumentPackage_trmm<T> trmmPack1;
    trmmPack1.order = blasEngineOrder::AblasColumnMajor;
    trmmPack1.side = blasEngineSide::AblasRight;
    trmmPack1.uplo = blasEngineUpLo::AblasUpper;
    trmmPack1.diag = blasEngineDiag::AblasNonUnit;
    trmmPack1.transposeA = blasEngineTranspose::AblasNoTrans;
    trmmPack1.alpha = 1.;
    MM3D<T,U,blasEngine>::Multiply(
      matrixRI, matrixA, miniCubeComm, commInfo3D, trmmPack1);
  }
  else
  {
    // Note: there are issues with serializing a square matrix into a rectangular. To bypass that,
    //        and also to communicate only the nonzeros, I will serialize into packed triangular buffers before calling TRSM
    Matrix<T,U,MatrixStructureUpperTriangular,Distribution> rectR(std::vector<T>(), localDimensionN, localDimensionN, globalDimensionN, globalDimensionN);
    Matrix<T,U,MatrixStructureUpperTriangular,Distribution> rectRI(std::vector<T>(), localDimensionN, localDimensionN, globalDimensionN, globalDimensionN);
    // Note: packedMatrix has no data right now. It will modify its buffers when serialized below
    Serializer<T,U,MatrixStructureSquare,MatrixStructureUpperTriangular>::Serialize(matrixR, rectR);
    Serializer<T,U,MatrixStructureSquare,MatrixStructureUpperTriangular>::Serialize(matrixRI, rectRI);
    // alpha and beta fields don't matter. All I need from this struct are whether or not transposes are used.
    gemmPack1.transposeA = blasEngineTranspose::AblasNoTrans;
    gemmPack1.transposeB = blasEngineTranspose::AblasNoTrans;
    blasEngineArgumentPackage_trmm<T> trmmPackage;
    trmmPackage.order = blasEngineOrder::AblasColumnMajor;
    trmmPackage.side = blasEngineSide::AblasRight;
    trmmPackage.uplo = blasEngineUpLo::AblasUpper;
    trmmPackage.diag = blasEngineDiag::AblasNonUnit;
    trmmPackage.transposeA = blasEngineTranspose::AblasNoTrans;
    trmmPackage.alpha = 1.;
    TRSM3D<T,U,blasEngine>::iSolveUpperLeft(
      matrixA, rectR, rectRI,
      baseCaseDimList.second, gemmPack1, trmmPackage, miniCubeComm, commInfo3D);
  }
  TAU_FSTOP(FactorTunable_cqr);
}

template<typename T,typename U, template<typename,typename> class blasEngine>
void CholeskyQR2<T,U,blasEngine>::BroadcastPanels(
											std::vector<T>& data,
											U size,
											bool isRoot,
											int pGridCoordZ,
											MPI_Comm panelComm
										    )
{
  TAU_FSTART(BroadcastPanels);
  if (isRoot)
  {
    MPI_Bcast(&data[0], size, MPI_DOUBLE, pGridCoordZ, panelComm);
  }
  else
  {
    data.resize(size);
    MPI_Bcast(&data[0], size, MPI_DOUBLE, pGridCoordZ, panelComm);
  }
  TAU_FSTOP(BroadcastPanels);
}
