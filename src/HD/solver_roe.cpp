#include "../idefix.hpp"
#include "solvers.hpp"

// Compute Riemann fluxes from states using ROE solver
void Tvdlf(DataBlock & data, int dir, real gamma, real C2Iso) {
    int ioffset,joffset,koffset;

    Kokkos::Profiling::pushRegion("ROE_Solver");
    ioffset=joffset=koffset=0;
    // Determine the offset along which we do the extrapolation
    if(dir==IDIR) ioffset=1;
    if(dir==JDIR) joffset=1;
    if(dir==KDIR) koffset=1;

    IdefixArray4D<real> PrimL = data.PrimL;
    IdefixArray4D<real> PrimR = data.PrimR;
    IdefixArray4D<real> Flux = data.FluxRiemann;
    IdefixArray1D<real> dx = data.dx[dir];
    IdefixArray3D<real> invDt = data.InvDtHyp;

    real gamma_m1 = gamma -ONE_F;
    real gmm1_inv = 1.0/gamma_m1;

    idefix_for("ROE_Kernel",data.beg[KDIR],data.end[KDIR]+koffset,data.beg[JDIR],data.end[JDIR]+joffset,data.beg[IDIR],data.end[IDIR]+ioffset,
                        KOKKOS_LAMBDA (int k, int j, int i) 
            {
                int VXn = VX1+dir;
                int MXn = VXn;
                int VXt, MXt;
                VXt = MXt = VX2;
                // Primitive variables
                real vL[NVAR];
                real vR[NVAR];
                real dv[NVAR];

                // Conservative variables
                real uL[NVAR];
                real uR[NVAR];

                // Flux (left and right)
                real fluxL[NVAR];
                real fluxR[NVAR];
                
                // Roe
                real Rc[NVAR][NVAR];
                real um[NVAR];
                real s, c, vel2;
                real h, hl, hr;
                real a, a2, a2L, a2R;

                // 1-- Store the primitive variables on the left, right, and averaged states
                for(int nv = 0 ; nv < NVAR; nv++) {
                    vL[nv] = PrimL(nv,k,j,i);
                    vR[nv] = PrimR(nv,k,j,i);
                    dv[nv] = vR[nv] - vL[nv];
                }

                // 2-- Compute the conservative variables
                K_PrimToCons(uL, vL, gamma_m1);
                K_PrimToCons(uR, vR, gamma_m1);

                // 3.1 Compute the square of the sound speed
                a2L = gamma * vL[PRS] / vL[RHO];
                a2R = gamma * vR[PRS] / vR[RHO];
                
                // 3.2 Compute the left and right fluxes
                K_Flux(fluxL, vL, uL, C2Iso, dir);
                K_Flux(fluxR, vR, uR, C2Iso, dir);

                // **- Starting Roe
                /*  ----  Define Wave Jumps  ----  */

#if ROE_AVERAGE == YES    
                s       = sqrt(vR[RHO]/vL[RHO]);
                um[RHO] = vL[RHO]*s;
                s       = 1.0/(1.0 + s); 
                c       = 1.0 - s;

                EXPAND(um[VX1] = s*vL[VX1] + c*vR[VX1];  ,
                um[VX2] = s*vL[VX2] + c*vR[VX2];  ,
                um[VX3] = s*vL[VX3] + c*vR[VX3];)

    #if EOS == IDEAL
                vel2 = EXPAND(um[VX1]*um[VX1], + um[VX2]*um[VX2], + um[VX3]*um[VX3]);

                hl  = 0.5*(EXPAND(vL[VX1]*vL[VX1], + vL[VX2]*vL[VX2], + vL[VX3]*vL[VX3]));    
                hl += a2L*gmm1_inv;

                hr = 0.5*(EXPAND(vR[VX1]*vR[VX1], + vR[VX2]*vR[VX2], + vR[VX3]*vR[VX3]));    
                hr += a2R*gmm1_inv;

                h = s*hl + c*hr;

                /* -------------------------------------------------
                the following should be  equivalent to 

                scrh = EXPAND(   dv[VX1]*dv[VX1],
                + dv[VX2]*dv[VX2],
                + dv[VX3]*dv[VX3]);

                a2 = s*a2L + c*a2R + 0.5*gamma_m1*s*c*scrh;

                and therefore always positive.
                just work out the coefficiendnts...
                -------------------------------------------------- */

                a2 = gamma_m1*(h - 0.5*vel2);
                a  = sqrt(a2);
    #endif
#else
                for(int nv = 0 ; nv < NVAR; nv++) {
                    um[nv] = vRL[nv];
                }
    #if EOS == IDEAL
                a2   = g_gamma*um[PRS]/um[RHO];
                a    = sqrt(a2);

                vel2 = EXPAND(um[VX1]*um[VX1], + um[VX2]*um[VX2], + um[VX3]*um[VX3]);
                h    = 0.5*vel2 + a2/gamma_m1;
    #endif /* EOS == IDEAL */
#endif /* ROE_AVERAGE == YES/NO */

#if EOS == ISOTHERMAL
                a2 = 0.5*(a2L + a2R);
                a  = sqrt(a2);
#endif
                
// **********************************************************************************
                /* ----------------------------------------------------------------
                define non-zero components of conservative eigenvectors Rc, 
                eigenvalues (lambda) and wave strenght eta = L.du     
                ----------------------------------------------------------------  */

                real lambda[NVAR], alambda[NVAR];
                real eta[NVAR];
                int nn;
                
                for(int nv1 = 0 ; nv1 < NVAR; nv1++) {
                    for(int nv2 = 0 ; nv2 < NVAR; nv2++) {
                        Rc[nv1][nv2] = 0;
                    }
                }
                
                /*  ---- (u - c_s)  ----  */ 

                nn         = 0;
                lambda[nn] = um[VXn] - a;
#if EOS == IDEAL
                eta[nn] = 0.5/a2*(dv[PRS] - dv[VXn]*um[RHO]*a);
#elif EOS == ISOTHERMAL
                eta[nn] = 0.5*(dv[RHO] - um[RHO]*dv[VXn]/a);
#endif

                Rc[RHO][nn]        = 1.0;
                EXPAND(Rc[MXn][nn] = um[VXn] - a;   ,
                Rc[MXt][nn] = um[VXt];       ,
                Rc[MXb][nn] = um[VXb];)
#if EOS == IDEAL
                Rc[ENG][nn] = h - um[VXn]*a;
#endif

                /*  ---- (u + c_s)  ----  */ 

                nn         = 1;
                lambda[nn] = um[VXn] + a;
#if EOS == IDEAL
                eta[nn]    = 0.5/a2*(dv[PRS] + dv[VXn]*um[RHO]*a);
#elif EOS == ISOTHERMAL
                eta[nn] = 0.5*(dv[RHO] + um[RHO]*dv[VXn]/a);
#endif

                Rc[RHO][nn]        = 1.0;
                EXPAND(Rc[MXn][nn] = um[VXn] + a;   ,
                Rc[MXt][nn] = um[VXt];       ,
                Rc[MXb][nn] = um[VXb];)
#if EOS == IDEAL
                Rc[ENG][nn] = h + um[VXn]*a;
#endif

                /*  ----  (u)  ----  */ 

#if EOS == IDEAL
                nn         = 2;
                lambda[nn] = um[VXn];
                eta[nn]    = dv[RHO] - dv[PRS]/a2;
                Rc[RHO][nn]        = 1.0;
                EXPAND(Rc[MX1][nn] = um[VX1];   ,
                Rc[MX2][nn] = um[VX2];   ,
                Rc[MX3][nn] = um[VX3];)
                Rc[ENG][nn]        = 0.5*vel2;
#endif

#if COMPONENTS > 1

                /*  ----  (u)  ----  */ 

                nn++;
                lambda[nn] = um[VXn];
                eta[nn]    = um[RHO]*dv[VXt];
                Rc[MXt][nn] = 1.0;
    #if EOS == IDEAL
                Rc[ENG][nn] = um[VXt];  
    #endif
#endif

#if COMPONENTS > 2

                /*  ----  (u)  ----  */ 

                nn++;
                lambda[nn] = um[VXn];
                eta[nn]    = um[RHO]*dv[VXb];
                Rc[MXb][nn] = 1.0;
    #if EOS == IDEAL
                Rc[ENG][nn] = um[VXb];  
    #endif
#endif

                /*  ----  get max eigenvalue  ----  */

                real cmax = FABS(um[VXn]) + a;
                //g_maxMach = FMAX(FABS(um[VXn]/a), g_maxMach);

                /* ---------------------------------------------
                use the HLL flux function if the interface 
                lies within a strong shock.
                The effect of this switch is visible
                in the Mach reflection test.
                --------------------------------------------- */

                real scrh, scrh1;
                real bmin, bmax;
#if EOS == IDEAL
                scrh  = FABS(vL[PRS] - vR[PRS]);
                scrh /= FMIN(vL[PRS],vR[PRS]);
#elif EOS == ISOTHERMAL
                scrh  = FABS(vL[RHO] - vR[RHO]);
                scrh /= FMIN(vL[RHO],vR[RHO]);
                scrh *= a*a;
#endif
                
/*#if CHECK_ROE_MATRIX == YES
                for(int nv = 0 ; nv < NVAR; nv++) {
                    um[nv] = 0.0;
                    for(int nv1 = 0 ; nv1 < NVAR; nv1++) {
                        for(int nv2 = 0 ; nv2 < NVAR; nv2++) {
                            um[nv] += Rc[nv][k]*(k==j)*lambda[k]*eta[j];
                        }
                    }
                }
                for(int nv = 0 ; nv < NVAR; nv++) {
                    scrh = fluxR[nv] - fluxL[nv] - um[nv];
                    if (nv == MXn) scrh += pR - pL;
                    if (FABS(scrh) > 1.e-6){
                        print ("! Matrix condition not satisfied %d, %12.6e\n", nv, scrh);
                        exit(1);
                    }
                }
#endif*/
                
                if (scrh > 0.5 && (vR[VXn] < vL[VXn])) {   /* -- tunable parameter -- */
#if DIMENSIONS > 1
                    bmin = FMIN(0.0, lambda[0]);
                    bmax = FMAX(0.0, lambda[1]);
                    scrh1 = 1.0/(bmax - bmin);
                    for(int nv = 0 ; nv < NVAR; nv++) {
                        Flux(nv,k,j,i)  = bmin*bmax*(uR[nv] - uL[nv])
                                +   bmax*fluxL[nv] - bmin*fluxR[nv];
                        Flux(nv,k,j,i) *= scrh1;
                    }
#endif
                }
                else {

                    /* -----------------------------------------------------------
                                        compute Roe flux 
                    ----------------------------------------------------------- */

                    for(int nv = 0 ; nv < NVAR; nv++) {
                        alambda[nv]  = fabs(lambda[nv]);
                    }

                    /*  ----  entropy fix  ----  */
                    real delta = 1.e-7;
                    if (alambda[0] <= delta) {
                        alambda[0] = HALF_F*lambda[0]*lambda[0]/delta + HALF_F*delta;
                    }
                    if (alambda[1] <= delta) {
                        alambda[1] = HALF_F*lambda[1]*lambda[1]/delta + HALF_F*delta;
                    }

                    for(int nv = 0 ; nv < NVAR; nv++) {
                        Flux(nv,k,j,i) = fluxL[nv] + fluxR[nv];
                        for(int nv2 = 0 ; nv2 < NVAR; nv2++) {
                            Flux(nv,k,j,i) -= alambda[nv2]*eta[nv2]*Rc[nv][nv2];
                        }
                        Flux(nv,k,j,i) *= HALF_F;
                    }
                }

                //6-- Compute maximum dt for this sweep
                const int ig = ioffset*i + joffset*j + koffset*k;

                invDt(k,j,i) += cmax/dx(ig);

            });

    Kokkos::Profiling::popRegion();

}
