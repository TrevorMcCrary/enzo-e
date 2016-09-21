// See LICENSE_CELLO file for license and copyright information

/// @file     mesh_Block.cpp
/// @author   James Bordner (jobordner@ucsd.edu)
/// @date     Mon Feb 28 13:22:26 PST 2011
/// @brief    Implementation of the Block object

#include "cello.hpp"

#include "mesh.hpp"
#include "main.hpp"
#include "charm_simulation.hpp"

// #define TRACE_BLOCK

// #define DEBUG_ADAPT 

// #define DEBUG_NEW_REFRESH

// KEEP CONSISTENT WITH _comm.hpp: phase_type
const char * phase_name[] = {
  "unknown",
  "initial_enter",
  "initial_exit",
  "adapt_enter",
  "adapt_called",
  "adapt_next",
  "adapt_end",
  "adapt_exit",
  "compute_enter",
  "compute_continue",
  "compute_exit",
  "refresh_enter",
  "refresh_exit",
  "stopping_enter",
  "stopping_exit",
  "output_enter",
  "output_exit",
  "restart",
  "balance",
  "enzo_matvec",
  "exit"
};

//----------------------------------------------------------------------

Block::Block ( MsgRefine * msg )
  :
  data_(NULL),
  child_data_(NULL),
  level_next_(0),
  stop_(false),
  index_initial_(0),
  children_(),
  sync_coarsen_(),
  count_sync_(),
  max_sync_(),
  face_level_curr_(),
  face_level_next_(),
  child_face_level_curr_(),
  child_face_level_next_(),
  count_coarsen_(0),
  coarsened_(false),
  delete_(false),
  is_leaf_(true),
  age_(0),
  face_level_last_(),
  name_(""),
  index_method_(-1)
{
  init (msg->index_,
	msg->nx_, msg->ny_, msg->nz_,
	msg->num_field_blocks_,
	msg->num_adapt_steps_,
	msg->cycle_, msg->time_,  msg->dt_,
	0, NULL, msg->refresh_type_,
	msg->num_face_level_, msg->face_level_);

  name_ = name();

#ifdef TRACE_BLOCK
  int v3[3];
  index_.values(v3);
  CkPrintf ("%d %s index TRACE_BLOCK Block(MsgRefine)  %d %d %d \n",  CkMyPe(),name_.c_str(),
	    v3[0],v3[1],v3[2]);
#endif

  bool is_first_cycle = 
    (cycle_ == simulation()->config()->initial_cycle);

  if (is_first_cycle) {
    apply_initial_();
  } else {
    msg->update(data());
  }

  delete msg;

}

//----------------------------------------------------------------------

void Block::init
(
 Index index,
 int nx, int ny, int nz,
 int num_field_blocks,
 int num_adapt_steps,
 int cycle, double time, double dt,
 int narray, char * array, int refresh_type,
 int num_face_level, int * face_level)

{
  index_ = index;
  cycle_ = cycle;
  time_ = time;
  dt_ = dt;
  adapt_step_ = num_adapt_steps;
  adapt_ = adapt_unknown;

  // Enable Charm++ AtSync() dynamic load balancing
  usesAtSync = true;

#ifdef CELLO_DEBUG
  index_.print("Block()",-1,2,false,simulation());
#endif

  Monitor * monitor = simulation()->monitor();
  if (monitor->is_verbose()) {
    char buffer [80];
    int v3[3];
    this->index().values(v3);
    sprintf (buffer,"Block() %s %d (%x %x %x) created",name().c_str(),
	     index.level(),v3[0],v3[1],v3[2]);
    monitor->print("Adapt",buffer);
  }
  int ibx,iby,ibz;
  index.array(&ibx,&iby,&ibz);

  double xm,ym,zm;
  lower(&xm,&ym,&zm);
  double xp,yp,zp;
  upper(&xp,&yp,&zp);

  // Allocate block data

  data_ = new Data  (nx, ny, nz, 
		     num_field_blocks,
		     xm,xp, ym,yp, zm,zp);

  data_->allocate();

  child_data_ = NULL;

  // Update state

  set_state (cycle,time,dt,stop_);

  // Perform any additional initialization for derived class 

  initialize ();

  const int rank = this->rank();
  
  sync_coarsen_.set_stop(NUM_CHILDREN(rank));
  sync_coarsen_.reset();

  for (int i=0; i<3; i++) {
    count_sync_[i] = 0;
    max_sync_[i] = 0;
  }

  // Initialize neighbor face levels

  face_level_last_.resize(27*8);

  if (num_face_level == 0) {

    face_level_curr_.resize(27);
    child_face_level_curr_.resize(NUM_CHILDREN(rank)*27);

    for (int i=0; i<27; i++) face_level_curr_[i] = 0;

  } else {

    face_level_curr_.resize(num_face_level);
    child_face_level_curr_.resize(NUM_CHILDREN(rank)*num_face_level);

    for (int i=0; i<num_face_level; i++) face_level_curr_[i] = face_level[i];

  }

  for (size_t i=0; i<face_level_last_.size(); i++) 
    face_level_last_[i] = 0;
  for (size_t i=0; i<child_face_level_curr_.size(); i++) 
    child_face_level_curr_[i] = 0;

  initialize_child_face_levels_();

  face_level_next_ = face_level_curr_;
  child_face_level_next_ = child_face_level_curr_;

  const int level = this->level();

  int na3[3];
  size_forest(&na3[0],&na3[1],&na3[2]);

  int ic3[3] = {0,0,0};
  if (level > 0) index_.child(level,&ic3[0],&ic3[1],&ic3[2]);

#ifdef DEBUG_NEW_REFRESH
  CkPrintf ("%p narray = %d\n",this,narray);
#endif
  if (narray != 0) {

    // Create field face of refined data from parent
    int if3[3] = {0,0,0};
    bool lg3[3] = {true,true,true};
    std::vector<int> field_list;
    FieldFace * field_face = create_face
      (if3, ic3, lg3, refresh_fine, field_list);

    // Copy refined field data
    field_face -> array_to_face (array, data()->field());

    delete field_face;

  }

  simulation()->monitor_insert_block();
  
  if (data()->any_particles()) {
    const int np = data()->particle().num_particles();
    simulation()->monitor_insert_particles(np);
  }

  if (level > 0) {

    control_sync (CkIndex_Main::p_adapt_end(),sync_quiescence);

  }

  setMigratable(true);

  debug_faces_("Block()");

}

//----------------------------------------------------------------------

void Block::pup(PUP::er &p)
{
  TRACEPUP;

  CBase_Block::pup(p);

  bool up = p.isUnpacking();

  if (up) data_ = new Data;
  p | *data_;

  // child_data_ may be NULL
  bool allocated=(child_data_ != NULL);
  p|allocated;
  if (allocated) {
    if (up) child_data_=new Data;
    p|*child_data_;
  } else {
    child_data_ = NULL;
  }

  p | index_;
  p | level_next_;
  p | cycle_;
  p | time_;
  p | dt_;
  p | stop_;
  p | index_initial_;
  p | children_;
  p | sync_coarsen_;
  PUParray(p,count_sync_, PHASE_COUNT);
  PUParray(p,max_sync_,   PHASE_COUNT);
  p | face_level_curr_;
  p | face_level_next_;
  p | child_face_level_curr_;
  p | child_face_level_next_;
  p | count_coarsen_;
  p | adapt_step_;
  p | adapt_;
  p | coarsened_;
  p | delete_;
  p | is_leaf_;
  p | age_;
  p | face_level_last_;
  p | name_;
  p | index_method_;
  p | refresh_;
  // SKIP method_: initialized when needed

  if (up) debug_faces_("PUP");
}

//----------------------------------------------------------------------

ItFace Block::it_face
(int min_face_rank,
 Index index,
 const int * ic3,
 const int * if3) throw()
{
  int rank = this->rank();
  int n3[3];
  size_forest(&n3[0],&n3[1],&n3[2]);
  bool periodic[3][2];
  periodicity(periodic);
  return ItFace (rank,min_face_rank,periodic,n3,index,ic3,if3);
}

//----------------------------------------------------------------------

ItNeighbor Block::it_neighbor (int min_face_rank, Index index) throw()
{
  int n3[3];
  size_forest(&n3[0],&n3[1],&n3[2]);
  bool periodic[3][2];
  periodicity(periodic);
  return ItNeighbor (this,min_face_rank,periodic,n3,index);
}

//----------------------------------------------------------------------

Method * Block::method () throw ()
{
  Problem * problem = simulation()->problem();
  Method * method = problem->method(index_method_);
  return method;
}

//----------------------------------------------------------------------

void Block::periodicity (bool p32[3][2]) const
{
  for (int axis=0; axis<3; axis++) {
    for (int face=0; face<2; face++) {
      p32[axis][face] = false;
    }
  }
  int index_boundary = 0;
  Problem * problem = simulation()->problem();
  Boundary * boundary;
  while ( (boundary = problem->boundary(index_boundary++)) ) {
    boundary->periodicity(p32);
  }
}

//======================================================================

void Block::apply_initial_() throw ()
{

  TRACE("Block::apply_initial_()");

  performance_switch_(perf_initial,__FILE__,__LINE__);

  FieldDescr * field_descr = simulation()->field_descr();
  ParticleDescr * particle_descr = simulation()->particle_descr();

  // Apply initial conditions

  index_initial_ = 0;
  Problem * problem = simulation()->problem();
  while (Initial * initial = problem->initial(index_initial_++)) {
    initial->enforce_block(this,field_descr, particle_descr,
			   simulation()->hierarchy());
  }
  //  performance_stop_(perf_initial);
}

//----------------------------------------------------------------------

Block::~Block()
{ 
#ifdef TRACE_BLOCK
  CkPrintf ("%d %s TRACE_BLOCK Block::~Block())\n",  CkMyPe(),name_.c_str());
#endif
#ifdef CELLO_DEBUG
  index_.print("~Block()",-1,2,false,simulation());
#endif

  Monitor * monitor = simulation()->monitor();
  if (monitor->is_verbose()) {
    char buffer [80];
    int v3[3];
    index().values(v3);
    sprintf (buffer,"~Block() %s (%d;%d;%d) destroyed",name().c_str(),
	     v3[0],v3[1],v3[2]);
    monitor->print("Adapt",buffer);
  }

  const int level = this->level();

  if (level > 0) {

    // Send restricted data to parent 

    int ic3[3];
    index_.child(level,ic3,ic3+1,ic3+2);

    int n; 
    char * array;
    int if3[3]={0,0,0};
    bool lg3[3]={false,false,false};
    
    std::vector<int> field_list;
    FieldFace * field_face = create_face
      ( if3,ic3,lg3,refresh_coarse,field_list);
    field_face->face_to_array(data()->field(),&n,&array);
    delete field_face;

    const Index index_parent = index_.index_parent();

    // --------------------------------------------------
    // ENTRY: #2 Block::~Block()-> Block::x_refresh_child()
    // ENTRY: parent if level > 0
    // --------------------------------------------------
    thisProxy[index_parent].x_refresh_child(n,array,ic3);
    // --------------------------------------------------

    delete [] array;
  }

  delete data_;
  data_ = 0;

  delete child_data_;
  child_data_ = 0;

  simulation()->monitor_delete_block();

}

//----------------------------------------------------------------------

void Block::x_refresh_child 
(
 int    n, 
 char * buffer, 
 int    ic3[3]
 )
{
  int  if3[3]  = {0,0,0};
  bool lg3[3] = {false,false,false};
  std::vector<int> field_list;
  FieldFace * field_face = create_face
    (if3, ic3, lg3, refresh_coarse,field_list);
  field_face -> array_to_face (buffer, data()->field());
  delete field_face;

}

//----------------------------------------------------------------------

Block::Block (CkMigrateMessage *m)
  : CBase_Block(m),
    data_(NULL),
    child_data_(NULL),
    index_(),
    level_next_(0),
    cycle_(0),
    time_(0.0),
    dt_(0.0),
    stop_(false),
    index_initial_(0),
    children_(),
    sync_coarsen_(),
    count_sync_(),
    max_sync_(),
    face_level_curr_(),
    face_level_next_(),
    child_face_level_curr_(),
    child_face_level_next_(),
    count_coarsen_(0),
    adapt_step_(0),
    adapt_(adapt_unknown),
    coarsened_(false),
    delete_(false),
    is_leaf_(true),
    age_(0),
    face_level_last_(),
    name_(""),
    index_method_(-1)
{ 
  simulation()->monitor_insert_block();
};

//----------------------------------------------------------------------

Simulation * Block::simulation() const
{ return proxy_simulation.ckLocalBranch(); }

//----------------------------------------------------------------------

int Block::rank() const
{ return simulation()->rank(); }

//----------------------------------------------------------------------

std::string Block::name() const throw()
{
  if (name_ != "") {
    return name_;
  } else {
    const int rank = this->rank();
    int blocking[3] = {1,1,1};
    simulation()->hierarchy()->blocking(blocking,blocking+1,blocking+2);
    for (int i=-1; i>=index().level(); i--) {
      blocking[0] /= 2;
      blocking[1] /= 2;
      blocking[2] /= 2;
    }

    int bits[3] = {0,0,0};
    
    blocking[0]--;
    blocking[1]--;
    blocking[2]--;
    if (blocking[0]) do { ++bits[0]; } while (blocking[0]/=2);
    if (blocking[1]) do { ++bits[1]; } while (blocking[1]/=2);
    if (blocking[2]) do { ++bits[2]; } while (blocking[2]/=2);

    std::string name = "B" + index_.bit_string(level(),rank,bits);
    return name;

  }
}

//----------------------------------------------------------------------

void Block::size_forest (int * nx, int * ny, int * nz) const throw ()
{  simulation()->hierarchy()->num_blocks(nx,ny,nz); }

//----------------------------------------------------------------------

void Block::lower
(double * xm, double * ym, double * zm) const throw ()
{
  int  ix, iy, iz;
  int  nx, ny, nz;

  index_global (&ix,&iy,&iz,&nx,&ny,&nz);

  Hierarchy * hierarchy = simulation()->hierarchy();
  double xdm, ydm, zdm;
  hierarchy->lower(&xdm,&ydm,&zdm);
  double xdp, ydp, zdp;
  hierarchy->upper(&xdp,&ydp,&zdp);

  double ax = 1.0*ix/nx;
  double ay = 1.0*iy/ny;
  double az = 1.0*iz/nz;

  double xbm = (1.0-ax)*xdm + ax*xdp;
  double ybm = (1.0-ay)*ydm + ay*ydp;
  double zbm = (1.0-az)*zdm + az*zdp;

  if (xm) (*xm) = xbm;
  if (ym) (*ym) = ybm;
  if (zm) (*zm) = zbm;
  TRACE6 ("DEBUG LOWER %d %d %d  %g %g %g",ix,iy,iz,*xm,*ym,*zm);
}

//----------------------------------------------------------------------

void Block::upper
(double * xp, double * yp, double * zp) const throw ()
{
  int  ix, iy, iz;
  int  nx, ny, nz;

  index_global (&ix,&iy,&iz,&nx,&ny,&nz);

  Hierarchy * hierarchy = simulation()->hierarchy();
  double xdm, ydm, zdm;
  hierarchy->lower(&xdm,&ydm,&zdm);
  double xdp, ydp, zdp;
  hierarchy->upper(&xdp,&ydp,&zdp);

  double ax = 1.0*(ix+1)/nx;
  double ay = 1.0*(iy+1)/ny;
  double az = 1.0*(iz+1)/nz;

  double xbp = (1.0-ax)*xdm + ax*xdp;
  double ybp = (1.0-ay)*ydm + ay*ydp;
  double zbp = (1.0-az)*zdm + az*zdp;

  if (xp) (*xp) = xbp;
  if (yp) (*yp) = ybp;
  if (zp) (*zp) = zbp;
}

//----------------------------------------------------------------------

void Block::cell_width 
(double * dx, double * dy, double * dz) const throw()
{ 
  double xm,ym,zm;
  lower(&xm,&ym,&zm);
  double xp,yp,zp;
  upper(&xp,&yp,&zp);
  data()->field_data()->cell_width(xm,xp,dx, ym,yp,dy, zm,zp,dz);
}

//----------------------------------------------------------------------

void Block::index_global
( int *ix, int *iy, int *iz,
  int *nx, int *ny, int *nz ) const
{
  
  index_forest(ix,iy,iz);
  size_forest (nx,ny,nz);

  Index index = this->index();

  const int level = this->level();

  for (int i=0; i<level; i++) {
    int bx,by,bz;
    index.child(i+1,&bx,&by,&bz);
    if (ix) (*ix) = ((*ix) << 1) | bx;
    if (iy) (*iy) = ((*iy) << 1) | by;
    if (iz) (*iz) = ((*iz) << 1) | bz;
    if (nx) (*nx) <<= 1;
    if (ny) (*ny) <<= 1;
    if (nz) (*nz) <<= 1;
  }
  TRACE6("DEBUG INDEX B %d %d %d  %d %d %d",
	 *ix,*iy,*iz,*nx,*ny,*nz);
}

//----------------------------------------------------------------------

FieldFace * Block::create_face
(int if3[3], int ic3[3], bool lg3[3],
 int refresh_type,
 std::vector<int> & field_list
 )
{
  FieldDescr * field_descr = simulation()->field_descr();
  FieldFace  * field_face = new FieldFace;

  field_face -> set_refresh (refresh_type);
  field_face -> set_child (ic3[0],ic3[1],ic3[2]);
  field_face -> set_face (if3[0],if3[1],if3[2]);
  field_face -> set_ghost(lg3[0],lg3[1],lg3[2]);

  if (field_list.size() == 0) {
    int n = field_descr->field_count();
    field_list.resize(n);
    for (int i=0; i<n; i++) field_list[i] = i;
  }
  field_face->set_field_list(field_list);
  return field_face;
}

//----------------------------------------------------------------------

void Block::is_on_boundary (bool is_boundary[3][2]) const throw()
{

  // Boundary * boundary = simulation()->problem()->boundary();
  // bool periodic = boundary->is_periodic();

  int n3[3];
  size_forest (&n3[0],&n3[1],&n3[2]);
  
  for (int axis=0; axis<3; axis++) {
    for (int face=0; face<2; face++) {
      is_boundary[axis][face] = 
	index_.is_on_boundary(axis,2*face-1,n3[axis]);
    }
  }
}

//======================================================================

void Block::determine_boundary_
(
 bool bndry[3][2],
 bool * fxm, bool * fxp,
 bool * fym, bool * fyp,
 bool * fzm, bool * fzp
 )
{
  // return bndry[] array of faces on domain boundary

  is_on_boundary (bndry);

  int nx,ny,nz;
  data()->field_data()->size (&nx,&ny,&nz);

  // Determine in which directions we need to communicate or update boundary

  if (fxm && bndry[0][0]) *fxm = (nx > 1);
  if (fxp && bndry[0][1]) *fxp = (nx > 1);
  if (fym && bndry[1][0]) *fym = (ny > 1);
  if (fyp && bndry[1][1]) *fyp = (ny > 1);
  if (fzm && bndry[2][0]) *fzm = (nz > 1);
  if (fzp && bndry[2][1]) *fzp = (nz > 1);
}

//----------------------------------------------------------------------

void Block::update_boundary_ ()
{
  bool is_boundary[3][2];
  bool fxm=0,fxp=0,fym=0,fyp=0,fzm=0,fzp=0;

  determine_boundary_(is_boundary,&fxm,&fxp,&fym,&fyp,&fzm,&fzp);

  int index = 0;
  Problem * problem = simulation()->problem();
  Boundary * boundary;

  while ((boundary = problem->boundary(index++))) {
    // Update boundaries
    if ( fxm ) boundary->enforce(this,face_lower,axis_x);
    if ( fxp ) boundary->enforce(this,face_upper,axis_x);
    if ( fym ) boundary->enforce(this,face_lower,axis_y);
    if ( fyp ) boundary->enforce(this,face_upper,axis_y);
    if ( fzm ) boundary->enforce(this,face_lower,axis_z);
    if ( fzp ) boundary->enforce(this,face_upper,axis_z);
  }
}

//----------------------------------------------------------------------

void Block::facing_child_(int jc3[3], const int ic3[3], const int if3[3]) const
{
  jc3[0] = if3[0] ? 1 - ic3[0] : ic3[0];
  jc3[1] = if3[1] ? 1 - ic3[1] : ic3[1];
  jc3[2] = if3[2] ? 1 - ic3[2] : ic3[2];
  TRACE9("facing_child %d %d %d  child %d %d %d  face %d %d %d",
	 jc3[0],jc3[1],jc3[2],ic3[0],ic3[1],ic3[2],if3[0],if3[1],if3[2]);
}

//----------------------------------------------------------------------

void Block::copy_(const Block & block) throw()
{
  data_->copy_(*block.data());
  if (child_data_) child_data_->copy_(*block.child_data());

  cycle_      = block.cycle_;
  time_       = block.time_;
  dt_         = block.dt_;
  stop_       = block.stop_;
  adapt_step_ = block.adapt_step_;
  adapt_      = block.adapt_;
  coarsened_  = block.coarsened_;
  delete_     = block.delete_;
}

//----------------------------------------------------------------------

Index Block::neighbor_ 
(
 const int of3[3],
 Index *   ind
 ) const
{
  Index index = (ind != 0) ? (*ind) : index_;

  int na3[3];
  size_forest (&na3[0],&na3[1],&na3[2]);
  // const bool periodic  = simulation()->problem()->boundary()->is_periodic();
  Index in = index.index_neighbor (of3,na3);
  return in;
}

//----------------------------------------------------------------------

void Block::performance_start_
(int index_region, std::string file, int line)
{
  simulation()->performance()->start_region(index_region,file,line);
}

//----------------------------------------------------------------------

void Block::performance_stop_
(int index_region, std::string file, int line)
{
  simulation()->performance()->stop_region(index_region,file,line);
}

//----------------------------------------------------------------------

void Block::performance_switch_
(int index_region, std::string file, int line)
{
  simulation()->performance()->switch_region(index_region,file,line);
}

//----------------------------------------------------------------------

void Block::check_leaf_()
{
  if ((  is_leaf() && children_.size() != 0) ||
      (! is_leaf() && children_.size() == 0)) {

    WARNING4("Block::refresh_begin_()",
	     "%s: is_leaf() == %s && children_.size() == %d"
	     "setting is_leaf_ <== %s",
	     name_.c_str(), is_leaf()?"true":"false",
	     children_.size(),is_leaf()?"false":"true");
    is_leaf_ = ! is_leaf();
  }
}


//----------------------------------------------------------------------

void Block::check_delete_()
{
  if (delete_) {
    WARNING1("refresh_begin_()",
	     "%s: refresh called on deleted Block",
	     name_.c_str());
    return;
  }
}

//----------------------------------------------------------------------

void Block::debug_faces_(const char * mesg)
{
#ifndef DEBUG_ADAPT
  return;
#endif

#ifdef CELLO_DEBUG
  FILE * fp_debug = simulation()->fp_debug();
#endif
  TRACE_ADAPT(mesg);
  int if3[3] = {0};
  int ic3[3] = {0};

  for (ic3[1]=1; ic3[1]>=0; ic3[1]--) {
    for (if3[1]=1; if3[1]>=-1; if3[1]--) {

      index_.print(mesg,-1,2,true,simulation());

      for (if3[0]=-1; if3[0]<=1; if3[0]++) {
#ifdef CELLO_DEBUG
	fprintf (fp_debug,(ic3[1]==1) ? "%d " : "  ",face_level(if3));
#endif
	PARALLEL_PRINTF ((ic3[1]==1) ? "%d " : "  ",face_level(if3));
      }
#ifdef CELLO_DEBUG
      fprintf (fp_debug,"| ");
#endif
      PARALLEL_PRINTF ("| ") ;
      for (if3[0]=-1; if3[0]<=1; if3[0]++) {
#ifdef CELLO_DEBUG
	fprintf (fp_debug,(ic3[1]==1) ? "%d " : "  ",face_level_next(if3));
#endif
	PARALLEL_PRINTF ((ic3[1]==1) ? "%d " : "  ",face_level_next(if3));
      }
#ifdef CELLO_DEBUG
      fprintf (fp_debug,"| ");
#endif
      PARALLEL_PRINTF ("| ");
      for (ic3[0]=0; ic3[0]<2; ic3[0]++) {
	for (if3[0]=-1; if3[0]<=1; if3[0]++) {
	  for (if3[0]=-1; if3[0]<=1; if3[0]++) {
#ifdef CELLO_DEBUG
	    fprintf (fp_debug,"%d ",child_face_level(ic3,if3));
#endif
	    PARALLEL_PRINTF ("%d ",child_face_level(ic3,if3));
	  }
	}
      }
#ifdef CELLO_DEBUG
      fprintf (fp_debug,"| ");
#endif
      PARALLEL_PRINTF ("| ");
      for (ic3[0]=0; ic3[0]<2; ic3[0]++) {
	for (if3[0]=-1; if3[0]<=1; if3[0]++) {
	  for (if3[0]=-1; if3[0]<=1; if3[0]++) {
#ifdef CELLO_DEBUG
	    fprintf (fp_debug,"%d ",child_face_level_next(ic3,if3));
#endif
	    PARALLEL_PRINTF ("%d ",child_face_level_next(ic3,if3));
	  }
	}
      }
#ifdef CELLO_DEBUG
      fprintf (fp_debug,"\n");
      fflush(fp_debug);
#endif
      PARALLEL_PRINTF ("\n");
      fflush(stdout);

    }
  }
}

