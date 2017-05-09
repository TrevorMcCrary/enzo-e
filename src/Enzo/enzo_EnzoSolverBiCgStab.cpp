// See LICENSE_CELLO file for license and copyright information

/// @file     enzo_EnzoSolverBiCgStab.cpp
/// @author   Daniel R. Reynolds (reynolds@smu.edu)
/// @author   James Bordner (jobordner@ucsd.edu)
/// @date     2014-10-23 16:19:06
/// @brief    Implements the EnzoSolverBiCgStab class
//
// The following is Matlab code for right-preconditioned BiCgStab, 
// optimized for both memory efficiency and for reduced 'reduction' 
// synchronization:
//
// function [x, err, iter, flag] = bcgs_opt(A, b, M, iter_max, res_tol, singular)
// % Usage: [x, err, iter, flag] = bcgs_opt(A, b, M, iter_max, res_tol, singular)
// % Solves the linear system Ax=b using the BiConjugate Gradient
// % Stabilized Method with right preconditioning.  We assume an
// % initial guess of x=0.
// %
// % input   A        REAL matrix
// %         b        REAL right hand side vector
// %         iter_max INTEGER maximum number of iterations
// %         res_tol  REAL error tolerance
// %         singular INTEGER flag denoting singular matrix
// %
// % output  x        REAL solution vector
// %         err      REAL error norm
// %         iter     INTEGER number of iterations performed
// %         flag     INTEGER: 0 = solution found to tolerance
// %                           1 = no convergence given iter_max
// %                          -1 = breakdown: rho = 0
// %                          -2 = breakdown: omega = 0
//
//   % initialize outputs
//   iter = 0;
//   flag = 0;
//
//   % for singular (Poisson) systems, project RHS to range space
//   if (singular)
//      bs = sum(b);                    % global reduction
//      bc = length(b);                 % global reduction
//      b = b - bs/bc;
//   end
//   
//   % compute initial residual, initialize temporary vectors
//   r = b;
//   r0 = r;
//   p = r;                             % Note: r, r0 and p are in R(A)
//   
//   % inner-product at initialization: <b,b>
//   rho0 = dot(b,b);                   % global reduction
//   beta_n = rho0;
// 
//   % store ||b|| for relative error checks
//   rho0 = sqrt(rho0);
//   if (rho0 == 0.0), rho0 = 1.0; end
//
//   % check initial relative residual to see if work is finished
//   err = sqrt(beta_n)/rho0;
//   if (err < res_tol), return; end
//
//   % begin iteration
//   for iter = 1:iter_max
//
//      % first half of step
//      y = M\p;                        % ? communication
//      v = A*y;                        % pt-to-pt communication
//      vr0 = dot(v,r0);                % global reduction
//      if (singular)
//         ys = sum(y);                 % global reduction
//         vs = sum(v);                 % global reduction
//      end
//      if (singular)                % project y and v into R(A)
//         y = y - ys/bc;            % note: fine to do after vr0 since <r0,e>=0
//         v = v - vs/bc;
//      end
//      alpha = beta_n / vr0;
//      q = r - alpha*v;
//      x = x + alpha*y;             % Note: q and x are in R(A)
//
//      % stabilization portion of step
//      y = M\q;                        % ? communication
//      u = A*y;                        % pt-to-pt communication
//      omega_d = dot(u,u);             % global reduction
//      omega_n = dot(u,q);             % global reduction
//      if (singular)
//         ys = sum(y);                 % global reduction
//         us = sum(u);                 % global reduction
//      end
//      if (singular)                % project y and u into R(A)
//         y = y - ys/bc;            % note: fine to do after omega_n since <q,e>=0
//         u = u - us/bc;
//         omega_d=omega_d-us^2/bc;  % fix due to reduction before projection
//      end
//      if (omega_d == 0),  omega_d = 1; end
//      omega = omega_n / omega_d;
//      if (omega == 0), flag = -2; return; end
//      x = x + omega*y;
//      r = q - omega*u;             % Note: r and x are in R(A)
//      if (beta_n == 0), flag = -1; return; end
//      err = sqrt(rr)/rho0;
//      if (err < res_tol), return; end
//
//      % compute new direction vector
//      beta = (beta_n/beta_d)*(alpha/omega);
//      p = r + beta*(p - omega*v);  % Note: p is in R(A)
//
//   end      
//
//   % non-convergent
//   flag = 1;
// 
// % END bcgs.m
//
//     V -- (temporary) matrix-vector product vector
//     Q -- (temporary) temporary vector
//     U -- (temporary) temporary vector
//   
//   Communicated scalars (results of inner-products):
//     rho0 -- floating-point, initial residual
//     beta_d -- floating-point
//     beta_n -- floating-point
//     vr0 -- floating-point
//     omega_d -- floating-point
//     omega_n -- floating-point
//     rr -- floating-point
//     bs -- floating-point, shift factor numerator
//     bc -- floating-point, shift factor denominator
//
//   Local scalars:
//     iter -- integer
//     beta -- floating-point
//     err -- floating-point
//     err0 -- floating-point
//     err_min -- floating-point
//     err_max -- floating-point
//     alpha -- floating-point
//     omega -- floating-point
//     iter_max -- integer (input)
//     res_tol -- floating-point (input)
//
// ======================================================================
//
// BiCgStab partitioned along parallel communication / synchronization steps
//
// --------------------
// compute_()
// --------------------
//
//    return_ = return_unknown;
// 
//    B = <right-hand side>
//    X = <initial solution X0> => initialize to zero
//    
//    iter = 0
//
//    if (is_singular) {
//       bs_ = SUM(B)   ==> r_solver_bicgstab_start_1
//       bc_ = COUNT(B) ==> r_solver_bicgstab_start_1
//    } else {
//       call solver_bicgstab_start_2
//    }  
//
// --------------------
// r_solver_bicgstab_start_1()
// --------------------
//
//    receive bs_ and bc_
//
//    call start_2
//
// --------------------
// start_2()
// --------------------
//
//    if (is_singular)
//      B = B - bs_/bc_;
//
//    R = B
//    R0 = R
//    P = R
//    beta_n_ = DOT(R, R) ==> r_solver_bicgstab_start_3
//
// --------------------
// r_solver_bicgstab_start_3()
// --------------------
//
//    receive beta_n_
// 
//    call loop_0
//
// ==================================================
//
// --------------------
// loop_0()
// --------------------
//
//    if (iter_ == 0) {
//       rho0_ = sqrt(beta_n_)
//       if (rho0_ == 0.0)  rho0_ = 1.0
//       err_ = sqrt(beta_n_) / rho0_
//       err0_ = err_
//       err_min_ = err_
//       err_max_ = err_
//    } else {
//       err_ = sqrt(rr_) / rho0_;
//       err_min_ = min(err_, err_min_)
//       err_max_ = max(err_, err_max_)
//    }
//
//    output solution progress (iteration, residual, etc)
//
//    if (err_ < res_tol_)
//       ==> end(return_converged)
//
//    if (iter_ >= iter_max_) {
//       ==> end(return_error);
//    }
// 
//    refresh(P) ==> p_solver_bicgstab_loop_1
//
// --------------------
// p_solver_bicgstab_loop_1()
// --------------------
//
//    call loop_2
//
// --------------------
// loop_2()
// --------------------
//
//    Y = SOLVE(M,P)
//
//    refresh(Y) ==> p_solver_bicgstab_loop_3
//
// --------------------
// p_solver_bicgstab_loop_3()
// --------------------
//  
//    call loop_4
// 
// --------------------
// loop_4()
// --------------------
//
//    V = MATVEC(A,Y)
//
//    vr0_ = DOT(V, R0) ==> r_solver_bicgstab_loop_5
//
//    if (is_singular) {
//       ys_ = SUM(Y) ==> r_solver_bicgstab_loop_5
//       vs_ = SUM(V) ==> r_solver_bicgstab_loop_5
//    }  
//
// --------------------
// r_solver_bicgstab_loop_5()
// --------------------
//  
//    receive vr0_, ys_ and vs_
//
//    call loop_6
// 
// --------------------
// loop_6()
// --------------------
//
//    if (is_singular) {
//      Y = Y - ys_/bc_;
//      V = V - vs_/bc_;
//    }
//
//    alpha_ = beta_n_ / ( vr0_ );
//    Q = R - alpha_*V
//    X = X + alpha_*Y
//
//    refresh(Q) ==> p_solver_bicgstab_loop_8
//
// --------------------
// p_solver_bicgstab_loop_7()
// --------------------
//  
//    call loop_8
//
// --------------------
// loop_8()
// --------------------
//  
//    Y = SOLVE(M,Q)
//
//    refresh(Y) ==> p_solver_bicgstab_loop_9
// 
// --------------------
// p_solver_bicgstab_loop_9()
// --------------------
//  
//    call loop_10
// 
// --------------------
// loop_10()
// --------------------
//
//    U = MATVEC(A,Y)
//
//    omega_d_ = DOT(U, U) ==> r_solver_bicgstab_loop_11
//    omega_n_ = DOT(U, Q) ==> r_solver_bicgstab_loop_11
//
//    if (is_singular) {
//       ys_ = SUM(Y) ==> r_solver_bicgstab_loop_11
//       us_ = SUM(U) ==> r_solver_bicgstab_loop_11
//    }  
//
// --------------------
// r_solver_bicgstab_loop_11()
// --------------------
//  
//    receive ys_, us_, omega_d_ and omega_n_
//
//    call loop_12
// 
// --------------------
// loop_12()
// --------------------
//
//    if (is_singular) {
//      Y = Y - ys_/bc_;
//      U = U - us_/bc_;
//
//      omega_d_ = omega_d - us_*us_/bc;
//    }
//
//    if (omega_d_ == 0.0)  omega_d_ = 1.0
//    if ( omega_n_ == 0.0 ) 
//       end(return_error)
//
//    omega_ = omega_n_ / omega_d_
//    X = X + omega_*Y
//    R = Q - omega_*U
//
//    beta_d_ = beta_n_
//
//    rr_     = DOT(R, R)  ==> r_solver_bicgstab_loop_13
//    beta_n_ = DOT(R, R0) ==> r_solver_bicgstab_loop_13
//
// --------------------
// r_solver_bicgstab_loop_13()
// --------------------
//
//    receive rr_ and beta_n_
//
//    call loop_14
//
// --------------------
// loop_14()
// --------------------
//
//    if ( beta_n_ == 0.0 )
//       end(return_error)
//
//    beta_ = (beta_n_/beta_d_)*(alpha_/omega_)
//    P = R + beta_*( P - omega_*V )
//
//    iter = iter_ + 1
//    (contribute iter to iter_)
//
//    ==> r_solver_bicgstab_loop_15()
//
// --------------------
// r_solver_bicgstab_loop_15()
// --------------------
//
//    receive iter_
//
//    call loop_0
//
//
// ==================================================
//
// --------------------
// end(return_)
// --------------------
//
//    if (retval == return_converged) {
//       potential = X
//       compute acceleration field
//       ==> solver_bicgstab_exit()
//    } else {
//       ERROR (retval)
//    }
//
// --------------------------------------------------

#include "cello.hpp"
#include "charm_simulation.hpp"
#include "enzo.hpp"
#include "enzo.decl.h"

#define CK_TEMPLATES_ONLY
#include "enzo.def.h"
#undef CK_TEMPLATES_ONLY

// #define DEBUG_ENTRY
// #define DEBUG_FIELD
// #define DEBUG_BICGSTAB

#ifdef DEBUG_BICGSTAB
#   define TRACE_BICGSTAB(MSG,BLOCK) \
  CkPrintf ("%s DEBUG_BICGSTAB %s\n", \
	    BLOCK->name().c_str(),MSG); \
  fflush(stdout);
#else
#   define TRACE_BICGSTAB(F,B) /*  ...  */
#endif

#ifdef DEBUG_FIELD

#   define TRACE_BCG(block,msg)						\
  CkPrintf ("%d %s TRACE_BCG %s\n",					\
	    CkMyPe(),(block != NULL) ? block->name().c_str() : "root",msg); \
  fflush(stdout);

#define TRACE_FIELD(IX,NAME)						\
  {									\
    Data * data = enzo_block->data();					\
    Field field = data->field();					\
    T * X = (T*) field.values(IX);					\
    double xx=0.0;							\
    double yy=0.0;							\
    double xs=0.0;							\
    double ys=0.0;							\
    for (int iz=0; iz<mz_; iz++) {					\
      for (int iy=0; iy<my_; iy++) {					\
	for (int ix=0; ix<mx_; ix++) {					\
	  int i = ix + mx_*(iy + my_*iz);				\
	  xs+=X[i];							\
	  xx+=X[i]*X[i];						\
	}								\
      }									\
    }									\
    for (int iz=gz_; iz<mz_-gz_; iz++) {				\
      for (int iy=gy_; iy<my_-gy_; iy++) {				\
	for (int ix=gx_; ix<mx_-gx_; ix++) {				\
	  int i = ix + mx_*(iy + my_*iz);				\
	  ys+=X[i];							\
	  yy+=X[i]*X[i];						\
	}								\
      }									\
    }									\
    xx = sqrt(xx);							\
    yy = sqrt(yy);							\
    CkPrintf ("%d %s TRACE_FIELD BCG %s [%g] (%g)\n",			\
	      __LINE__,enzo_block->name().c_str(),NAME,			\
	      xs/xx,ys/yy);						\
  }

#else

#   define TRACE_FIELD(IX,NAME) /* ... */
#endif
//----------------------------------------------------------------------

EnzoSolverBiCgStab::EnzoSolverBiCgStab
(FieldDescr* field_descr,
 int monitor_iter, int rank,
 int iter_max, double res_tol,
 int min_level, int max_level,
 int index_precon
 ) 
  : Solver(monitor_iter,min_level,max_level), 
    A_(NULL),
    index_precon_(index_precon),
    first_call_(true),
    rank_(rank),
    iter_max_(iter_max), 
    res_tol_(res_tol),
    rho0_(0), err_(0), err0_(0),err_min_(0), err_max_(0),
    ib_(0), ix_(0), ir_(0), ir0_(0), ip_(0), 
    iy_(0), iv_(0), iq_(0), iu_(0),
    nx_(0), ny_(0), nz_(0),
    mx_(0), my_(0), mz_(0),
    gx_(0), gy_(0), gz_(0),
    iter_(0),
    beta_d_(0), beta_n_(0), beta_(0), 
    omega_d_(0), omega_n_(0), omega_(0), 
    vr0_(0), rr_(0), alpha_(0),
    bs_(0.0),bc_(0.0),
    ys_(0.0),vs_(0.0),us_(0.0)
{

  ir_ = field_descr->insert_temporary();
  ir0_ = field_descr->insert_temporary();
  ip_ = field_descr->insert_temporary();
  iy_ = field_descr->insert_temporary();
  iv_ = field_descr->insert_temporary();
  iq_ = field_descr->insert_temporary();
  iu_ = field_descr->insert_temporary();
  
  /// Initialize default Refresh (called before entry to compute())
  
  const int ir = add_refresh(4, 0, neighbor_type_(), sync_type_());
  refresh(ir)->add_all_fields(field_descr->field_count());
  
  refresh(ir)->add_field (ir_);
  refresh(ir)->add_field (ir0_);
  refresh(ir)->add_field (ip_);
  refresh(ir)->add_field (iy_);
  refresh(ir)->add_field (iv_);
  refresh(ir)->add_field (iq_);
  refresh(ir)->add_field (iu_);
  
}

//----------------------------------------------------------------------

EnzoSolverBiCgStab::~EnzoSolverBiCgStab() throw()
{
  delete A_;
  A_ = NULL;
}

//----------------------------------------------------------------------

void EnzoSolverBiCgStab::apply
( Matrix * A, int ix, int ib, Block * block) throw()
{
#ifdef DEBUG_ENTRY
    CkPrintf ("%d %s %p bicgstab DEBUG_ENTRY begin\n",
	      CkMyPe(),block->name().c_str(),this);
#endif
  TRACE_BICGSTAB("apply()",block);
  
  Solver::begin_(block);
  
  A_ = A;
  ix_ = ix;
  ib_ = ib;

  Field field = block->data()->field();

  allocate_temporary_(field,block);

  /// cast input argument to the EnzoBlock associated with this char
  EnzoBlock* enzo_block = static_cast<EnzoBlock*> (block);

  /// access the field infromation on this block
  field.size(&nx_, &ny_, &nz_);
  
  field.dimensions(0, &mx_, &my_, &mz_);
  field.ghost_depth(0, &gx_, &gy_, &gz_);

  /// call templated internal compute_ routine
  // assuming all fields have same precision
  int precision = field.precision(0);
  if      (precision == precision_single)    compute_<float>      (enzo_block);
  else if (precision == precision_double)    compute_<double>     (enzo_block);
  else if (precision == precision_quadruple) compute_<long double>(enzo_block);
  else 
    ERROR1("EnzoSolverBiCgStab()", "precision %d not recognized",
	   precision);
}

//======================================================================

template<class T>
void EnzoSolverBiCgStab::compute_(EnzoBlock* enzo_block) throw() {

  TRACE_BICGSTAB("compute_",enzo_block);

  /// initialize BiCgStab iteration counter
  iter_ = 0;

  /// access field container on this block
  Data* data  = enzo_block->data();
  Field field = data->field();

  /// construct RHS B, initialize initial solution X to zero (only on
  /// leaf blocks)
  if (is_active_(enzo_block)) {

    /// access relevant fields

    T* X       = (T*) field.values(ix_);

    /// set X = 0 [Q: necessary?  couldn't we reuse the solution from
    /// the previous solve?]
    
    if (first_call_) {
      int m = mx_*my_*mz_;
      for (int i=0; i<m; i++) {
	X[i] = 0.0;
      }
    }
  }

  /// for singular Poisson problems, N(A) is not empty, so project B into R(A)
  if (A_->is_singular()) {

    /// set bs_ = SUM(B)   ==> r_solver_bicgstab_start_1
    /// set bc_ = COUNT(B) ==> r_solver_bicgstab_start_1
    long double reduce[2] = {0.0, 0.0};
    if (is_active_(enzo_block)) {
      T* B = (T*) field.values(ib_);
      const int i0 = gx_ + mx_*(gy_ + my_*gz_);
      reduce[0] = 0.0;
      for (int iz=0; iz<nz_; iz++) {
	for (int iy=0; iy<ny_; iy++) {
	  for (int ix=0; ix<nx_; ix++) {
	    int i = i0 + ix + mx_*(iy + my_*iz);
	    reduce[0] += B[i];
	  }
	}
      }
      reduce[1] = 1.0*nx_*ny_*nz_;
    }

    /// initiate callback for r_solver_bicgstab_start_1 and
    /// contribute to sum and count
    
    CkCallback callback(CkIndex_EnzoBlock::r_solver_bicgstab_start_1<T>(NULL), 
			enzo_block->proxy_array());
    
    enzo_block->contribute(2*sizeof(long double), &reduce, 
			   sum_long_double_2_type, callback);

  } else {

    /// nonsingular system, just call start_2 directly
    this->start_2<T>(enzo_block);

  }
}

//----------------------------------------------------------------------

template<class T>
void EnzoBlock::r_solver_bicgstab_start_1(CkReductionMsg* msg) {
  TRACE_BICGSTAB("r_solver_bicgstab_start_1()",this);
  performance_start_(perf_compute,__FILE__,__LINE__);

  /// EnzoBlock accumulates global contributions to SUM(B) and COUNT(B)
  EnzoSolverBiCgStab* solver = 
    static_cast<EnzoSolverBiCgStab*> (this->solver());
  long double* data = (long double*) msg->getData();
  solver->set_bs( data[0] );
  solver->set_bc( data[1] );
  delete msg;

  /// call start_2 to continue
  solver->start_2<T>(this);
  performance_stop_(perf_compute,__FILE__,__LINE__);

}

//----------------------------------------------------------------------

template<class T>
void EnzoSolverBiCgStab::start_2(EnzoBlock* enzo_block) throw() {
  TRACE_BICGSTAB("start_2()",this);

  /// access field container on this block
  Data* data = enzo_block->data();
  Field field = data->field();

  /// update B and initialize temporary vectors (on leaf blocks only)
  long double reduce = 0.0;
  if (is_active_(enzo_block)) {

    /// access relevant fields
    T* B  = (T*) field.values(ib_);
    T* R0 = (T*) field.values(ir0_);
    T* P  = (T*) field.values(ip_);
    T* R  = (T*) field.values(ir_);

    /// for singular problems, project B into R(A)
    int m = mx_*my_*mz_;
    if (A_->is_singular()) {
      T shift = -bs_ / bc_;
      for (int i=0; i<m; i++) {
	B[i] += shift;
      }
    }
    /// initialize R = R0 = P = B
    for (int i=0; i<m; i++) {
      R[i] = R0[i] = P[i] = B[i];
    }

    TRACE_FIELD(ir_,"R");
    /// Compute local contributions to beta_n_ = DOT(R, R)
    reduce = 0.0;
    const int i0 = gx_ + mx_*(gy_ + my_*gz_);
    for (int iz=0; iz<nz_; iz++) {
      for (int iy=0; iy<ny_; iy++) {
	for (int ix=0; ix<nx_; ix++) {
	  int i = i0 + ix + mx_*(iy + my_*iz);
	  reduce += R[i]*R[i];
	}
      }
    }
  }
  
  /// initiate callback for r_solver_bicgstab_start_3 and contribute
  /// to dot-product
  CkCallback callback(CkIndex_EnzoBlock::r_solver_bicgstab_start_3<T>(NULL), 
		      enzo_block->proxy_array());

  enzo_block->contribute(sizeof(long double), &reduce, 
			 sum_long_double_type, callback);
}

//----------------------------------------------------------------------

template<class T> void EnzoBlock::r_solver_bicgstab_start_3(CkReductionMsg* msg) {

  performance_start_(perf_compute,__FILE__,__LINE__);

  /// EnzoBlock accumulates global contributions to DOT(R, R)
  EnzoSolverBiCgStab* solver = 
    static_cast<EnzoSolverBiCgStab*> (this->solver());
  long double* data = (long double*) msg->getData();
  solver->set_beta_n( data[0] );
  delete msg;

  /// call loop_0 to begin solver loop
  solver->loop_0<T>(this);
  performance_stop_(perf_compute,__FILE__,__LINE__);
}

//----------------------------------------------------------------------

template<class T> void EnzoSolverBiCgStab::loop_0(EnzoBlock* enzo_block) throw() {

  TRACE_BICGSTAB("loop_0()",enzo_block);

  /// verify legal floating-point value for preceding reduction result
  cello::check(beta_n_,"beta_n_",__FILE__,__LINE__);

  /// initialize/update current error, store error statistics
  if (iter_ == 0) {
    rho0_ = sqrt(beta_n_);
    if (rho0_ == 0.0)  rho0_ = 1.0;
    err_ = sqrt(beta_n_) / rho0_;
    err0_ = err_;
    err_min_ = err_;
    err_max_ = err_;
  } else {
    err_ = sqrt(rr_) / rho0_;
    err_min_ = std::min(err_, err_min_);
    err_max_ = std::max(err_, err_max_);
  }

  const bool is_converged = (err_ < res_tol_);
  const bool is_diverged = (iter_ >= iter_max_);
  
  /// monitor output solution progress (iteration, residual, etc)

  const bool l_output =
    ( enzo_block->index().is_root() &&
      ( (iter_ == 0) ||
	(is_converged || is_diverged) ||
	(monitor_iter_ && (iter_ % monitor_iter_) == 0 )) );

  if (l_output) {
    Monitor* monitor = enzo_block->simulation()->monitor();
    monitor_output_(enzo_block,iter_,err0_,err_min_,err_,err_max_);
  }

  if (is_converged) {

    this->end<T>(enzo_block, return_converged);

  } else if (is_diverged)  {

    this->end<T>(enzo_block, return_diverged);

    
  } else {

    // Refresh field faces then call solver_bicgstab_loop_1

    loop_2<T>(enzo_block);
  
  }
}

//----------------------------------------------------------------------

template<class T>
void EnzoSolverBiCgStab::loop_2(EnzoBlock* enzo_block) throw() {

  TRACE_BICGSTAB("start coarse solve()",enzo_block);

  TRACE_FIELD(ip_,"P");
  TRACE_FIELD(iy_,"Y");
  /// access field container on this block
  Data* data = enzo_block->data();
  Field field = data->field();

  if (index_precon_ >= 0) {
    Simulation * simulation = proxy_simulation.ckLocalBranch();
    Solver * precon = simulation->problem()->solver(index_precon_);
    precon->set_sync_id (8);
    precon->set_min_level(min_level_);
    precon->set_max_level(max_level_);

    precon->set_callback(CkIndex_EnzoBlock::p_solver_bicgstab_loop_2());
    precon->apply(A_,iy_,ip_,enzo_block);
    
  } else {

    T * Y = (T*) field.values(iy_);
    T * P = (T*) field.values(ip_);

    int m = mx_*my_*mz_;
    for (int i=0; i<m; i++) {
      Y[i] = P[i];
    }

    loop_25<T>(enzo_block);
  }

}

//----------------------------------------------------------------------

void EnzoBlock::p_solver_bicgstab_loop_2() {
  TRACE_BICGSTAB("return from coarse solve",this);
  performance_start_(perf_compute,__FILE__,__LINE__);

  /// re-entry into loop_25, using template with appropriate precision
  EnzoSolverBiCgStab* solver = 
    static_cast<EnzoSolverBiCgStab*> (this->solver());
  EnzoBlock* enzo_block = static_cast<EnzoBlock*> (this);
  Field field = data()->field();
  // assuming all fields have same precision
  int precision = field.precision(0);
  if      (precision == precision_single)    
    solver->loop_25<float>(enzo_block);
  else if (precision == precision_double)    
    solver->loop_25<double>(enzo_block);
  else if (precision == precision_quadruple) 
    solver->loop_25<long double>(enzo_block);
  else 
    ERROR1("EnzoSolverBiCgStab()", "precision %d not recognized",
	   precision);

  performance_stop_(perf_compute,__FILE__,__LINE__);

}

template<class T>
void EnzoSolverBiCgStab::loop_25 (EnzoBlock * enzo_block) throw() {
  
  TRACE_BICGSTAB("refresh after coarse solve",enzo_block);

  // refresh Y with callback to p_solver_bicgstab_loop_25 to handle re-entry

  // Refresh field faces then call p_solver_bicgstab_loop_25()
  Refresh refresh (4,0,neighbor_type_(), sync_type_());
  refresh.set_active(is_active_(enzo_block));
  refresh.add_all_fields(enzo_block->data()->field().field_count());
  
  refresh.add_field (ir_);
  refresh.add_field (ir0_);
  refresh.add_field (ip_);
  refresh.add_field (iy_);
  refresh.add_field (iv_);
  refresh.add_field (iq_);
  refresh.add_field (iu_);

  enzo_block->refresh_enter
    (CkIndex_EnzoBlock::p_solver_bicgstab_loop_3(),&refresh);

}

//----------------------------------------------------------------------

void EnzoBlock::p_solver_bicgstab_loop_3() {

  TRACE_BICGSTAB("return from coarse solve refresh",this);

  performance_start_(perf_compute,__FILE__,__LINE__);

  /// re-entry into loop_4, using template with appropriate precision
  EnzoSolverBiCgStab* solver = 
    static_cast<EnzoSolverBiCgStab*> (this->solver());
  EnzoBlock* enzo_block = static_cast<EnzoBlock*> (this);
  Field field = data()->field();
  // assuming all fields have same precision
  int precision = field.precision(0);
  if      (precision == precision_single)    
    solver->loop_4<float>(enzo_block);
  else if (precision == precision_double)    
    solver->loop_4<double>(enzo_block);
  else if (precision == precision_quadruple) 
    solver->loop_4<long double>(enzo_block);
  else 
    ERROR1("EnzoSolverBiCgStab()", "precision %d not recognized",
	   precision);

  performance_stop_(perf_compute,__FILE__,__LINE__);
  
}

//----------------------------------------------------------------------

template<class T>
void EnzoSolverBiCgStab::loop_4(EnzoBlock* enzo_block) throw() {

  TRACE_BICGSTAB("loop_4()",enzo_block);
  /// access field container on this block
  Data* data = enzo_block->data();
  Field field = data->field();

  /// V = MATVEC(A,Y)
  if (is_active_(enzo_block)) {
    /// apply matrix to local block
    A_->matvec(iv_, iy_, enzo_block);     
  }

  /// compute local contributions to vr0_ = DOT(V, R0)
  long double reduce[4] = {0.0, 0.0, 0.0, 0.0};
  if (is_active_(enzo_block)) {
    T* R0 = (T*) field.values(ir0_);
    T* V  = (T*) field.values(iv_);
    const int i0 = gx_ + mx_*(gy_ + my_*gz_);
    reduce[0] = 0.0;
    for (int iz=0; iz<nz_; iz++) {
      for (int iy=0; iy<ny_; iy++) {
	for (int ix=0; ix<nx_; ix++) {
	  int i = i0 + ix + mx_*(iy + my_*iz);
	  reduce[0] += V[i]*R0[i];
	}
      }
    }
  }

  /// for singular Poisson problems need all vectors in R(A), so
  /// project both Y and V into R(A)

  if (A_->is_singular()) {
    /// set ys_ = SUM(Y)
    /// set vs_ = SUM(V)
    if (is_active_(enzo_block)) {
      T* Y = (T*) field.values(iy_);
      T* V = (T*) field.values(iv_);
      const int i0 = gx_ + mx_*(gy_ + my_*gz_);
      reduce[1] = 0.0;
      reduce[2] = 0.0;
      for (int iz=0; iz<nz_; iz++) {
	for (int iy=0; iy<ny_; iy++) {
	  for (int ix=0; ix<nx_; ix++) {
	    int i = i0 + ix + mx_*(iy + my_*iz);
	    reduce[1] += Y[i];
	    reduce[2] += V[i];
	  }
	}
      }
    }
  }
  /// initiate callback to r_solver_bicgstab_loop_5 and contribute to
  /// global sums

  CkCallback callback(CkIndex_EnzoBlock::r_solver_bicgstab_loop_5<T>(NULL), 
		      enzo_block->proxy_array());

  enzo_block->contribute(3*sizeof(long double), &reduce, 
			 sum_long_double_3_type, callback);
}

//----------------------------------------------------------------------

template<class T>
void EnzoBlock::r_solver_bicgstab_loop_5(CkReductionMsg* msg) {
  TRACE_BICGSTAB("EnzoBlock::loop_5()",this);

  performance_start_(perf_compute,__FILE__,__LINE__);

  /// EnzoBlock accumulates global contributions to SUM(Y) and SUM(V)
  EnzoSolverBiCgStab* solver = 
    static_cast<EnzoSolverBiCgStab*> (this->solver());
  long double* data = (long double*) msg->getData();
  solver->set_vr0( data[0] );
  solver->set_ys(  data[1] );
  solver->set_vs(  data[2] );
  delete msg;

  /// call loop_6 to continue
  solver->loop_6<T>(this);

  performance_stop_(perf_compute,__FILE__,__LINE__);
}

//----------------------------------------------------------------------

template<class T>
void EnzoSolverBiCgStab::loop_6(EnzoBlock* enzo_block) throw() {

  TRACE_BICGSTAB("loop_6()",enzo_block);
  /// access field container on this block
  Data* data = enzo_block->data();
  Field field = data->field();

  /// for singular problems, project Y and V into R(A)
  int m = mx_*my_*mz_;
  if (is_active_(enzo_block) && A_->is_singular()) {
    T* Y = (T*) field.values(iy_);
    T* V = (T*) field.values(iv_);
    T yshift = -ys_ / bc_;
    T vshift = -vs_ / bc_;
    for (int i=0; i<m; i++) {
      Y[i] += yshift;
      V[i] += vshift;
    }
  }

  TRACE_FIELD(iv_,"V");
  TRACE_FIELD(ip_,"P");
  TRACE_FIELD(iy_,"Y");
  /// compute alpha factor in BiCgStab algorithm (all blocks)
  alpha_ = beta_n_ / vr0_;

  /// update vectors (on leaf blocks only)
  if (is_active_(enzo_block)) {

    /// access relevant fields
    T* Q = (T*) field.values(iq_);
    T* R = (T*) field.values(ir_);
    T* V = (T*) field.values(iv_);
    T* X = (T*) field.values(ix_);
    T* Y = (T*) field.values(iy_);

    TRACE_FIELD(ix_,"X");
    /// update: Q = -alpha_*V + R
    /// update: X = alpha_*Y + X

    for (int i=0; i<m; i++) {
      Q[i] = R[i] - alpha_*V[i];
      X[i] = X[i] + alpha_*Y[i];
    }

  }

  // Refresh field faces then call p_solver_bicgstab_loop_7

  /// refresh Q with callback to p_solver_bicgstab_loop_7 to handle re-entry

  loop_8<T>(enzo_block);
  //enzo_block->p_solver_bicgstab_loop_7();
}

//----------------------------------------------------------------------

template<class T>
void EnzoSolverBiCgStab::loop_8(EnzoBlock* enzo_block) throw() {

  TRACE_BICGSTAB("loop_8()",enzo_block);
  /// access field container on this block
  Data* data = enzo_block->data();
  Field field = data->field();

  if (index_precon_ >= 0) {
    Simulation * simulation = proxy_simulation.ckLocalBranch();
    Solver * precon = simulation->problem()->solver(index_precon_);
    precon->set_sync_id (10);
    precon->set_min_level(min_level_);
    precon->set_max_level(max_level_);

    precon->set_callback(CkIndex_EnzoBlock::p_solver_bicgstab_loop_8());

    precon->apply(A_,iy_,iq_,enzo_block);
  } else {

    T * Y = (T*) field.values(iy_);
    T * Q = (T*) field.values(iq_);
    int m = mx_*my_*mz_;
    for (int i=0; i<m; i++) {
      Y[i] = Q[i];
    }
    loop_85<T>(enzo_block);
  }
}

//----------------------------------------------------------------------

void EnzoBlock::p_solver_bicgstab_loop_8() {
  TRACE_BICGSTAB("EnzoBlock::loop_8()",this);
  performance_start_(perf_compute,__FILE__,__LINE__);

  /// re-entry into loop_85, using template with appropriate precision
  EnzoSolverBiCgStab* solver = 
    static_cast<EnzoSolverBiCgStab*> (this->solver());
  EnzoBlock* enzo_block = static_cast<EnzoBlock*> (this);
  Field field = data()->field();
  // assuming all fields have same precision
  int precision = field.precision(0);
  if      (precision == precision_single)    
    solver->loop_85<float>(enzo_block);
  else if (precision == precision_double)    
    solver->loop_85<double>(enzo_block);
  else if (precision == precision_quadruple) 
    solver->loop_85<long double>(enzo_block);
  else 
    ERROR1("EnzoSolverBiCgStab()", "precision %d not recognized",
	   precision);

  performance_stop_(perf_compute,__FILE__,__LINE__);

}

template<class T>
void EnzoSolverBiCgStab::loop_85 (EnzoBlock * enzo_block) throw() {
  
  TRACE_BICGSTAB("loop_85()",enzo_block);

  // refresh Y with callback to p_solver_bicgstab_loop_85 to handle re-entry

  // Refresh field faces then call p_solver_bicgstab_loop_85()

  Refresh refresh (4,0,neighbor_type_(), sync_type_());
  refresh.set_active(is_active_(enzo_block));
  refresh.add_all_fields(enzo_block->data()->field().field_count());

  refresh.add_field (ir_);
  refresh.add_field (ir0_);
  refresh.add_field (ip_);
  refresh.add_field (iy_);
  refresh.add_field (iv_);
  refresh.add_field (iq_);
  refresh.add_field (iu_);
  
  enzo_block->refresh_enter
    (CkIndex_EnzoBlock::p_solver_bicgstab_loop_9(),&refresh);

}

//----------------------------------------------------------------------

void EnzoBlock::p_solver_bicgstab_loop_9() {

  performance_start_(perf_compute,__FILE__,__LINE__);
  
  TRACE_BICGSTAB("EnzoBlock::loop_9()",this);
  /// re-entry into loop_10, using template with appropriate precision
  EnzoSolverBiCgStab* solver = 
    static_cast<EnzoSolverBiCgStab*> (this->solver());
  EnzoBlock* enzo_block = static_cast<EnzoBlock*> (this);
  Field field = data()->field();
  // assuming all fields have same precision
  int precision = field.precision(0); 
  if      (precision == precision_single)    
    solver->loop_10<float>(enzo_block);
  else if (precision == precision_double)    
    solver->loop_10<double>(enzo_block);
  else if (precision == precision_quadruple) 
    solver->loop_10<long double>(enzo_block);
  else 
    ERROR1("EnzoSolverBiCgStab()", "precision %d not recognized", precision);

  performance_stop_(perf_compute,__FILE__,__LINE__);
  
}

//----------------------------------------------------------------------

template<class T> void EnzoSolverBiCgStab::loop_10(EnzoBlock* enzo_block) throw() {

  TRACE_BICGSTAB("loop_10()",enzo_block);
  /// access field container on this block
  Data* data = enzo_block->data();
  Field field = data->field();

  // U = MATVEC(A,Y)
  if (is_active_(enzo_block)) {
    A_->matvec(iu_, iy_, enzo_block);     /// apply matrix to local block
  }

  /// compute local contributions to
  /// omega_d_ = DOT(U, U)
  /// omega_n_ = DOT(U, Q)
  long double reduce[4] = {0.0, 0.0, 0.0, 0.0};
  if (is_active_(enzo_block)) {
    T* U  = (T*) field.values(iu_);
    T* Q  = (T*) field.values(iq_);
    
    const int i0 = gx_ + mx_*(gy_ + my_*gz_);
    reduce[0] = 0.0;
    reduce[1] = 0.0;
    for (int iz=0; iz<nz_; iz++) {
      for (int iy=0; iy<ny_; iy++) {
	for (int ix=0; ix<nx_; ix++) {
	  int i = i0 + ix + mx_*(iy + my_*iz);
	  reduce[0] += U[i]*U[i];
	  reduce[1] += U[i]*Q[i];
	}
      }
    }
  }

  /// for singular Poisson problems, project both Y and U into R(A)
  if (A_->is_singular() && is_active_(enzo_block)) {

    /// set ys_ = SUM(Y)
    /// set us_ = SUM(U)
    T* Y = (T*) field.values(iy_);
    T* U = (T*) field.values(iu_);
    const int i0 = gx_ + mx_*(gy_ + my_*gz_);
    reduce[2] = 0.0;
    reduce[3] = 0.0;
    for (int iz=0; iz<nz_; iz++) {
      for (int iy=0; iy<ny_; iy++) {
	for (int ix=0; ix<nx_; ix++) {
	  int i = i0 + ix + mx_*(iy + my_*iz);
	  reduce[2] += Y[i];
	  reduce[3] += U[i];
	}
      }
    }
  }

  /// initiate callback to r_solver_bicgstab_loop_11, and contribute to overall dot-products
  CkCallback callback(CkIndex_EnzoBlock::r_solver_bicgstab_loop_11<T>(NULL), 
		      enzo_block->proxy_array());
  enzo_block->contribute(4*sizeof(long double), &reduce, 
			 sum_long_double_4_type, callback);
    
}

//----------------------------------------------------------------------

template<class T> void EnzoBlock::r_solver_bicgstab_loop_11(CkReductionMsg* msg) {

  performance_start_(perf_compute,__FILE__,__LINE__);

  /// EnzoBlock accumulates global contributions to SUM(Y) and SUM(U)
  EnzoSolverBiCgStab* solver = 
    static_cast<EnzoSolverBiCgStab*> (this->solver());
  long double* data = (long double*) msg->getData();
  solver->set_omega_d( data[0] );
  solver->set_omega_n( data[1] );
  solver->set_ys( data[2] );
  solver->set_us( data[3] );
  delete msg;

  /// call loop_12 to continue
  solver->loop_12<T>(this);
  performance_stop_(perf_compute,__FILE__,__LINE__);
}

//----------------------------------------------------------------------

template<class T> void EnzoSolverBiCgStab::loop_12(EnzoBlock* enzo_block) throw() {

  TRACE_BICGSTAB("loop_12()",enzo_block);
  /// verify legal floating-point values for preceding reduction results
  cello::check(omega_d_,"omega_d_",__FILE__,__LINE__);
  cello::check(omega_n_,"omega_n_",__FILE__,__LINE__);

  /// access field container on this block
  Data* data = enzo_block->data();
  Field field = data->field();

  /// for singular problems, update omega_d_ and project Y and U into R(A)

  int m = mx_*my_*mz_;

  if (A_->is_singular()) {

    omega_d_ = omega_d_ - us_*us_/bc_;

    if (is_active_(enzo_block)) {
      T* Y = (T*) field.values(iy_);
      T* U = (T*) field.values(iu_);
      T yshift = -ys_ / bc_;
      T ushift = -us_ / bc_;
      for (int i=0; i<m; i++) {
    	Y[i] += yshift;
    	U[i] += ushift;
      }
    }
  }

  /// fix omega_d_ if necessary (for division)
  if (omega_d_ == 0.0)  omega_d_ = 1.0;
  
  /// compute omega factor in BiCgStab algorithm (all blocks)
  omega_ = omega_n_ / omega_d_;

  /// check for breakdown in BiCgStab
  if ( omega_ == 0.0 ) {
    WARNING ("EnzoSolverBiCgStab::loop12()",
	     "Solver error: omega_ == 0");
    this->end<T>(enzo_block, return_error);
  }

  /// update vectors (on leaf blocks only)
  if (is_active_(enzo_block)) {

    /// access relevant fields
    T* X = (T*) field.values(ix_);
    T* Y = (T*) field.values(iy_);
    T* R = (T*) field.values(ir_);
    T* Q = (T*) field.values(iq_);
    T* U = (T*) field.values(iu_);
    
    /// update: X = omega_*Y + X
    /// update: R = -omega_*U + Q

    for (int i=0; i<m; i++) {
      X[i] = X[i] + omega_*Y[i];
      R[i] = Q[i] - omega_*U[i];
    }

  }

  /// Update previous beta value (beta_d_) to current value (beta_n_)
  beta_d_ = beta_n_;

  /// compute local contributions to
  /// rr_     = DOT(R, R)
  /// beta_n_ = DOT(R, R0)
  long double reduce[4] = {0.0, 0.0, 0.0, 0.0};
  if (is_active_(enzo_block)) {
    T* R  = (T*) field.values(ir_);
    T* R0 = (T*) field.values(ir0_);
    const int i0 = gx_ + mx_*(gy_ + my_*gz_);
    reduce[0] = 0.0;
    reduce[1] = 0.0;
    for (int iz=0; iz<nz_; iz++) {
      for (int iy=0; iy<ny_; iy++) {
	for (int ix=0; ix<nx_; ix++) {
	  int i = i0 + ix + mx_*(iy + my_*iz);
	  reduce[0] += R[i]*R[i];
	  reduce[1] += R[i]*R0[i];
	}
      }
    }
  }

  /// initiate callback to r_solver_bicgstab_loop_13, and contribute to overall dot-products
  CkCallback callback(CkIndex_EnzoBlock::r_solver_bicgstab_loop_13<T>(NULL), 
		      enzo_block->proxy_array());
  enzo_block->contribute(2*sizeof(long double), &reduce, 
			 sum_long_double_2_type, callback);
    
}

//----------------------------------------------------------------------

template<class T> void EnzoBlock::r_solver_bicgstab_loop_13(CkReductionMsg* msg) {

  performance_start_(perf_compute,__FILE__,__LINE__);

  // EnzoBlock accumulates global contributions to DOT(R,R) and DOT(R,R0)
  EnzoSolverBiCgStab* solver = 
    static_cast<EnzoSolverBiCgStab*> (this->solver());
  long double* data = (long double*) msg->getData();
  solver->set_rr(     data[0] );
  solver->set_beta_n( data[1] );
  delete msg;

  // call loop_14 to continue
  solver->loop_14<T>(this);

  performance_stop_(perf_compute,__FILE__,__LINE__);
}

//----------------------------------------------------------------------

template<class T> void EnzoSolverBiCgStab::loop_14(EnzoBlock* enzo_block) throw() {

  TRACE_BICGSTAB("loop_14()",enzo_block);
  /// verify legal floating-point values for preceding reduction results
  cello::check(rr_,    "rr_",    __FILE__,__LINE__);
  cello::check(beta_n_,"beta_n_",__FILE__,__LINE__);

  /// access field container on this block
  Data* data = enzo_block->data();
  Field field = data->field();

  /// check for breakdown in BiCgStab
  if (beta_n_ == 0.0) {
    WARNING ("EnzoSolverBiCgStab::loop14()",
	     "Solver error: beta_n_ == 0");
    this->end<T>(enzo_block, return_error);
  }
  
  /// compute beta factor in BiCgStab algorithm (all blocks)
  beta_ = (beta_n_/beta_d_)*(alpha_/omega_);

  /// update direction vector (on leaf blocks only) -- P = R+beta*(P-omega*V)
  if (is_active_(enzo_block)) {

    /// access relevant fields
    T* P = (T*) field.values(ip_);
    T* R = (T*) field.values(ir_);
    T* V = (T*) field.values(iv_);

    /// update: P = beta_*P + R
    /// update: P = -beta_*omega_*V + P

    int m = mx_*my_*mz_;
    for (int i=0; i<m; i++) {
      P[i] = R[i] + beta_*(P[i] - omega_*V[i]);
    }
  }

  /// contribute to global iteration counter
  int iter = iter_ + 1;

  /// initiate callback to r_solver_bicgstab_loop_15, and contribute to overall max
  CkCallback callback(CkIndex_EnzoBlock::r_solver_bicgstab_loop_15<T>(NULL), 
		      enzo_block->proxy_array());
  enzo_block->contribute(sizeof(int), &iter, 
			 CkReduction::max_int, callback);

}

//----------------------------------------------------------------------

template<class T> void EnzoBlock::r_solver_bicgstab_loop_15(CkReductionMsg* msg) {

  performance_start_(perf_compute,__FILE__,__LINE__);

  // EnzoBlock accumulates global contributions to iter
  EnzoSolverBiCgStab* solver = 
    static_cast<EnzoSolverBiCgStab*> (this->solver());
  solver->set_iter ( ((int*)msg->getData())[0] );
  delete msg;

  // call loop_0 to continue to next iteration
  solver->loop_0<T>(this);

  performance_stop_(perf_compute,__FILE__,__LINE__);
}

//----------------------------------------------------------------------

template<class T> void EnzoSolverBiCgStab::end
(EnzoBlock* enzo_block, int retval) throw () {

  TRACE_BICGSTAB("end()",this);

  Field field = enzo_block->data()->field();

  deallocate_temporary_(field,enzo_block);
  
  Solver::end_(enzo_block);
  
#ifdef DEBUG_ENTRY
    CkPrintf ("%d %s %p bicgstab DEBUG_ENTRY calling callback_ %d\n",
	      CkMyPe(),enzo_block->name().c_str(),this,callback_);
#endif

    CkCallback(callback_,
	     CkArrayIndexIndex(enzo_block->index()),
	     enzo_block->proxy_array()).send();

}

//======================================================================
