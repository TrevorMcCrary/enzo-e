// See LICENSE_CELLO file for license and copyright information

/// @file     enzo_EnzoEOSIdeal.cpp
/// @author   Matthew Abruzzo (matthewabruzzo@gmail.com)
/// @date     Thurs May 2 2019
/// @brief    [\ref Enzo] Implementation of EnzoEOSIdeal

#include "cello.hpp"
#include "enzo.hpp"

// #define DEBUG_MATCHING_ARRAY_SHAPES

//----------------------------------------------------------------------

void EnzoEOSIdeal::pup (PUP::er &p)
{
  // NOTE: change this function whenever attributes change
  PUP::able::pup(p);
  p|gamma_;
  p|density_floor_;
  p|pressure_floor_;
  p|dual_energy_formalism_;
  p|dual_energy_formalism_eta_;
}

//----------------------------------------------------------------------

void EnzoEOSIdeal::primitive_from_integration
(const EnzoEFltArrayMap &integration_map, EnzoEFltArrayMap &primitive_map,
 const int stale_depth, const str_vec_t &passive_list,
 const bool ignore_grackle) const
{

  const CelloArray<const enzo_float, 3> density = integration_map.at("density");
  const int mz = density.shape(0);
  const int my = density.shape(1);
  const int mx = density.shape(2);

  // The EOS object doesn't necessarily know what the integration quantities
  // are. This means we take something of an exhaustive approach. This could be
  // more clever if this operations were made a part of the Hydro Integrator
  std::vector<std::string> quantity_list =
    EnzoCenteredFieldRegistry::get_registered_quantities(true, true);

  for (const std::string& key : quantity_list){
    if ((!integration_map.contains(key)) || (!primitive_map.contains(key))){
      continue;
    }

    const CelloArray<const enzo_float, 3> integ_array = integration_map.at(key);
    const CelloArray<enzo_float, 3> prim_array = primitive_map.at(key);

#ifdef DEBUG_MATCHING_ARRAY_SHAPES
    ASSERT6("EnzoEOSIdeal::primitive_from_integration",
            ("The array being copied from integration_map has shape "
             "(%d,%d,%d), while the destination array has shape (%d,%d,%d). "
             "They should be the same."),
            mz, my, mx,
            prim_array.shape(0), prim_array.shape(1), prim_array.shape(2),
            ((prim_array.shape(0) == mz) &&
             (prim_array.shape(1) == my) &&
             (prim_array.shape(2) == mx)));
#endif

    for (int iz = stale_depth; iz < mz - stale_depth; iz++) {
      for (int iy = stale_depth; iy < my - stale_depth; iy++) {
        for (int ix = stale_depth; ix < mx - stale_depth; ix++) {
          prim_array(iz,iy,ix) = integ_array(iz,iy,ix);
        }
      }
    }
  }

  // Convert the passive scalars from conserved-form (i.e. a density) to
  // specific-form (mass fractions)
  for (const std::string& key : passive_list){
    const CelloArray<const enzo_float, 3> cur_conserved
      = integration_map.at(key);
    const CelloArray<enzo_float, 3> out_specific = primitive_map.at(key);

    for (int iz = stale_depth; iz < mz - stale_depth; iz++) {
      for (int iy = stale_depth; iy < my - stale_depth; iy++) {
        for (int ix = stale_depth; ix < mx - stale_depth; ix++) {
          out_specific(iz,iy,ix) = cur_conserved(iz,iy,ix)/density(iz,iy,ix);
        }
      }
    }
  }

  pressure_from_integration(integration_map, primitive_map.at("pressure"),
                            stale_depth, ignore_grackle);
}



//----------------------------------------------------------------------

void EnzoEOSIdeal::pressure_from_integration
(const EnzoEFltArrayMap &integration_map, const EFlt3DArray &pressure,
 const int stale_depth, const bool ignore_grackle) const
{

  const bool mhd = (integration_map.contains("bfield_x") ||
                    integration_map.contains("bfield_y") ||
                    integration_map.contains("bfield_z"));

  EnzoComputePressure::compute_pressure(EnzoFieldAdaptor(integration_map),
                                        pressure, mhd,
                                        this->uses_dual_energy_formalism(),
                                        get_gamma(), stale_depth,
                                        ignore_grackle);
}

//----------------------------------------------------------------------

// based on the enzo's hydro_rk implementation of synchronization (found in the
// Grid_UpdateMHD.C file)
void EnzoEOSIdeal::apply_floor_to_energy_and_sync
(EnzoEFltArrayMap &integration_map, const int stale_depth) const
{

  // This function's application of a floor isn't technically correct here for
  // a variable gamma.
  // - for (enzo::config()->method_grackle_chemistry->primordial_chemistry > 1)
  //   Grackle adjusts the "nominal gamma value" (usually ~ 5/3) based on the
  //   the relative abundance of molecular hydrogen & the specific internal
  //   energy (since the number of degrees of freedom depend on temperature)
  // - Grackle provides routines for calculating pressure and gamma given the
  //   mass_dens, eint, mass_dens_primordials, and mass_dens_H2.
  // - One could hypothetic invert the routine for pressure to acquire
  //   eint(mass_dens, pressure, mass_dens_primordials, and mass_dens_H2),
  //   but this is not presently available...
  //
  // The "correct" approach is to use the hypothetical eint function to compute
  // the local value of the internal energy floor for each cell using the
  // pressure_floor and the local values of mass_dens, mass_dens_primordials,
  // and mass_dens_H2.
  //
  // Since we don't have this hypothetical routine, we instead estimate the
  // local value of the internal energy floor for each cell using the pressure
  // floor, the "nominal gamma value", and the local mass_dens value, according
  // to eint = pressure / ((gamma - 1) * rho).
  // - This somewhat overestimates the true value of gamma.
  // - Thus, when you convert our eint_floor estimate back to pressure (with
  //   the Grackle routine), you'll recover a value smaller than the pressure
  //   floor.

  const bool idual = this->uses_dual_energy_formalism();
  const bool mag = (integration_map.contains("bfield_x") ||
                    integration_map.contains("bfield_y") ||
                    integration_map.contains("bfield_z"));
  // in hydro_rk, eta was set equal to eta1 (it didn't use eta2 at all)
  const double eta = dual_energy_formalism_eta_;

  const EFlt3DArray etot = integration_map.at("total_energy");
  const EFlt3DArray eint =
    (idual) ? integration_map.at("internal_energy") : EFlt3DArray();

  using RdOnlyEFlt3DArray = CelloArray<const enzo_float, 3>;
  const RdOnlyEFlt3DArray density = integration_map.at("density");
  const RdOnlyEFlt3DArray vx = integration_map.at("velocity_x");
  const RdOnlyEFlt3DArray vy = integration_map.at("velocity_y");
  const RdOnlyEFlt3DArray vz = integration_map.at("velocity_z");

  const RdOnlyEFlt3DArray bx = (mag) ?
    RdOnlyEFlt3DArray(integration_map.at("bfield_x")) : RdOnlyEFlt3DArray();
  const RdOnlyEFlt3DArray by = (mag) ?
    RdOnlyEFlt3DArray(integration_map.at("bfield_y")) : RdOnlyEFlt3DArray();
  const RdOnlyEFlt3DArray bz = (mag) ?
    RdOnlyEFlt3DArray(integration_map.at("bfield_z")) : RdOnlyEFlt3DArray();

  float ggm1 = get_gamma()*(get_gamma() - 1.);
  enzo_float pressure_floor = get_pressure_floor();
  enzo_float inv_gm1 = 1./(get_gamma()-1.);

  // a requirement for an element of the internal energy field, cur_eint,
  // to be updated to the value computed from the total energy field, eint_1,
  // is that cur_eint > half_factor * cur_eint, where half_factor is 0.5. To
  // allow eta = 0, to specify that this update should always occur, we set
  // half_factor = 0 when eta = 0.
  const double half_factor = (eta != 0.) ? 0.5 : 0.;

  for (int iz = stale_depth; iz < (density.shape(0) - stale_depth); iz++) {
    for (int iy = stale_depth; iy < (density.shape(1) - stale_depth); iy++) {
      for (int ix = stale_depth; ix < (density.shape(2) - stale_depth); ix++) {

	enzo_float inv_rho = 1./density(iz,iy,ix);
	enzo_float eint_floor = pressure_floor*inv_gm1*inv_rho;

	enzo_float v2 = (vx(iz,iy,ix) * vx(iz,iy,ix) +
			 vy(iz,iy,ix) * vy(iz,iy,ix) +
			 vz(iz,iy,ix) * vz(iz,iy,ix));
        enzo_float non_thermal_e =  0.5*v2;
        enzo_float b2 = 0;
        if (mag){
          b2 = (bx(iz,iy,ix) * bx(iz,iy,ix) +
                by(iz,iy,ix) * by(iz,iy,ix) +
                bz(iz,iy,ix) * bz(iz,iy,ix));
          non_thermal_e += (0.5 * b2 *inv_rho);
        }

	if (idual){
	  enzo_float eint_1 = etot(iz,iy,ix) - non_thermal_e;
	  enzo_float cur_eint = eint(iz,iy,ix);

	  // compute cs^2 with estimate of eint from etot
	  // p = rho*(gamma-1)*eint
	  // cs^2 = gamma * p / rho = gamma*(gamma-1)*eint
	  enzo_float cs2_1 = std::fmax(0., ggm1*eint_1);

	  // half_factor = 0.5 when eta !=0. Otherwise it's 0.
	  if ( (cs2_1 > std::fmax(eta*v2, eta*b2*inv_rho)) &&
	       (eint_1 > half_factor*cur_eint) ){
	    cur_eint = eint_1;
	  }
	  cur_eint = EnzoEquationOfState::apply_floor(cur_eint, eint_floor);

	  eint(iz,iy,ix) = cur_eint;
	  etot(iz,iy,ix) = cur_eint + non_thermal_e;
	} else {

	  enzo_float etot_floor = eint_floor + non_thermal_e;
	  etot(iz,iy,ix) = EnzoEquationOfState::apply_floor(etot(iz,iy,ix),
							    etot_floor);
	}
      }
    }
  }
}
