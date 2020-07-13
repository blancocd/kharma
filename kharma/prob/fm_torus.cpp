
#include "fm_torus.hpp"

#include "mpi.hpp"
#include "prob_common.hpp"

#include <random>
#include "Kokkos_Random.hpp"

void InitializeFMTorus(MeshBlock *pmb, GRCoordinates& G, GridVars P, const EOS* eos,
                       GReal rin, GReal rmax, Real kappa)
{
    int n1 = pmb->cellbounds.ncellsi(IndexDomain::entire);
    int n2 = pmb->cellbounds.ncellsj(IndexDomain::entire);
    int n3 = pmb->cellbounds.ncellsk(IndexDomain::entire);

    // Get coordinate system pointers for later
    // Only compatible with KS coords as base (TODO BL for fun?)
    SphKSCoords ks = mpark::get<SphKSCoords>(G.coords.base);
    SphBLCoords bl = SphBLCoords(ks.a);

    // Fishbone-Moncrief parameters
    Real l = lfish_calc(ks.a, rmax);

    FLAG("Initializing rho");
    pmb->par_for("fm_torus_init", 0, n3-1, 0, n2-1, 0, n1-1,
        KOKKOS_LAMBDA_3D {
            GReal Xnative[GR_DIM], Xembed[GR_DIM];
            G.coord(k, j, i, Loci::center, Xnative);
            G.coord_embed(k, j, i, Loci::center, Xembed);
            GReal r = Xembed[1], th = Xembed[2];
            GReal sth = sin(th);
            GReal cth = cos(th);

            Real lnh = lnh_calc(ks.a, l, rin, r, th);

            // Region inside magnetized torus; u^i is calculated in
            // Boyer-Lindquist coordinates, as per Fishbone & Moncrief,
            // so it needs to be transformed at the end
            // everything outside/else is initialized to 0 already
            if (lnh > 0. && r > rin) {
                GReal sth = sin(th);
                GReal cth = cos(th);

                Real r2 = pow(r, 2);
                Real a2 = pow(ks.a, 2);
                Real DD = r2 - 2. * r + a2;
                Real AA = pow(r2 + a2, 2) - DD * a2 * sth * sth;
                Real SS = r2 + a2 * cth * cth;

                // Calculate rho and u
                Real hm1 = exp(lnh) - 1.;
                Real rho = pow(hm1 * (eos->gam - 1.) / (kappa * eos->gam),
                                    1. / (eos->gam - 1.));
                Real u = kappa * pow(rho, eos->gam) / (eos->gam - 1.);

                // Calculate u^phi
                Real expm2chi = SS * SS * DD / (AA * AA * sth * sth);
                Real up1 = sqrt((-1. + sqrt(1. + 4. * l * l * expm2chi)) / 2.);
                Real up = 2. * ks.a * r * sqrt(1. + up1 * up1) / sqrt(AA * SS * DD) +
                            sqrt(SS / AA) * up1 / sth;

                // Convert u^phi from 3-velocity to 4-velocity
                Real ucon_bl[GR_DIM] = {0., 0., 0., up};
                Real gcov_bl[GR_DIM][GR_DIM];
                bl.gcov_embed(Xembed, gcov_bl);
                set_ut(gcov_bl, ucon_bl);

                // Then transform that 4-vector to KS, then to native
                Real ucon_ks[GR_DIM], ucon_mks[GR_DIM];
                ks.vec_from_bl(Xembed, ucon_bl, ucon_ks);
                G.coords.con_vec_to_native(Xnative, ucon_ks, ucon_mks);

                // Convert native 4-vector to primitive u-twiddle, see Gammie '04
                Real gcon[GR_DIM][GR_DIM], u_prim[GR_DIM];
                G.gcon(Loci::center, j, i, gcon);
                fourvel_to_prim(gcon, ucon_mks, u_prim);

                P(prims::rho, k, j, i) = rho;
                P(prims::u, k, j, i) = u;
                P(prims::u1, k, j, i) = u_prim[1];
                P(prims::u2, k, j, i) = u_prim[2];
                P(prims::u3, k, j, i) = u_prim[3];
                P(prims::B1, k, j, i) = 0;
                P(prims::B2, k, j, i) = 0;
                P(prims::B3, k, j, i) = 0;
            }
        }
    );

    // Find rho_max "analytically" by looking over the whole mesh domain for the maximum in the midplane
    // Done device-side not for speed (should be fast regardless) but so that *eos dereferences correctly!
    // Note this will have to either repeat or change a bit to record u_max if that's needed...
    // Also note this covers the full domain, so we don't need to MPI reduce 
    GReal xin = log(rin);
    GReal xout = pmb->pmy_mesh->mesh_size.x1max;
    GReal dx = 0.001;
    int nx = (xout - xin) / dx + 1;

    Real rho_max = 0;
    Kokkos::Max<Real> max_reducer(rho_max);
    Kokkos::parallel_reduce("fm_torus_maxrho", nx,
        KOKKOS_LAMBDA_1D_REDUCE {
            GReal x = xin + i*dx;
            GReal r = exp(x);
            GReal th = M_PI/2;

            // Abbreviated version of the full primitives calculation
            Real lnh = lnh_calc(ks.a, l, rin, r, th);

            // Calculate rho
            Real hm1 = exp(lnh) - 1.;
            Real rho = pow(hm1 * (eos->gam - 1.) / (kappa * eos->gam),
                                1. / (eos->gam - 1.));
            //Real u = kappa * pow(rho, eos->gam) / (eos->gam - 1.);

            // Record max
            if (rho > local_result) local_result = rho;
            //if (u > u_max) u_max = u;
        }
    , max_reducer);

    pmb->par_for("fm_torus_normalize", 0, n3-1, 0, n2-1, 0, n1-1,
        KOKKOS_LAMBDA_3D {
            P(prims::rho, k, j, i) /= rho_max;
            P(prims::u, k, j, i) /= rho_max;
        }
    );
}

void PerturbU(MeshBlock *pmb, GridVars P, Real u_jitter, int rng_seed=31337)
{
    int n1 = pmb->cellbounds.ncellsi(IndexDomain::entire);
    int n2 = pmb->cellbounds.ncellsj(IndexDomain::entire);
    int n3 = pmb->cellbounds.ncellsk(IndexDomain::entire);
    // Should only jitter physical zones...

    // SERIAL VERSION -- better determinism guarantee but CPU only
    // Initialize RNG
    // std::mt19937 gen(rng_seed);
    // std::uniform_real_distribution<Real> dis(-u_jitter/2, u_jitter/2);

    // for(int k=0; k < n3; k++)
    //     for(int j=0; j < n2; j++)
    //         for(int i=0; i < n3; i++)
    //             P(prims::u, k, j, i) *= 1. + dis(gen);


    // Kokkos version.  This would probably be much faster (and more deterministic?) as 2D internal, 1D outside
    // TODO check determinism...
    typedef typename Kokkos::Random_XorShift64_Pool<> RandPoolType;
    RandPoolType rand_pool(rng_seed);
    typedef typename RandPoolType::generator_type gen_type;
    pmb->par_for("perturb_u", 0, n3-1, 0, n2-1, 0, n1-1,
        KOKKOS_LAMBDA_3D {
            gen_type rgen = rand_pool.get_state();
            P(prims::u, k, j, i) *= 1. + Kokkos::rand<gen_type, Real>::draw(rgen, -u_jitter/2, u_jitter/2);
            rand_pool.free_state(rgen);
        }
    );
}