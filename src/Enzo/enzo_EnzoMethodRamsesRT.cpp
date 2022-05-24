// See LICENSE_CELLO file for license and copyright information

/// @file     enzo_EnzoMethodRamsesRT.cpp
/// @author   William Hicks (whicks@ucsd.edu)
/// @date     Mon Aug 16 17:05:23 PDT 2021
/// @brief    Implements the EnzoMethodRamsesRT  class

#include "cello.hpp"

#include "enzo.hpp"

//#define DEBUG_TURN_OFF_ATTENUATION
//#define DEBUG_TURN_OFF_CHEMISTRY

//#define DEBUG_PRINT_GROUP_PARAMETERS
//#define DEBUG_RECOMBINATION
//#define DEBUG_ALPHA
//#define DEBUG_RATES
#define DEBUG_INJECTION
//#define DEBUG_TRANSPORT
//#define DEBUG_ATTENUATION

//----------------------------------------------------------------------

EnzoMethodRamsesRT ::EnzoMethodRamsesRT(const int N_groups, const double clight)
  : Method()
    , N_groups_(N_groups)
    , clight_(clight)
    , eps_()
    , sigN_()
    , sigE_()
    , gfracN_()
    , gfracF_()
    , ir_injection_(-1)
    , ir_transport_(-1)
    //, igroup_()
{

  const int rank = cello::rank();

  this->required_fields_ = std::vector<std::string> {"photon_density"}; //number density

  if (rank >= 1) {
    this->required_fields_.push_back("flux_x");
    this->required_fields_.push_back("P00"); // elements of the pressure tensor
  }                                         
  if (rank >= 2) {
    this->required_fields_.push_back("flux_y");
    this->required_fields_.push_back("P10");
    this->required_fields_.push_back("P01");
    this->required_fields_.push_back("P11");
  }
  if (rank >= 3) {
    this->required_fields_.push_back("flux_z");
    this->required_fields_.push_back("P02");
    this->required_fields_.push_back("P12");
    this->required_fields_.push_back("P20");
    this->required_fields_.push_back("P21");
    this->required_fields_.push_back("P22");
  }


  for (int i=0; i<N_groups_; i++) {
    std::string istring = std::to_string(i); 
    this->required_fields_.push_back("photon_density_" + istring);
    if (rank >= 1) this->required_fields_.push_back("flux_x_" + istring);
    if (rank >= 2) this->required_fields_.push_back("flux_y_" + istring);
    if (rank >= 3) this->required_fields_.push_back("flux_z_" + istring);   
  }

  // define required fields if they do not exist
  this->define_fields();

  // Initialize default Refresh object

  cello::simulation()->new_refresh_set_name(ir_post_,name());
  Refresh * refresh = cello::refresh(ir_post_);
  refresh->add_field("photon_density");

  if (rank >= 1) refresh->add_field("flux_x");
  if (rank >= 2) refresh->add_field("flux_y");
  if (rank >= 3) refresh->add_field("flux_z");

  for (int i=0; i<N_groups_; i++) {
    std::string istring = std::to_string(i); 
    refresh->add_field("photon_density_" + istring);
    if (rank >= 1) refresh->add_field("flux_x_" + istring);
    if (rank >= 2) refresh->add_field("flux_y_" + istring);
    if (rank >= 3) refresh->add_field("flux_z_" + istring);   
  }

  //TODO: Add specific fields rather than refreshing everything here
  //      need to add chemistry fields and maybe ionization/heating rates
  //
  
  refresh->add_all_fields();
  // Initialize Refresh object for after injection step 
  ir_injection_ = add_new_refresh_();

  cello::simulation()->new_refresh_set_name(ir_injection_, name()+":injection");
  Refresh * refresh_injection = cello::refresh(ir_injection_);
  refresh_injection->add_field("photon_density");

  if (rank >= 1) refresh_injection->add_field("flux_x");
  if (rank >= 2) refresh_injection->add_field("flux_y");
  if (rank >= 3) refresh_injection->add_field("flux_z");
  
  for (int i=0; i<N_groups_; i++) {
    std::string istring = std::to_string(i); 
    refresh_injection->add_field("photon_density_" + istring);
    if (rank >= 1) refresh_injection->add_field("flux_x_" + istring);
    if (rank >= 2) refresh_injection->add_field("flux_y_" + istring);
    if (rank >= 3) refresh_injection->add_field("flux_z_" + istring);   
  }
 
  refresh_injection->set_callback(CkIndex_EnzoBlock::p_method_ramses_rt_solve_transport_eqn()); 
  
  // store frequency group attributes as ScalarData variables
  ScalarDescr * scalar_descr = cello::scalar_descr_double();
  
  int N_species_ = 3; //only three ionizable species (HI, HeI, HeII)
  for (int i=0; i<N_groups_; i++) {
    eps_ .push_back( scalar_descr->new_value( eps_string(i) ));
    for (int j=0; j<N_species_; j++) {
      std::string jstring = std::to_string(j);
      sigN_.push_back( scalar_descr->new_value( sigN_string(i,j) ));
      sigE_.push_back( scalar_descr->new_value( sigE_string(i,j) ));
    }
  }

}

//----------------------------------------------------------------------

void EnzoMethodRamsesRT ::pup (PUP::er &p)
{

  // NOTE: change this function whenever attributes change

  TRACEPUP;

  Method::pup(p);

  p | N_groups_;
  p | clight_;
  p | eps_;
  p | sigN_;
  p | sigE_;
  p | gfracN_;
  p | gfracF_;
  p | ir_injection_;
  p | ir_transport_;
  //p | igroup_;
}

//----------------------------------------------------------------------

void EnzoMethodRamsesRT::compute ( Block * block) throw()
{

  if (block->is_leaf()) {

    compute_ (block);
  }

  //block->compute_done();
}

//----------------------------------------------------------------------

double EnzoMethodRamsesRT::timestep ( Block * block ) const throw()
{
  Data * data = block->data();
  Field field = data->field();

  int mx,my,mz;
  field.dimensions (0,&mx,&my,&mz);
  const int rank = ((mz == 1) ? ((my == 1) ? 1 : 2) : 3);

  double hx,hy,hz;
  block->cell_width(&hx,&hy,&hz);

  double h_min = std::numeric_limits<double>::max();
  if (rank >= 1) h_min = std::min(h_min,hx);
  if (rank >= 2) h_min = std::min(h_min,hy);
  if (rank >= 3) h_min = std::min(h_min,hz);

  const EnzoConfig * enzo_config = enzo::config();
  EnzoUnits * enzo_units = enzo::units();

  return h_min / (3*enzo_config->method_ramses_rt_clight_frac*cello::clight / enzo_units->velocity());
}

//-----------------------------------------------------------------------

double EnzoMethodRamsesRT::integrate_simpson(double a, double b,
                    int n, // Number of intervals
                    std::function<double(double,double,double,int)> f, double v1, double v2, int v3) throw()
{
    //TODO (?): The elegant thing to do here would be to somehow let the parameter f be a 
    //          variadic function, and take va_list as a parameter instead of (v1,v2,v3,...). 
    //          If I'm only using this to integrate planck functions though, I probably don't need to 
    //          waste time making this as general as possible
              
    // solve 1D integral using composite simpson's rule
    double h = (b - a) / n;

    // Internal sample points, there should be n - 1 of them
    double sum_odds = 0.0;
    for (int i = 1; i < n; i += 2)
    {
        sum_odds += f(a + i * h, v1, v2, v3);
    }
    double sum_evens = 0.0;
    for (int i = 2; i < n; i += 2)
    {
        sum_evens += f(a + i * h, v1, v2, v3);
    }

    return (f(a,v1,v2,v3) + f(b,v1,v2,v3) + 2 * sum_evens + 4 * sum_odds) * h / 3;
}

double EnzoMethodRamsesRT::planck_function(double nu, double T, double clight, int dependent_variable) throw()
{
  double prefactor = 0.0;
  switch (dependent_variable)
  {
    case 0: //photon density
       prefactor = 8*cello::pi*nu*nu / (clight*clight*clight); 
       break;
    case 1: //energy density
       prefactor = 8*cello::pi*cello::hplanck*nu*nu*nu / (clight*clight*clight); 
       break;
  }
  
  return prefactor / ( std::exp(cello::hplanck*nu/(cello::kboltz*T)) -1 );

}

//-------------------------- INJECTION STEP ----------------------------
double EnzoMethodRamsesRT::get_star_temperature(double M) throw()
{
  double L, R;
  // mass-luminosity relations for main sequence stars
  if      (M/cello::mass_solar < 0.43) L = 0.23*pow(M/cello::mass_solar,2.3);
  else if (M/cello::mass_solar < 2   ) L =      pow(M/cello::mass_solar,4.0);
  else if (M/cello::mass_solar < 55  ) L =  1.4*pow(M/cello::mass_solar,3.5);
  else L = 32000*(M/cello::mass_solar);

  // mass-radius relations (need to find more accurate version for large masses?)
  if (M/cello::mass_solar < 1) R = pow(M/cello::mass_solar, 0.8);
  else R = pow(M/cello::mass_solar, 0.57);
  
  L *= cello::luminosity_solar;
  R *= cello::radius_solar;

  return pow( L/(4*cello::pi*R*R*cello::sigma_SF), 0.25);
}

void EnzoMethodRamsesRT::get_radiation_flat(EnzoBlock * enzo_block, enzo_float * N, int i, double energy,
           double dt, double inv_vol) throw()
{
  // 
  // energy is passed in with units of eV
  EnzoUnits * enzo_units = enzo::units();
  double lunit = enzo_units->length();
  double munit = enzo_units->mass();
  double vunit = enzo_units->velocity();
  double eunit = munit*vunit*vunit;

  const EnzoConfig * enzo_config = enzo::config();

  Scalar<double> scalar = enzo_block->data()->scalar_double();
 
  double intensity = enzo_config->method_ramses_rt_Nphotons_per_sec * enzo_units->time(); 

  // TODO: This loop only goes through primordial ionizable species. Update once support for
  //       > 6 species is added
  int igroup = enzo_block->method_ramses_rt_igroup;
  for (int j; j<3; j++) {
    double sigma_j = sigma_vernier(energy,j); // cm^2

    #ifdef DEBUG_INJECTION
      CkPrintf("MethodRamsesRT::get_radiation_flat -- j = %d; energy = %f eV; sigma_j = %1.2e cm^2 \n", j, energy, sigma_j);
    #endif

    *(scalar.value( scalar.index(sigN_string(igroup, j) )))
                                       = sigma_j / (lunit*lunit);
    *(scalar.value( scalar.index(sigE_string(igroup, j) )))
                                       = sigma_j / (lunit*lunit);
    *(scalar.value( scalar.index( eps_string(igroup   ) )))
                                       = energy * cello::erg_eV / eunit;
  }

  N[i] = intensity * inv_vol * dt;
  #ifdef DEBUG_INJECTION
    CkPrintf("MethodRamsesRT::get_radiation_flat -- N[i] = %1.2e code_length^-3 \n", N[i] / (lunit*lunit*lunit));
  #endif

}

// -------

void EnzoMethodRamsesRT::get_radiation_blackbody(EnzoBlock * enzo_block, enzo_float * N, int i, double T, 
                   double freq_lower, double freq_upper, double clight, double f_esc) throw()
{
  // Does all calculations in CGS, then converts to whatever unit system at the end
  EnzoUnits * enzo_units = enzo::units();
  double lunit = enzo_units->length(); 
  double tunit = enzo_units->time();
  double eunit = enzo_units->mass() * lunit*lunit / (tunit*tunit);

  int n = 10; // number of partitions for simpson's method
              // TODO: Make this a parameter??

  //need to use lambda expression to pass in planck_function() function as a parameter
  //because member function in c++ are automatically attached to the `this` pointer,
  //so you need to capture `this` in order for the compiler to know which function
  //to point to
  
  if (freq_lower == 0.0) freq_lower = 1; //planck function undefined at zero
                                         //1 Hz is a very small frequency compared to ~1e16 Hz
                                         
  double N_integrated = integrate_simpson(freq_lower,freq_upper,n, 
                 [this](double a, double b, double c, int d) {return planck_function(a,b,c,d);},
                 T,clight,0); 
  double E_integrated = integrate_simpson(freq_lower,freq_upper,n, 
                 [this](double a, double b, double c, int d) {return planck_function(a,b,c,d);},
                 T,clight,1);
   
   //for testing
   //N[i] = 1e16;
   std::vector<std::string> chemistry_fields = {"HI_density", 
                                               "HeI_density", "HeII_density"};
   std::vector<double> masses = {cello::mass_hydrogen,
                      4*cello::mass_hydrogen, 4*cello::mass_hydrogen};

   //Calculate photon group attributes----
#ifdef DEBUG_PRINT_GROUP_PARAMETERS
   if (enzo_block->method_ramses_rt_igroup > 0) std::cout << "~~~~~~" << std::endl;
#endif
   Scalar<double> scalar = enzo_block->data()->scalar_double(); 
 
   //eq. B3 ----> int(E_nu dnu) / int(N_nu dnu)
   *(scalar.value( scalar.index( eps_string(enzo_block->method_ramses_rt_igroup) ) ))
                              =
                 E_integrated / N_integrated / eunit;
   
 

   for (int j=0; j<chemistry_fields.size(); j++) {

     //TODO: Need to average sigma_ij over all stars in the simulation. See eq. B6-B8
     //      Could be more computationally efficient in Enzo-E/Cello to average over
     //      star particles in each block separately and leave it at that.
  
     // eq. B4 ----> int(sigma_nuj * N_nu dnu)/int(N_nu dnu)
     *(scalar.value( scalar.index(sigN_string(enzo_block->method_ramses_rt_igroup, j) ) )) 
                                     =
            integrate_simpson(freq_lower,freq_upper,n, 
                 [this,j](double nu, double b, double c, int d){
                   return sigma_vernier(cello::hplanck*nu / cello::erg_eV, j)*planck_function(nu,b,c,d);
                 },
                 T,clight,0) / N_integrated / (lunit*lunit);

     // eq. B5 ----> int(sigma_nuj * E_nu dnu)/int(E_nu dnu)
     *(scalar.value( scalar.index(sigE_string(enzo_block->method_ramses_rt_igroup, j)) ))
                                     =
            integrate_simpson(freq_lower,freq_upper,n, 
                 [this,j](double nu, double b, double c, int d){
                   return sigma_vernier(cello::hplanck*nu / cello::erg_eV, j)*planck_function(nu,b,c,d);
                 },
                 T,clight,1) / E_integrated / (lunit*lunit);


#ifdef DEBUG_PRINT_GROUP_PARAMETERS

     const EnzoConfig * enzo_config = enzo::config();
     
     std::cout<<'['  << enzo_config->method_ramses_rt_bin_lower[enzo_block->method_ramses_rt_igroup] 
              << ", " << enzo_config->method_ramses_rt_bin_upper[enzo_block->method_ramses_rt_igroup]
              << "] " << j
              << ' '  << *(scalar.value( scalar.index("sigN_" + 
                          std::to_string(enzo_block->method_ramses_rt_igroup) + std::to_string(j)) ))
              << ' '  << *(scalar.value( scalar.index("sigE_" + 
                          std::to_string(enzo_block->method_ramses_rt_igroup) + std::to_string(j)) )) 
              << ' '  << *(scalar.value( scalar.index("eps_" + 
                          std::to_string(enzo_block->method_ramses_rt_igroup) ))) / cello::erg_eV
              << std::endl;  
#endif         


   // for testing
   //*(scalar.value( scalar.index("sigN_" + std::to_string(enzo_block->method_ramses_rt_igroup) 
   //                           + std::to_string(j)) )) = enzo_block->method_ramses_rt_igroup * 2e-14; 

  }
  //----------------------------------
  
  N[i] = f_esc * N_integrated * lunit*lunit*lunit;

  #ifdef DEBUG_INJECTION
    CkPrintf("MethodRamsesRT::get_radiation_blackbody -- N[i] = %1.2e code_length^-3, T = %1.2e K \n", N[i], T);
  #endif



}

// ----

void EnzoMethodRamsesRT::inject_photons ( EnzoBlock * enzo_block ) throw()
{
  // Solve dN_i/dt = Ndot^*_i
  // routine for identifying star particles and getting their grid position
  // copy/pasted from enzo_EnzoMethodFeedback::compute_()

  const EnzoConfig * enzo_config = enzo::config();
  EnzoUnits * enzo_units = enzo::units();
  double f_esc = 1.0; // enzo_config->method_radiation_injection_escape_fraction;

  Field field = enzo_block->data()->field();
  int mx,my,mz;  
  field.dimensions(0,&mx, &my, &mz); //field dimensions, including ghost zones
  int gx,gy,gz;
  field.ghost_depth(0,&gx, &gy, &gz);
  
  int idx = 1;
  int idy = mx;
  int idz = mx*my;
 
  const int rank = ((mz == 1) ? ((my == 1) ? 1 : 2) : 3);
 
  double xm,ym,zm;
  double xp,yp,zp;
  double hx,hy,hz;
  enzo_block->data()->lower(&xm,&ym,&zm);
  enzo_block->data()->upper(&xp,&yp,&zp);
  field.cell_width(xm,xp,&hx,ym,yp,&hy,zm,zp,&hz);

  // if (cosmology) {...}
 
  double inv_vol = 1.0/(hx*hy*hz); 

  double dt = enzo_block->dt;

  // get relevant field variables
  enzo_float * N  = (enzo_float *) field.values(
        "photon_density_"+std::to_string(enzo_block->method_ramses_rt_igroup));
 

  /*
  enzo_float * Fx  = (enzo_float *) field.values(
        "flux_x_"+std::to_string(enzo_block->method_ramses_rt_igroup));
  enzo_float * Fy  = (enzo_float *) field.values(
        "flux_y_"+std::to_string(enzo_block->method_ramses_rt_igroup));
  enzo_float * Fz  = (enzo_float *) field.values(
        "flux_z_"+std::to_string(enzo_block->method_ramses_rt_igroup));
  */

  Particle particle = enzo_block->data()->particle();
  int it = particle.type_index("star");
  
  // if no stars, don't do anything
  if (particle.num_particles(it) == 0) return;

  const int ia_m = particle.attribute_index (it, "mass");

  const int ia_x = (rank >= 1) ? particle.attribute_index (it, "x") : -1;
  const int ia_y = (rank >= 2) ? particle.attribute_index (it, "y") : -1;
  const int ia_z = (rank >= 3) ? particle.attribute_index (it, "z") : -1;

  const int dm = particle.stride(it, ia_m);
  const int dp = particle.stride(it, ia_x);

  const int nb = particle.num_batches(it);

  // bin energies in eV
  double E_lower = (enzo_config->method_ramses_rt_bin_lower)[enzo_block->method_ramses_rt_igroup];
  double E_upper = (enzo_config->method_ramses_rt_bin_upper)[enzo_block->method_ramses_rt_igroup];
  double E_mean = 0.5*(E_upper + E_lower); 

  // convert energies to frequency in Hz
  double freq_lower = E_lower * cello::erg_eV / cello::hplanck;
  double freq_upper = E_upper * cello::erg_eV / cello::hplanck;

  double clight = enzo_config->method_ramses_rt_clight_frac * cello::clight; 
  std::string radiation_spectrum = enzo_config->method_ramses_rt_radiation_spectrum; 

  for (int ib=0; ib<nb; ib++){
    enzo_float *px=0, *py=0, *pz=0;
    enzo_float *pmass=0;
   
    pmass = (enzo_float *) particle.attribute_array(it, ia_m, ib);

    px = (enzo_float *) particle.attribute_array(it, ia_x, ib);
    py = (enzo_float *) particle.attribute_array(it, ia_y, ib);
    pz = (enzo_float *) particle.attribute_array(it, ia_z, ib);

    int np = particle.num_particles(it,ib);

    // loop through particles within each batch
    for (int ip=0; ip<np; ip++) {
      int ipdp = ip*dp;
      int ipdm = ip*dm;

      //TODO: Add check for whether star particle is still active (has it blown up yet?)
        
      // get corresponding grid position
      double x_part = (px[ipdp] - xm) / hx;
      double y_part = (py[ipdp] - ym) / hy;
      double z_part = (pz[ipdp] - zm) / hz;

      // get 3D grid index for particle - account for ghost zones!!
      int ix = ((int) std::floor(x_part))  + gx;
      int iy = ((int) std::floor(y_part))  + gy;
      int iz = ((int) std::floor(z_part))  + gz;

      // now get index of this cell
      int i = INDEX(ix,iy,iz,mx,my);
      
      // deposit photons
       
      // This function will take in particle mass as a parameter, and will fit
      // and integrate over the SED to get the total injection rate. The current
      // implementation in RAMSES assumes that SED of gas stays constant.
      // How do I keep track of the gas SED if different mass stars radiate differently?
      
      // Get temperature of star 

      double T = get_star_temperature(pmass[ipdm]*enzo_units->mass());

      //TODO: Enforce that photon density stellar spectrum is the minimum value
      //      for cells that contain stars. Necessary because after the transport step,
      //      N[cell with star] can drop to zero after the transport step, which is 
      //      unphysical.
      if (radiation_spectrum == "blackbody") {
        get_radiation_blackbody(enzo_block, N, i, T, freq_lower, freq_upper, clight,f_esc);
      }
      else if (radiation_spectrum == "flat") {
        get_radiation_flat(enzo_block, N, i, E_mean, dt, inv_vol);
      }
     
      //TODO: Only call get_cross_section here every N cycles, where N is an input parameter.
      //      Also only call if igroup=0 so that it only gets called once
      //
     
      //TODO: Add flux to more than one cell. Will have to use refresh+accumulate to handle edge cases

      /*
      // initialize fluxes within a radius of one cell
      for (int k_=iz-1; k_<=iz+1;k_++) {
        for (int j_=iy-1; j_<=iy+1;j_++) {
          for (int i_=ix-1; i_<=ix+1;i_++) {
            int index = INDEX(i_,j_,k_,mx,my);
            if (index == i) continue;
            double distsqr = (i_-ix)*(i_-ix)*hx*hx + (j_-iy)*(i_-iy)*hy*hy + (k_-iz)*(k_-iz)*hz*hz;
                        
            N[index]  = N[i]/27;//distsqr;
            
            Fx[index] = (i_-ix); //i_-ix is either +- 1, so this gives direction of flux
            Fy[index] = (j_-iy);
            Fz[index] = (k_-iz);
          
            // normalize fluxes 
            double Ftot = Fx[index]*Fx[index] + Fy[index]*Fy[index] + Fz[index]*Fz[index];                      
            Fx[index] *= clight*N[index]/Ftot; 
            Fy[index] *= clight*N[index]/Ftot;
            Fz[index] *= clight*N[index]/Ftot; 
         }
        }
      }
      */
//      N[i] += f_esc * injection_rate * dt * inv_vol; // photons/s * timestep / volume

 
      // I don't have to directly alter the fluxes here because that naturally gets
      // taken care of during the transport step 
      
    }
  } 
}

//---------------------------------------------------------------------

//----------------------------------------------------------------------

double EnzoMethodRamsesRT::flux_function (double U_l, double U_lplus1,
					   double Q_l, double Q_lplus1,double clight,  
					   std::string type) 
					   throw()
{
  // returns face-flux of a cell at index idx
  // TODO: Add HLL flux function
  const EnzoConfig * enzo_config = enzo::config();
  if (type == "GLF") {
    return 0.5*(  Q_l+Q_lplus1 - clight*(U_lplus1-U_l) ); 
  }

  else {
    ERROR("EnzoMethodRamsesRT::flux_function",
	     "flux_function type not recognized");
    return 0.0; 
  }
}

double EnzoMethodRamsesRT::deltaQ_faces (double U_l, double U_lplus1, double U_lminus1,
                                                  double Q_l, double Q_lplus1, double Q_lminus1,
                                                  double clight) throw()
{
  // calls flux_function(), and calculates Q_{i-1/2} - Q_{i+1/2}
  
  return flux_function(U_lminus1, U_l, Q_lminus1, Q_l, clight, "GLF") - flux_function(U_l, U_lplus1, Q_l, Q_lplus1,clight, "GLF"); 
}



void EnzoMethodRamsesRT::get_reduced_variables (double * chi, double (*n)[3], int i, double clight,
                               enzo_float * N, enzo_float * Fx, enzo_float * Fy, enzo_float * Fz) throw()
{
        double Fnorm = sqrt(Fx[i]*Fx[i] + Fy[i]*Fy[i] + Fz[i]*Fz[i]);
        double f = Fnorm / (clight*N[i]); // reduced flux
        *chi = (3 + 4*f*f) / (5 + 2*sqrt(4-3*f*f));
        (*n)[0] = Fx[i]/Fnorm;
        (*n)[1] = Fy[i]/Fnorm; 
        (*n)[2] = Fz[i]/Fnorm;
}

void EnzoMethodRamsesRT::get_pressure_tensor (EnzoBlock * enzo_block, 
                       enzo_float * N, enzo_float * Fx, enzo_float * Fy, enzo_float * Fz, double clight) 
                       throw()
{
  Field field = enzo_block->data()->field();
  int mx,my,mz;  
  field.dimensions(0,&mx, &my, &mz); //field dimensions, including ghost zones
  int gx,gy,gz;
  field.ghost_depth(0,&gx, &gy, &gz);

  // if rank >= 1
  enzo_float * P00 = (enzo_float *) field.values("P00");
  // if rank >= 2
  enzo_float * P10 = (enzo_float *) field.values("P10");
  enzo_float * P01 = (enzo_float *) field.values("P01");
  enzo_float * P11 = (enzo_float *) field.values("P11");
  // if rank >= 3
  enzo_float * P02 = (enzo_float *) field.values("P02");
  enzo_float * P12 = (enzo_float *) field.values("P12");
  enzo_float * P20 = (enzo_float *) field.values("P20");
  enzo_float * P21 = (enzo_float *) field.values("P21"); 
  enzo_float * P22 = (enzo_float *) field.values("P22"); 

  // Need to directly calculate pressure tensor elements 
  // one layer deep into the ghost zones because active cells
  // need information about their neighbors, and there's no 
  // guarantee that a neighboring block will have updated
  // its pressure tensor by the time this block starts

  for (int iz=gz-1; iz<mz-gz+1; iz++) { 
   for (int iy=gy-1; iy<my-gy+1; iy++) {
    for (int ix=gx-1; ix<mx-gx+1; ix++) {
      int i = INDEX(ix,iy,iz,mx,my); //index of current cell
      double chi, n[3]; 
      get_reduced_variables( &chi, &n, i, clight, 
                            N, Fx, Fy, Fz);
      P00[i] = N[i] * ( 0.5*(1.0-chi) + 0.5*(3.0*chi - 1)*n[0]*n[0] );
      P10[i] = N[i] * 0.5*(3.0*chi - 1)*n[1]*n[0];
      P01[i] = N[i] * 0.5*(3.0*chi - 1)*n[0]*n[1];
      P11[i] = N[i] * ( 0.5*(1.0-chi) + 0.5*(3.0*chi - 1)*n[1]*n[1] );
      P02[i] = N[i] * 0.5*(3.0*chi - 1)*n[0]*n[2];
      P12[i] = N[i] * 0.5*(3.0*chi - 1)*n[1]*n[2];
      P20[i] = N[i] * 0.5*(3.0*chi - 1)*n[2]*n[0];
      P21[i] = N[i] * 0.5*(3.0*chi - 1)*n[2]*n[1];
      P22[i] = N[i] * ( 0.5*(1.0-chi) + 0.5*(3.0*chi - 1)*n[2]*n[2] );
    }
   }
  }

}

void EnzoMethodRamsesRT::get_U_update (EnzoBlock * enzo_block, double * N_update, 
                       double * Fx_update, double * Fy_update, double * Fz_update, 
                       enzo_float * N, enzo_float * Fx, enzo_float * Fy, enzo_float * Fz,
                       double hx, double hy, double hz, double dt, double clight, 
                       int i, int idx, int idy, int idz) throw()
{
  Field field = enzo_block->data()->field();
  // if rank >= 1
  enzo_float * P00 = (enzo_float *) field.values("P00");
  // if rank >= 2
  enzo_float * P10 = (enzo_float *) field.values("P10");
  enzo_float * P01 = (enzo_float *) field.values("P01");
  enzo_float * P11 = (enzo_float *) field.values("P11");
  // if rank >= 3
  enzo_float * P02 = (enzo_float *) field.values("P02");
  enzo_float * P12 = (enzo_float *) field.values("P12");
  enzo_float * P20 = (enzo_float *) field.values("P20");
  enzo_float * P21 = (enzo_float *) field.values("P21"); 
  enzo_float * P22 = (enzo_float *) field.values("P22");  

  // if rank >= 1
  *N_update += dt/hx * deltaQ_faces( N[i],  N[i+idx],  N[i-idx],
                                   Fx[i], Fx[i+idx], Fx[i-idx], clight );
    
  *Fx_update += dt/hx * deltaQ_faces(Fx[i], Fx[i+idx], Fx[i-idx],
                                   clight*clight*P00[i],
                                   clight*clight*P00[i+idx],
                                   clight*clight*P00[i-idx], clight );

  // if rank >= 2
  *N_update += dt/hy * deltaQ_faces( N[i],  N[i+idy],  N[i-idy],
                                   Fy[i], Fy[i+idy], Fy[i-idy], clight ); 
    
  *Fx_update += dt/hy * deltaQ_faces(Fx[i], Fx[i+idy], Fx[i-idy],
                                   clight*clight*P10[i],
                                   clight*clight*P10[i+idy],
                                   clight*clight*P10[i-idy], clight );

  *Fy_update += dt/hx * deltaQ_faces(Fy[i], Fy[i+idx], Fy[i-idx],
                                   clight*clight*P01[i],
                                   clight*clight*P01[i+idx],
                                   clight*clight*P01[i-idx], clight );

  *Fy_update += dt/hy * deltaQ_faces(Fy[i], Fy[i+idy], Fy[i-idy],
                                   clight*clight*P11[i],
                                   clight*clight*P11[i+idy],
                                   clight*clight*P11[i-idy], clight );


   // if rank >= 3
  *N_update += dt/hz * deltaQ_faces( N[i],  N[i+idz],  N[i-idz],
                                   Fz[i], Fz[i+idz], Fz[i-idz], clight );

  *Fx_update += dt/hz * deltaQ_faces(Fx[i], Fx[i+idz], Fx[i-idz],
                                   clight*clight*P20[i],
                                   clight*clight*P20[i+idz],
                                   clight*clight*P20[i-idz], clight );

  *Fy_update += dt/hz * deltaQ_faces(Fy[i], Fy[i+idz], Fy[i-idz],
                                   clight*clight*P21[i],
                                   clight*clight*P21[i+idz],
                                   clight*clight*P21[i-idz], clight);

  *Fz_update += dt/hx * deltaQ_faces(Fz[i], Fz[i+idx], Fz[i-idx],
                                   clight*clight*P02[i],
                                   clight*clight*P02[i+idx],
                                   clight*clight*P02[i-idx], clight);

  *Fz_update += dt/hy * deltaQ_faces(Fz[i], Fz[i+idy], Fz[i-idy],
                                   clight*clight*P12[i],
                                   clight*clight*P12[i+idy],
                                   clight*clight*P12[i-idy], clight);

  *Fz_update += dt/hz * deltaQ_faces(Fz[i], Fz[i+idz], Fz[i-idz],
                                   clight*clight*P22[i],
                                   clight*clight*P22[i+idz],
                                   clight*clight*P22[i-idz], clight);


}

//----------------------------------
double EnzoMethodRamsesRT::sigma_vernier (double energy, int type) throw()
{
  // copy FindCrossSection.C from Enzo
  // Uses fits from Vernier et al. (1996) to calculate photoionization cross-section 
  // between photons of energy E and gas of a given species. Then need to average this
  // value over  
 
  double sigma;
  double e_th, e_max, e0, sigma0, ya, P, yw, y0, y1;

  switch (type) { 
    // HI
  case 0:
    e_th = 13.6;
    e_max = 5e4;
    e0 = 4.298e-1;
    sigma0 = 5.475e4;
    ya = 32.88;
    P = 2.963;
    yw = y0 = y1 = 0.0;
    break;

    // HeI
  case 1:
    e_th = 24.59;
    e_max = 5e4;
    e0 = 13.61;
    sigma0 = 9.492e2;
    ya = 1.469;
    P = 3.188;
    yw = 2.039;
    y0 = 0.4434;
    y1 = 2.136;
    break;
  
    // HeII
  case 2:
    e_th = 54.42;
    e_max = 5e4;
    e0 = 1.720;
    sigma0 = 1.369e4;
    ya = 32.88;
    P = 2.963;
    yw = y0 = y1 = 0.0;
    break;      
  }

  // return 0 if below ionization threshold
  if (energy < e_th) return 0.0;

  double x, y, fy;
  x = energy/e0 - y0;
  y = sqrt(x*x + y1*y1);
  fy = ((x-1.0)*(x-1.0) + yw*yw) * pow(y, 0.5*P-5.5) * 
    pow((1.0 + sqrt(y/ya)), -P);

  sigma = sigma0 * fy * 1e-18;

  return sigma;

}

//---------------------------------

void EnzoMethodRamsesRT::get_photoionization_and_heating_rates (EnzoBlock * enzo_block, double clight) throw() 
{
  // Calculates photoionization and heating rates in each cell according to RAMSES-RT prescription
  // See pg. 14 of https://grackle.readthedocs.io/_/downloads/en/latest/pdf/ for relavent Grackle
  // parameters. 
  // ionization -- first term of eq. A21 -- sum_i(sigmaN*clight*Ni), where i iterates over frequency groups
  // ionization rates should be in code_time^-1
  // heating rates should be in erg s^-1 cm^-3 

  const EnzoConfig * enzo_config = enzo::config();
  EnzoUnits * enzo_units = enzo::units();

  Field field = enzo_block->data()->field();
  Scalar<double> scalar = enzo_block->data()->scalar_double(); 

  int mx,my,mz;  
  field.dimensions(0,&mx, &my, &mz); //field dimensions, including ghost zones
  int gx,gy,gz;
  field.ghost_depth(0,&gx, &gy, &gz);
   
  double xm,ym,zm;
  double xp,yp,zp;
  enzo_block->lower(&xm,&ym,&zm);
  enzo_block->upper(&xp,&yp,&zp);

  enzo_float * e_density     = (enzo_float *) field.values("e_density");
  enzo_float * HI_density    = (enzo_float *) field.values("HI_density");
  enzo_float * HeI_density   = (enzo_float *) field.values("HeI_density");
  enzo_float * HeII_density  = (enzo_float *) field.values("HeII_density");
  enzo_float * temperature   = (enzo_float *) field.values("temperature");
  
  enzo_float * RT_HI_ionization_rate   = (enzo_float *) field.values("RT_HI_ionization_rate");
  enzo_float * RT_HeI_ionization_rate  = (enzo_float *) field.values("RT_HeI_ionization_rate");
  enzo_float * RT_HeII_ionization_rate = (enzo_float *) field.values("RT_HeII_ionization_rate");

  enzo_float * RT_heating_rate = (enzo_float *) field.values("RT_heating_rate");

  std::vector<enzo_float*> chemistry_fields = {HI_density, HeI_density, HeII_density};

  std::vector<enzo_float*> ionization_rate_fields = {RT_HI_ionization_rate, 
                               RT_HeI_ionization_rate, RT_HeII_ionization_rate};

  double lunit = enzo_units->length();
  double tunit = enzo_units->time();
  double munit = enzo_units->mass();
  double eunit = munit * lunit*lunit / (tunit*tunit);
  double volunit = lunit*lunit*lunit;
  double rhounit = enzo_units->density();
  double Tunit = enzo_units->temperature();
  double Nunit = 1/volunit;

  std::vector<double> Eion = {13.6*cello::erg_eV, 24.59*cello::erg_eV, 54.42*cello::erg_eV};
  double mH = cello::mass_hydrogen;
  double mEl = cello::mass_electron;
  std::vector<double> masses = {mH,4*mH,4*mH};

  double mH_mEl = mH/cello::mass_electron;

  std::vector<enzo_float*> photon_densities = {};
  for (int igroup=0; igroup<enzo_config->method_ramses_rt_N_groups; igroup++) { 
    photon_densities.push_back( (enzo_float *) field.values("photon_density_" + std::to_string(igroup))) ;
  }

  // loop through cells
  for (int iz=gz; iz<mz-gz; iz++) {
    for (int iy=gy; iy<my-gy; iy++) {
      for (int ix=gx; ix<mx-gx; ix++) {
        int i = INDEX(ix,iy,iz,mx,my); //index of current cell
        double nHI = HI_density[i] * rhounit / mH; 
        double heating_rate = 0.0; 
        for (int j=0; j<3; j++) { //loop over species
          //double beta = get_beta(temperature[i]*Tunit, j); 
          //double ionization_rate = beta * e_density[i] * rhounit / mEl; // beta*n_e in s^-1
          double ionization_rate = 0.0;
          for (int igroup=0; igroup<enzo_config->method_ramses_rt_N_groups; igroup++) { //loop over groups

            double sigmaN = *(scalar.value( scalar.index( sigN_string(igroup,j) ))) * lunit*lunit; // cm^2 
            double sigmaE = *(scalar.value( scalar.index( sigE_string(igroup,j) ))) * lunit*lunit; // cm^2
            double eps    = *(scalar.value( scalar.index(  eps_string(igroup  ) ))) * eunit; // erg 
 
            double N_i = (photon_densities[igroup])[i] * Nunit; // cm^-3
            double n_j = (chemistry_fields[j])[i] * rhounit / masses[j]; //number density of species j
           
            double ediff = eps*sigmaE - Eion[j]*sigmaN;
            
            // NOTE: clight is passed into this function in cgs
            ionization_rate += std::max( sigmaN*clight*N_i, 0.0 ); 
            heating_rate    += std::max( n_j*clight*N_i*ediff, 0.0 ); // Equation A16

#ifdef DEBUG_RATES 
            std::cout << "i = " << i << "; heating_rate = " << heating_rate << "; n_j = " << n_j << 
            "; N_i = " << N_i << "; eps = " << eps << "; Ediff = " << (eps*sigmaE - Eion[j]*sigmaN) <<  std::endl;
#endif          
          }
          (ionization_rate_fields[j])[i] = ionization_rate * tunit; //update fields with new value, put ionization rates in 1/time_units
        }
        
        RT_heating_rate[i] = heating_rate / nHI; // * eunit/volunit/tunit; //put into code units
#ifdef DEBUG_RATES
        std::cout << "RT_heating_rate[i] = " << RT_heating_rate[i] << " erg/cm^3/s; RT_HI_ionization_rate[i] = " << (ionization_rate_fields[0])[i] / tunit << " s^-1" << std::endl;
#endif 
      }
    }
  } 

 
}

//---------------------------------

double EnzoMethodRamsesRT::get_beta (double T, int species) throw()
{
  // Return collisional ionization rate coefficients according to appendix E1

  double a,b;
  switch (species) {
    case 0: // HI
      a = 5.85e-11;
      b = 157809.1;
      break;  
    case 1: // HeI
      a = 2.38e-11;
      b = 285335.4;
      break;
    case 2: // HeII
      a = 5.68e-12;
      b = 631515;
      break;
  }
 
  return a*sqrt(T)/(1+sqrt(T/1e5)) * pow(cello::e,-b/T);
}

//---------------------------------

double EnzoMethodRamsesRT::get_alpha (double T, int species, char rec_case) throw()
{
  // Return recombination rate coefficients according to appendix E2
  // Takes temperature, species, and recombination type (case A or B) 
  // and spits out the rate coefficient
  
  double lambda, lambda_0;
  double a,b,c,d;
  
  switch (rec_case) {
    case 'A': // case A recombination
      switch (species) {
        case 0: // HII + e -> HI + photon
          lambda = 315614 / T;
          lambda_0 = 0.522;
          a = 1.269e-13;
          b = 1.503;
          c = 0.47;
          d = 1.923;
          break;
        case 1: // HeII + e -> HeI + photon
          lambda = 570670 / T;
          return 3e-14 * pow(lambda,0.654);
        case 2: // HeIII + e -> HeII + photon
          lambda = 1263030 / T;
          lambda_0 = 0.522;
          a = 2.538e-13;
          b = 1.503;
          c = 0.47;
          d = 1.923;
          break;
      }    

      break;

    case 'B': // case B recombination
      switch (species) {
        case 0: // HII + e -> HI + photon
          lambda = 315614 / T;
          lambda_0 = 2.74;
          a = 2.753e-14;
          b = 1.5;
          c = 0.407;
          d = 2.242;
          break;
        case 1: // HeII + e -> HeI + photon
          lambda = 570670 / T;
          return 1.26e-14 * pow(lambda,0.75);
        case 2: // HeIII + e -> HeII + photon
          lambda = 1263030 / T;
          lambda_0 = 2.74;
          a = 5.506e-14;
          b = 1.5;
          c = 0.407;
          d = 2.242;
          break;
      }    

      break;

  } 
#ifdef DEBUG_ALPHA
  std::cout << "a = " << a << "; lamdba = " << lambda << "; T = " << T << std::endl;
#endif 
  return a * pow(lambda,b) * pow( 1+pow(lambda/lambda_0,c), -d); 
}

//---------------------------------
int EnzoMethodRamsesRT::get_b_boolean (double E_lower, double E_upper, int species) throw()
{
  // boolean 1 or 0 which specifies whether or not photon from given recombination
  // lies within given energy range
 
  // QUESTION: What do I do if case A recombination is in the group, but case B is not???
  //           What energy do I use for case B recombination??
 
  // On-the-spot approximation assumes all recombination photons get absorbed immediately
  //     -> b = 0
  //
  // Case B recombination energies are just the ionization energies

  switch (species) { // species after recombination
    case 0: // HI
      if ( (E_lower <= 13.6 ) && (13.6  < E_upper)) return 1;
    case 1: // HeI
      if ( (E_lower <= 24.59) && (24.59 < E_upper)) return 1;
    case 2: // HeII
      if ( (E_lower <= 54.42) && (54.42 < E_upper)) return 1;
  }
  
  // if energy not in this group, return 0
  return 0;
}

//---------------------------------


void EnzoMethodRamsesRT::recombination_photons (EnzoBlock * enzo_block, enzo_float * N, 
                                                enzo_float * T, int i, double E_lower, double E_upper) throw()
{
  // update photon_density to account for recombination (2nd half of eq 25).
  // this is called once for each group.

  Field field = enzo_block->data()->field();
  int igroup = enzo_block->method_ramses_rt_igroup;
  EnzoUnits * enzo_units = enzo::units();
  double Tunit = enzo_units->temperature();
  double munit = enzo_units->mass();

  if (!field.is_field("density")) return;
  
  //enzo_float * density = (enzo_float *) field.values("density");
  enzo_float * e_density = (enzo_float *) field.values("e_density");

  std::vector<std::string> chemistry_fields = {"HI_density", 
                                               "HeI_density", "HeII_density"};
  double mH  = cello::mass_hydrogen / munit; // code units
  double mEl = cello::mass_electron / munit;

  std::vector<double> masses = {mH,4*mH, 4*mH};

  double dN_dt = 0.0;
  double alpha_units = enzo_units->volume() / enzo_units->time();
  for (int j=0; j<chemistry_fields.size(); j++) {  
    enzo_float * density_j = (enzo_float *) field.values(chemistry_fields[j]);     
    int b = get_b_boolean(E_lower, E_upper, j);
  
    //TODO: To speed things up, could just have get_alpha return alpha_A - alpha_B
    double alpha_A = get_alpha(T[i]*Tunit, j, 'A') / alpha_units;
    double alpha_B = get_alpha(T[i]*Tunit, j, 'B') / alpha_units;     

    double n_j = density_j[i]/masses[j];
    double n_e = e_density[i]/mEl;
    dN_dt += b*(alpha_A-alpha_B) * n_j*n_e;
#ifdef DEBUG_RECOMBINATION
    CkPrintf("MethodRamsesRT::recombination_photons -- j=%d; alpha_A = %1.3e; alpha_B = %1.3e; n_j = %1.3e; n_e = %1.3e; b_boolean = %d\n", j, alpha_A, alpha_B, n_j, n_e, b);
#endif
  }

#ifdef DEBUG_RECOMBINATION
  CkPrintf("MethodRamsesRT::recombination_photons -- dN_dt[i] = %1.3e", dN_dt);
#endif 
  N [i] += dN_dt * enzo_block->dt;
}

//---------------------------------

void EnzoMethodRamsesRT::recombination_chemistry (EnzoBlock * enzo_block) throw()
{
  // update density fields to account for recombination (2nd half of eqs. 28-30).
  // this does a sum over all groups
  // Grackle handles collisional ionization and photoionization (have to feed it photoionization rates though)
  Field field = enzo_block->data()->field();
  int mx,my,mz;  
  field.dimensions(0,&mx, &my, &mz); //field dimensions, including ghost zones
  int gx,gy,gz;
  field.ghost_depth(0,&gx, &gy, &gz);
   
  double xm,ym,zm;
  double xp,yp,zp;
  enzo_block->lower(&xm,&ym,&zm);
  enzo_block->upper(&xp,&yp,&zp);

  double dt = enzo_block->dt;

  enzo_float * HI_density    = (enzo_float *) field.values("HI_density");
  enzo_float * HII_density   = (enzo_float *) field.values("HII_density");
  enzo_float * HeI_density   = (enzo_float *) field.values("HeI_density");
  enzo_float * HeII_density  = (enzo_float *) field.values("HeII_density");
  enzo_float * HeIII_density = (enzo_float *) field.values("HeIII_density");
  enzo_float * e_density     = (enzo_float *) field.values("e_density");

  enzo_float * temperature = (enzo_float *) field.values("temperature");

  EnzoUnits * enzo_units = enzo::units();
  double Tunit = enzo_units->temperature();
  double munit = enzo_units->mass();
  double rhounit = enzo_units->density();
  double alpha_units = enzo_units->volume() / enzo_units->time();

  double mH_mEl = cello::mass_hydrogen / cello::mass_electron;
 
  for (int iz=gz; iz<mz-gz; iz++) {
    for (int iy=gy; iy<my-gy; iy++) {
      for (int ix=gx; ix<mx-gx; ix++) {
        int i = INDEX(ix,iy,iz,mx,my); //index of current cell
         
        double alphaA_HII   = get_alpha(temperature[i]*Tunit,0,'A')/alpha_units;
        double alphaA_HeII  = get_alpha(temperature[i]*Tunit,1,'A')/alpha_units;
        double alphaA_HeIII = get_alpha(temperature[i]*Tunit,2,'A')/alpha_units;
  
                     
        // eq. 28
        HII_density  [i] -= HII_density[i]*alphaA_HII*e_density[i]*mH_mEl * dt;

        //eq. 29
        HeII_density [i] += (HeIII_density[i]*alphaA_HeIII - HeII_density[i]*alphaA_HeII) 
                                  *e_density[i]*4*mH_mEl * dt;

        //eq. 30
        HeIII_density[i] -= HeIII_density[i]*alphaA_HeIII*e_density[i]*4*mH_mEl * dt;
      }
    }
  } 

}

//---------------------------------

void EnzoMethodRamsesRT::add_attenuation ( EnzoBlock * enzo_block, enzo_float * N, 
                     enzo_float * Fx, enzo_float * Fy, enzo_float * Fz, double clight, int i) throw()
{
  // TODO: Need to also update photon density field and to account for recombinations (2nd half of eq. 25)
  //       Right now, this only accounts for attenuation (first half)
  const EnzoConfig * enzo_config = enzo::config();
  EnzoUnits * enzo_units = enzo::units();
  double dt = enzo_block->dt;
  double tunit = enzo_units->time();  
  double rhounit = enzo_units->density();
  double munit = enzo_units->mass();

  Field field = enzo_block->data()->field();
  int igroup = enzo_block->method_ramses_rt_igroup;

  if (!field.is_field("density")) return;
  
  //enzo_float * density = (enzo_float *) field.values("density");

  std::vector<std::string> chemistry_fields = {"HI_density", 
                                               "HeI_density", "HeII_density"};
  double mH = cello::mass_hydrogen / munit;
  std::vector<double> masses = {mH,4*mH, 4*mH};
 
  //it's okay to use the same cross section for both attenuation (affects N) and 
  //radiation pressure (affects F) because F and N have approximately the 
  //same spectral shape.

  Scalar<double> scalar = enzo_block->data()->scalar_double();
  double d_dt = 0.0;
  for (int j=0; j<chemistry_fields.size(); j++) {  
    enzo_float * density_j = (enzo_float *) field.values(chemistry_fields[j]);
    double n_j = density_j[i] / masses[j];     
    double sigN_ij = *(scalar.value( scalar.index( sigN_string(igroup, j) )));
    d_dt += n_j * clight*sigN_ij; // code_time^-1
    //std::cout << density_j[i] << ' ' <<  masses[j] << ' ' << clight << ' ' << sigN_ij << std::endl;
  }
 
#ifdef DEBUG_ATTENUATION
  // NOTE d_dt * dt > 1 must be true. If this is happening, it is likely that your timestep is too large.
  //                                  This can be fixed by either increasing resolution, decreasing global timestep,
  //                                  or subcycling RT.
  if (d_dt * dt > 1) {
    CkPrintf("MethodRamsesRT::add_attenuation() -- WARNING: d_dt*dt = %f (> 1)\n", d_dt*dt);
    CkPrintf("i=%d; d_dt=%e; N[i] = %e; Fx[i] = %e; dt = %e\n",i, d_dt, N[i], Fx[i], dt);
  }
#endif 
 
  N [i] -= d_dt*N [i] * dt;
  Fx[i] -= d_dt*Fx[i] * dt;
  Fy[i] -= d_dt*Fy[i] * dt;
  Fz[i] -= d_dt*Fz[i] * dt; 
  
}

//----------------------

void EnzoMethodRamsesRT::solve_transport_eqn ( EnzoBlock * enzo_block ) throw()
{

  // TODO: Adapt for 2D and 3D cases
  
  // Solve dU/dt + del[F(U)] = 0; F(U) = { (Fx,Fy,Fz), c^2 P }
  //                                U  = { N, (Fx,Fy,Fz) }
  // M1 closure: P_i = D_i * N_i, where D_i is the Eddington tensor for 
  // photon group i
 
  const EnzoConfig * enzo_config = enzo::config();
  EnzoUnits * enzo_units = enzo::units();

  Field field = enzo_block->data()->field();
  int mx,my,mz;  
  field.dimensions(0,&mx, &my, &mz); //field dimensions, including ghost zones
  int gx,gy,gz;
  field.ghost_depth(0,&gx, &gy, &gz);
   
  const int rank = ((mz == 1) ? ((my == 1) ? 1 : 2) : 3);
 
  double xm,ym,zm;
  double xp,yp,zp;
  enzo_block->lower(&xm,&ym,&zm);
  enzo_block->upper(&xp,&yp,&zp);

  // array incremenent (because 3D array of field values are flattened to 1D)
  const int idx = 1;
  const int idy = mx;
  const int idz = mx*my; 

  // energy bounds for this group (leave in eV)
  double E_lower = enzo_config->method_ramses_rt_bin_lower[enzo_block->method_ramses_rt_igroup]; 
  double E_upper = enzo_config->method_ramses_rt_bin_upper[enzo_block->method_ramses_rt_igroup]; 

  std::string istring = std::to_string(enzo_block->method_ramses_rt_igroup);
  enzo_float * N  = (enzo_float *) field.values("photon_density_" + istring);
  enzo_float * Fx = (enzo_float *) field.values("flux_x_" + istring);
  enzo_float * Fy = (enzo_float *) field.values("flux_y_" + istring);
  enzo_float * Fz = (enzo_float *) field.values("flux_z_" + istring);

  // TODO: Call compute_temperature here in case temperature isn't specified as derived_field 
  enzo_float * T = (enzo_float *) field.values("temperature");

  const int m = mx*my*mz;
  // extra copy of fields needed to store
  // the evolved values until the end
  enzo_float * Nnew  = new enzo_float[m]; 
  enzo_float * Fxnew = new enzo_float[m];
  enzo_float * Fynew = new enzo_float[m];
  enzo_float * Fznew = new enzo_float[m];
 
  double dt = enzo_block->dt;
  double hx = (xp-xm)/(mx-2*gx);
  double hy = (yp-ym)/(my-2*gy);
  double hz = (zp-zm)/(mz-2*gz);

  // Adjust values for cosmological expansion here? 
  // if (cosmology) {
  // }

  // if 2D, gz = 0 by default (I think) and mz = 1, so the outermost loop
  // would just be for (int iz=0, iz<1, iz++)



  //if (true) get_group_attributes(); // if (enzo_block->cycle % attribute_cycle_step == 0)
  double lunit = enzo_units->length();
  double tunit = enzo_units->time();
  double Nunit = 1/enzo_units->volume();

  double clight = enzo_config->method_ramses_rt_clight_frac*cello::clight * tunit/lunit; // code_velocity

  for (int i=0; i<m; i++)
  {
    Nnew [i] = N [i];
    Fxnew[i] = Fx[i];
    Fynew[i] = Fy[i];
    Fznew[i] = Fz[i];
  }


  //calculate the pressure tensor
  get_pressure_tensor(enzo_block, N, Fx, Fy, Fz, clight);

  for (int iz=gz; iz<mz-gz; iz++) {
    for (int iy=gy; iy<my-gy; iy++) {
      for (int ix=gx; ix<mx-gx; ix++) {
        int i = INDEX(ix,iy,iz,mx,my); //index of current cell
        double N_update=0, Fx_update=0, Fy_update=0, Fz_update=0;
      
        get_U_update( enzo_block, &N_update, &Fx_update, &Fy_update, &Fz_update,
                             N, Fx, Fy, Fz, hx, hy, hz, dt, clight,
                             i, idx, idy, idz ); 

        // get updated fluxes

        Fxnew[i] += Fx_update;
        Fynew[i] += Fy_update;
        Fznew[i] += Fz_update;
         
        // now get updated photon densities
        Nnew[i] += N_update;
        // can occasionally end up in situations where
        // N[i] is slightly negative due to roundoff error.
        // Make it a small positive number instead 
        if (Nnew[i] < 1e-16) Nnew[i] = 1e-16 / Nunit;

      #ifdef DEBUG_TRANSPORT
        std::cout << "i = " << i << "; Nnew[i] = " << Nnew[i] << "; N_update = " << N_update << "; Fx_update = " << Fx_update << "; hx = " << hx << "; dt = " << dt << std::endl;
      #endif

      #ifndef DEBUG_TURN_OFF_ATTENUATION 
        // add interactions with matter 
        add_attenuation(enzo_block, Nnew, Fxnew, Fynew, Fznew, clight, i); 
      #endif
        if (Nnew[i] < 1e-16) Nnew[i] = 1e-16 / Nunit;

        // update photon density due to recombinations
        // TODO: Only do this if not using on-the-spot approximation
        recombination_photons(enzo_block, Nnew, T, i, E_lower, E_upper);
             

      }
    }   
  } 
  


  // now copy values over  
  for (int iz=gz; iz<mz-gz; iz++) {
    for (int iy=gy; iy<my-gy; iy++) {
      for (int ix=gx; ix<mx-gx; ix++) {
        int i = INDEX(ix,iy,iz,mx,my); //index of current cell
        N [i] = Nnew [i];
        Fx[i] = Fxnew[i];
        Fy[i] = Fynew[i];
        Fz[i] = Fznew[i];
   
       // if (N [i] < 1e-16) N [i] = 1e-16;

      }
    }
  } 

  delete [] Nnew;
  delete [] Fxnew;
  delete [] Fynew;
  delete [] Fznew;
}


//----------------------------------------------------------------------
void EnzoMethodRamsesRT::call_inject_photons(EnzoBlock * enzo_block) throw()
{
  const EnzoConfig * enzo_config = enzo::config();
  enzo_block->method_ramses_rt_igroup = 0;

#ifdef DEBUG_PRINT_GROUP_PARAMETERS
  //for printing out table of group parameters
  std::cout<<"[E_lower, E_upper] (eV); j; sigN_ij; sigE_ij; eps_i" << std::endl;
  std::cout<<"---------------------------------------------------" << std::endl;  
#endif 
 
  for (int i=0; i<enzo_config->method_ramses_rt_N_groups; i++) {
    this->inject_photons(enzo_block);
    enzo_block->method_ramses_rt_igroup += 1;
  }
  // do refresh, start transport step once that's done
 
  cello::refresh(ir_injection_)->set_active(enzo_block->is_leaf()); 
  enzo_block->new_refresh_start(ir_injection_, CkIndex_EnzoBlock::p_method_ramses_rt_solve_transport_eqn());
}

//-----------------------------------
void EnzoMethodRamsesRT::call_solve_transport_eqn(EnzoBlock * enzo_block) throw()
{
  const EnzoConfig * enzo_config = enzo::config();
  double clight = enzo_config->method_ramses_rt_clight_frac * cello::clight;
  enzo_block->method_ramses_rt_igroup = 0;
  // loop through groups and solve transport equation for each group
  for (int i=0; i<enzo_config->method_ramses_rt_N_groups;i++) {
    this->solve_transport_eqn(enzo_block);

    enzo_block->method_ramses_rt_igroup += 1;
  }

#ifndef DEBUG_TURN_OFF_CHEMISTRY
  // update chemistry fields to account for recombinations
  recombination_chemistry(enzo_block);

  // Calculate photoheating and photoionization rate
  
  //TODO: Make reduced clight parameter hang off of EnzoBlock class so 
  //that we don't have to make it a parameter for every function and 
  //can access it without it being erased after refresh

  //TODO: Make photoionization/heating/chemistry optional 
  get_photoionization_and_heating_rates(enzo_block, clight);
#endif
}

//-------------------------------

void EnzoBlock::p_method_ramses_rt_solve_transport_eqn()
{
  EnzoMethodRamsesRT * method = static_cast<EnzoMethodRamsesRT*> (this->method());
  method->call_solve_transport_eqn(this);

  // sum group fields and end compute()
  // TODO: Make tracking integrated group fields optional?
   
  method->sum_group_fields(this); 
  this->compute_done(); 
}

//-----------------------------------------
void EnzoMethodRamsesRT::sum_group_fields(EnzoBlock * enzo_block) throw()
{
  Field field = enzo_block->data()->field();
  int mx,my,mz;  
  field.dimensions(0,&mx, &my, &mz); //field dimensions, including ghost zones
  int gx,gy,gz;
  field.ghost_depth(0,&gx, &gy, &gz);

  const int m = mx*my*mz;
  const EnzoConfig * enzo_config = enzo::config();

  enzo_float * N  = (enzo_float *) field.values("photon_density");
  enzo_float * Fx = (enzo_float *) field.values("flux_x");
  enzo_float * Fy = (enzo_float *) field.values("flux_y");
  enzo_float * Fz = (enzo_float *) field.values("flux_z");

  for (int j=0; j<m; j++)
  {
    N [j] = 0.0;
    Fx[j] = 0.0;
    Fy[j] = 0.0;
    Fz[j] = 0.0; 
  }

  for (int i=0; i < enzo_config->method_ramses_rt_N_groups; i++) {
    std::string istring = std::to_string(i);
    enzo_float *  N_i = (enzo_float *) field.values("photon_density_" + istring);
    enzo_float * Fx_i = (enzo_float *) field.values("flux_x_" + istring);
    enzo_float * Fy_i = (enzo_float *) field.values("flux_y_" + istring);
    enzo_float * Fz_i = (enzo_float *) field.values("flux_z_" + istring);
    for (int j=0; j<m; j++)
    {
      N [j] +=  N_i[j];
      Fx[j] += Fx_i[j];
      Fy[j] += Fy_i[j];
      Fz[j] += Fz_i[j];
    }    
  }
}


//======================================================================

void EnzoMethodRamsesRT::compute_ (Block * block) throw()
{
  const EnzoConfig * enzo_config = enzo::config();
 
  // TODO: lump the rest of the radiation that doesn't fall into these bins into one "other" group
  // and evolve that one too.
  //
  //
  // QUESTION: How do I get N_nu from the SED?
  // 
  //
  // LOOK AT DAN REYNOLDS' BRANCH OF ENZO-BW TO SEE HOW THIS IS IMPLEMENTED
  // See ComputeRadiationIntegrals() function and EnforceRadiationBounds()?
  //
  //
  // To test that multigroup is working, sample from a blackbody spectrum and plot the intensity in each group. Should trace out blackbody.

  Field field = block->data()->field();
  int mx,my,mz;  
  field.dimensions(0,&mx, &my, &mz); //field dimensions, including ghost zones
  int gx,gy,gz;
  field.ghost_depth(0,&gx, &gy, &gz);

  const int m = mx*my*mz;


  if (block->cycle() == 0)
  {
    EnzoUnits * enzo_units = enzo::units();
    double Nunit = 1.0 / enzo_units->volume();
    double Funit = enzo_units->velocity() * Nunit;    
    for (int i=0; i<enzo_config->method_ramses_rt_N_groups; i++) {
      std::string istring = std::to_string(i);
      enzo_float *  N_i = (enzo_float *) field.values("photon_density_" + istring);
      enzo_float * Fx_i = (enzo_float *) field.values("flux_x_" + istring);
      enzo_float * Fy_i = (enzo_float *) field.values("flux_y_" + istring);
      enzo_float * Fz_i = (enzo_float *) field.values("flux_z_" + istring);
      for (int j=0; j<m; j++)
      {
        // initialize fields to zero 
        // TODO: write a problem initializer so that we don't have to do this 
        N_i [j] = 1e-16 / Nunit;
        Fx_i[j] = 1e-16 / Funit;
        Fy_i[j] = 1e-16 / Funit;
        Fz_i[j] = 1e-16 / Funit; 
      }    
    }
  }

  //start photon injection step
  //This function will start the transport step after a refresh
  EnzoBlock * enzo_block = enzo::block(block);

  this->call_inject_photons(enzo_block);
}
