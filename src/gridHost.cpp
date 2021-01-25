// ***********************************************************************************
// Idefix MHD astrophysical code
// Copyright(C) 2020-2021 Geoffroy R. J. Lesur <geoffroy.lesur@univ-grenoble-alpes.fr>
// and other code contributors
// Licensed under CeCILL 2.1 License, see COPYING for more information
// ***********************************************************************************

#include <string>

#include "idefix.hpp"
#include "grid.hpp"
#include "gridHost.hpp"

GridHost::GridHost() {
  // Void
}

GridHost::GridHost(Grid &grid) {
  idfx::pushRegion("GridHost::GridHost(Grid)");

  this->grid=&grid;
  for(int dir = 0 ; dir < 3 ; dir++) {
    nghost[dir] = grid.nghost[dir];
    np_tot[dir] = grid.np_tot[dir];
    np_int[dir] = grid.np_int[dir];

    lbound[dir] = grid.lbound[dir];
    rbound[dir] = grid.rbound[dir];

    xbeg[dir] = grid.xbeg[dir];
    xend[dir] = grid.xend[dir];
  }

  this->haveAxis = grid.haveAxis;

  // Create mirrors on host
  for(int dir = 0 ; dir < 3 ; dir++) {
    x[dir] = Kokkos::create_mirror_view(grid.x[dir]);
    xr[dir] = Kokkos::create_mirror_view(grid.xr[dir]);
    xl[dir] = Kokkos::create_mirror_view(grid.xl[dir]);
    dx[dir] = Kokkos::create_mirror_view(grid.dx[dir]);
  }

  idfx::popRegion();
}

void GridHost::MakeGrid(Input &input) {
  idfx::pushRegion("GridHost::MakeGrid");

  real xstart[3];
  real xend[3];
  // Create the grid

  // Get grid parameters from input file, block [Grid]
  idfx::cout << "GridHost::MakeGrid" << std::endl;
  for(int dir = 0 ; dir < 3 ; dir++) {
    std::string label = std::string("X")+std::to_string(dir+1)+std::string("-grid");
    int numPatch = input.GetInt("Grid",label,0);

    xstart[dir] = input.GetReal("Grid",label,1);
    xend[dir] = input.GetReal("Grid",label,4+(numPatch-1)*3);

    this->xbeg[dir] = xstart[dir];
    this->xend[dir] = xend[dir];


    if(dir<DIMENSIONS) {
      // Loop on all the patches
      int idxstart = nghost[dir];
      for(int patch = 0 ; patch < numPatch ; patch++) {
        std::string patchType = input.GetString("Grid",label,3+patch*3);
        real patchStart = input.GetReal("Grid",label,1+patch*3);
        real patchEnd = input.GetReal("Grid",label,4+patch*3);
        int patchSize = input.GetInt("Grid",label,2+patch*3);

        // If this is the first or last patch, also define ghost cells
        int ghostStart = 0;
        int ghostEnd = 0;
        if(patch == 0) ghostStart = nghost[dir];
        if(patch == numPatch-1) ghostEnd = nghost[dir];
        // Define the grid depending on patch type
        if(patchType.compare("u")==0) {
          // Uniform patch
          for(int i = 0 - ghostStart ; i < patchSize + ghostEnd ; i++) {
            dx[dir](i+idxstart) = (patchEnd-patchStart)/(patchSize);
            x[dir](i+idxstart)=patchStart + (i+HALF_F)*dx[dir](i+idxstart);
            xl[dir](i+idxstart)=patchStart  + i*dx[dir](i+idxstart);
            xr[dir](i+idxstart)=patchStart  + (i+1)*dx[dir](i+idxstart);
          }
        } else if(patchType.compare("l")==0) {
          // log-increasing patch

          double alpha = (patchEnd + fabs(patchStart) - patchStart)/fabs(patchStart);

          for(int i = 0 - ghostStart ; i < patchSize + ghostEnd ; i++) {
            xl[dir](i+idxstart) = patchStart * pow(alpha,
                                    static_cast<double>(i) / (static_cast<double>(patchSize)));
            xr[dir](i+idxstart) = patchStart * pow(alpha,
                                    static_cast<double>(i+1) / (static_cast<double>(patchSize)));
            dx[dir](i+idxstart) = xr[dir](i+idxstart) - xl[dir](i+idxstart);
            x[dir](i+idxstart)= 0.5*(xr[dir](i+idxstart) + xl[dir](i+idxstart));
          }
        } else if((patchType.compare("s+")==0)||(patchType.compare("s-")==0)) {
          // Stretched grid
          // - means we take the initial dx on the left side, + on the right side
          // refPatch corresponds to the patch from which we compute the initial dx
          // of the stretched grid

          int refPatch=patch;
          if(patchType.compare("s+")==0) {
            refPatch=patch+1;
          } else {
            refPatch=patch-1;
          }
          // Sanity check
          // Check that the reference patch actually exist
          if(refPatch<0 || refPatch >= numPatch) {
            IDEFIX_ERROR("You're attempting to construct a stretched patch "
                         "from a non-existent patch");
          }
          // Check that the reference patch is a uniform one
          if(input.GetString("Grid",label,3+3*refPatch).compare("u")) {
            IDEFIX_ERROR("You're attempting to construct a stretched patch "
                         "from a non-uniform grid");
          }
          // Ok, we have a well-behaved reference patch, compute dx from the reference patch
          real refPatchStart = input.GetReal("Grid",label,1+refPatch*3);
          real refPatchEnd = input.GetReal("Grid",label,4+refPatch*3);
          int refPatchSize = input.GetInt("Grid",label,2+refPatch*3);
          double delta = (refPatchEnd-refPatchStart)/refPatchSize;
          double logdelta = log((patchEnd-patchStart)/delta);
          // Next we have to compute the stretch factor q. Let's start with a guess
          double q=1.05;
          // Use Newton method
          for(int iter=0; iter <= 50; iter++) {
            double f = log((pow(q,patchSize+1)-q)/(q-1))-logdelta;
            double fp = ((patchSize+1)*pow(q,patchSize)-1)/(pow(q,patchSize+1)-q)-1/(q-1);
            double dq = f/fp;
            // advance the guess
            q = q - dq;
            // Check whether we have converged
            if(fabs(dq)<1e-14*q) break;
            if(iter==50) IDEFIX_ERROR("Failed to create the stretched grid");
          }
          // once we know q, we can make the grid
          if(patchType.compare("s-")==0) {
            for(int i = 0 - ghostStart ; i < patchSize + ghostEnd ; i++) {
              xl[dir](i+idxstart) = patchStart + q*(pow(q,i)-1)/(q-1)*delta;
              xr[dir](i+idxstart) = patchStart + q*(pow(q,i+1)-1)/(q-1)*delta;
              dx[dir](i+idxstart) = pow(q,i+1)*delta;
              x[dir](i+idxstart)= 0.5*(xr[dir](i+idxstart) + xl[dir](i+idxstart));
            }
          } else {
            for(int i = 0 - ghostStart ; i < patchSize + ghostEnd ; i++) {
              xl[dir](i+idxstart) = patchEnd - q*(pow(q,patchSize-i)-1)/(q-1)*delta;
              xr[dir](i+idxstart) = patchEnd - q*(pow(q,patchSize-i-1)-1)/(q-1)*delta;
              dx[dir](i+idxstart) = pow(q,patchSize-i+1)*delta;
              x[dir](i+idxstart)= 0.5*(xr[dir](i+idxstart) + xl[dir](i+idxstart));
            }
          }
        } else {
          std::stringstream msg;
          msg << "GridHost::MakeGrid: Unknown grid type :" << patchType << std::endl;
          IDEFIX_ERROR(msg);
        }

        // Increment offset
        idxstart += patchSize;
      }

      std::string lboundString, rboundString;
      switch(lbound[dir]) {
        case outflow:
          lboundString="outflow";
          break;
        case reflective:
          lboundString="reflective";
          break;
        case periodic:
          lboundString="periodic";
          break;
        case internal:
          lboundString="internal";
          break;
        case shearingbox:
          lboundString="shearingbox";
          break;
        case axis:
          lboundString="axis";
          break;
        case userdef:
          lboundString="userdef";
          break;
        default:
          lboundString="unknown";
      }
      switch(rbound[dir]) {
        case outflow:
          rboundString="outflow";
          break;
        case reflective:
          rboundString="reflective";
          break;
        case periodic:
          rboundString="periodic";
          break;
        case internal:
          rboundString="internal";
          break;
        case shearingbox:
          rboundString="shearingbox";
          break;
        case axis:
          lboundString="axis";
          break;
        case userdef:
          rboundString="userdef";
          break;
        default:
          rboundString="unknown";
      }

      idfx::cout << "\t Direction X" << (dir+1) << ": " << lboundString << "\t" << xstart[dir]
                 << "...." << np_int[dir] << "...." << xend[dir] << "\t" << rboundString
                 << std::endl;
    } else {
      // dir >= DIMENSIONS/ Init simple uniform grid
      for(int i = 0 ; i < np_tot[dir] ; i++) {
        dx[dir](i) = (xend[dir]-xstart[dir])/(np_int[dir]);
        x[dir](i)=xstart[dir] + (i-nghost[dir]+HALF_F)*dx[dir](i);
        xl[dir](i)=xstart[dir] + (i-nghost[dir])*dx[dir](i);
        xr[dir](i)=xstart[dir] + (i-nghost[dir]+1)*dx[dir](i);
      }
    }
  }

  // Check that axis treatment is compatible with the domain
if(haveAxis) {
  #if GEOMETRY != SPHERICAL
    IDEFIX_ERROR("Axis boundaries only compatible with Spherical boundary conditions");
  #endif
  #if DIMENSIONS < 2
    IDEFIX_ERROR("Axis Boundaries requires at least two dimenions");
  #endif
  if((fabs(xbeg[JDIR])>1e-10) && (lbound[JDIR] == axis)) {
    IDEFIX_ERROR("Axis Boundaries requires your X2 domain to start at exactly x2=0.0");
  }
  if((fabs(xend[JDIR]-M_PI)>1e-10) && (rbound[JDIR] == axis )) {
    IDEFIX_ERROR("Axis Boundaries requires your X2 domain to end at exactly x2=Pi");
  }
}

  idfx::popRegion();
}

void GridHost::SyncFromDevice() {
  idfx::pushRegion("GridHost::SyncFromDevice");

  for(int dir = 0 ; dir < 3 ; dir++) {
    Kokkos::deep_copy(x[dir],grid->x[dir]);
    Kokkos::deep_copy(xr[dir],grid->xr[dir]);
    Kokkos::deep_copy(xl[dir],grid->xl[dir]);
    Kokkos::deep_copy(dx[dir],grid->dx[dir]);

    xbeg[dir] = grid->xbeg[dir];
    xend[dir] = grid->xend[dir];
  }

  idfx::popRegion();
}

void GridHost::SyncToDevice() {
  idfx::pushRegion("GridHost::SyncToDevice");

  // Sync with the device
  for(int dir = 0 ; dir < 3 ; dir++) {
    Kokkos::deep_copy(grid->x[dir],x[dir]);
    Kokkos::deep_copy(grid->xr[dir],xr[dir]);
    Kokkos::deep_copy(grid->xl[dir],xl[dir]);
    Kokkos::deep_copy(grid->dx[dir],dx[dir]);

    grid->xbeg[dir] = xbeg[dir];
    grid->xend[dir] = xend[dir];
  }

  idfx::popRegion();
}
