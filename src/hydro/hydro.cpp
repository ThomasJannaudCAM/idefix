// ***********************************************************************************************
// Idefix MHD astrophysical code
// Copyright(C) 2020 Geoffroy R. J. Lesur <geoffroy.lesur@univ-grenoble-alpes.fr
// and other code contributors
// Licensed under CeCILL 2.1 License, see COPYING for more information
// ***********************************************************************************************

#include <string>

#include "../idefix.hpp"
#include "hydro.hpp"


Hydro::Hydro(Input &input, Grid &grid, DataBlock *datain) {
  idfx::pushRegion("Hydro::Hydro(input)");
  // Save the datablock to which we are attached from now on
  this->data = datain;

  if(input.CheckEntry("Hydro","gamma")>0) {
    this->gamma = input.GetReal("Hydro","gamma",0);
    idfx::cout << "Hydro:: adiabatic EOS with gamma=" << this->gamma <<std::endl;
  } else {
    this->gamma = 5.0/3.0;
#if HAVE_ENERGY
    idfx::cout << "Hydro:: Warning! no gamma has been set in the input file, assuming gamma=5/3."
               << std::endl;
#endif
  }

  if(input.CheckEntry("Hydro","csiso")>0) {
    real cs = input.GetReal("Hydro","csiso",0);
    this->C2Iso = cs*cs;
  } else {
#if HAVE_ENERGY
    // set the isothermal soundspeed, even though it will not be used
    this->C2Iso = 1.0;
#else
    IDEFIX_ERROR("You are using the ISOTHERMAL approximation "
                 "but have not set csiso in the ini file.");
#endif
  }

  // read Solver from input file
  std::string solverString = input.GetString("Hydro","Solver",0);

  if (solverString.compare("tvdlf") == 0) {
    mySolver = TVDLF;
  } else if (solverString.compare("hll") == 0) {
    mySolver = HLL;
#if MHD == YES
  } else if (solverString.compare("hlld") == 0) {
    mySolver = HLLD;
#else
  } else if (solverString.compare("hllc") == 0) {
    mySolver = HLLC;
#endif
  } else if (solverString.compare("roe") == 0) {
    mySolver = ROE;
  } else {
    std::stringstream msg;
#if MHD == YES
    msg << "Unknown MHD solver type " << solverString;
#else
    msg << "Unknown HD solver type " << solverString;
#endif
    IDEFIX_ERROR(msg);
  }

  // No userdefBoundary by default
  this->haveUserDefBoundary = false;
  this->haveInternalBoundary = false;
  this->haveEmfBoundary = false;

  // Source terms (always activated when non-cartesian geometry because of curvature source terms)
#if GEOMETRY == CARTESIAN
  this->haveSourceTerms = false;
#else
  this->haveSourceTerms = true;
#endif
  this->haveUserSourceTerm = false;

  // Check whether we have rotation
  int rotation = input.CheckEntry("Hydro","Rotation");

  if(rotation>=0 ) {
    this->haveSourceTerms = true;
    this->haveRotation = true;
    if(rotation != 3) IDEFIX_ERROR("Rotation needs a 3 components vector in idefix.ini");
    this->OmegaX1 = input.GetReal("Hydro","Rotation",0);
    this->OmegaX2 = input.GetReal("Hydro","Rotation",1);
    this->OmegaX3 = input.GetReal("Hydro","Rotation",2);

    idfx::cout << "Hydro: Rotation enabled with Omega=(" << this->OmegaX1 << ", "
               << this->OmegaX2 << ", " << this->OmegaX3 << ")" << std::endl;
  } else {
    this->haveRotation = false;
  }

  // Check whether we have shearing box
  int shearingbox = input.CheckEntry("Hydro","ShearingBox");

  if(shearingbox>=0 ) {
    this->haveShearingBox = true;
    this->haveSourceTerms = true;
    if(shearingbox != 1) {
      IDEFIX_ERROR("Shearing box needs a scalar value for the shear rate in idefix.ini");
    }

    this->sbS = input.GetReal("Hydro","ShearingBox",0);
    // Get box size
    this->sbLx = grid.xend[IDIR] - grid.xbeg[IDIR];

    idfx::cout << "Hydro: ShearingBox enabled with Shear rate= " << this->sbS
               << "and Lx= " << sbLx << std::endl;
  } else {
    this->haveShearingBox = false;
  }

  // Gravitational potential
  this->haveGravPotential = false;
  this->gravPotentialFunc = nullptr;
  int gravPotential = input.CheckEntry("Hydro","GravPotential");

  if(gravPotential>=0) {
    std::string potentialString = input.GetString("Hydro","GravPotential",0);
    if(potentialString.compare("userdef") == 0) {
      this->haveGravPotential = true;
      idfx::cout << "Hydro:: Enabling user-defined gravitational potential" << std::endl;
    } else {
      IDEFIX_ERROR("Unknown type of gravitational potential in idefix.ini. "
                   "Only userdef is implemented");
    }
  }

  // Parabolic term
  haveParabolicTerms = false;

  // Viscosity
  haveViscosity = false;

  // Nonideal MHD
  haveResistivity = Disabled;
  haveHall = Disabled;
  haveAmbipolar = Disabled;

  ohmicDiffusivityFunc = NULL;
  ambipolarDiffusivityFunc = NULL;
  hallDiffusivityFunc = NULL;

  this->needCurrent = false;

  // Check whether viscosity is enabled, if so, construct a viscosity object
  if(input.CheckEntry("Hydro","Viscosity")>=0) {
    this->haveParabolicTerms = true;
    this->haveViscosity = true;
    this->viscosity = Viscosity(input, grid, this);
  }
  
#if MHD == YES
  if(input.CheckEntry("Hydro","Resistivity")>=0 ||
     input.CheckEntry("Hydro","Ambipolar")>=0 ||
     input.CheckEntry("Hydro","Hall")>=0 ) {
    //
    this->needCurrent = true;

    if(input.CheckEntry("Hydro","Resistivity")>=0) {
      if(input.GetString("Hydro","Resistivity",0).compare("constant") == 0) {
        idfx::cout << "Hydro: Enabling Ohmic resistivity with constant diffusivity." << std::endl;
        this->etaO = input.GetReal("Hydro","Resistivity",1);
        this->haveParabolicTerms = true;
        this->haveResistivity = Constant;
      } else if(input.GetString("Hydro","Resistivity",0).compare("userdef") == 0) {
        idfx::cout << "Hydro: Enabling Ohmic resistivity with user-defined diffusivity function."
                   << std::endl;
        this->haveParabolicTerms = true;
        this->haveResistivity = UserDefFunction;
      } else {
        IDEFIX_ERROR("Unknown resistivity definition in idefix.ini. "
                     "Can only be constant or userdef.");
      }
    }

    if(input.CheckEntry("Hydro","Ambipolar")>=0) {
      if(input.GetString("Hydro","Ambipolar",0).compare("constant") == 0) {
        idfx::cout << "Hydro: Enabling ambipolar diffusion with constant diffusivity."
                   << std::endl;
        this->xA = input.GetReal("Hydro","Ambipolar",1);
        this->haveParabolicTerms = true;
        this->haveAmbipolar = Constant;
      } else if(input.GetString("Hydro","Ambipolar",0).compare("userdef") == 0) {
        idfx::cout << "Hydro: Enabling ambipolar diffusion with user-defined diffusivity function."
                   << std::endl;
        this->haveParabolicTerms = true;
        this->haveAmbipolar = UserDefFunction;
      } else {
        IDEFIX_ERROR("Unknown ambipolar definition in idefix.ini. "
                     "Can only be constant or userdef.");
      }
    }

    if(input.CheckEntry("Hydro","Hall")>=0) {
      // Check consistency
      if(mySolver != HLL )
        IDEFIX_ERROR("Hall effect is only compatible with HLL Riemann solver.");

  #if EMF_AVERAGE != ARITHMETIC
      IDEFIX_ERROR("the Hall effect module is demonstrated stable only when using "
                   "EMF_AVERAGE=ARITHMETIC");
  #endif

      if(input.GetString("Hydro","Hall",0).compare("constant") == 0) {
        idfx::cout << "Hydro: Enabling Hall effect with constant diffusivity." << std::endl;
        this->xH = input.GetReal("Hydro","Hall",1);
        this->haveHall = Constant;
      } else if(input.GetString("Hydro","Hall",0).compare("userdef") == 0) {
        idfx::cout << "Hydro: Enabling Hall effect with user-defined diffusivity function."
                   << std::endl;
        this->haveHall = UserDefFunction;
      } else {
        IDEFIX_ERROR("Unknown Hall definition in idefix.ini. Can only be constant or userdef.");
      }
    }
  }
#endif // MHD

/////////////////////////////////////////
//  ALLOCATION SECION ///////////////////
/////////////////////////////////////////

// We now allocate the fields required by the hydro solver
  Vc = IdefixArray4D<real>("Hydro_Vc", NVAR,
                           data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);
  Uc = IdefixArray4D<real>("Hydro_Uc", NVAR,
                           data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);
  Uc0 = IdefixArray4D<real>("Hydro_Uc0", NVAR,
                           data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);

  InvDt = IdefixArray3D<real>("Hydro_InvDt",
                              data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);
  cMax = IdefixArray3D<real>("Hydro_cMax",
                              data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);
  dMax = IdefixArray3D<real>("Hydro_dMax",
                              data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);
  PrimL =  IdefixArray4D<real>("Hydro_PrimL", NVAR,
                               data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);
  PrimR =  IdefixArray4D<real>("Hydro_PrimR", NVAR,
                                data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);
  FluxRiemann =  IdefixArray4D<real>("Hydro_FluxRiemann", NVAR,
                                     data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);

#if MHD == YES
  Vs = IdefixArray4D<real>("Hydro_Vs", DIMENSIONS,
              data->np_tot[KDIR]+KOFFSET, data->np_tot[JDIR]+JOFFSET, data->np_tot[IDIR]+IOFFSET);

  Vs0 = IdefixArray4D<real>("Hydro_Vs0", DIMENSIONS,
              data->np_tot[KDIR]+KOFFSET, data->np_tot[JDIR]+JOFFSET, data->np_tot[IDIR]+IOFFSET);
  this->emf = ElectroMotiveForce(this->data);
#endif


// Allocate gravitational potential when needed
  if(this->haveGravPotential)
    phiP = IdefixArray3D<real>("Hydro_PhiP",
                               data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);

  if(this->needCurrent) {
    // Allocate current (when hydro needs it)
    this->haveCurrent = true;
    J = IdefixArray4D<real>("Hydro_J", 3,
                            data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);
  }

  // Allocate nonideal MHD effects array when a user-defined function is used
  if(this->haveResistivity ==  UserDefFunction)
    etaOhmic = IdefixArray3D<real>("Hydro_etaOhmic",
                                    data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);
  if(this->haveAmbipolar == UserDefFunction)
    xAmbipolar = IdefixArray3D<real>("Hydro_xAmbipolar",
                                     data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);
  if(this->haveHall == UserDefFunction)
    xHall = IdefixArray3D<real>("Hydro_xHall",
                                  data->np_tot[KDIR], data->np_tot[JDIR], data->np_tot[IDIR]);

  // Fill the names of the fields
  for(int i = 0 ; i < NVAR ;  i++) {
    switch(i) {
      case RHO:
        VcName.push_back("RHO");
        break;
      case VX1:
        VcName.push_back("VX1");
        break;
      case VX2:
        VcName.push_back("VX2");
        break;
      case VX3:
        VcName.push_back("VX3");
        break;
      case BX1:
        VcName.push_back("BX1");
        break;
      case BX2:
        VcName.push_back("BX2");
        break;
      case BX3:
        VcName.push_back("BX3");
        break;
#if HAVE_ENERGY
      case PRS:
        VcName.push_back("PRS");
        break;
#endif
      default:
        VcName.push_back("Vc_"+std::to_string(i));
    }
  }

  for(int i = 0 ; i < DIMENSIONS ; i++) {
    switch(i) {
      case 0:
        VsName.push_back("BX1s");
        break;
      case 1:
        VsName.push_back("BX2s");
        break;
      case 2:
        VsName.push_back("BX3s");
        break;
      default:
        VsName.push_back("Vs_"+std::to_string(i));
    }
  }


  idfx::popRegion();
}

void Hydro::EnrollUserDefBoundary(UserDefBoundaryFunc myFunc) {
  this->userDefBoundaryFunc = myFunc;
  this->haveUserDefBoundary = true;
  idfx::cout << "Hydro: User-defined boundary condition has been enrolled" << std::endl;
}

void Hydro::EnrollInternalBoundary(InternalBoundaryFunc myFunc) {
  this->internalBoundaryFunc = myFunc;
  this->haveInternalBoundary = true;
  idfx::cout << "Hydro: User-defined internal boundary condition has been enrolled" << std::endl;
}

void Hydro::EnrollEmfBoundary(EmfBoundaryFunc myFunc) {
  this->emfBoundaryFunc = myFunc;
  this->haveEmfBoundary = true;
  idfx::cout << "Hydro: User-defined EMF boundary condition has been enrolled" << std::endl;
}

void Hydro::EnrollGravPotential(GravPotentialFunc myFunc) {
  if(!this->haveGravPotential) {
    IDEFIX_ERROR("In order to enroll your gravitational potential, "
                 "you need to enable it first in the .ini file.");
  }
  this->gravPotentialFunc = myFunc;
  idfx::cout << "Hydro: User-defined gravitational potential has been enrolled" << std::endl;
}

void Hydro::EnrollUserSourceTerm(SrcTermFunc myFunc) {
  this->userSourceTerm = myFunc;
  this->haveUserSourceTerm = true;
  this->haveSourceTerms = true;
  idfx::cout << "Hydro: User-defined source term has been enrolled" << std::endl;
}

void Hydro::EnrollOhmicDiffusivity(DiffusivityFunc myFunc) {
  if(this->haveResistivity < UserDefFunction) {
    IDEFIX_ERROR("Ohmic diffusivity enrollment requires Hydro/Resistivity "
                 "to be set to userdef in .ini file");
  }
  this->ohmicDiffusivityFunc = myFunc;
  idfx::cout << "Hydro: User-defined ohmic diffusivity has been enrolled" << std::endl;
}

void Hydro::EnrollAmbipolarDiffusivity(DiffusivityFunc myFunc) {
  if(this->haveAmbipolar < UserDefFunction) {
    IDEFIX_ERROR("Ambipolar diffusivity enrollment requires Hydro/Ambipolar "
                 "to be set to userdef in .ini file");
  }
  this->ambipolarDiffusivityFunc = myFunc;
  idfx::cout << "Hydro: User-defined ambipolar diffusivity has been enrolled" << std::endl;
}

void Hydro::EnrollHallDiffusivity(DiffusivityFunc myFunc) {
  if(this->haveHall < UserDefFunction) {
    IDEFIX_ERROR("Hall diffusivity enrollment requires Hydro/Hall "
                 "to be set to userdef in .ini file");
  }
  this->hallDiffusivityFunc = myFunc;
  idfx::cout << "Hydro: User-defined Hall diffusivity has been enrolled" << std::endl;
}

Hydro::Hydro() {
  // Do nothing !
}

real Hydro::GetGamma() {
  return(this->gamma);
}

real Hydro::GetC2iso() {
  return(this->C2Iso);
}

void Hydro::ResetStage() {
  // Reset variables required at the beginning of each stage (essentially linked to timestep evaluation)
  idfx::pushRegion("Hydro::ResetStage");

  IdefixArray3D<real> InvDt=this->InvDt;
  IdefixArray3D<real> dMax=this->dMax;
  bool haveParabolicTerms=this->haveParabolicTerms;

  idefix_for("HydroResetStage",0,data->np_tot[KDIR],0,data->np_tot[JDIR],0,data->np_tot[IDIR],
    KOKKOS_LAMBDA (int k, int j, int i) {
      InvDt(k,j,i) = ZERO_F;
      if(haveParabolicTerms) {
        dMax(k,j,i) = ZERO_F;
      }
  });

  idfx::popRegion();
}