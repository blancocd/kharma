/* 
 *  File: harm.cpp
 *  
 *  BSD 3-Clause License
 *  
 *  Copyright (c) 2020, AFD Group at UIUC
 *  All rights reserved.
 *  
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  
 *  1. Redistributions of source code must retain the above copyright notice, this
 *     list of conditions and the following disclaimer.
 *  
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *  
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <iostream>

#include <parthenon/parthenon.hpp>
#include <interface/update.hpp>

#include "decs.hpp"

#include "bondi.hpp"
#include "boundaries.hpp"
#include "debug.hpp"
#include "fixup.hpp"
#include "fluxes.hpp"
#include "grmhd.hpp"
#include "harm.hpp"

#include "iharm_restart.hpp"

TaskCollection HARMDriver::MakeTaskCollection(BlockList_t &blocks, int stage)
{
    // TODO extra Refinement flux steps in whatever form they look like when I get around to SMR :)
    // Then same with updating refinement when maybe eventually AMR
    // TODO Figure out when in here to grab the beginning & end states & calculate jcon,
    // then only do so on output steps

    // TaskCollections are split into regions, each of which can be tackled by a specified number of independent threads.
    // We inherit our split, like most things in this function, from the advection_example in Parthenon
    using namespace Update;
    TaskCollection tc;
    TaskID t_none(0);

    const Real beta = integrator->beta[stage - 1];
    const Real dt = integrator->dt;

    auto num_task_lists_executed_independently = blocks.size();
    TaskRegion &async_region1 = tc.AddRegion(num_task_lists_executed_independently);

    for (int i = 0; i < blocks.size(); i++) {
        auto &pmb = blocks[i];
        auto &tl = async_region1[i];
        // first make other useful containers
        if (stage == 1) {
            auto &base = pmb->meshblock_data.Get();
            pmb->meshblock_data.Add("dUdt", base);
            for (int i = 1; i < integrator->nstages; i++)
                pmb->meshblock_data.Add(stage_name[i], base);
        }

        // pull out the container we'll use to get fluxes and/or compute RHSs
        auto &sc0 = pmb->meshblock_data.Get(stage_name[stage - 1]);
        // pull out a container we'll use to store dU/dt.
        // This is just -flux_divergence in this example
        auto &dudt = pmb->meshblock_data.Get("dUdt");
        // pull out the container that will hold the updated state
        // effectively, sc1 = sc0 + dudt*dt
        auto &sc1 = pmb->meshblock_data.Get(stage_name[stage]);

        auto t_start_recv = tl.AddTask(t_none, &MeshBlockData<Real>::StartReceiving, sc1.get(),
                                    BoundaryCommSubset::all);

        // Calculate the HLL fluxes in each direction
        // This reconstructs the primitives (P) at faces and uses them to calculate fluxes
        // of the conserved variables (U)
        // All subsequent operations until FillDerived are applied only to U
        auto t_calculate_flux1 = tl.AddTask(t_start_recv, HLLE::GetFlux, sc0, X1DIR);
        auto t_calculate_flux2 = tl.AddTask(t_start_recv, HLLE::GetFlux, sc0, X2DIR);
        auto t_calculate_flux3 = tl.AddTask(t_start_recv, HLLE::GetFlux, sc0, X3DIR);
        auto t_calculate_flux = t_calculate_flux1 | t_calculate_flux2 | t_calculate_flux3;

        // Fix the conserved fluxes (exclusively B1/2/3) so that they obey divB==0
        // TODO combine...
        auto t_fix_flux = tl.AddTask(t_calculate_flux, FixFlux, sc0);
        auto t_flux_ct = tl.AddTask(t_fix_flux, GRMHD::FluxCT, sc0);

        // Parthenon can calculate a flux divergence, but we save a kernel launch by also adding
        // both the GRMHD source term and any "wind" source coefficients
        auto t_flux_apply = tl.AddTask(t_none, GRMHD::ApplyFluxes, tm, sc0.get(), dudt.get());

        if (pmesh->multilevel) { // TODO technically this should be "if face-centered fields"
            auto send_flux =
                tl.AddTask(t_flux_ct, &MeshBlockData<Real>::SendFluxCorrection, sc0.get());
            auto recv_flux =
                tl.AddTask(t_flux_ct, &MeshBlockData<Real>::ReceiveFluxCorrection, sc0.get());
        }
    }

    const int num_partitions = pmesh->DefaultNumPartitions();
    // note that task within this region that contains one tasklist per pack
    // could still be executed in parallel
    TaskRegion &single_tasklist_per_pack_region = tc.AddRegion(num_partitions);
    for (int i = 0; i < num_partitions; i++) {
        auto &tl = single_tasklist_per_pack_region[i];
        auto &mbase = pmesh->mesh_data.GetOrAdd("base", i);
        auto &mc0 = pmesh->mesh_data.GetOrAdd(stage_name[stage - 1], i);
        auto &mc1 = pmesh->mesh_data.GetOrAdd(stage_name[stage], i);
        auto &mdudt = pmesh->mesh_data.GetOrAdd("dUdt", i);

        // TODO mesh-wide ApplyFluxes, or switch back to Divergence+AddSource
        //auto t_flux_apply = tl.AddTask(t_none, GRMHD::ApplyFluxes, tm, mc0.get(), mdudt.get());

        auto t_avg_data = tl.AddTask(t_none, AverageIndependentData<MeshData<Real>>,
                                mc0.get(), mbase.get(), beta);
        // apply du/dt to all independent fields in the container
        auto t_update = tl.AddTask(t_avg_data, UpdateIndependentData<MeshData<Real>>, mc0.get(),
                                mdudt.get(), beta * dt, mc1.get());
    }

    const auto &buffer_send_pack =
        blocks[0]->packages["GRMHD"]->Param<bool>("buffer_send_pack");
    if (buffer_send_pack) {
        TaskRegion &tr = tc.AddRegion(num_partitions);
        for (int i = 0; i < num_partitions; i++) {
            auto &mc1 = pmesh->mesh_data.GetOrAdd(stage_name[stage], i);
            tr[i].AddTask(t_none, cell_centered_bvars::SendBoundaryBuffers, mc1);
        }
    } else {
        TaskRegion &tr = tc.AddRegion(num_task_lists_executed_independently);
        for (int i = 0; i < blocks.size(); i++) {
            auto &sc1 = blocks[i]->meshblock_data.Get(stage_name[stage]);
            tr[i].AddTask(t_none, &MeshBlockData<Real>::SendBoundaryBuffers, sc1.get());
        }
    }

    const auto &buffer_recv_pack =
        blocks[0]->packages["GRMHD"]->Param<bool>("buffer_recv_pack");
    if (buffer_recv_pack) {
        TaskRegion &tr = tc.AddRegion(num_partitions);
        for (int i = 0; i < num_partitions; i++) {
            auto &mc1 = pmesh->mesh_data.GetOrAdd(stage_name[stage], i);
            tr[i].AddTask(t_none, cell_centered_bvars::ReceiveBoundaryBuffers, mc1);
        }
    } else {
        TaskRegion &tr = tc.AddRegion(num_task_lists_executed_independently);
        for (int i = 0; i < blocks.size(); i++) {
            auto &sc1 = blocks[i]->meshblock_data.Get(stage_name[stage]);
            tr[i].AddTask(t_none, &MeshBlockData<Real>::ReceiveBoundaryBuffers, sc1.get());
        }
    }

    const auto &buffer_set_pack =
        blocks[0]->packages["GRMHD"]->Param<bool>("buffer_set_pack");
    if (buffer_set_pack) {
        TaskRegion &tr = tc.AddRegion(num_partitions);
        for (int i = 0; i < num_partitions; i++) {
            auto &mc1 = pmesh->mesh_data.GetOrAdd(stage_name[stage], i);
            tr[i].AddTask(t_none, cell_centered_bvars::SetBoundaries, mc1);
        }
    } else {
        TaskRegion &tr = tc.AddRegion(num_task_lists_executed_independently);
        for (int i = 0; i < blocks.size(); i++) {
            auto &sc1 = blocks[i]->meshblock_data.Get(stage_name[stage]);
            tr[i].AddTask(t_none, &MeshBlockData<Real>::SetBoundaries, sc1.get());
        }
    }

    TaskRegion &async_region2 = tc.AddRegion(num_task_lists_executed_independently);

    for (int i = 0; i < blocks.size(); i++) {
        auto &pmb = blocks[i];
        auto &tl = async_region2[i];
        auto &sc0 = pmb->meshblock_data.Get(stage_name[stage-1]);
        auto &sc1 = pmb->meshblock_data.Get(stage_name[stage]);

        auto t_clear_comm_flags = tl.AddTask(t_none, &MeshBlockData<Real>::ClearBoundary,
                                        sc1.get(), BoundaryCommSubset::all);

        auto t_prolongBound = t_none;
        if (pmesh->multilevel) {
            t_prolongBound = tl.AddTask(t_none, ProlongateBoundaries, sc1);
        }

        // Fill primitives, bringing U and P back into lockstep
        auto t_fill_derived = tl.AddTask(t_prolongBound, Update::FillDerived<MeshBlockData<Real>>, sc1.get());

        // ApplyCustomBoundaries is a catch-all for things HARM needs done:
        // Inflow checks, renormalizations, Bondi outer boundary.  All keep lockstep.
        auto t_set_custom_bc = tl.AddTask(t_fill_derived, ApplyCustomBoundaries, sc1);

        // TODO is this UserWorkAfterLoop material?
        // TODO Should jcon be calculated here?  We would need to know if we're dumping this step...
        auto t_diagnostics = tl.AddTask(t_set_custom_bc, Diagnostic, sc1, IndexDomain::interior);
        auto t_step_done = t_diagnostics;

        // TODO work custom stuff into original Parthenon call?
        //auto set_bc = tl.AddTask(prolongBound, ApplyBoundaryConditions, sc1);

        // Estimate next time step based on ctop from the *ORIGINAL* state, where we calculated the fluxes
        if (stage == integrator->nstages) {
            auto new_dt =
                tl.AddTask(t_step_done, EstimateTimestep<MeshBlockData<Real>>, sc0.get());

            // Update refinement.  For much, much later
            // if (pmesh->adaptive) {
            //     auto tag_refine = tl.AddTask(
            //         t_step_done, parthenon::Refinement::Tag<MeshBlockData<Real>>, sc1.get());
            // }
        }
    }

    return tc;
}