// Copyright (c) 2015-2017, RAPtor Developer Team
// License: Simplified BSD, http://opensource.org/licenses/BSD-2-Clause
#ifndef RAPTOR_ML_PARMULTILEVEL_H
#define RAPTOR_ML_PARMULTILEVEL_H

#include "core/types.hpp"
#include "core/par_matrix.hpp"
#include "core/par_vector.hpp"
#include "multilevel/par_level.hpp"
#include "util/linalg/par_relax.hpp"
#include "ruge_stuben/par_interpolation.hpp"
#include "ruge_stuben/par_cf_splitting.hpp"
#include "multilevel/par_sparsify.hpp"

#ifdef USING_HYPRE
#include "_hypre_utilities.h"
#include "HYPRE.h"
#include "_hypre_parcsr_mv.h"
#include "_hypre_parcsr_ls.h"
#endif

/**************************************************************
 *****   ParMultilevel Class
 **************************************************************
 ***** This class constructs a parallel multigrid hierarchy
 *****
 ***** Attributes
 ***** -------------
 ***** Af : ParCSRMatrix*
 *****    Fine-level matrix
 ***** strength_threshold : double (default 0.0)
 *****    Threshold for strong connections
 ***** coarsen_type : coarsen_t (default Falgout)
 *****    Type of coarsening scheme.  Options are 
 *****      - RS : ruge stuben splitting
 *****      - CLJP 
 *****      - Falgout : RS on_proc, but CLJP on processor boundaries
 *****      - PMIS 
 *****      - HMIS
 ***** interp_type : interp_t (default Direct)
 *****    Type of interpolation scheme.  Options are
 *****      - Direct 
 *****      - Classical (modified classical interpolation)
 *****      - Extended (extended + i interpolation)
 ***** relax_type : relax_t (default SOR)
 *****    Relaxation scheme used in every cycle of solve phase.
 *****    Options are:
 *****      - Jacobi: weighted jacobi for both on and off proc
 *****      - SOR: weighted jacobi off_proc, SOR on_proc
 *****      - SSOR : weighted jacobi off_proc, SSOR on_proc
 ***** num_smooth_sweeps : int (defualt 1)
 *****    Number of relaxation sweeps (both pre and post smoothing)
 *****    to be performed during each cycle of the AMG solve.
 ***** relax_weight : double
 *****    Weight used in Jacobi, SOR, or SSOR
 ***** max_coarse : int (default 50)
 *****    Maximum global num rows allowed in coarsest matrix
 ***** max_levels : int (default -1)
 *****    Maximum number of levels in hierarchy, or no maximum if -1
 ***** 
 ***** Methods
 ***** -------
 ***** solve(x, b, num_iters)
 *****    Solves system Ax = b, performing at most num_iters iterations
 *****    of AMG.
 **************************************************************/

namespace raptor
{
    // BLAS LU routine that is used for coarse solve
    extern "C" void dgetrf_(int* dim1, int* dim2, double* a, int* lda, 
            int* ipiv, int* info);
    extern "C" void dgetrs_(char *TRANS, int *N, int *NRHS, double *A, 
            int *LDA, int *IPIV, double *B, int *LDB, int *INFO );

    class ParMultilevel
    {
        public:

            ParMultilevel(double _strong_threshold, 
                    strength_t _strength_type,
                    relax_t _relax_type) // which level to start tap_amg (-1 == no TAP)
            {
                strong_threshold = _strong_threshold;
                strength_type = _strength_type;
                relax_type = _relax_type;
                num_smooth_sweeps = 1;
                relax_weight = 1.0;
                max_coarse = 50;
                max_levels = 25;
                tap_amg = -1;
                weights = NULL;
                store_residuals = true;
                sparsify_tol = 0.0;
            }

            virtual ~ParMultilevel()
            {
                if (levels[num_levels-1]->A->local_num_rows)
                {
                    MPI_Comm_free(&coarse_comm);
                }
                for (std::vector<ParLevel*>::iterator it = levels.begin();
                        it != levels.end(); ++it)
                {
                    delete *it;
                }
            }
            
            virtual void setup(ParCSRMatrix* Af) = 0;

            void setup_helper(ParCSRMatrix* Af)
            {
                double t0;
                int rank, num_procs;
                MPI_Comm_rank(MPI_COMM_WORLD, &rank);
                MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
                int last_level = 0;

                t0 = MPI_Wtime();

                // Add original, fine level to hierarchy
                levels.push_back(new ParLevel());
                levels[0]->A = new ParCSRMatrix(Af);
                levels[0]->A->sort();
                levels[0]->A->on_proc->move_diag();
                levels[0]->x.resize(Af->global_num_rows, Af->local_num_rows,
                        Af->partition->first_local_row);
                levels[0]->b.resize(Af->global_num_rows, Af->local_num_rows,
                        Af->partition->first_local_row);
                levels[0]->tmp.resize(Af->global_num_rows, Af->local_num_rows,
                        Af->partition->first_local_row);
                if (tap_amg == 0 && !Af->tap_comm)
                {
                    levels[0]->A->tap_comm = new TAPComm(Af->partition,
                            Af->off_proc_column_map, Af->on_proc_column_map);
                }
                strength_times.push_back(0);
                coarsen_times.push_back(0);
                interp_times.push_back(0);
                matmat_times.push_back(0);
                matmat_comm_times.push_back(0);

                if (weights == NULL)
                {
                    form_rand_weights(Af->local_num_rows, Af->partition->first_local_row);
                }

                // Add coarse levels to hierarchy 
                while (levels[last_level]->A->global_num_rows > max_coarse && 
                        (max_levels == -1 || (int) levels.size() < max_levels))
                {
                    extend_hierarchy();
                    last_level++;

                    strength_times.push_back(0);
                    coarsen_times.push_back(0);
                    interp_times.push_back(0);
                    matmat_times.push_back(0);
                    matmat_comm_times.push_back(0);
                }

                if (sparsify_tol > 0.0)
                {
                    for (int i = 0; i < num_levels-1; i++)
                    {
                        ParLevel* l = levels[i];
                        sparsify(l->A, l->P, l->I, l->AP, levels[i+1]->A, sparsify_tol);
                        delete l->AP;
                        delete l->I;
                        l->AP = NULL;
                        l->I = NULL;
                    }
                }

                num_levels = levels.size();
                delete[] weights;

                // Duplicate coarsest level across all processes that hold any
                // rows of A_c
                duplicate_coarse();

                setup_times.push_back(MPI_Wtime() - t0);
            }


            void form_rand_weights(int local_n, int first_n)
            {
                if (local_n == 0) return;

                weights = new double[local_n];
                srand(2448422 + first_n);
                for (int i = 0; i < local_n; i++)
                {
                    weights[i] = double(rand())/RAND_MAX;
                }
            }
                
            virtual void extend_hierarchy() = 0;

            void duplicate_coarse()
            {
                int rank, num_procs;
                MPI_Comm_rank(MPI_COMM_WORLD, &rank);
                MPI_Comm_size(MPI_COMM_WORLD, &num_procs);

                int last_level = num_levels - 1;
                ParCSRMatrix* Ac = levels[last_level]->A;
                aligned_vector<int> proc_sizes(num_procs);
                aligned_vector<int> active_procs;
                MPI_Allgather(&(Ac->local_num_rows), 1, MPI_INT, proc_sizes.data(),
                        1, MPI_INT, MPI_COMM_WORLD);
                for (int i = 0; i < num_procs; i++)
                {
                    if (proc_sizes[i])
                    {
                        active_procs.push_back(i);
                    }
                }
                MPI_Group world_group;
                MPI_Comm_group(MPI_COMM_WORLD, &world_group);                
                MPI_Group active_group;
                MPI_Group_incl(world_group, active_procs.size(), active_procs.data(),
                        &active_group);
                MPI_Comm_create_group(MPI_COMM_WORLD, active_group, 0, &coarse_comm);
                if (Ac->local_num_rows)
                {
                    int num_active, active_rank;
                    MPI_Comm_rank(coarse_comm, &active_rank);
                    MPI_Comm_size(coarse_comm, &num_active);

                    int proc;
                    int global_col, local_col;
                    int start, end;

                    aligned_vector<double> A_coarse_lcl;

                    // Gather global col indices
                    coarse_sizes.resize(num_active);
                    coarse_displs.resize(num_active+1);
                    coarse_displs[0] = 0;
                    for (int i = 0; i < num_active; i++)
                    {
                        proc = active_procs[i];
                        coarse_sizes[i] = proc_sizes[proc];
                        coarse_displs[i+1] = coarse_displs[i] + coarse_sizes[i]; 
                    }

                    aligned_vector<int> global_row_indices(coarse_displs[num_active]);

                    MPI_Allgatherv(Ac->local_row_map.data(), Ac->local_num_rows, MPI_INT,
                            global_row_indices.data(), coarse_sizes.data(), 
                            coarse_displs.data(), MPI_INT, coarse_comm);
    
                    std::map<int, int> global_to_local;
                    int ctr = 0;
                    for (aligned_vector<int>::iterator it = global_row_indices.begin();
                            it != global_row_indices.end(); ++it)
                    {
                        global_to_local[*it] = ctr++;
                    }

                    coarse_n = Ac->global_num_rows;
                    A_coarse_lcl.resize(coarse_n*Ac->local_num_rows, 0);
                    for (int i = 0; i < Ac->local_num_rows; i++)
                    {
                        start = Ac->on_proc->idx1[i];
                        end = Ac->on_proc->idx1[i+1];
                        for (int j = start; j < end; j++)
                        {
                            global_col = Ac->on_proc_column_map[Ac->on_proc->idx2[j]];
                            local_col = global_to_local[global_col];
                            A_coarse_lcl[i*coarse_n + local_col] = Ac->on_proc->vals[j];
                        }

                        start = Ac->off_proc->idx1[i];
                        end = Ac->off_proc->idx1[i+1];
                        for (int j = start; j < end; j++)
                        {
                            global_col = Ac->off_proc_column_map[Ac->off_proc->idx2[j]];
                            local_col = global_to_local[global_col];
                            A_coarse_lcl[i*coarse_n + local_col] = Ac->off_proc->vals[j];
                        }
                    }

                    A_coarse.resize(coarse_n*coarse_n);
                    for (int i = 0; i < num_active; i++)
                    {
                        coarse_sizes[i] *= coarse_n;
                        coarse_displs[i+1] *= coarse_n;
                    }
                    
                    MPI_Allgatherv(A_coarse_lcl.data(), A_coarse_lcl.size(), MPI_DOUBLE,
                            A_coarse.data(), coarse_sizes.data(), coarse_displs.data(), 
                            MPI_DOUBLE, coarse_comm);

                    LU_permute.resize(coarse_n);
                    int info;
                    dgetrf_(&coarse_n, &coarse_n, A_coarse.data(), &coarse_n, 
                            LU_permute.data(), &info);

                    for (int i = 0; i < num_active; i++)
                    {
                        coarse_sizes[i] /= coarse_n;
                        coarse_displs[i+1] /= coarse_n;
                    }
                }
            }

            void cycle(ParVector& x, ParVector& b, int level = 0)
            {
                ParCSRMatrix* A = levels[level]->A;
                ParCSRMatrix* P = levels[level]->P;
                ParVector& tmp = levels[level]->tmp;

                if (level == num_levels - 1)
                {
                    if (A->local_num_rows)
                    {
                        int active_rank;
                        MPI_Comm_rank(coarse_comm, &active_rank);

                        char trans = 'N'; //No transpose
                        int nhrs = 1; // Number of right hand sides
                        int info; // result

                        aligned_vector<double> b_data(coarse_n);
                        MPI_Allgatherv(b.local.data(), b.local_n, MPI_DOUBLE, b_data.data(), 
                                coarse_sizes.data(), coarse_displs.data(), 
                                MPI_DOUBLE, coarse_comm);

                        dgetrs_(&trans, &coarse_n, &nhrs, A_coarse.data(), &coarse_n, 
                                LU_permute.data(), b_data.data(), &coarse_n, &info);
                        for (int i = 0; i < b.local_n; i++)
                        {
                            x.local[i] = b_data[i + coarse_displs[active_rank]];
                        }
                    }
                }
                else if (tap_amg >= 0 && tap_amg <= level) // TAP AMG
                {
                    levels[level+1]->x.set_const_value(0.0);
                    
                    // Relax
                    switch (relax_type)
                    {
                        case Jacobi:
                            tap_jacobi(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                        case SOR:
                            tap_sor(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                        case SSOR:
                            tap_ssor(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                    }


                    A->tap_residual(x, b, tmp);
                    P->tap_mult_T(tmp, levels[level+1]->b);

                    cycle(levels[level+1]->x, levels[level+1]->b, level+1);

                    P->tap_mult(levels[level+1]->x, tmp);
                    for (int i = 0; i < A->local_num_rows; i++)
                    {
                        x.local[i] += tmp.local[i];
                    }

                    switch (relax_type)
                    {
                        case Jacobi:
                            tap_jacobi(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                        case SOR:
                            tap_sor(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                        case SSOR:
                            tap_ssor(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                    }

                }
                else // Standard AMG
                {
                    levels[level+1]->x.set_const_value(0.0);
                    
                    // Relax
                    switch (relax_type)
                    {
                        case Jacobi:
                            jacobi(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                        case SOR:
                            sor(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                        case SSOR:
                            ssor(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                    }

                    A->residual(x, b, tmp);
                    P->mult_T(tmp, levels[level+1]->b);

                    cycle(levels[level+1]->x, levels[level+1]->b, level+1);

                    P->mult(levels[level+1]->x, tmp);
                    for (int i = 0; i < A->local_num_rows; i++)
                    {
                        x.local[i] += tmp.local[i];
                    }

                    switch (relax_type)
                    {
                        case Jacobi:
                            jacobi(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                        case SOR:
                            sor(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                        case SSOR:
                            ssor(A, x, b, tmp, num_smooth_sweeps, relax_weight);
                            break;
                    }
                }
            }

            int solve(ParVector& sol, ParVector& rhs, int num_iterations = 100)
            {
                double b_norm = rhs.norm(2);
                double r_norm;
                int iter = 0;

                if (store_residuals)
                {
                    residuals.resize(num_iterations + 1);
                }

                // Iterate until convergence or max iterations
                ParVector resid(rhs.global_n, rhs.local_n, rhs.first_local);
                levels[0]->A->residual(sol, rhs, resid);
                if (fabs(b_norm) > zero_tol)
                {
                    r_norm = resid.norm(2) / b_norm;
                }
                else
                {
                    r_norm = resid.norm(2);
                }
                if (store_residuals)
                {
                    residuals[iter] = r_norm;
                }

                while (r_norm > 1e-07 && iter < num_iterations)
                {
                    cycle(sol, rhs, 0);

                    iter++;
                    levels[0]->A->residual(sol, rhs, resid);
                    if (fabs(b_norm) > zero_tol)
                    {
                        r_norm = resid.norm(2) / b_norm;
                    }
                    else
                    {
                        r_norm = resid.norm(2);
                    }
                    if (store_residuals)
                    {
                        residuals[iter] = r_norm;
                    }
                }


                return iter;
            }

            aligned_vector<double>& get_residuals()
            {
                return residuals;
            }

            strength_t strength_type;
            relax_t relax_type;

            int num_smooth_sweeps;
            int max_coarse;
            int max_levels;
            int tap_amg;

            double strong_threshold;
            double relax_weight;
            double sparsify_tol;

            bool store_residuals;

            double* weights;
            aligned_vector<double> residuals;

            std::vector<ParLevel*> levels;
            aligned_vector<int> LU_permute;
            int num_levels;
            int num_variables;
            
            aligned_vector<double> spmv_times;
            aligned_vector<double> spmv_comm_times;
            aligned_vector<double> setup_times;
            aligned_vector<double> strength_times;
            aligned_vector<double> coarsen_times;
            aligned_vector<double> interp_times;
            aligned_vector<double> matmat_times;
            aligned_vector<double> matmat_comm_times;
            
            double setup_comm_t;
            double solve_comm_t;
            int setup_comm_n;
            int setup_comm_s;
            int solve_comm_n;
            int solve_comm_s;

            int coarse_n;
            aligned_vector<double> A_coarse;
            aligned_vector<int> coarse_sizes;
            aligned_vector<int> coarse_displs;
            MPI_Comm coarse_comm;
    };
}
#endif
