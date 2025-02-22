/* Author: Edward Hutter */

#ifndef MATRIX_H_
#define MATRIX_H_

// Local includes -- the policy classes
#include "structure.h"

template<typename ScalarType = double, typename DimensionType = int64_t, typename StructurePolicy = rect, typename OffloadPolicy = OffloadEachGemm>
class matrix : public StructurePolicy{
public:
  // Type traits (some inherited from matrixBase)
  using ScalarType = ScalarType;
  using DimensionType = DimensionType;
  using StructureType = StructurePolicy;
  using OffloadType = OffloadPolicy;

  explicit matrix(){this->filled=false; this->danger=true; this->_data=nullptr; this->_scratch=nullptr; this->_pad=nullptr;}// = delete;
  explicit matrix(DimensionType globalDimensionX, DimensionType globalDimensionY, int64_t globalPgridX, int64_t globalPgridY);	// Regular constructor
  // Injection constructor below assumes data is stored in column-major format
  explicit matrix(ScalarType* data, DimensionType dimensionX, DimensionType dimensionY, DimensionType globalDimensionX, DimensionType globalDimensionY, DimensionType globalPgridX, DimensionType globalPgridY);			// Injection constructor
  explicit matrix(ScalarType* data, DimensionType dimensionX, DimensionType dimensionY, DimensionType globalPgridX, DimensionType globalPgridY);			// Injection constructor
  explicit matrix(ScalarType* data, DimensionType dimensionX, DimensionType dimensionY, DimensionType globalPgridX, DimensionType globalPgridY, bool);			// Injection constructor
  matrix(const matrix& rhs);
  matrix(matrix&& rhs);
  matrix& operator=(const matrix& rhs);
  matrix& operator=(matrix&& rhs);
  ~matrix();
  // Special methods used for policy optimizations
  void _fill_();
  void _register_(DimensionType globalDimensionX, DimensionType globalDimensionY, int64_t globalPgridX, int64_t globalPgridY);
  void _destroy_();
  void _restrict_(DimensionType startX, DimensionType endX, DimensionType startY, DimensionType endY);
  void _derestrict_();

  // automatically inlined
  // returning an lvalue by virtue of its reference type -- note: this isnt the safest thing, but it provides better speed. 
  inline ScalarType*& data() { return this->_data; }
  inline ScalarType* data() const { return this->_data; }
  //inline ScalarType* get_data() { ScalarType* data = this->_data; this->_data=nullptr; return data; }	// only to be used if internal pointer is needed and instance is never to be used again
  inline ScalarType*& scratch() { return this->_scratch; }
  inline ScalarType* scratch() const { return this->_scratch; }
  inline ScalarType*& pad() { return this->_pad; }
  inline ScalarType* pad() const { return this->_pad; }
  inline DimensionType num_elems() const { return this->_numElems; }
  inline DimensionType num_elems(DimensionType rangeX, DimensionType rangeY) const { return _num_elems(rangeX, rangeY); }
  inline DimensionType num_rows_local() const { return this->_dimensionY; }
  inline DimensionType num_columns_local() const { return this->_dimensionX; }
  inline DimensionType num_rows_global() const { return this->_globalDimensionY; }
  inline DimensionType num_columns_global() const { return this->_globalDimensionX; }

  inline DimensionType offset_local(DimensionType coordX, DimensionType coordY, size_t buffer=0) const { return buffer != 2 ? _offset(coordX,coordY,this->_dimensionX,this->_dimensionY) : rect::_offset(coordX,coordY,this->_dimensionX,this->_dimensionY);}
  inline DimensionType offset_global(DimensionType coordX, DimensionType coordY) const { static_assert(0,"not implemented"); return -1;}//TODO

  inline void swap() { ScalarType* ptr = this->data(); this->data() = this->scratch(); this->scratch() = ptr; } 
  inline void swap_pad() { ScalarType* ptr = this->scratch(); this->scratch() = this->pad(); this->pad() = ptr; } 

  inline void set_num_rows_local(DimensionType arg) { this->_dimensionY = arg; }
  inline void set_num_columns_local(DimensionType arg) { this->_dimensionX = arg; }
  inline void set_num_rows_global(DimensionType arg) { this->_globalDimensionY = arg; }
  inline void set_num_columns_global(DimensionType arg) { this->_globalDimensionX = arg; }
  inline void set_num_elems(DimensionType arg) { this->_numElems = arg; }

  // dim.first -- column index (X) , dim.second -- row index (Y)
  void distribute_random(int64_t localPgridX, int64_t localPgridY, int64_t globalPgridX, int64_t globalPgridY, int64_t key);
  void distribute_symmetric(int64_t localPgridX, int64_t localPgridY, int64_t globalPgridX, int64_t globalPgridY, int64_t key, bool diagonallyDominant);
  void distribute_identity(int64_t localPgridX, int64_t localPgridY, int64_t globalPgridX, int64_t globalPgridY, ScalarType val=1.);
  void distribute_debug(int64_t localPgridX, int64_t localPgridY, int64_t globalPgridX, int64_t globalPgridY);
  void print() const;
  void print_data() const;
  void print_scratch() const;
  void print_pad() const;

private:
  void copy(const matrix& rhs);
  void mover(matrix&& rhs);

  ScalarType* _data;				// Where the matrix data lives as a contiguous 1d array
  ScalarType* _scratch;				// Extra storage for summa and other computations that require one2all and all2one communications
  ScalarType* _pad;				// Extra storage for uppertri and lowertri structures only used in avoiding extra allocations in summa
  bool allocated_data;				// Asks if the raw data was allocated by the user or ourselves
  bool filled;					// Tracks whether the matrix instance has been filled with data in the 2-part construction
  bool danger;					// notifies me if default constructor was used.

  DimensionType _numElems;			// Number of elements in matrix
  DimensionType _dimensionX;			// Number of columns owned locally
  DimensionType _dimensionY;			// Number of rows owned locally
  DimensionType _globalDimensionX;		// Number of columns in global matrix
  DimensionType _globalDimensionY;		// Number of rows in global matrix

  // Special members for _restrict_ and _destrict_ methods
  ScalarType* _data_;
  ScalarType* _scratch_;
  DimensionType _numElems_;			// Number of elements in matrix
  DimensionType _dimensionX_;			// Number of columns owned locally
  DimensionType _dimensionY_;			// Number of rows owned locally
};

#include "matrix.hpp"

#endif /* MATRIX_H_ */
