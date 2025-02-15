// See LICENSE_CELLO file for license and copyright information

/// @file     array_CelloArray.hpp
/// @author   Matthew Abruzzo (matthewabruzzo@gmail.com)
/// @date     Thurs May 30 2019
/// @brief    Declaration and implementation of the CelloArray class template

#include <stdio.h>
#include <cstddef>
#include <type_traits>
#include <limits>
#include <memory>

#include "cello_defines.hpp"

#ifndef ARRAY_CELLO_ARRAY_HPP
#define ARRAY_CELLO_ARRAY_HPP

//----------------------------------------------------------------------

// CelloArray
// Like arrays in Athena++, the indices are listed in order of increasing
//   access speed. Imagine a 3D array with shape {mz,my,mx} array(k,j,i) is
//   equivalent to accessing index ((k*my + j)*mx + i) of the pointer
// Dimensions are numbered with increasing indexing speed (dim0, dim1, ...)
// See the online documentation for a description of how to use CelloArray with
//   examples

//----------------------------------------------------------------------
/// @typedef intp
/// @brief  Array indexing type
///
/// This convention is borrowed from numpy. We set it the larger precision 
/// integer type: int OR ptrdiff_t to guarantee int promotion
/// (Alternatively, we could define it as int)

typedef std::conditional<sizeof(int) >= sizeof(std::ptrdiff_t),
			   int, std::ptrdiff_t>::type intp;

//----------------------------------------------------------------------
/// @def    ARRAY_SIZE_MAX
/// @brief  Maximum number of array elements
///
/// If we forced intp to always be int, then we'd use min(INT_MAX,PTRDIFF_MAX)

#define ARRAY_SIZE_MAX PTRDIFF_MAX

//----------------------------------------------------------------------

// To allow the CSlice constructor to accept accept NULL, nullptr, and
// arguments of type intp, we need to accept template arguments and perform a
// a typecheck (there are ambiguities associated simple function overloading)
// The following macro that checks if an argument type is allowed: 
#define IS_ALLOWED_SLICE_ARGUMENT_TYPE(T)                                    \
  (std::is_same<T, std::nullptr_t>::value ||                                 \
   (std::is_integral<T>::value && std::is_signed<T>::value                   \
    && sizeof(T)<=sizeof(intp)))

class CSlice
{
  /// @class    CSlice
  /// @ingroup  Array
  /// @brief    [\ref Array] represents a slice along a single axis of a
  ///           CelloArray

public:

  /// Constructor for CSlice
  ///
  /// To specify a specify a slice including indices from start through stop-1
  /// call CSlice(start, stop). Negative indexing is also supported.
  ///
  /// To indicate that the slice should extend to the end of the axis, use:
  /// - CSlice(start, NULL) OR CSlice(start, nullptr)
  /// To indicate that the slice should extend from the start of the axis to
  /// some known stop index, use:
  /// - CSlice(0, stop) OR CSlice(NULL, stop) OR CSlice(nullptr, stop)
  /// To indicate that the slice should extend over the full axis, use:
  /// - CSlice(0, NULL), CSlice(NULL, NULL), CSlice(0, nullptr) OR
  ///   CSlice(0, nullptr)
  ///
  /// 2 important usage notes:
  /// 1. Due to the implementation defined nature of NULL, 0 can be used in
  ///    place of NULL or nullptr. However the convention is to avoid this
  /// 2. The class is equipped with a default constructor to allow for
  ///    construction of arrays of CSlice. However, all default constructed
  ///    instances MUST be assigned the value of non-default constructed
  ///    instances before passing them to CelloArray.subarray (This is done to
  ///    help catch mistakes)
  template<class T1, class T2>
  CSlice(T1 start, T2 stop)
  {
    // Check argument types (this could be done with std::enable_if in template
    // declaration, but it would errors harder to understand)
    static_assert(IS_ALLOWED_SLICE_ARGUMENT_TYPE(T1),
		  "The first argument of CSlice must be NULL, nullptr, or be "
		  "of a signed integral type that promotes to intp (which is "
		  "the larger of int and std::ptrdiff_t)");
    static_assert(IS_ALLOWED_SLICE_ARGUMENT_TYPE(T2),
		  "The second argument of CSlice must be NULL, nullptr, or be "
		  "of a signed integral type that promotes to intp (which is "
		  "the larger of int and std::ptrdiff_t)");

    start_ = (std::is_same<T1, std::nullptr_t>::value) ? 0 : (intp)start;
    stop_  = (std::is_same<T2, std::nullptr_t>::value) ? 0 : (intp)stop;

    if ((start_ >= 0 && stop_ > 0) || (start_ < 0 && stop_ < 0)){
      // The following will only possibly raise an error if start and stop are
      // integers of the the same sign
      ASSERT("CSlice", "start must be less than stop.", stop_ > start_);
    }
    initialized_ = (true);
  }

  /// Default constructor. This only exists to exists to allow for arrays of
  /// slices. All instances in the array must be assigned a non-default
  /// constructed value before use
  CSlice() : start_(0), stop_(0), initialized_(false) {}

  // Gets the start index of the slice
  intp get_start() const
  {
    ASSERT("CSlice", ("Default constructed CSlices cannot be used without "
		      "explicit assignment of values."), initialized_);
    return start_;
  }

  /// Returns the stop index of the slice. If the stop index should be the
  /// length of sliced axis, 0 is returned.
  intp get_stop() const
  {
    ASSERT("CSlice", ("Default constructed CSlices cannot be used without "
		      "explicit assignment of values."), initialized_);
    return stop_;
  }
    
private:
  /// start index
  intp start_;
  /// stop index (Should be 0 if the full length if the dimension will be used)
  intp stop_;

  /// true if constructed with non-default constructor OR if assigned values
  /// from a non-default constructed instance.
  bool initialized_;
};

//----------------------------------------------------------------------

/// This serves as the terminating function call in the tail recursion to check
/// the validity of the indicies passed to CelloArray
///
/// @param shape pointer to the final element in the cstyle array holding the
///     shape of the array
/// @param first The final index to check the shape of
///
/// @tparam T the type of the index (should be int or intp) 
template<typename T>
bool check_bounds_(intp *shape, T first) {return *shape > first;}

/// a helper function template that helps check the validity of indices passed
/// to CelloArray if debugger mode for checking indices has been enabled
///
/// @param shape pointer to the current element of the cstyle array holding the
///     shape of the array. The number of elements in shape, after the first,
///     is the same as the number of indices in rest
/// @param first the current index that we are checking the shape of
/// @param rest the remaining indices that we need to check the name of
///
/// @tparam T the type of the first index (should be int or intp)
/// @tparam Rest... the types of the remaining indices. These should all match
///     T (although this would be checked before calling this function)
///
/// This uses tail recursion (so that iteration is unrolled at compile time).
/// Basically the first index is peeled off from the parameter pack of indices
/// the first element of shape is peeled off. They are compared to check the
/// validity of the index and then the remaining indices and rest of the shape
/// are recursively passed to this function again.
template<typename T, typename... Rest>
bool check_bounds_(intp *shape, T first, Rest... rest){
  return (*shape > first) && check_bounds_(shape+1, rest...);
}

// if CHECK_BOUNDS has been defined, the CHECK_BOUND3D and CHECK_BOUNDND are
// defined to actually check the validity of indices passed to the array
#ifdef CHECK_BOUNDS
#  define CHECK_BOUND3D(shape, k, j, i)                                       \
  ASSERT("CelloArray","Invalid index", check_bounds_(shape,k,j,i));
#  define CHECK_BOUNDND(shape, ARGS)                                          \
  ASSERT("CelloArray","Invalid index", check_bounds_(shape, ARGS...));
#else
#  define CHECK_BOUND3D(shape, k, j, i)   /* ... */
#  define CHECK_BOUNDND(shape, ARGS)      /* ... */
#endif

//----------------------------------------------------------------------

/// Helper function template to be used for debugging to check if a floating
/// point element contained by an array is finite (not a NaN or inf). If the
/// element is not a floating point, then it is assumed to be finite
template<typename T>
bool check_if_finite_(const T elem){
  return (std::is_floating_point<T>::value) ? std::isfinite(elem) : true;
}


// Defining the macro CHECK_FINITE_ELEMENTS returns an error everytime a NaN or
// inf gets retrieved from the array or an array is initialized by wrapping an
// existing pointer holding a NaN or inf
#ifdef CHECK_FINITE_ELEMENTS
#  define CHECK_IF_FINITE(value)                                              \
  ASSERT("CelloArray","Non-Finite Value", check_if_finite_(value));
#  define CHECK_IF_ARRAY_FINITE(value)    this->assert_all_entries_finite_();
#else
#  define CHECK_IF_FINITE(value)          /* ... */
#  define CHECK_IF_ARRAY_FINITE(value)    /* ... */
#endif

//----------------------------------------------------------------------

// To define CelloArray to have arbitrary dimensions it needs to accept a
// variable number of arguments to indicate shape during construction, to
// produce a subarray, and to access elements. The number of dimensions of the
// array as a template argument and accept values with variadic template
// arguments to check that the appropriate the number of values are specified
// at compile time. In each case, we need to guarantee that all arguments
// are a given type. The solution is based on:
//   - https://stackoverflow.com/a/28253503
//   - https://stackoverflow.com/a/31767710
template <bool...> struct bool_pack;
template <bool... vals> using all_true = std::is_same<bool_pack<true, vals...>,
						      bool_pack<vals..., true>>;

/// @def      REQUIRE_TYPE
/// @brief    Macro that should be included as an item in a template parameter
///           list of function template. This only allows the function template
///           to be defined if all variadic template arguments (passed as first
///           argument) all have a specific type (passed as the second argument)
#define REQUIRE_TYPE(T,type1)                                                \
  class = std::enable_if<all_true<(std::is_same<T,type1>::value)...>::value>
#define REQUIRE_TYPE2(T,type1,type2)					     \
  class = std::enable_if<all_true<(std::is_same<T,type1>::value ||           \
				   std::is_same<T,type2>::value)...>::value>

/// @def      REQUIRE_INT
/// @brief    Macro that should be included as an item in a template parameter
///           list of function template. This only allows the function template
///           to be defined if all variadic template arguments (passed as the
///           macro argument) are all a single type and that type is either int
///           or intp (which may be the same based on the system)
#define REQUIRE_INT(T) REQUIRE_TYPE2(T,intp,int)

//----------------------------------------------------------------------

/// This serves as the terminal tail recursion function call used to convert
/// multi-dimensional indices to a single index of the underlying pointer
/// wrapped by CelloArray
template<typename T>
intp calc_index_(const intp* stride, T first, T last){
  // last element in stride is alway 1
  return (*stride)*first + last;
}

/// This is a function overload for calc_index that handles the edge case where
/// the array is 1 dimensional
template<typename T>
intp calc_index_(const intp* stride, T first){return first;}

/// a helper function template that helps convert multi-dimensional indices to
/// a single index (or address) of the appropriate element in the underlying
/// pointer wrapped by CelloArray
///
/// @param stride pointer to the current element of the cstyle array holding the
///     strides for each array dimension. The number of elements in stride_,
///     after the first, is the same as the number of indices in rest.
///     (Additionally, the final value is always 1)
/// @param first the current multidimensional-index that we are including
/// @param rest the remaining indices that we need to include
///
/// @tparam T the type of the first index (should be int or intp)
/// @tparam Rest... the types of the remaining indices. These should all match
///     T (although this would be checked before calling this function)
///
/// This uses tail recursion (so that iteration is unrolled at compile time).
/// If you imagine the array of strides and the parameter pack of all
/// multidimensional indices as mathematical vectors, we are essentially
/// returning the dot product of the vectors.
template<typename T, typename... Rest>
intp calc_index_(const intp* stride, T first, Rest... rest){
  // gets unrolled at compile time
  return (*stride)*first + calc_index_(stride+1, rest...);
}

//----------------------------------------------------------------------

/// This is an overload for calc_index_ to be used when the multidimensional
/// indices are specified as a cstyle array rather than as variadic template
/// argument arguments (it is useful for dynamically iterating over arrays)
///
/// @param D the number of dimensions of the array
/// @param offset the constant offset of the start of the pointer holding the
///     block of memory holding the array and the first element in the array.
///     (If the array is the same size as the allocated memory block, this is
///     0, but if the array is a subarray, this will be a positive number. 
/// @param stride array of strides for each dimension. In other words, the ith
///     element indicates the offset in the memory address associated with
///     incrementing the ith element by 1. Element D-1 is always 1.
/// @param indices array of multidimensional indices of length D.
///
/// @tparam the type of the indices (should be int or intp)
template<typename T>
intp calc_index_(const std::size_t D, const intp offset,
		 const intp* stride, const T* indices){
  std::size_t out = offset + indices[D-1];
  for (std::size_t i = 0; i+1<D;i++){
    out += stride[i]*indices[i];
  }
  return out;
}

//----------------------------------------------------------------------

/// Increment the "outer" indices of an array. This is used to help dynamically
/// iterate over an array
///
/// @param D the number of dimensions of the array
/// @param indices array of multidimensional indices of length D that is to be
///     modified. The D-2 index is incremented by 1 and if the result is equal
///     to shape[D-2], the D-2 index is reset to 0 and a value of 1 is carried
///     to index[D-3]. The process is repeated (if applicable) until the
///     index[0] is equal to shape[0] (at which point the return value is
///     modified)
/// @param shape the length of each dimension of the array
///
/// @return Returns false if the increment caused the indices[0] to be equal to
///     to shape[0]. Otherwise returns true. This is used to help signal when
///     to stop dynamically iterating over an array.
static inline bool increment_outer_indices_(std::size_t D, intp *indices,
                                            const intp *shape){
  std::size_t i = D-1;
  ASSERT("increment_out_indices", "the dimension must be positive",
         D>0);
  while (0 != (i--)){
    indices[i]+=1;
    if (indices[i] != shape[i]){
      return true;
    } else if (i > 0){
      indices[i] = 0;
    }
  }
  return false;
}

//----------------------------------------------------------------------

template<typename T, std::size_t D>
class CelloArray
{
  /// @class    CelloArray
  /// @ingroup  Array
  /// @brief    [\ref Array] class template that encapsulates a
  ///           multidimensional numeric array with a fixed number of
  ///           dimensions.
  ///
  /// The semantics of this template class resemble those of numpy arrays and
  /// pointers instead of those c++ standard library containers (like vectors).
  /// The class effectively acts as an address to the underlying data. The
  /// copy constructor and copy assignment operation effectively make shallow
  /// copies and deepcopies must be explicitly created. Note that a consequnce
  /// of this behavior is that when instances are passed to functions by value,
  /// any modifications to the array within the function will be reflected
  /// everywhere.

public: // interface

  typedef T value_type;
  typedef typename std::add_const<T>::type const_value_type;
  typedef typename std::remove_const<T>::type nonconst_value_type;

  friend class CelloArray<const_value_type,D>;
  friend class CelloArray<T,D+1>;

  /// Default constructor. Constructs an uninitialized CelloArray.
  CelloArray()
    : shared_data_(),
      offset_(0),
      shape_(),
      stride_()
  { };

  /// Construct a multidimensional numeric array that allocates its own data
  /// (where the data is freed once no arrays reference it anymore)
  ///
  /// @param args the lengths of each dimension. There must by D values and
  ///     they must all have the same type - int or intp
  template<typename... Args, REQUIRE_INT(Args)>
  CelloArray(Args... args);

  /// Construct a multidimensional numeric array that wraps an existing pointer
  ///
  /// @param array The pointer to the existing array data
  /// @param args the lengths of each dimension. There must by D values and
  ///     they must all have the same type - int or intp
  ///
  /// @note Note that instances of CelloArray that wrap existing pointers are
  ///     inherently less safe. Segmentation faults can more easily arise due
  ///     incorrect shapes being specified at this constructor and due to the
  ///     memory of the wrapped array being freed
  template<typename... Args, REQUIRE_INT(Args)>
  CelloArray(T* array, Args... args);

  /// conversion constructor that facilitates implicit casts from
  /// CelloArray<nonconst_value_type,D> to CelloArray<const_value_type,D>
  ///
  /// @note
  /// This is only defined for instances of CelloArray for which T is const-
  /// qualified. If it were defined in cases where T is not const-qualified,
  /// then it would duplicate the copy-constructor.
  template<class = std::enable_if<std::is_same<T, const_value_type>::value>>
  CelloArray(const CelloArray<nonconst_value_type, D> &other) : CelloArray() {
    std::shared_ptr<const_value_type> other_shared_data
      = std::const_pointer_cast<const_value_type>(other.shared_data_);
    this->shallow_copy_init_helper_(other_shared_data, other.offset_,
                                    other.shape_, other.stride_);
  }

  // The following is defined to give nice error messages for invalid
  // casts/copies. Without the following, the CelloArray(Args... args)
  // constructor gets invoked and the errors are hard to understand
  template<typename oT, std::size_t oD,
	   class = std::enable_if<!std::is_same<T, const_value_type>::value ||
	                          !std::is_same<T, oT>::value || oD != D>>
  CelloArray(const CelloArray<oT, oD> &other) : CelloArray() {
    static_assert(!std::is_same<oT, const_value_type>::value,
		  "can't cast CelloArray<const T,D> to CelloArray<T,D>");
    static_assert(!std::is_same<oT, T>::value,
		  "incompatible types for cast/copy");
    static_assert(D == oD, "number of array dimensions must be equal");
  }

  /// Copy constructor. Makes *this a shallow copy of other.
  ///
  /// @note The fact that this accepts const reference reflects the fact that
  ///     CelloArray has pointer-like semantics
  CelloArray(const CelloArray<T,D>& other) : CelloArray() {
    this->shallow_copy_init_helper_(other.shared_data_, other.offset_,
                                    other.shape_, other.stride_);
  }

  /// Move constructor. Constructs the array with the contents of other
  CelloArray(CelloArray<T,D>&& other) : CelloArray() {swap(*this,other);}

  // Destructor
  ~CelloArray() = default;

  /// Copy assignment operator. Makes *this a shallow copy of other.
  ///
  /// (The contents of any previously created shallow copies or subarrays of
  /// *this are unaffected by this method)
  CelloArray<T,D>& operator=(const CelloArray<T,D>& other){
    this->shallow_copy_init_helper_(other.shared_data_, other.offset_,
                                    other.shape_, other.stride_);
    return *this;
  }

  /// Move assignment operator. Replaces the contents of *this with those of
  /// other.
  ///
  /// (Contents of any previously created shallow copies or subarrays of
  /// *this are unaffected)
  CelloArray<T,D>& operator=(CelloArray<T,D>&& other) {
    swap(*this,other);
    return *this;
  }

  /// access array Elements.
  ///
  /// @param args The indices for each dimension of the array. The number of
  ///     provided indices must match the number of array dimensions, D.
  template<typename... Args, REQUIRE_INT(Args)>
  FORCE_INLINE T& operator() (Args... args) const noexcept {
    static_assert(D==sizeof...(args),
		  "Number of indices don't match number of dimensions");
    CHECK_BOUNDND(shape_, args)
    CHECK_IF_FINITE(shared_data_.get()[offset_ + calc_index_(stride_,args...)]);
    return shared_data_.get()[offset_ + calc_index_(stride_,args...)];
  }

  // Specialized implementation for 3D arrays (to reduce compile time)
  FORCE_INLINE T& operator() (const int k, const int j, const int i) const
    noexcept {
    static_assert(D==3, "3 indices should only be specified for 3D arrays");
    CHECK_BOUND3D(shape_, k, j, i)
    CHECK_IF_FINITE(shared_data_.get()[offset_ + k*stride_[0] + j*stride_[1] + i])
    return shared_data_.get()[offset_ + k*stride_[0] + j*stride_[1] + i];
  }

  /// Return a subarray with the same number of dimensions, D.
  ///
  /// @param args Instances of CSlice for each array dimension. If no arguments
  ///    are provided then a shallowcopy of the array is returned. Otherwise,
  ///    the same number of arguments must be provided as there are dimensions
  ///    of the array.
  ///
  /// For a 3D CelloArray, the function declaration might look like:
  /// CelloArray<T,3> subarray(CSlice k_slice, CSlice j_slice, CSlice i_slice);
  template<typename... Args, REQUIRE_TYPE(Args,CSlice)>
  CelloArray<T,D> subarray(Args... args) const noexcept;

  /// Return a subarray with one fewer dimensions
  template<class = std::enable_if< D>=2 >>
  CelloArray<T,D-1> subarray(int i) const noexcept;

  /// Returns the length of a given dimension
  ///
  /// @param dim Indicates the dimension for which we want the shape
  int shape(unsigned int dim) const noexcept{
    ASSERT1("CelloArray::shape", "%ui is greater than the number of dimensions",
	    dim, dim<D);
    return (int)shape_[dim];
  }

  /// Returns the total number of elements held by the array
  intp size() const noexcept{
    intp out = 1;
    for (std::size_t i=0; i<D; i++){
      out*=shape_[i];
    }
    return out;
  }

  /// Returns the stride for a given dimension
  int stride(unsigned int dim) const noexcept{
    ASSERT1("CelloArray::stride",
            "%ui is greater than the number of dimensions",
	    dim, dim<D);
    return (int)stride_[dim];
  }

  /// Returns the number of dimensions
  constexpr std::size_t rank() const noexcept {return D;}

  /// Swaps the contents of this array with the contents of a different array.
  /// This is only defined for arrays with the same numbers of dimensions D
  friend void swap(CelloArray<T,D> &first, CelloArray<T,D> &second){
    std::swap(first.shared_data_, second.shared_data_);
    std::swap(first.offset_, second.offset_);
    std::swap(first.shape_, second.shape_);
    std::swap(first.stride_, second.stride_);
  }

  /// Returns pointer to the underlying data
  T* data() const noexcept {
    T* ptr = shared_data_.get();
    return (ptr == nullptr) ? nullptr : ptr + offset_;
  }

  /// Returns whether the CelloArray wraps a nullptr (i.e. it's unitialized)
  bool is_null() const noexcept{
    // it's *technically* possible for a std::shared_ptr to be empty and store
    // a non-nullptr. I'm fairly confident that this only happens if you do
    // something like the following:
    // std::shared_ptr<char> empty();
    // int* pInt = new int(63);
    // std::shared_ptr<int> pShared(empty, pInt);
    // Since we never anything quite like this, we can assume that when
    // shared_data_ is empty, it holds a nullptr AND vice-versa
    return shared_data_.get() == nullptr;
  }

  /// Produce a deepcopy of the array.
  CelloArray<T,D> deepcopy() const noexcept;

  /// Returns whether CelloArray is a perfect alias of other.
  ///
  /// Returns False if there is just partial overlap, either array is
  /// uninitialized, or if the number of dimensions of the arrays differs.
  template<typename oT, std::size_t oD>
  bool is_alias(const CelloArray<oT,oD>& other) const noexcept;

  /// Copy elements from the current array to ``dest``. Both arrays must have
  /// the same shape.
  ///
  /// @param dest The array where elements are copied into.
  ///
  /// @note It might be better to make this a standalone function.
  /// @note This could be optimized
  void copy_to(const CelloArray<T,D>& other) const noexcept;

protected:

  /// Assists with the initialization of the CelloArray instances
  void init_helper_(const std::shared_ptr<T> &shared_data,
		    const intp shape_arr[D], const intp offset,
                    bool require_valid_data = true);

  /// helps initialize CelloArray instances by shallow copy
  void shallow_copy_init_helper_(const std::shared_ptr<T> &shared_data_o,
                                 intp offset_o, const intp shape_o[D],
                                 const intp stride_o[D]) {
    // if `*this` wasn't initialized, we won't require `shared_data` to be a
    // non-empty/non-null pointer. This let's us write code where we might
    // conditionally initialize a ``const CelloArray`` using a ternary operator
    // (in the null-case, we would need to copy a default constructed array).
    bool require_valid_data = !(this->is_null());
    init_helper_(shared_data_o, shape_o, offset_o, require_valid_data);

    // stride_ is copied from other, not initialized from shape_, in order to
    // appropriately handle cases where other is a subarray
    for (std::size_t i = 0; i<D; i++){ stride_[i] = stride_o[i]; }
  }

  /// this method is provided to assist with the optional debugging mode that
  /// checks if provided wrapped arrays contain NaNs or infs
  ///
  /// this is implicitly called, so it is not made available to users
  void assert_all_entries_finite_() const noexcept;

private: // attributes

  /// shared pointer to data
  std::shared_ptr<T> shared_data_;

  /// offset of the address of the first array element from the address of the
  /// start of the underlying pointer
  intp offset_;

  /// lists the length of each dimension, ordered with increasing indexing speed
  intp shape_[D];

  /// Provides the stride for each dimension. For a given dimension, a stride
  /// quanitifies the offset in the address of an element caused by
  /// incrementing the dimension's index. The last index is always 1
  intp stride_[D];

};

//----------------------------------------------------------------------

template<typename T, std::size_t D>
void CelloArray<T,D>::init_helper_(const std::shared_ptr<T> &shared_data,
                                   const intp shape_arr[D], const intp offset,
                                   bool require_valid_data /* = true*/){
  if (require_valid_data & ((shared_data.get() == nullptr) ||
                            (shared_data.use_count() == 0)   )){
    ERROR("CelloArray::init_helper_",
          "shared_data must not hold a NULL pointer or be empty. The "
          "current array is probably being moved/copied from an "
          "uninitialized array.");
  }
  shared_data_ = shared_data;
  offset_ = offset;

  std::size_t i = D;
  while (i>0){
    --i;
    shape_[i] = shape_arr[i];
    if (i + 1 == D){
      stride_[i] = 1;
    } else {
      stride_[i] = shape_[i+1] * stride_[i+1];
    }
  }
}

//----------------------------------------------------------------------

/// check the validity of the array shape is allowed (all elements are positive
/// and not too big)
inline void check_array_shape_(intp shape[], std::size_t D)
{
  ASSERT("check_array_shape_", "Positive dimensions are required.", shape[0]>0);
  ASSERT1("check_array_shape_", "The array cannot exceed %ld elements.",
	  (long)ARRAY_SIZE_MAX, (ARRAY_SIZE_MAX / shape[0]) >= 1);
  intp cur_size = 1;

  for (std::size_t i = 0; i+1 < D; i++){
    cur_size *= shape[i];
    ASSERT("check_array_shape_", "Positive dimensions are required.",
           shape[i+1]>0);
    ASSERT1("check_array_shape_", "The array cannot exceed %ld elements.",
	    (long)ARRAY_SIZE_MAX, (ARRAY_SIZE_MAX / shape[i+1]) >= cur_size);
  }
}

//----------------------------------------------------------------------

// Constructor of CelloArray by allocating new data
template<typename T, std::size_t D>
template<typename... Args, class>
CelloArray<T,D>::CelloArray(Args... args)
{
  static_assert(D==sizeof...(args), "Incorrect number of dimensions");
  intp shape[D] = {((intp)args)...};
  check_array_shape_(shape, D);

  intp size = 1;
  for (std::size_t i=0; i < D; i++){
    size *= shape[i];
  }

  std::shared_ptr<T> shared_data(new T[size](), // allocate & set entries to 0
                                 std::default_delete<T[]>());
  init_helper_(shared_data, shape, 0);
}

//----------------------------------------------------------------------

// Constructor of array that wraps an existing c-style array
template<typename T, std::size_t D>
template<typename... Args, class>
CelloArray<T,D>::CelloArray(T* array, Args... args)
{
  static_assert(D==sizeof...(args), "Incorrect number of dimensions");
  intp shape[D] = {((intp)args)...};
  check_array_shape_(shape, D);

  // specify a dummy deleter functor so that the array is not actually deleted
  // when reference counts go to zero
  std::shared_ptr<T> shared_data(array, [](T* p) { /* dummy deleter */ });
  init_helper_(shared_data, shape, 0);
  CHECK_IF_ARRAY_FINITE(value)
}

//----------------------------------------------------------------------

/// Helper function that checks that the provided slices are valid and prepares
/// an array of slices that indicate the absolute start and stop values of the
/// slice along each dimension
///
/// The latter effect is necessary since slices can include negative indices or
/// extend to the end of a dimension without knowledge of an array shape
inline void prep_slices_(const CSlice* slices, const intp shape[],
			 const std::size_t D, CSlice* out_slices)
{
   for (std::size_t i=0; i<D; i++){
     intp start = slices[i].get_start();
     if (start < 0){
       start += shape[i]; // start was negative
     }

     intp stop = slices[i].get_stop();
     if (stop <= 0){
       // includes negative values of stop and case when it's equal to zero
       // (which means that the slice should stop at shape[i])
       stop += shape[i];
     }

     ASSERT3("prep_slices_",
	     "slice start of %ld doesn't lie in bound of dim %ld of size %ld.",
	     (long)slices[i].get_start(), (long)i, (long)shape[i],
	     start < shape[i]);
     ASSERT3("prep_slices_",
	     "slice stop of %d doesn't lie in bound of dim %ld of size %ld.",
	     (long)slices[i].get_stop(), (long)i, (long)shape[i],
	     stop <= shape[i]);
     ASSERT4("prep_slices_", ("slice stop (%ld) doesn't exceed slice start "
                              "(%ld) must for dim %ld of size %ld."),
	     (long)slices[i].get_start(), (long)slices[i].get_stop(), (long)i,
	     (long)shape[i], stop>start);
     out_slices[i] = CSlice(start,stop);
  }
}

//----------------------------------------------------------------------

// Returnd CelloArray representing a view of a subarray of the current instance
template<typename T, std::size_t D>
template<typename... Args, class>
CelloArray<T,D> CelloArray<T,D>::subarray(Args... args) const noexcept{
  static_assert(D == sizeof...(args),
		"Number of slices don't match number of dimensions");
  CelloArray<T,D> subarray;
  // the dummy value at the end of input_slices keeps the compiler from
  // freaking out when no arguments are passed
  CSlice input_slices[1+sizeof...(args)] = {args...,CSlice()};
  CSlice slices[D];
  prep_slices_(input_slices, shape_, D, slices);

  intp new_shape[D];
  intp new_offset = offset_;
  for (std::size_t dim=0; dim<D; dim++){
    new_shape[dim] = slices[dim].get_stop() - slices[dim].get_start();
    new_offset += slices[dim].get_start() * stride_[dim];
  }

  subarray.init_helper_(shared_data_,new_shape,new_offset);
  for (std::size_t dim=0; dim<D; dim++){
    subarray.stride_[dim] = stride_[dim];
  }
  return subarray;
}

//----------------------------------------------------------------------

template<typename T, std::size_t D>
template<class>
CelloArray<T,D-1> CelloArray<T,D>::subarray(int i) const noexcept{
  //ASSERT2("CelloArray::subarray",
  //        "i value of %d must be >= 0 and < shape[0], where shape[0] = %d",
  //        i, shape_[0], start < shape_[0]);
  CHECK_BOUNDND(shape_, i); // checks i against shape_[0] when bounds checking
                            // is enabled
  CelloArray<T,D-1> subarray;
  intp new_shape[D-1];
  for (std::size_t dim=0; dim<(D-1); dim++){
    new_shape[dim] = shape_[dim+1];
  }

  intp new_offset = offset_ + i * stride_[0];

  subarray.init_helper_(shared_data_,new_shape,new_offset);
  for (std::size_t dim=0; dim<(D-1); dim++){
    subarray.stride_[dim] = stride_[dim+1];
  }
  return subarray;
}

//----------------------------------------------------------------------

template<typename T, std::size_t D>
template<typename oT, std::size_t oD>
bool CelloArray<T,D>::is_alias(const CelloArray<oT,oD>& other) const noexcept {
  typedef typename CelloArray<T,D>::const_value_type const_T;
  typedef typename CelloArray<oT,oD>::const_value_type const_oT;
  if (!std::is_same<const_T,const_oT>::value) {return false;}

  if (this->rank() != other.rank()) {return false;}

  if ((this->data() == nullptr) || (other.data() == nullptr)) {return false;}

  void* ptr1 = (void*)(this->data());
  void* ptr2 = (void*)(other.data());
  if (ptr1 != ptr2){ return false; }

  for (std::size_t i = 0; i < D; i++){
    bool different_extents = this->shape(i) != other.shape(i);
    bool different_strides = this->stride(i) != other.stride(i);
    if (different_extents || different_strides) {return false;}
  }
  return true;
}

//----------------------------------------------------------------------

template<typename T, std::size_t D>
void CelloArray<T,D>::copy_to(const CelloArray<T,D>& other) const noexcept{
  const intp o_offset = other.offset_;
  const intp *o_stride = other.stride_;
  const intp *o_shape = other.shape_;
  T* o_data = other.shared_data_.get();
  for (std::size_t i = 0; i<D; i++){
    ASSERT("CelloArray::copy_to", "shapes aren't the same.",
           this->shape_[i] == o_shape[i]);
  }
  bool continue_outer_iter = true;
  intp indices[D] = {}; // all elements to 0
  while (continue_outer_iter){
    intp index = calc_index_(D, this->offset_, this->stride_, indices);
    intp o_index = calc_index_(D, o_offset, o_stride, indices);
    for (intp i = 0; i<this->shape_[D-1]; i++){
      o_data[o_index] = this->shared_data_.get()[index];
      index++; o_index++;
    }
    continue_outer_iter = increment_outer_indices_(D, indices, this->shape_);
  }
}

//----------------------------------------------------------------------

template<typename T, std::size_t D>
CelloArray<T,D> CelloArray<T,D>::deepcopy() const noexcept
{
  std::shared_ptr<T> new_data(new T[this->size()], std::default_delete<T[]>());
  CelloArray<T,D> out;
  out.init_helper_(new_data, this->shape_, 0);
  this->copy_to(out);
  return out;
}

//----------------------------------------------------------------------

template<typename T, std::size_t D>
void CelloArray<T,D>::assert_all_entries_finite_() const noexcept
{
  bool continue_outer_iter = true;
  intp indices[D] = {}; // all elements to 0
  while (continue_outer_iter){
    intp index = calc_index_(D, this->offset_, this->stride_, indices);
    for (intp i = 0; i<this->shape_[D-1]; i++){

      if (!check_if_finite_(this->shared_data_.get()[index])){
	//  let's get the indices as a string
	std::string str_indices = "";
	for (std::size_t j=0; j+1<D; j++){
	  str_indices+= std::to_string(indices[j]) + ", ";
	}
	str_indices += std::to_string(i);

	ASSERT1("CelloArray::assert_all_entries_finite_()",
                "The element at (%s) has a non-finite value.",
		str_indices.c_str(),false);

      }
      index++;
    }
    continue_outer_iter = increment_outer_indices_(D, indices, this->shape_);
  }
}

#endif /* ARRAY_CELLO_ARRAY_HPP */
