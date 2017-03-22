//#include "solver.h" -> Not done here because of template issues

/*
  Turn on debugging statements when necessary but flipping the 0 to 1
*/
#define DEBUGGING 0
#define PROCESSOR_X_ 0
#define PROCESSOR_Y_ 0
#define PROCESSOR_Z_ 0

template<typename T>
solver<T>::solver(uint32_t rank, uint32_t size, uint32_t nDims, uint32_t matrixDimSize)
{
  this->worldRank = rank;
  this->worldSize = size;
  this->nDims = nDims;
  this->matrixDimSize = matrixDimSize;

/*
  Precompute a list of cubes for quick lookUp of cube dimensions based on processor size
  Maps number of processors involved in the computation to its cubic root to avoid expensive cubic root routine
  Table should reside in the L1 cache for quick lookUp, but only needed once
*/
  for (uint32_t i=1; i<500; i++)
  {
    uint64_t size = i;
    size *= i;
    size *= i;
    this->gridSizeLookUp[size] = i;
  }
}

template <typename T>
void solver<T>::startUp(bool &flag)
{
  /*
	Look up to see if the number of processors in startup phase is a cubic. If not, then return.
	We can reduce this restriction after we have it working for a perfect cubic processor grid
	If found, this->processorGridDimSize will represent the number of processors along one dimension, such as along a row or column (P^(1/3))
  */

  if (this->gridSizeLookUp.find(this->worldSize) == gridSizeLookUp.end())
  {
    #if DEBUGGING
    std::cout << "Requested number of processors is not valid for a 3-Dimensional processor grid. Program is ending." << std::endl;
    #endif
    flag = true;
    return;
  }

  this->processorGridDimSize = this->gridSizeLookUp[this->worldSize];
  
  /*
    this->baseCaseSize gives us a size in which to stop recursing in the CholeskyRecurse method
    = n/P^(2/3). The math behind it is written about in my report and other papers
  */
  this->baseCaseSize = this->matrixDimSize/(this->processorGridDimSize*this->processorGridDimSize);		// watch for possible overflow here later on

  this->gridDims.resize(this->nDims,this->processorGridDimSize);
  this->gridCoords.resize(this->nDims); 
  
  /*
    The 3D Cartesian Communicator is used to distribute the random data in a cyclic fashion
    The other communicators are created for specific communication patterns involved in the algorithm.
  */
  std::vector<int> boolVec(3,0);
  MPI_Cart_create(MPI_COMM_WORLD, this->nDims, &this->gridDims[0], &boolVec[0], false, &this->grid3D);
  MPI_Comm_rank(this->grid3D, &this->grid3DRank);
  MPI_Comm_size(this->grid3D, &this->grid3DSize);
  MPI_Cart_coords(this->grid3D, this->grid3DRank, this->nDims, &this->gridCoords[0]);

  /*
    Before creating row and column sub-communicators, grid3D must be split into 2D Layer communicators.
  */

  // 2D (xy) Layer Communicator (split by z coordinate)
  MPI_Comm_split(this->grid3D, this->gridCoords[2],this->grid3DRank,&this->layerComm);
  MPI_Comm_rank(this->layerComm,&this->layerCommRank);
  MPI_Comm_size(this->layerComm,&this->layerCommSize);
  // Row Communicator
  MPI_Comm_split(this->layerComm, this->gridCoords[0],this->gridCoords[1],&this->rowComm);
  MPI_Comm_rank(this->rowComm,&this->rowCommRank);
  MPI_Comm_size(this->rowComm,&this->rowCommSize);
  // column Communicator
  MPI_Comm_split(this->layerComm, this->gridCoords[1],this->gridCoords[0],&this->colComm);
  MPI_Comm_rank(this->colComm,&this->colCommRank);
  MPI_Comm_size(this->colComm,&this->colCommSize);
  // Depth Communicator
  MPI_Comm_split(this->grid3D,this->gridCoords[0]*this->processorGridDimSize+this->gridCoords[1],this->gridCoords[2],&this->depthComm);
  MPI_Comm_rank(this->depthComm,&this->depthCommRank);
  MPI_Comm_size(this->depthComm,&this->depthCommSize);
  
  #if DEBUGGING
  std::cout << "Program at end of startUp method. Details below.\n";
  std::cout << "Size of matrix -> " << this->matrixDimSize << std::endl;
  std::cout << "Matrix size for base case of Recursive Cholesky Algorithm -> " << this->baseCaseSize << std::endl;
  std::cout << "Size of MPI_COMM_WORLD -> " << this->worldSize << std::endl;
  std::cout << "Rank of my processor in MPI_COMM_WORLD -> " << this->worldRank << std::endl;
  std::cout << "Number of dimensions of processor grid -> " << this->nDims << std::endl;
  std::cout << "Number of processors along one dimension of 3-Dimensional grid -> " << this->processorGridDimSize << std::endl;
  std::cout << "Grid coordinates in 3D Processor Grid for my processor -> (" << this->gridCoords[0] << "," << this->gridCoords[1] << "," << this->gridCoords[2] << ")" << std::endl;
  std::cout << "Size of 2D Layer Communicator -> " << this->layerCommSize << std::endl;
  std::cout << "Rank of my processor in 2D Layer Communicator -> " << this->layerCommRank << std::endl;
  std::cout << "Size of Row Communicator -> " << this->colCommSize << std::endl;
  std::cout << "Rank of my processor in Row Communicator -> " << this->rowCommRank << std::endl;
  std::cout << "Size of Column Communicator -> " << this->colCommSize << std::endl;
  std::cout << "Rank of my processor in Column Communicator -> " << this->colCommRank << std::endl;
  std::cout << "Size of Depth Communicator Communicator -> " << this->depthCommSize << std::endl;
  std::cout << "Rank of my processor in Depth Communicator -> " << this->depthCommRank << std::endl;
  #endif
}


/*
  Cyclic distribution of data
*/
template <typename T>
void solver<T>::distributeDataCyclicSequential()
{
  /*
    If we think about the Cartesian rank coordinates, then we dont care what layer (in the z-direction) we are in. We care only about the (x,y) coordinates.
    Sublayers exist in a cyclic distribution, so that in the first sublayer of the matrix, all processors are used.
    Therefore, we cycle them from (0,1,2,0,1,2,0,1,2,....) in the x-dimension of the cube (when traveling down from the top-front-left corner) to the bottom-front-left corner.
    Then for each of those, there are the processors in the y-direction as well, which are representing via the nested loop.
  */

  /*
    Note that because matrix A is supposed to be symmetric, I will only give out the lower-triangular portion of A
    Note that this would require a change in my algorithm, because I do reference uper part of A.
    Instead, I will only give values for the upper-portion of A. At a later date, I can change the code to only
    allocate a triangular portion instead of storing half as zeros.
  */

  #if DEBUGGING
  std::cout << "Program is distributing the matrix data in a cyclic manner\n";
  #endif

  uint64_t size = this->processorGridDimSize;			// Expensive division? Lots of compute cycles? Worse than pushing back?
  size *= size;
  uint64_t matSize = this->matrixDimSize;
  matSize *= matSize;
  size = matSize/size;						// Watch for overflow, N^2 / P^{2/3} is the initial size of the data that each p owns
  //this->matrixA.push_back(std::vector<T>());
  this->matrixA.resize(1,std::vector<T>(size,0.));		// Try this. Resize vs. push_back, better for parallel version?
  // Note: above, I am not sure if the vector can hold more than 2^32 elements. Maybe this isnt too big of a deal, but ...

  uint64_t counter = 0;						// 64 bits needed, really?
  for (uint32_t i=this->gridCoords[0]; i<this->matrixDimSize; i+=this->processorGridDimSize)
  {
    this->localSize = 0;
    for (uint32_t j=this->gridCoords[1]; j<this->matrixDimSize; j+=this->processorGridDimSize)
    {
      if (i > j)
      {
        srand(i*this->matrixDimSize + j);		// Assuming no overflow here
      }
      else
      {
        srand(j*this->matrixDimSize + i);		// Assuming no overflow here
      }
      
      //this->matrixA[this->matrixA.size()-1].push_back((rand()%100)*1./100.);
      this->matrixA[this->matrixA.size()-1][counter++] = (rand()%100)*1./100;
      if (i==j)
      {
        //matrixA[this->matrixA.size()-1][matrixA[this->matrixA.size()-1].size()-1] += 10.;		// All diagonals will be dominant for now.
        this->matrixA[this->matrixA.size()-1][counter-1] += 10.;
      }
      this->localSize++;
    }
  }

  #if DEBUGGING
  if ((this->gridCoords[0] == PROCESSOR_X_) && (this->gridCoords[1] == PROCESSOR_Y_) && (this->gridCoords[2] == PROCESSOR_Z_))
  {
    std::cout << "About to print what processor (" << this->gridCoords[0] << "," << this->gridCoords[1] << "," << this->gridCoords[2] << ") owns.\n";
    for (uint32_t i=0; i<this->localSize; i++)
    {
      for (uint32_t j=0; j<this->localSize; j++)
      {
        std::cout << this->matrixA[i*this->localSize+j] << " ";
      }
      std::cout << "\n";
    }
  }
  #endif

  uint64_t tempSize = this->localSize;                   // n(n+1)/2 is the size needed to hold the triangular portion of the matrix
  tempSize *= (this->localSize+1);
  tempSize >>= 1;
  this->matrixL.resize(tempSize,0.);
  this->matrixLInverse.resize(tempSize,0.);

}

/*
  Cyclic distribution of data on one layer, then broadcasting the data to the other P^{1/3}-1 layers, similar to how Scalapack does it
*/
template <typename T>
void solver<T>::distributeDataCyclicParallel()
{
  /*
    If we think about the Cartesian rank coordinates, then we dont care what layer (in the z-direction) we are in. We care only about the (x,y) coordinates.
    Sublayers exist in a cyclic distribution, so that in the first sublayer of the matrix, all processors are used.
    Therefore, we cycle them from (0,1,2,0,1,2,0,1,2,....) in the x-dimension of the cube (when traveling down from the top-front-left corner) to the bottom-front-left corner.
    Then for each of those, there are the processors in the y-direction as well, which are representing via the nested loop.
  */

  /*
    Note that because matrix A is supposed to be symmetric, I will only give out the lower-triangular portion of A
    Note that this would require a change in my algorithm, because I do reference uper part of A.
    Instead, I will only give values for the upper-portion of A. At a later date, I can change the code to only
    allocate a triangular portion instead of storing half as zeros.
  */

  #if DEBUGGING
  std::cout << "Program is distributing the matrix data in a cyclic manner\n";
  #endif
  
  uint64_t size = this->processorGridDimSize;			// Expensive division? Lots of compute cycles? Worse than pushing back?
  size *= size;
  uint64_t matSize = this->matrixDimSize;
  matSize *= matSize;
  size = matSize/size;						// Watch for overflow, N^2 / P^{2/3} is the initial size of the data that each p owns
  //this->matrixA.push_back(std::vector<T>());
  this->matrixA.resize(1,std::vector<T>(size,0.));		// Try this. Resize vs. push_back, better for parallel version?
  // Note: above, I am not sure if the vector can hold more than 2^32 elements. Maybe this isnt too big of a deal, but ...

  // Only distribute the data sequentially on the first layer. Then we broadcast down the 3D processor cube using the depth communicator
  if (this->gridCoords[2] == 0)
  {
    uint64_t counter = 0;
    for (uint32_t i=this->gridCoords[0]; i<this->matrixDimSize; i+=this->processorGridDimSize)
    {
      this->localSize = 0;
      for (uint32_t j=this->gridCoords[1]; j<this->matrixDimSize; j+=this->processorGridDimSize)
      {
        if (i > j)
        {
          srand(i*this->matrixDimSize + j);			// Watch for overflow?
        }
        else
        {
          srand(j*this->matrixDimSize + i);			// Watch for overflow?
        }
      
        //this->matrixA[this->matrixA.size()-1].push_back((rand()%100)*1./100.);
        this->matrixA[this->matrixA.size()-1][counter++] = (rand()%100)*1./100;
        if (i==j)
        {
          //matrixA[this->matrixA.size()-1][matrixA[this->matrixA.size()-1].size()-1] += 10.;		// All diagonals will be dominant for now.
          this->matrixA[this->matrixA.size()-1][counter-1] += 10.;
        }
        this->localSize++;
      }
    }

    MPI_Bcast(&this->matrixA[this->matrixA.size()-1][0], size, MPI_DOUBLE, this->depthCommRank, this->depthComm);  
  }
  else			// All other processor not on that 1st layer
  {
    // I am assuming that the root has rank 0 in the depth communicator
    MPI_Bcast(&this->matrixA[this->matrixA.size()-1][0], size, MPI_DOUBLE, 0, this->depthComm);
  }

  #if DEBUGGING
  if ((this->gridCoords[0] == PROCESSOR_X_) && (this->gridCoords[1] == PROCESSOR_Y_) && (this->gridCoords[2] == PROCESSOR_Z_))
  {
    std::cout << "About to print what processor (" << this->gridCoords[0] << "," << this->gridCoords[1] << "," << this->gridCoords[2] << ") owns.\n";
    for (uint32_t i=0; i<this->localSize; i++)
    {
      for (uint32_t j=0; j<this->localSize; j++)
      {
        std::cout << this->matrixA[i*this->localSize+j] << " ";
      }
      std::cout << "\n";
    }
  }
  #endif

  this->localSize = this->matrixDimSize/this->processorGridDimSize;	// Expensive division. Lots of compute cycles
  uint64_t tempSize = this->localSize;                   // n(n+1)/2 is the size needed to hold the triangular portion of the matrix
  tempSize *= (this->localSize+1);
  tempSize >>= 1;
  this->matrixL.resize(tempSize,0.);
  this->matrixLInverse.resize(tempSize,0.);

}

/********************************************************************************************************************************************/
// This divides the program. If anything above this point is incorrect, then everything below will be incorrect as well.
/********************************************************************************************************************************************/



/*
  The solve method will initiate the solving of this Cholesky Factorization algorithm
*/
template<typename T>
void solver<T>::solve()
{
  #if DEBUGGING
  std::cout << "Starting solver::CholeskyRecurse(" << 0 << "," << this->localSize << "," << 0 << "," << this->localSize << "," << this->localSize << "," << this->matrixDimSize << "," << this->localSize << ")\n";
  #endif

  CholeskyRecurse(0,this->localSize,0,this->localSize,this->localSize,this->matrixDimSize, this->localSize);
}

/*
  Write function description
*/
template<typename T>
void solver<T>::CholeskyRecurse(uint32_t dimXstart, uint32_t dimXend, uint32_t dimYstart, uint32_t dimYend, uint32_t matrixWindow, uint32_t matrixSize, uint32_t matrixCutSize)
{

  if (matrixSize == this->baseCaseSize)
  {
    CholeskyRecurseBaseCase(dimXstart,dimXend,dimYstart,dimYend,matrixWindow,matrixSize, matrixCutSize);
    return;
  }

  /*
	Recursive case -> Keep recursing on the top-left half
  */

  uint32_t shift = matrixWindow>>1;
  CholeskyRecurse(dimXstart,dimXend-shift,dimYstart,dimYend-shift,shift,(matrixSize>>1), matrixCutSize);
  
  // Add MPI_SendRecv in here
  fillTranspose(dimXstart, dimXend-shift, dimYstart, dimYend-shift, shift, 0);

  MM(dimXstart+shift,dimXend,dimYstart,dimYend-shift,0,0,0,0,dimXstart+shift,dimXend,dimYstart,dimYend-shift,shift,(matrixSize>>1),0, matrixCutSize);

  fillTranspose(dimXstart+shift, dimXend, dimYstart, dimYend-shift, shift, 1);
  

  //Below is the tricky one. We need to create a vector via MM(..), then subtract it from A via some sort easy method...
  MM(dimXstart+shift,dimXend,dimYstart,dimYend-shift,0,0,0,0,dimXstart,dimXend-shift,dimYstart,dimYend-shift,shift,(matrixSize>>1),1, matrixCutSize);

  // Big question: CholeskyRecurseBaseCase relies on taking from matrixA, but in this case, we would have to take from a modified matrix via the Schur Complement.
  // Note that the below code has room for optimization via some call to LAPACK, but I would have to loop over and put into a 1d array first and then
  // pack it back into my 2d array. So in general, I need to look at whether modifying the code to use 1D vectors instead of 2D vectors is worth it.

  // REPLACE THE BELOW WITH LAPACK SYRK ONCE I FINISH FIXING CORRECTNESS ERRORS
 

  // BELOW : an absolutely critical new addition to the code. Allows building a state recursively
  uint64_t tempSize = shift;
  tempSize *= shift;
  this->matrixA.push_back(std::vector<T>(tempSize,0.));					// New submatrix of size shift*shift

  // This indexing may be wrong in this new way that I am doing it.
  //int temp = (dimXstart+shift)*this->localSize + dimYstart+shift;
  uint64_t start = shift;
  start *= matrixCutSize;
  start += shift;
  //int start = shift*matrixCutSize+shift;
  for (uint32_t i=0; i<shift; i++)
  {
    uint64_t save = i*shift;
    for (uint32_t j=0; j<shift; j++)
    {
      // Only need to push back to this
      uint64_t hold1 = save;
      hold1 += j;
      uint64_t hold2 = start;
      hold2 += j;
      this->matrixA[this->matrixA.size()-1][hold1] = this->matrixA[this->matrixA.size()-2][hold2] - this->holdMatrix[hold1];
//      if (this->worldRank == 0) {std::cout << "RECURSE index - " << start+j << " and val - " << this->matrixA[this->matrixA.size()-1][save+j] << "\n"; }
    }
    //temp += this->localSize;
    start += matrixCutSize;
  }

  CholeskyRecurse(dimXstart+shift,dimXend,dimYstart+shift,dimYend,shift,(matrixSize>>1), shift);		// changed to shift, not matrixCutSize/2

  // These last 4 matrix multiplications are for building up our LInverse and UInverse matrices
  MM(dimXstart+shift,dimXend,dimYstart,dimYend-shift,dimXstart,dimXend-shift,dimYstart,dimYend-shift,dimXstart,dimXend-shift,dimYstart,dimYend-shift,shift,(matrixSize>>1),2, matrixCutSize);

  MM(dimXstart+shift,dimXend,dimYstart+shift,dimYend,dimXstart,dimXend-shift,dimYstart,dimYend-shift,dimXstart+shift,dimXend,dimYstart,dimYend-shift,shift,(matrixSize>>1),3, matrixCutSize);

  this->matrixA.pop_back();			// Absolutely critical. Get rid of that memory that we won't use again.

}

/*
  Recursive Matrix Multiplication
  This routine requires that matrix data is duplicated across all layers of the 3D Processor Grid
*/
template<typename T>
void solver<T>::MM(uint32_t dimXstartA, uint32_t dimXendA, uint32_t dimYstartA, uint32_t dimYendA, uint32_t dimXstartB, uint32_t dimXendB, uint32_t dimYstartB, uint32_t dimYendB, uint32_t dimXstartC, uint32_t dimXendC, uint32_t dimYstartC, uint32_t dimYendC, uint32_t matrixWindow, uint32_t matrixSize, uint32_t key, uint32_t matrixCutSize)
{
  /*
    I need two broadcasts, then an AllReduce
  */

  /*
    I think there are only 2 possibilities here. Either size == matrixWindow*(matrixWindow)/2 if the first element fits
						     or size == matrixWindow*(matrixWindow)/2 - 1 if the first element does not fit.
  */
  std::vector<T> buffer1;  // use capacity() or reserve() methods here?
  std::vector<T> buffer2;

  if (this->rowCommRank == this->gridCoords[2])     // different matches on each of the P^(1/3) layers
  {
    switch (key)
    {
      case 0:
      {
        // (square,lower) -> Note for further optimization.
        // -> This copy loop may not be needed. I might be able to purely send in &this->matrixLInverse[0] into broadcast
        uint64_t buffer1Size = matrixWindow;
        buffer1Size *= matrixWindow;
        buffer1.resize(buffer1Size,0.);
        uint64_t startOffset = matrixCutSize;
        startOffset *= matrixWindow;
        uint64_t index1 = 0;
        uint64_t index2 = startOffset;
        for (uint32_t i=0; i<matrixWindow; i++)
        {
          for (uint32_t j=0; j<matrixWindow; j++)
          {
            uint64_t temp = index2;
            temp += j;
            buffer1[index1++] = this->matrixA[this->matrixA.size()-1][temp];
            #if DEBUGGING
            if ((this->gridCoords[0] == PROCESSOR_X_) && (this->gridCoords[1] == PROCESSOR_Y_) && (this->gridCoords[2] == PROCESSOR_Z_))
            {
              std::cout << "index - " << temp << " and val - " << buffer1[index1-1] << " and matWindow - " << matrixWindow << " and matCutSize - " << matrixCutSize << std::endl;
            }
            #endif
          }
          index2 += matrixCutSize;
        }
        break;
      }
      case 1:
      {
        // Not triangular
        uint64_t buffer1Size = matrixWindow;
        buffer1Size *= matrixWindow;
        buffer1.resize(buffer1Size);
        uint64_t startOffset = dimXstartA;
        startOffset *= (dimXstartA+1);
        startOffset >>= 1;
        //int startOffset = (dimXstartA*(dimXstartA+1))>>1;
        uint64_t index1 = 0;
        uint64_t index2 = startOffset;
        index2 += dimYstartA;
        for (uint32_t i=0; i<matrixWindow; i++)
        {
          for (uint32_t j=0; j<matrixWindow; j++)
          {
            buffer1[index1++] = this->matrixL[index2++];
          }
          uint64_t temp = dimYstartA;
          temp += i;
          temp++;
          //index2 += (dimYstartA+i+1);
          index2 += temp;
        }
        break;
      }
      case 2:		// Part of the Inverse L calculation
      {
        // Not Triangular
        uint64_t buffer1Size = matrixWindow;
        buffer1Size *= matrixWindow;
        buffer1.resize(buffer1Size);
        uint64_t startOffset = dimXstartA;
        startOffset *= (dimXstartA+1);
        startOffset >>= 1;
        //int startOffset = (dimXstartA*(dimXstartA+1))>>1;
        uint64_t index1 = 0;
        uint64_t index2 = startOffset;
        index2 += dimYstartA;
        for (uint64_t i=0; i<matrixWindow; i++)
        {
          for (uint64_t j=0; j<matrixWindow; j++)
          {
            buffer1[index1++] = this->matrixL[index2++];
          }
          uint64_t temp = dimYstartA;
          temp += i;
          temp++;
          index2 += temp;
          //index2 += (dimYstartA+i+1);
        }
        break;
      }
      case 3:
      {
        // Triangular -> Lower
        // As noted above, size depends on whether or not the gridCoords lie in the lower-triangular portion of first block
        uint64_t buffer1Size = matrixWindow;
        buffer1Size *= (matrixWindow+1);
        buffer1Size >>= 1;
        buffer1.resize(buffer1Size);
        uint64_t startOffset = dimXstartA;
        startOffset *= (dimXstartA+1);
        startOffset >>= 1;
        //int startOffset = (dimXstartA*(dimXstartA+1))>>1;
        uint64_t index1 = 0;
        uint64_t index2 = startOffset;
        index2 += dimYstartA;
        for (uint32_t i=0; i<matrixWindow; i++)		// start can take on 2 values corresponding to the situation above: 1 or 0
        {
          for (uint32_t j=0; j<=i; j++)
          {
            buffer1[index1++] = this->matrixLInverse[index2++];
          }
          index2 += dimYstartA;
        }
        break;
      }
    }
    
    // Note that this broadcast will broadcast different sizes of buffer1, so on the receiving end, we will need another case statement
    // so that a properly-sized buffer is used.
    MPI_Bcast(&buffer1[0],buffer1.size(),MPI_DOUBLE,this->gridCoords[2],this->rowComm);
  }
  else
  {
    switch (key)
    {
      case 0:
      {
        uint64_t buffer1Size = matrixWindow;
        buffer1Size *= matrixWindow;
        buffer1.resize(buffer1Size);
        break;
      }
      case 1:
      {
        uint64_t buffer1Size = matrixWindow;
        buffer1Size *= matrixWindow;
        buffer1.resize(buffer1Size);
        break;
      }
      case 2:
      {
        uint64_t buffer1Size = matrixWindow;
        buffer1Size *= matrixWindow;
        buffer1.resize(buffer1Size);
        break;
      }
      case 3:
      {
        uint64_t buffer1Size = matrixWindow;
        buffer1Size *= (matrixWindow+1);
        buffer1Size >>= 1;
        buffer1.resize(buffer1Size);
        break;
      }
    }

    // Note that depending on the key, the broadcast received here will be of different sizes. Need care when unpacking it later.
    MPI_Bcast(&buffer1[0],buffer1.size(),MPI_DOUBLE,this->gridCoords[2],this->rowComm);
//    if ((this->gridCoords[0] == 0) && (this->gridCoords[1] == 1) && (this->gridCoords[2] == 1)) { for (int i=0; i<buffer1.size(); i++) { std::cout << "Inv2? - " << buffer1[i] << " of size - " << buffer1.size() << std::endl; } std::cout << "\n"; }
  }

  if (this->colCommRank == this->gridCoords[2])    // May want to recheck this later if bugs occur.
  {
    switch (key)
    {
      case 0:
      {
/*
        // Triangular
        buffer2.resize(matrixWindow*matrixWindow);
        int startOffset = dimXstartB*this->localSize;	// Problem! startOffset needs to be based off of this->localRank, not anything else
        int index1 = 0;
        int index2 = startOffset + dimYstartB;
        for (int i=0; i<matrixWindow; i++)
        {
          for (int j=0; j<matrixWindow; j++)
          {
            buffer2[index1++] = (matrixTrack ? this->matrixB[index2+j] : this->matrixA[index2+j]);
//            if ((this->gridCoords[0] == 1) && (this->gridCoords[1] == 1) && (this->gridCoords[2] == 1)) {std::cout << "GAY - " << index2+j << " " << buffer2[index1-1] << std::endl; }
          }
          index2 += this->localSize;
        }
*/
        buffer2 = this->holdTransposeL;			// Is this copy necessary?
        break;
      }
      case 1:
      {
        // Not Triangular, but transpose
        // POSSIBLE OPTIMIZATION PLACE! Is this transpose necessary? Or can the LAPACK routine do this for us in a more efficient manner?
        uint64_t buffer2Size = matrixWindow;
        buffer2Size *= matrixWindow;
        buffer2.resize(buffer2Size);
/*
        int startOffset = (dimXstartB*(dimXstartB+1))>>1;
        int index1 = 0;
        for (int i=0; i<matrixWindow; i++)
        {
          int index2 = startOffset + dimYstartB + i;
          int temp = //dimYstartB+//1+matrixWindow;			// I added the +1 back in
          for (int j=0; j<matrixWindow; j++)
          {
            buffer2[index1++] = this->matrixL[index2];                  // Transpose has poor spatial locality
            index2 += temp+j+dimYstartB;
          }
        }
*/
        // Simple transpose into buffer2 using this->holdTransposeL, but we can make this more efficient later via 2D tiling to reduce cache misses
       for (uint32_t i=0; i<matrixWindow; i++)
        {
          for (uint32_t j=0; j<matrixWindow; j++)
          {
            uint64_t index1 = j;
            index1 *= matrixWindow;
            index1 += i;
            uint64_t index2 = i;
            index2 *= matrixWindow;
            index2 += j;
            buffer2[index1] = this->holdTransposeL[index2];
          }
        }
        break;
      }
      case 2:
      {
        // Triangular -> Lower
        // As noted above, size depends on whether or not the gridCoords lie in the lower-triangular portion of first block
        uint64_t buffer2Size = matrixWindow;
        buffer2Size *= (matrixWindow+1);
        buffer2Size >>= 1;
        buffer2.resize(buffer2Size);	// change to bitshift later
        uint64_t startOffset = dimXstartB;
        startOffset *= (dimXstartB+1);
        startOffset >>= 1;
        //int startOffset = (dimXstartB*(dimXstartB+1))>>1;
        uint64_t index1 = 0;
        uint64_t index2 = startOffset;
        index2 += dimYstartB;
        for (uint32_t i=0; i<matrixWindow; i++)
        {
          for (uint32_t j=0; j<=i; j++)
          {
            buffer2[index1++] = this->matrixLInverse[index2++];
          }
          index2 += dimYstartB;
        }
        break;
      }
      case 3:
      {
        // Not Triangular, but this requires a special traversal because we are using this->holdMatrix
        uint64_t buffer2Size = matrixWindow;
        buffer2Size *= matrixWindow;
        buffer2.resize(buffer2Size);
        //int startOffset = (dimXstartB*(dimXstartB-1))>>1;
        uint64_t index1 = 0;
        uint64_t index2 = 0;
        //int index2 = startOffset + dimYstartB;
        for (uint32_t i=0; i<matrixWindow; i++)
        {
          for (uint32_t j=0; j<matrixWindow; j++)
          {
            buffer2[index1++] = (-1)*this->holdMatrix[index2++];
          }
          //index2 += dimYstartB;
        }
        break;
      }
    }
    // Note that this broadcast will broadcast different sizes of buffer1, so on the receiving end, we will need another case statement
    // so that a properly-sized buffer is used.
    MPI_Bcast(&buffer2[0],buffer2.size(),MPI_DOUBLE,this->gridCoords[2],this->colComm);
  }
  else
  {
    switch (key)
    {
      case 0:
      {
        // What I can do here is receive with a buffer of size matrixWindow*(matrixWindow+1)/2 and then iterate over the last part to see what kind it is
        // i was trying to do it with gridCoords, but then I would have needed this->rowRank, stuff like that so this is a bit easier
        // Maybe I can fix this later to make it more stable
        uint64_t buffer2Size = matrixWindow;
        buffer2Size *= (matrixWindow+1);
        buffer2Size >>= 1;
        buffer2.resize(buffer2Size);	// this is a special trick
        break;
      }
      case 1:
      {
        uint64_t buffer2Size = matrixWindow;
        buffer2Size *= matrixWindow;
        buffer2.resize(buffer2Size);
        break;
      }
      case 2:
      {
        // What I can do here is receive with a buffer of size matrixWindow*(matrixWindow+1)/2 and then iterate over the last part to see what kind it is
        // i was trying to do it with gridCoords, but then I would have needed this->rowRank, stuff like that so this is a bit easier
        // Maybe I can fix this later to make it more stable
        uint64_t buffer2Size = matrixWindow;
        buffer2Size *= (matrixWindow+1);
        buffer2Size >>= 1;
        buffer2.resize(buffer2Size);	// this is a special trick
        break;
      }
      case 3:
      {
        uint64_t buffer2Size = matrixWindow;
        buffer2Size *= matrixWindow;
        buffer2.resize(buffer2Size);
        break;
      }
    }
    // Note that depending on the key, the broadcast received here will be of different sizes. Need care when unpacking it later.
    MPI_Bcast(&buffer2[0],buffer2.size(),MPI_DOUBLE,this->gridCoords[2],this->colComm);
//    if ((this->gridCoords[0] == 1) && (this->gridCoords[1] == 1) && (this->gridCoords[2] == 1)) { for (int i=0; i<buffer2.size(); i++) { std::cout << "GAY2 - " << buffer2[i] << " of size - " << buffer2.size() << std::endl; } std::cout << "\n"; }
  }

  /*
    Now once here, I have the row data and the column data residing in buffer1 and buffer2
    I need to use the data in these buffers correctly to calculate partial sums for each place that I own. And store them together in order to
      properly reduce the sums. So use another temporary buffer. Only update this->matrixU or this->matrixL after the reduction if root or after the final
        broadcast if non-root.
  */


  // So iterate over everything that we own and calculate its partial sum that will be used in reduction
  // buffer3 will be used to hold the partial matrix sum result that will contribute to the reduction.
  uint64_t buffer4Size = matrixWindow;
  buffer4Size *= matrixWindow;
  std::vector<T> buffer4(buffer4Size,0.);	// need to give this outside scope

  switch (key)
  {
    case 0:			// LATER OPTIMIZATION : I may not need to do this triangular transpose. The LAPACK routine might be able to do it for me!
    {
      // Triangular Multiplicaton -> (lower,square)
      uint64_t updatedVectorSize = matrixWindow;
      updatedVectorSize *= matrixWindow;
      std::vector<T> updatedVector(updatedVectorSize,0.);
      uint64_t index = 0;
      uint64_t counter = 1;
      for (uint32_t i = 0; i<matrixWindow; i++)
      {
        for (uint32_t j = i; j <matrixWindow; j++)
        {
          uint64_t temp = i;
          temp *= matrixWindow;
          temp += j;
          //int temp = i*matrixWindow+j;				// JUST CHANGED THIS!
          updatedVector[temp] = buffer2[index]; 
          index += counter;
          counter++;
          //index += counter++;
        }
        uint64_t temp = i+2;
        temp *= (i+3);
        temp >>= 1;
        temp--;
        //index = (((i+2)*(i+3))>>1)-1;
        index = temp;
        counter = i+2;
      }

      cblas_dtrmm(CblasRowMajor,CblasRight,CblasUpper,CblasNoTrans,CblasNonUnit,matrixWindow,matrixWindow,1.,&updatedVector[0], matrixWindow, &buffer1[0], matrixWindow);

      MPI_Allreduce(&buffer1[0],&buffer4[0],buffer1.size(),MPI_DOUBLE,MPI_SUM,this->depthComm);

      break;
    }
    case 1:
    {
      // Matrix Multiplication -> (square,square)
      uint64_t buffer3Size = matrixWindow;
      buffer3Size *= matrixWindow;
      std::vector<T> buffer3(buffer3Size);
/*
      if ((this->gridCoords[0] == 0) && (this->gridCoords[1] == 0) && (this->gridCoords[2] == 0))
      {
        std::cout << "L21 - ******************************************************************************************\n";
        for (int i=0; i<buffer1.size(); i++)
        {
          std::cout << buffer1[i] << " ";
          if ((i+1)%matrixWindow == 0) {std::cout << "\n"; }
        }
        std::cout << "LT21 - ******************************************************************************************\n";
        for (int i=0; i<buffer1.size(); i++)
        {
          std::cout << buffer2[i] << " ";
          if ((i+1)%matrixWindow == 0) {std::cout << "\n"; }
        }
        std::cout << "\n\n";
      }
*/
      cblas_dgemm(CblasRowMajor,CblasNoTrans,CblasNoTrans,matrixWindow,matrixWindow,matrixWindow,1.,&buffer1[0],matrixWindow,&buffer2[0],matrixWindow,1.,&buffer3[0],matrixWindow);
      MPI_Allreduce(&buffer3[0],&buffer4[0],buffer3.size(),MPI_DOUBLE,MPI_SUM,this->depthComm);
      break;
    }
    case 2:
    {
      // Triangular Multiplication -> (square,lower)
      uint64_t updatedVectorSize = matrixWindow;
      updatedVectorSize *= matrixWindow;
      std::vector<T> updatedVector(updatedVectorSize,0.);
      uint64_t index = 0;
      for (uint32_t i = 0; i<matrixWindow; i++)
      {
        uint64_t temp = i;
        i *= matrixWindow;
        //int temp = i*matrixWindow;
        for (uint32_t j = 0; j <= i; j++)	// this could be wrong ???
        {
          uint64_t updatedVectorIndex = temp;
          updatedVectorIndex += j;
          updatedVector[updatedVectorIndex] = buffer2[index++];
        }
      }
      
      cblas_dtrmm(CblasRowMajor,CblasRight,CblasLower,CblasNoTrans,CblasNonUnit,matrixWindow,matrixWindow,1.,&updatedVector[0],matrixWindow,&buffer1[0],matrixWindow);
      MPI_Allreduce(&buffer1[0],&buffer4[0],buffer1.size(),MPI_DOUBLE,MPI_SUM,this->depthComm);	// use buffer4
      break;
    }
    case 3:
    {
      // Triangular Multiplication -> (lower,square)
      uint64_t updatedVectorSize = matrixWindow;
      updatedVectorSize *= matrixWindow;
      std::vector<T> updatedVector(updatedVectorSize,0.);
      uint64_t index = 0;
      for (uint32_t i = 0; i<matrixWindow; i++)
      {
        uint64_t temp = i;
        temp *= matrixWindow;
        //int temp = i*matrixWindow;
        for (uint32_t j = 0; j <= i; j++)	// This could be wrong?
        {
          uint64_t updatedVectorSize = temp;
          updatedVectorSize += j;
          updatedVector[updatedVectorSize] = buffer1[index++];
        }
      }

      cblas_dtrmm(CblasRowMajor,CblasLeft,CblasLower,CblasNoTrans,CblasNonUnit,matrixWindow,matrixWindow,1.,&updatedVector[0],matrixWindow,&buffer2[0],matrixWindow);
      MPI_Allreduce(&buffer2[0],&buffer4[0],buffer2.size(),MPI_DOUBLE,MPI_SUM,this->depthComm);
      break;
    }
  }

  /*
    Now I need a case statement of where to put the guy that was just broadasted.
  */

  uint64_t holdMatrixSize = matrixWindow;
  holdMatrixSize *= matrixWindow;
  this->holdMatrix.resize(holdMatrixSize,0.);
  switch (key)
  {
    case 0:
    {
      // Square
      uint64_t startOffset = dimXstartC;
      startOffset *= (dimXstartC+1);
      startOffset >>= 1;
      //int startOffset = (dimXstartC*(dimXstartC+1))>>1;
      uint64_t index1 = 0;
      uint64_t index2 = startOffset;
      index2 += dimYstartC;
      for (uint32_t i=0; i<matrixWindow; i++)
      {
        for (uint32_t j=0; j<matrixWindow; j++)
        {
          this->matrixL[index2++] = buffer4[index1++];                    // Transpose has poor spatial locality. Could fix later if need be
        }
        uint64_t temp = dimYstartC;
        temp += i;
        temp++;
        //index2 += (dimYstartC+i+1);
        index2 += temp;
      }
      break;
    }
    case 1:						// Can this just be done with a simple copy statement??
    {
      uint64_t index1 = 0;
      for (uint32_t i=0; i<matrixWindow; i++)
      {
        uint64_t temp = i;
        temp *= matrixWindow;
        for (uint32_t j=0; j<matrixWindow; j++)
        {
          uint64_t holdMatrixIndex = temp;
          holdMatrixIndex += j;
          this->holdMatrix[holdMatrixIndex] = buffer4[index1++];
        }
      }
      break;
    }
    case 2:						// Can this just be done with a simple copy statement??
    {
      uint64_t index1 = 0;
      uint64_t index2 = 0;
      for (uint32_t i=0; i<matrixWindow; i++)
      {
        for (uint32_t j=0; j<matrixWindow; j++)
        {
          this->holdMatrix[index2++] = buffer4[index1++];
        }
      }
      break;
    }
    case 3:
    {
      uint64_t startOffset = dimXstartC;
      startOffset *= (dimXstartC+1);
      startOffset >>= 1;
      //int startOffset = (dimXstartC*(dimXstartC+1))>>1;
      uint64_t index1 = 0;
      uint64_t index2 = startOffset;
      startOffset  += dimYstartC;
      for (uint32_t i=0; i<matrixWindow; i++)
      {
        for (uint32_t j=0; j<matrixWindow; j++)
        {
          this->matrixLInverse[index2++] = buffer4[index1++];
//          if ((this->gridCoords[0] == 0) && (this->gridCoords[1] == 1) && (this->gridCoords[2] == 0)) {std::cout << "check index2 - " << index2-1 << " " << this->matrixLInverse[index2-1] << std::endl; }
          
        }
        uint64_t temp = dimYstartC;
        temp += i;
        temp++;
        index2 += temp;
      }
      break;
    }
  }
}


/*
  Transpose Communication Helper Function
*/
template<typename T>
void solver<T>::fillTranspose(uint32_t dimXstart, uint32_t dimXend, uint32_t dimYstart, uint32_t dimYend, uint32_t matrixWindow, uint32_t dir)
{
  switch (dir)
  {
    case 0:
    {
      // copy L11-inverse data into holdTransposeInverse to be send/received
      this->holdTransposeL.clear();
      this->holdTransposeL.resize((matrixWindow*(matrixWindow+1))>>1, 0.);
      int startOffset = ((dimXstart*(dimXstart+1))>>1);
      int index1 = 0;
      int index2 = startOffset + dimXstart;
      for (int i=0; i<matrixWindow; i++)
      {
        for (int j=0; j<=i; j++)
        {
          this->holdTransposeL[index1++] = this->matrixLInverse[index2++];
        }
        index2 += dimYstart;
      }
/*
        if ((this->gridCoords[0] == 0) && (this->gridCoords[1] == 1) && (this->gridCoords[2] == 1))
        {
	  std::cout << "sending this shit over, size - " << this->holdTransposeL.size() << "****************************************************************************************************\n";
          for (int i=0; i<this->holdTransposeL.size(); i++)
          {
            std::cout << "gay - " << this->holdTransposeL[i] << std::endl;
          }
          std::cout << "checkTranspose****************************************************************************************************\n";
        }
*/
      if ((this->gridCoords[0] != this->gridCoords[1]))
      {
        // perform MPI_SendRecv_replace
        int destRank = -1;
        int rankArray[3] = {this->gridCoords[1], this->gridCoords[0], this->gridCoords[2]};
        MPI_Cart_rank(this->grid3D, &rankArray[0], &destRank);
        MPI_Status stat;
        MPI_Sendrecv_replace(&this->holdTransposeL[0], this->holdTransposeL.size(), MPI_DOUBLE,
          destRank, 0, destRank, MPI_ANY_TAG, MPI_COMM_WORLD, &stat);
        // anything else?
/*
        if ((this->gridCoords[0] == 1) && (this->gridCoords[1] == 0) && (this->gridCoords[2] == 1))
        {
	  std::cout << "checkTranspose, size - " << this->holdTransposeL.size() << "****************************************************************************************************\n";
          for (int i=0; i<this->holdTransposeL.size(); i++)
          {
            std::cout << "huh - " << this->holdTransposeL[i] << std::endl;
          }
          std::cout << "checkTranspose****************************************************************************************************\n";
        }
*/
      }
      break;
    }
    case 1:
    {
      // copy L11-inverse data into holdTransposeInverse to be send/received
      this->holdTransposeL.clear();
      this->holdTransposeL.resize(matrixWindow*matrixWindow, 0.);
      int startOffset = ((dimXstart*(dimXstart+1))>>1);
      int index1 = 0;
      int index2 = startOffset + dimYstart;
      for (int i=0; i<matrixWindow; i++)
      {
        for (int j=0; j<matrixWindow; j++)
        {
          this->holdTransposeL[index1++] = this->matrixL[index2++];
        }
        index2 += (dimYstart+i+1);
      }

/*
      if ((this->gridCoords[0] == 0) && (this->gridCoords[1] == 0) && (this->gridCoords[2] == 0))
      {
        for (int i=0; i<this->holdTransposeL.size(); i++)
        {
          std::cout << this->holdTransposeL[i] << " ";
          if ((i+1)%matrixWindow == 0) {std::cout << std::endl; }
        }
      }
*/
      if ((this->gridCoords[0] != this->gridCoords[1]))
      {
        // perform MPI_SendRecv_replace
        MPI_Status stat;
        int destRank = -1;
        int rankArray[3] = {this->gridCoords[1], this->gridCoords[0], this->gridCoords[2]};
        MPI_Cart_rank(this->grid3D, &rankArray[0], &destRank);
        MPI_Sendrecv_replace(&this->holdTransposeL[0], this->holdTransposeL.size(), MPI_DOUBLE,
          destRank, 0, destRank, 0, MPI_COMM_WORLD, &stat);

        // anything else?
      }    
      break;
    }
  }
}


/*
  Base case of CholeskyRecurse
*/
template<typename T>
void solver<T>::CholeskyRecurseBaseCase(uint32_t dimXstart, uint32_t dimXend, uint32_t dimYstart, uint32_t dimYend, uint32_t matrixWindow, uint32_t matrixSize, uint32_t matrixCutSize)
{
  /*
	1) AllGather onto a single processor, which has to be chosen carefully
	2) Sequential BLAS-3 routines to solve for LU and inverses

	For now, I can just create a 1-d buffer of the data that I want to send first.
	Later, I can try to optimize this to avoid needless copying just for a collective

	Also remember the importance of using the corect communicator here. We need the sheet communicator of size P^{2/3}
	This avoids traffic and is the only way it could work

	I think that I will need to receive the data in a buffer, and then use another buffer to line up the data properly
	This is extra computational cost, but for the moment, I don't see any other option

	This data will be used to solve for L,U,L-I, and U-I. So it is very important that the input buffer be correct for BLAS-3
	Then we will work on communicating the correct data to the correct places so that RMM operates correctly.
 
	So below, I need to transfer a 2-d vector into a 1-d vector
  */

  std::vector<T> sendBuffer((matrixWindow*(matrixWindow+1))>>1,0.);

  /* Note that for now, we are accessing matrix A as a 1d square vector, NOT a triangular vector, so startOffset is not sufficient
  int startOffset = (dimXstart*(dimXstart+1))>>1;
  */

  int index1 = 0;
  int index2 = 0;
  for (int i=0; i<matrixWindow; i++)
  {
    for (int j=0; j<=i; j++)			// Note that because A is upper-triangular (shouldnt really matter), I must
						// access A differently than before
    {
      sendBuffer[index1++] = this->matrixA[this->matrixA.size()-1][index2 + j];//this->matrixA[index2+j];
//      if (this->worldRank == 0) {std::cout << "BC index - " << index2+j << " and val - " << sendBuffer[index1-1] << std::endl; }
    }
    //index2 += this->localSize;
    index2 += matrixCutSize;
  }

  std::vector<T> recvBuffer(sendBuffer.size()*this->processorGridDimSize*this->processorGridDimSize,0);
  MPI_Allgather(&sendBuffer[0], sendBuffer.size(),MPI_DOUBLE,&recvBuffer[0],sendBuffer.size(),MPI_DOUBLE,this->layerComm);


  /*
	After the all-gather, I need to format recvBuffer so that it works with OpenBLAS routines!
  
        Word of caution: How do I know the ordering of the blocks? It should be by processor rank within the communicator used, but that processor
	  rank has no correlation to the 2D gridCoords that were used to distribute the matrix.
  */


  // I still need to fix the below. The way an allgather returns data is in blocks
  int count = 0;
  sendBuffer.clear();				// This is new. Edgar says to use capacity??
  sendBuffer.resize(matrixSize*matrixSize,0.);
  for (int i=0; i<this->processorGridDimSize*this->processorGridDimSize; i++)  // MACRO loop over all processes' data (stored contiguously)
  {
    for (int j=0; j<matrixWindow; j++)
    {
      for (int k=0; k<=j; k++)		// I changed this. It should be correct.
      {
        int index = j*this->processorGridDimSize*matrixSize+(i/this->processorGridDimSize)*matrixSize + k*this->processorGridDimSize+(i%this->processorGridDimSize);  // remember that recvBuffer is stored as P^(2/3) consecutive blocks of matrix data pertaining to each p
        int xCheck = index/matrixSize;
        int yCheck = index%matrixSize;
        if (xCheck >= yCheck)
        {
          sendBuffer[index] = recvBuffer[count++];
        }
        else								// serious corner case
        {
          sendBuffer[index] = 0.;
          count++;
        }
      }
    }
  }

  // Cholesky factorization
  LAPACKE_dpotrf(LAPACK_ROW_MAJOR,'L',matrixSize,&sendBuffer[0],matrixSize);

  //recvBuffer.resize(sendBuffer.size(),0.);			// I am just re-using vectors. They change form as to what data they hold all the time
 
  recvBuffer = sendBuffer;

  // I am assuming that the diagonals are ok (maybe they arent all 1s, but that should be ok right?) 
  LAPACKE_dtrtri(LAPACK_ROW_MAJOR,'L','N',matrixSize,&recvBuffer[0],matrixSize);

  int pIndex = this->gridCoords[0]*this->processorGridDimSize+this->gridCoords[1];
  int startOffset = (dimXstart*(dimXstart+1))>>1;
  int rowCounter = 0;
  for (int i=0; i<matrixWindow; i++)
  {
    int temp = startOffset + (i+1)*dimYstart + rowCounter;
    for (int j=0; j<=i; j++)	// for matrixWindow==2, j should always go to 
    {
      // I need to use dimXstart and dimXend and dimYstart and dimYend ...
      int index = i*this->processorGridDimSize*matrixSize+(pIndex/this->processorGridDimSize)*matrixSize + j*this->processorGridDimSize+(pIndex%this->processorGridDimSize);
      this->matrixL[temp+j] = sendBuffer[index];
      this->matrixLInverse[temp+j] = recvBuffer[index];
    }
    rowCounter += (i+1);
  }
}

template<typename T>
void solver<T>::printL()
{
  // We only need to print out a single layer, due to replication of L on each layer
  if ((this->gridCoords[2] == 0) && (this->gridCoords[1] == 0) && (this->gridCoords[0] == 0))
  {
    int tracker = 0;
    for (int i=0; i<this->localSize; i++)
    {
      for (int j=0; j<this->localSize; j++)
      {
        if (i >= j)
        {
          std::cout << this->matrixL[tracker++] << " ";
        }
        else
        {
          std::cout << 0 << " ";
        }
      }
      std::cout << "\n";
    }
  }
}

template<typename T>
void solver<T>::lapackTest(std::vector<T> &data, std::vector<T> &dataL, std::vector<T> &dataInverse, uint32_t n)
{
  //std::vector<T> data(n*n); Assume that space has been allocated for data vector on the caller side.
  for (int i=0; i<n; i++)
  {
    for (int j=0; j<n; j++)
    {
      if (i > j)
      {
        srand(i*n+j);
      }
      else
      {
        srand(j*n+i);
      }
      data[i*n+j] = (rand()%100)*1./100.;
      //std::cout << "hoogie - " << i*n+j << " " << data[i*n+j] << std::endl;
      if (i==j)
      {
        data[i*n+j] += 10.;
      }
    }
  }
  
  #if DEBUGGING
  std::cout << "*************************************************************************************************************\n";
  for (int i=0; i<n; i++)
  {
    for (int j=0; j<n; j++)
    {
      std::cout << data[i*n+j] << " ";
    }
    std::cout << "\n";
  }
  std::cout << "*************************************************************************************************************\n";
  #endif

  dataL = data;			// big copy
  LAPACKE_dpotrf(LAPACK_ROW_MAJOR,'L',n,&dataL[0],n);
  dataInverse = dataL;				// expensive copy
  LAPACKE_dtrtri(LAPACK_ROW_MAJOR,'L','N',n,&dataInverse[0],n);

  #if DEBUGGING
  std::cout << "Cholesky Solution is below *************************************************************************************************************\n";

  for (int i=0; i<n; i++)
  {
    for (int j=0; j<n; j++)
    {
      std::cout << dataL[i*n+j] << " ";
    }
    std::cout << "\n";
  }
  #endif

  return;
}

template<typename T>
void solver<T>::solveScalapack()
{
/*

  // Scalapack Cholesky.
  vector<int> desc(9);
  int info,icontxt;
  char order = 'R';
  int npRow = 2;
  int npCol = 2;		// these two can change obviously
  int myCol = this->worldSize%npRow;
  int myRow = this->worldSize/npRow;
  BLACS_GET(0,0,&icontxt);
  BLACS_GRIDINIT(&icontxt,order,npRow,npCol);
  BLACS_GRIDINFO(icontxt,&npRow,&npCol,&myRow,&myCol);	// the last 4 arguments are apparently output arguments

  // Set up the descriptor vector
  desc[0] = 1;
  desc[1] = iscontxt;
  desc[2] = this->matrixDimSize;
  desc[3] = this->matrixDimSize;
  desc[4] = ;
  desc[5] = ;
  desc[6] = ;
  desc[7] = ;
  desc[8] = ;

  // Now I guess I would distribute the input matrix over the P processors.

  PDPOTRF('L',this->matrixDimSize,....,1,1,&desc[0],&info);

*/
}

template<typename T>
void solver<T>::compareSolutionsSequential()
{
  /*
	We want to perform a reduction on the data on one of the P^{1/3} layers, then call lapackTest with a single
	processor. Then we can, in the right order, compare these solutions for correctness.
  */

/*
  if ((this->gridCoords[0] == 0) && (this->gridCoords[1] == 1))
  {
    std::cout << "\n";
    for (int i=0; i<this->matrixL.size(); i++)
    {
      std::cout << this->matrixL[i] << std::endl;
    }
    std::cout << "\n\n";
  }
*/


  if (this->gridCoords[2] == 0)		// 1st layer
  {
    //std::vector<T> sendData(this->matrixDimSize * this->matrixDimSize);
    std::vector<T> recvData(this->processorGridDimSize*this->processorGridDimSize*this->matrixA[this->matrixA.size()-1].size());	// only the bottom half, remember?
    MPI_Gather(&this->matrixA[this->matrixA.size()-1][0],this->matrixA[this->matrixA.size()-1].size(), MPI_DOUBLE, &recvData[0],this->matrixA[this->matrixA.size()-1].size(), MPI_DOUBLE,
	0, this->layerComm);		// use 0 as root rank, as it needs to be the same for all calls
    std::vector<T> recvDataL(this->processorGridDimSize*this->processorGridDimSize*this->matrixL.size());	// only the bottom half, remember?
    MPI_Gather(&this->matrixL[0],this->matrixL.size(), MPI_DOUBLE, &recvDataL[0],this->matrixL.size(), MPI_DOUBLE,
	0, this->layerComm);		// use 0 as root rank, as it needs to be the same for all calls
    std::vector<T> recvDataLInverse(this->processorGridDimSize*this->processorGridDimSize*this->matrixLInverse.size());	// only the bottom half, remember?
    MPI_Gather(&this->matrixLInverse[0],this->matrixLInverse.size(), MPI_DOUBLE, &recvDataLInverse[0],this->matrixLInverse.size(), MPI_DOUBLE,
	0, this->layerComm);		// use 0 as root rank, as it needs to be the same for all calls

    if (this->layerCommRank == 0)
    {
      /*
	Now on this specific rank, we currently have all the algorithm data that we need to compare, but now we must call the sequential
        lapackTest method in order to get what we want to compare the Gathered data against.
      */
      std::vector<T> data(this->matrixDimSize*this->matrixDimSize);
      std::vector<T> lapackData(this->matrixDimSize*this->matrixDimSize);
      std::vector<T> lapackDataInverse(this->matrixDimSize*this->matrixDimSize);
      lapackTest(data, lapackData, lapackDataInverse, this->matrixDimSize);		// pass this vector in by reference and have it get filled up

      // Now we can start comparing this->matrixL with data
      // Lets just first start out by printing everything separately too the screen to make sure its correct up till here
      
      {
        int index = 0;
        std::vector<int> pCounters(this->processorGridDimSize*this->processorGridDimSize,0);		// start each off at zero
        std::cout << "\n";
        for (int i=0; i<this->matrixDimSize; i++)
        {
          for (int j=0; j<=i; j++)
	  {
            int PE = (j%this->processorGridDimSize) + (i%this->processorGridDimSize)*this->processorGridDimSize;
	    std::cout << lapackData[index] << " " << recvDataL[PE*this->matrixL.size() + pCounters[PE]] << " " << index << std::endl;
	    std::cout << lapackData[index] - recvDataL[PE*this->matrixL.size() + pCounters[PE]] << std::endl;
            double diff = lapackData[index++] - recvDataL[PE*this->matrixL.size() + pCounters[PE]++];
            diff = abs(diff);
            this->matrixLNorm += (diff*diff);
          }
          if (i%2==0)
          {
            pCounters[1]++;		// this is a serious edge case due to the way I handled the actual code
          }
          index = this->matrixDimSize*(i+1);			// try this. We skip the non lower triangular elements
          std::cout << std::endl;
        }
        this->matrixLNorm = sqrt(this->matrixLNorm);
      }
      {
        int index = 0;
        std::vector<int> pCounters(this->processorGridDimSize*this->processorGridDimSize,0);		// start each off at zero
        std::cout << "\n";
        for (int i=0; i<this->matrixDimSize; i++)
        {
          for (int j=0; j<this->matrixDimSize; j++)
	  {
            int PE = (j%this->processorGridDimSize) + (i%this->processorGridDimSize)*this->processorGridDimSize;
	    std::cout << data[index] << " " << recvData[PE*this->matrixA[this->matrixA.size()-1].size() + pCounters[PE]] << " " << index << std::endl;
	    std::cout << data[index] - recvData[PE*this->matrixA[matrixA.size()-1].size() + pCounters[PE]] << std::endl;
            double diff = data[index++] - recvData[PE*this->matrixA[this->matrixA.size()-1].size() + pCounters[PE]++];
            diff = abs(diff);
            this->matrixANorm += (diff*diff);
          }
//          if (i%2==0)
//          {
//            pCounters[1]++;		// this is a serious edge case due to the way I handled the actual code
//          }
          index = this->matrixDimSize*(i+1);			// try this. We skip the non lower triangular elements
          std::cout << std::endl;
        }
        this->matrixANorm = sqrt(this->matrixANorm);
      }
      {
        int index = 0;
        std::vector<int> pCounters(this->processorGridDimSize*this->processorGridDimSize,0);		// start each off at zero
        std::cout << "\n";
        for (int i=0; i<this->matrixDimSize; i++)
        {
          for (int j=0; j<=i; j++)
	  {
            int PE = (j%this->processorGridDimSize) + (i%this->processorGridDimSize)*this->processorGridDimSize;
	    std::cout << lapackDataInverse[index] << " " << recvDataLInverse[PE*this->matrixLInverse.size() + pCounters[PE]] << " " << index << std::endl;
	    std::cout << lapackDataInverse[index] - recvDataLInverse[PE*this->matrixLInverse.size() + pCounters[PE]] << std::endl;
            double diff = lapackDataInverse[index++] - recvDataLInverse[PE*this->matrixLInverse.size() + pCounters[PE]++];
            diff = abs(diff);
            this->matrixLInverseNorm += (diff*diff);
          }
          if (i%2==0)
          {
            pCounters[1]++;		// this is a serious edge case due to the way I handled the code
          }
          index = this->matrixDimSize*(i+1);			// try this. We skip the non lower triangular elements
          std::cout << std::endl;
        }
        this->matrixLInverseNorm = sqrt(this->matrixLInverseNorm);
      }
      
      std::cout << "matrix A Norm - " << this->matrixANorm << std::endl;
      std::cout << "matrix L Norm - " << this->matrixLNorm << std::endl;
      std::cout << "matrix L Inverse Norm - " << this->matrixLInverseNorm << std::endl;
    }
  }
}

template<typename T>
void solver<T>::getResidualSequential()
{
  // Should be similar to getResidualSequential
  // Only computes the residual on one layer, but all layers should be the same.
  if (this->gridCoords[2] == 0)		// 1st layer
  {
    //std::vector<T> sendData(this->matrixDimSize * this->matrixDimSize);
    std::vector<T> recvData(this->processorGridDimSize*this->processorGridDimSize*this->matrixA[this->matrixA.size()-1].size());	// only the bottom half, remember?
    MPI_Gather(&this->matrixA[this->matrixA.size()-1][0],this->matrixA[this->matrixA.size()-1].size(), MPI_DOUBLE, &recvData[0],this->matrixA[this->matrixA.size()-1].size(), MPI_DOUBLE,
	0, this->layerComm);		// use 0 as root rank, as it needs to be the same for all calls
    std::vector<T> recvDataL(this->processorGridDimSize*this->processorGridDimSize*this->matrixL.size());	// only the bottom half, remember?
    MPI_Gather(&this->matrixL[0],this->matrixL.size(), MPI_DOUBLE, &recvDataL[0],this->matrixL.size(), MPI_DOUBLE,
	0, this->layerComm);		// use 0 as root rank, as it needs to be the same for all calls
    std::vector<T> recvDataLInverse(this->processorGridDimSize*this->processorGridDimSize*this->matrixLInverse.size());	// only the bottom half, remember?
    MPI_Gather(&this->matrixLInverse[0],this->matrixLInverse.size(), MPI_DOUBLE, &recvDataLInverse[0],this->matrixLInverse.size(), MPI_DOUBLE,
	0, this->layerComm);		// use 0 as root rank, as it needs to be the same for all calls

    if (this->layerCommRank == 0)
    {
      /*
	Now on this specific rank, we currently have all the algorithm data that we need to compare, but now we must call the sequential
        lapackTest method in order to get what we want to compare the Gathered data against.
      */
      std::vector<T> data(this->matrixDimSize*this->matrixDimSize);
      std::vector<T> lapackData(this->matrixDimSize*this->matrixDimSize);
      std::vector<T> lapackDataInverse(this->matrixDimSize*this->matrixDimSize);
      lapackTest(data, lapackData, lapackDataInverse, this->matrixDimSize);		// pass this vector in by reference and have it get filled up

      // Now we can start comparing this->matrixL with data
      // Lets just first start out by printing everything separately too the screen to make sure its correct up till here
      
      {
        int index = 0;
        std::vector<int> pCounters(this->processorGridDimSize*this->processorGridDimSize,0);		// start each off at zero
        for (int i=0; i<this->matrixDimSize; i++)
        {
          for (int j=0; j<=i; j++)
	  {
            int PE = (j%this->processorGridDimSize) + (i%this->processorGridDimSize)*this->processorGridDimSize;
            double diff = lapackData[index++] - recvDataL[PE*this->matrixL.size() + pCounters[PE]++];
            diff = abs(diff);
            this->matrixLNorm += (diff*diff);
          }
          if (i%2==0)
          {
            pCounters[1]++;		// this is a serious edge case due to the way I handled the actual code
          }
          index = this->matrixDimSize*(i+1);			// try this. We skip the non lower triangular elements
        }
        this->matrixLNorm = sqrt(this->matrixLNorm);
      }
      {
        int index = 0;
        std::vector<int> pCounters(this->processorGridDimSize*this->processorGridDimSize,0);		// start each off at zero
        for (int i=0; i<this->matrixDimSize; i++)
        {
          for (int j=0; j<this->matrixDimSize; j++)
	  {
            int PE = (j%this->processorGridDimSize) + (i%this->processorGridDimSize)*this->processorGridDimSize;
            double diff = data[index++] - recvData[PE*this->matrixA[this->matrixA.size()-1].size() + pCounters[PE]++];
            diff = abs(diff);
            this->matrixANorm += (diff*diff);
          }
//          if (i%2==0)
//          {
//            pCounters[1]++;		// this is a serious edge case due to the way I handled the actual code
//          }
          index = this->matrixDimSize*(i+1);			// try this. We skip the non lower triangular elements
        }
        this->matrixANorm = sqrt(this->matrixANorm);
      }
      {
        int index = 0;
        std::vector<int> pCounters(this->processorGridDimSize*this->processorGridDimSize,0);		// start each off at zero
        for (int i=0; i<this->matrixDimSize; i++)
        {
          for (int j=0; j<=i; j++)
	  {
            int PE = (j%this->processorGridDimSize) + (i%this->processorGridDimSize)*this->processorGridDimSize;
            double diff = lapackDataInverse[index++] - recvDataLInverse[PE*this->matrixLInverse.size() + pCounters[PE]++];
            diff = abs(diff);
            this->matrixLInverseNorm += (diff*diff);
          }
          if (i%2==0)
          {
            pCounters[1]++;		// this is a serious edge case due to the way I handled the code
          }
          index = this->matrixDimSize*(i+1);			// try this. We skip the non lower triangular elements
        }
        this->matrixLInverseNorm = sqrt(this->matrixLInverseNorm);
      }
      
      std::cout << "matrix A Norm - " << this->matrixANorm << std::endl;
      std::cout << "matrix L Norm - " << this->matrixLNorm << std::endl;
      std::cout << "matrix L Inverse Norm - " << this->matrixLInverseNorm << std::endl;
    }
  }
}

template <typename T>
void solver<T>::getResidualParallel()
{
  // Waiting on scalapack support. No other way to this
}

template<typename T>
void solver<T>::printInputA()
{
  if (this->gridCoords[2] == 0)		// 1st layer
  {
    //std::vector<T> sendData(this->matrixDimSize * this->matrixDimSize);
    std::vector<T> recvData(this->processorGridDimSize*this->processorGridDimSize*this->matrixA[this->matrixA.size()-1].size());	// only the bottom half, remember?
    MPI_Gather(&this->matrixA[this->matrixA.size()-1][0],this->matrixA[this->matrixA.size()-1].size(), MPI_DOUBLE, &recvData[0],this->matrixA[this->matrixA.size()-1].size(), MPI_DOUBLE,
	0, this->layerComm);		// use 0 as root rank, as it needs to be the same for all calls

    if (this->layerCommRank == 0)
    {
      /*
	Now on this specific rank, we currently have all the algorithm data that we need to compare, but now we must call the sequential
        lapackTest method in order to get what we want to compare the Gathered data against.
      */
      
      for (int i=0; i<recvData.size(); i++)
      {
        if (i%this->matrixDimSize == 0)
        { std::cout << "\n"; }
        std::cout << recvData[i] << " ";
      }

      std::cout << "\n\n";

      std::vector<int> pCounters(this->processorGridDimSize*this->processorGridDimSize,0);		// start each off at zero
      for (int i=0; i<this->matrixDimSize; i++)
      {
        for (int j=0; j < this->matrixDimSize; j++)
	{
          int PE = (j%this->processorGridDimSize) + (i%this->processorGridDimSize)*this->processorGridDimSize;
          std::cout << recvData[PE*this->matrixA[this->matrixA.size()-1].size() + pCounters[PE]++] << " ";
        }
//        if (i%2==0)
//        {
//          pCounters[1]++;		// this is a serious edge case due to the way I handled the actual code
//        }
        std::cout << std::endl;
      }
    }
  }
}
