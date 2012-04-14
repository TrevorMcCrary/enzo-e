// See LICENSE_CELLO file for license and copyright information

/// @file     enzo_EnzoSimulationMpi.hpp
/// @author   James Bordner (jobordner@ucsd.edu)
/// @date     2010-05-11
/// @brief    [\ref Enzo] Declaration of the EnzoSimulationMpi class

#ifndef ENZO_ENZO_SIMULATION_MPI_HPP
#define ENZO_ENZO_SIMULATION_MPI_HPP

#if defined(CONFIG_USE_MPI) || ! defined(CONFIG_USE_CHARM)

class EnzoSimulationMpi : public EnzoSimulation {

  /// @class    EnzoSimulationMpi
  /// @ingroup  Enzo
  /// @brief    [\ref Enzo] Simulation class for MPI Enzo-P

public: // functions

  /// Constructor
  EnzoSimulationMpi
  ( const char * parameter_file,
    const GroupProcess * group_process) throw();

  /// Destructor
  ~EnzoSimulationMpi() throw();

  /// Run the simulation
  virtual void run() throw();

protected:

  void update_boundary_ (Block * block, bool boundary[3][2]) throw();
  void refresh_ghost_   (Block * block, Patch * patch, bool boundary[3][2]) throw();
  // void is_block_on_boundary_ (Block * block, bool boundary[3][2]) throw();
  
};

#endif /* ! CONFIG_USE_CHARM */

#endif /* ENZO_ENZO_SIMULATION_MPI_HPP */

