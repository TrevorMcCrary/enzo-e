// See LICENSE_CELLO file for license and copyright information

/// @file	enzo_EnzoMethodSinkMaker.hpp
/// @author     Stefan Arridge (stefan.arridge@gmail.com)
/// @date       1 June 2022
/// @brief      Implements a method for forming sink particles based on
///             the method described in Krumholz+ 2004 and Federrath+ 2010.

#ifndef ENZO_ENZO_METHOD_SINKMAKER
#define ENZO_ENZO_METHOD_SINKMAKER

class EnzoMethodSinkMaker : public Method {

  /// @class   EnzoMethodSinkMaker
  /// @ingroup Enzo
  /// @brief   [\ref Enzo] Implements a method for forming sink particles based on 
  ///          the method described in Krumholz+ 2004 and Federrath+ 2010.


public:

  // Constructor
  EnzoMethodSinkMaker
  (double jeans_length_resolution_cells,
   double density_threshold,
   bool   check_density_maximum,
   double max_mass_fraction,
   double min_sink_mass_solar);

  /// Destructor
  virtual ~EnzoMethodSinkMaker() throw() {};

  /// Charm++ Pup::able declarations
  PUPable_decl(EnzoMethodSinkMaker);

  /// Charm++ PUP::able migration constructor
  EnzoMethodSinkMaker (CkMigrateMessage *m)
    : Method (m)
    {  }

  /// CHARM++ Pack / Unpack function
  void pup (PUP::er &p);

  /// Apply the method
  virtual void compute( Block * block) throw();

  /// Name
  virtual std::string name () throw()
  { return "sink_maker"; }

  virtual std::string particle_type () throw()
  { return "sink";}

  // Compute the maximum timestep for this method
  virtual double timestep ( Block * block) const throw();

protected: // methods

  // Called when block is at highest refinement level.
  // Loops over active cells and creates sink particles in cells which
  // satisfy certain conditions.
  void compute_(Block * block) throw();

  // Returns true if the local Jeans length is not sufficiently resolved, i.e., if it is less than
  // jeans_length_resolution_cells_ multiplied by the maximum cell width.
  // `const_G` is the value of the gravitational constant in code units.
  // `i` is the 1D index of the cell.
  // Returns false otherwise.
  bool jeans_length_not_resolved_(Block * block, int i,
				  double const_G) throw();

  // Returns true if the flow around the cell (with 3D indices `ix`, `iy`, `iz`) is converging
  // in all directions, and returns false otherwise.
  // Converging flow is checked by computing the symmetrised grad velocity tensor
  // (also known as the strain tensor)
  // a_{ij} = 0.5*(dv_i/dx_j + dv_j/dx_i), then first checking its trace is negative
  // (i.e. the velocity divergence is negative), then if this is satisfied we check
  // if all the eigenvalues are negative.
  // Returns false otherwise.
  bool flow_is_converging_(Block * block, int ix, int iy, int iz) throw();

  // Returns true if the density in the given cell (with 3D indices
  // `ix`, `iy`, `iz`) is a local maximum, i.e. it is larger than the densities in all
  // 26 neighboring cells.
  // Returns false otherwise.
  bool density_is_local_maximum_(Block * block,
				 int ix, int iy, int iz) throw();

  /// Does various checks which need to be done at the first compute cycle
  void do_checks_(Block* block) throw();

protected: // attributes

  // If the local Jeans length in a cell is less than this quantity multiplied by the maximum
  // cell width, then the cell is a candidate for forming a sink.
  double jeans_length_resolution_cells_;

  // Density in a cell must be greater than density_threshold_ to be able to form a sink.
  // The density in a cell after sink formation will be no less than density_threshold_.
  double density_threshold_;

  // Determines whether or not the "local density maximum" check is performed when deciding if
  // a cell forms a sink particle.
  bool check_density_maximum_;

  // Mass of newly-formed sink is bounded above by max_mass_fraction_ multiplied by the cell
  // density multiplied by the cell volume.
  double max_mass_fraction_;

  // The minimum sink mass in terms of the solar mass.
  // If a cell passes all the conditions for forming a sink, but the sink mass imposed by
  // density_threshold_ and max_mass_fraction_ is less than the minimum sink mass, no sink
  // particle is formed.
  double min_sink_mass_solar_;

};

#endif /* EnzoMethodSinkMaker */
