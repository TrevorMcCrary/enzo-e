// See LICENSE_CELLO file for license and copyright information

/// @file     enzo_EnzoSolverJacobi.cpp
/// @author   James Bordner (jobordner@ucsd.edu)
/// @date     2014-10-27 22:37:41
/// @brief    Implements the EnzoSolverJacobi class

#include "cello.hpp"

#include "enzo.hpp"

// #define DEBUG_ENTRY
// #define DEBUG_SMOOTH
// #define DEBUG_VERBOSE
// #define DEBUG_SOLVER

#if defined(DEBUG_SMOOTH) && defined(DEBUG_VERBOSE)
#   define PRINT_BDRX				\
  for (int ix=0; ix<mx; ix++) {			\
    for (int iy=0; iy<my; iy++) {		\
      for (int iz=0; iz<mz; iz++) {		\
	int i = ix + mx*(iy + my*iz);		\
	CkPrintf ("[%d %d] %d B=%6.3g D=%6.3g R=%6.3g X=%6.3g\n",	\
		ix,iy,i,B[i],D[i],R[i],X[i]);	\
      }						\
    }						\
  }
#else 
#  define PRINT_BDRX ;
#endif

#define NEW_REFRESH

//----------------------------------------------------------------------

EnzoSolverJacobi::EnzoSolverJacobi
( FieldDescr * field_descr, double weight, int iter_max) throw()
  : Solver(0),
    A_ (NULL),
    ix_ (-1),
    ib_ (-1),
    ir_ (-1),
    id_ (-1),
    w_(weight),
    n_(iter_max)
{
  // Reserve temporary fields
  id_ = field_descr->insert_temporary();
  ir_ = field_descr->insert_temporary();
}

//----------------------------------------------------------------------

void EnzoSolverJacobi::apply
( Matrix * A, int ix, int ib, Block * block) throw()
{
  
  begin_(block);

#ifdef DEBUG_SMOOTH
    printf ("%s:%d %s DEBUG_SMOOTH apply() called\n",
	    __FILE__,__LINE__,block->name().c_str());
#endif

  A_ = A;
  ix_ = ix;
  ib_ = ib;

  Field field = block->data()->field();

  allocate_temporary_(field,block);
  
  // Refresh X
  Refresh refresh (4,0,neighbor_type_(), sync_type_(), sync_id_());

  const int num_fields = field.field_count();

#ifdef NEW_REFRESH
  refresh.add_field (ix_);
  //  refresh.add_field (id_);

  //  refresh.add_field (ir_);
  //  refresh.add_field (ib_);
#else
  refresh.add_all_fields(num_fields);
  refresh.add_field (id_);
  refresh.add_field (ir_);
#endif  

  // try refresh.add_field(ix);

#ifdef DEBUG_SMOOTH
  printf ("%s:%d %s DEBUG_SMOOTH calling p_solver_jacobi_continue() sync_type %d sync_id %d\n",
	  __FILE__,__LINE__,block->name().c_str(), sync_type_(),sync_id_());
#endif
#ifdef DEBUG_ENTRY
    CkPrintf ("%d %s %p jacobi DEBUG_ENTRY enter refresh then p_solver_jacobi_continue\n",
	      CkMyPe(),block->name().c_str(),this);
#endif

    block->refresh_enter
    (CkIndex_EnzoBlock::p_solver_jacobi_continue(),&refresh);
}

//----------------------------------------------------------------------

void EnzoBlock::p_solver_jacobi_continue()
{
  performance_start_(perf_compute,__FILE__,__LINE__);
#ifdef DEBUG_ENTRY
    CkPrintf ("%d %s %p jacobi DEBUG_ENTRY enter p_solver_jacobi_continue\n",
	      CkMyPe(),name().c_str(),this);
#endif
#ifdef DEBUG_SMOOTH
    printf ("%s:%d %s DEBUG_SMOOTH p_solver_jacobi_continue()\n",
	    __FILE__,__LINE__,name().c_str());
#endif
  EnzoSolverJacobi * solver = 
    static_cast<EnzoSolverJacobi *> (this->solver());

  solver->compute(this);
#ifdef DEBUG_ENTRY
    CkPrintf ("%d %s %p jacobi DEBUG_ENTRY  exit p_solver_jacobi_continue\n",
	      CkMyPe(),name().c_str(),this);
#endif
    performance_stop_(perf_compute,__FILE__,__LINE__);
}

//----------------------------------------------------------------------

void EnzoSolverJacobi::compute(Block * block)
{
  
  EnzoBlock* enzo_block = static_cast<EnzoBlock*> (block);
  Field field = enzo_block->data()->field();
  // assume all fields have same precision
  int precision = field.precision(0);

  if (precision == precision_single) {
    apply_<float>(block);
  } else if (precision == precision_double) {
    apply_<double>(block);
  } else if (precision == precision_quadruple) {
    apply_<long double>(block);
  }
}

//----------------------------------------------------------------------

template <typename T>
void EnzoSolverJacobi::apply_(Block * block)
{
#ifdef DEBUG_SMOOTH
    printf ("%s:%d %s DEBUG_SMOOTH apply_ called\n",
	    __FILE__,__LINE__,block->name().c_str());
#endif
  Field field = block->data()->field();

  int mx,my,mz;
  field.dimensions(ix_,&mx,&my,&mz);


  const int g0 = n_;

  const int ix0 = (mx > 1) ? g0 : 0;
  const int iy0 = (my > 1) ? g0 : 0;
  const int iz0 = (mz > 1) ? g0 : 0;

  A_->diagonal (id_, block,g0);

  for (int iter=0; iter<n_; iter++) {

    /// Loop bounds minimal given iteration, ending at
    /// ghost_depth iter=n_-1

    //        const int g0 = MAX(gx - (n_-1-iter), 0);

    // XXXX iter = 0
#ifdef DEBUG_SMOOTH
    printf ("%s:%d %s DEBUG_SMOOTH Computing residual %d\n",
	    __FILE__,__LINE__,block->name().c_str(),iter);
#endif
    PRINT_BDRX;

    A_->residual (ir_, ib_, ix_, block,g0);

#ifdef DEBUG_SMOOTH
    CkPrintf ("%s:%d %s DEBUG_SMOOTH Computing diagonal\n",
	      __FILE__,__LINE__,block->name().c_str());
#endif
    PRINT_BDRX;
    
#ifdef DEBUG_SMOOTH
    CkPrintf ("%s:%d %s DEBUG_SMOOTH Computing X=R/D\n",
	      __FILE__,__LINE__,block->name().c_str());
#endif
    PRINT_BDRX;

#ifdef DEBUG_SMOOTH
    CkPrintf ("%s:%d DEBUG LOOP LIMITS X=R/D ix=%d:%d iy=%d:%d iz=%d:%d\n",
	      __FILE__,__LINE__,ix0,mx-ix0,iy0,my-iy0, iz0, mz-iz0);
#endif

    T * X = (T*) field.values(ix_);
    T * R = (T*) field.values(ir_);
    T * D = (T*) field.values(id_);

    for (int iz=iz0; iz<mz-iz0; iz++) {
      for (int iy=iy0; iy<my-iy0; iy++) {
	for (int ix=ix0; ix<mx-ix0; ix++) {
	  int i = ix + mx*(iy + my*iz);
	  X[i] += R[i] / D[i];
	}
      }
    }

#ifdef DEBUG_SMOOTH
    CkPrintf ("%s:%d %s DEBUG_SMOOTH  Done with SmoothJacobi\n",
	      __FILE__,__LINE__,block->name().c_str());
#endif
    PRINT_BDRX;
  }
  
#ifdef DEBUG_SMOOTH
  CkPrintf ("%s:%d %s DEBUG_SMOOTH  Calling Solver::end_()\n",
	    __FILE__,__LINE__,block->name().c_str());
#endif
  deallocate_temporary_ (field,block);
  Solver::end_(block);
#ifdef DEBUG_ENTRY
    CkPrintf ("%d %s %p jacobi DEBUG_ENTRY calling callback %d\n",
	      CkMyPe(),name().c_str(),this,callback_);
#endif
  CkCallback(callback_,
	     CkArrayIndexIndex(block->index()),
	     block->proxy_array()).send();
}

