// Copyright (c) 2015-2017, RAPtor Developer Team
// License: Simplified BSD, http://opensource.org/licenses/BSD-2-Clause
#include <mpi.h>
#include <math.h>
#include <stdlib.h>
#include <iostream>
#include <assert.h>

#include "clear_cache.hpp"

#include "raptor.hpp"

void print_comm_data(ParMatrix* A)
{
    int rank, num_procs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
    int num_nodes = A->comm->topology->num_nodes;
    int num_send, size_send;
    int proc, node;
    aligned_vector<int> A_comm_procs(num_procs, 0);
    aligned_vector<int> A_comm_nodes(num_nodes, 0);
    aligned_vector<int> A_proc_num_sends(num_procs);
    aligned_vector<int> A_proc_size_sends(num_procs);
    aligned_vector<int> A_node_num_sends(num_procs);
    aligned_vector<int> A_node_size_sends(num_procs);

    for (int i = 0; i < A->comm->send_data->num_msgs; i++)
    {
        proc = A->comm->send_data->procs[i];
        node = A->comm->topology->get_node(proc);
        A_comm_procs[proc] = 1;
        A_comm_nodes[node]++;
    }
    MPI_Allreduce(MPI_IN_PLACE, A_comm_procs.data(), num_procs, MPI_INT, MPI_SUM, A->comm->topology->local_comm);
    MPI_Allreduce(MPI_IN_PLACE, A_comm_nodes.data(), num_nodes, MPI_INT, MPI_SUM, A->comm->topology->local_comm);

    num_send = 0;
    size_send = 0;
    for (int i = 0; i < num_procs; i++)
    {
        if (A_comm_procs[i])
        {
            num_send++;
            size_send += A_comm_procs[i];
        }
    }
    MPI_Allgather(&num_send, 1, MPI_INT, A_proc_num_sends.data(), 1, MPI_INT, MPI_COMM_WORLD);
    MPI_Allgather(&size_send, 1, MPI_INT, A_proc_size_sends.data(), 1, MPI_INT, MPI_COMM_WORLD);

    num_send = 0;
    size_send = 0;
    for (int i = 0; i < num_nodes; i++)
    {
        if (A_comm_nodes[i])
        {
            num_send++;
            size_send += A_comm_nodes[i];
        }
    }
    MPI_Allgather(&num_send, 1, MPI_INT, A_node_num_sends.data(), 1, MPI_INT, MPI_COMM_WORLD);
    MPI_Allgather(&size_send, 1, MPI_INT, A_node_size_sends.data(), 1, MPI_INT, MPI_COMM_WORLD);

    if (rank == 0)
    {
        for (int i = 0; i < num_procs; i++)
        {
            printf("Proc %d:\t%d\t%d\t%d\t%d\n", i, A_proc_num_sends[i],
                    A_proc_size_sends[i], A_node_num_sends[i], A_node_size_sends[i]);
        }
    }
}


int main(int argc, char* argv[])
{
    MPI_Init(&argc, &argv);
    int rank, num_procs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs);

    int n = 5;
    int system = 0;
    double strong_threshold = 0.25;
    int iter;
    int num_variables = 1;

    coarsen_t coarsen_type = HMIS;
    interp_t interp_type = Extended;

    ParMultilevel* ml;
    ParCSRMatrix* A;
    ParVector x;
    ParVector b;

    double t0, tfinal;

    int num_send, size_send;

    if (argc > 1)
    {
        system = atoi(argv[1]);
    }
    if (system < 2)
    {
        int dim;
        double* stencil = NULL;
        aligned_vector<int> grid;
        if (argc > 2)
        {
            n = atoi(argv[2]);
        }

        if (system == 0)
        {
            dim = 3;
            grid.resize(dim, n);
            stencil = laplace_stencil_27pt();
        }
        else if (system == 1)
        {
            coarsen_type = Falgout;
            interp_type = ModClassical;

            dim = 2;
            grid.resize(dim, n);
            double eps = 0.001;
            double theta = M_PI/4.0;
            if (argc > 3)
            {
                eps = atof(argv[3]);
                if (argc > 4)
                {
                    theta = atof(argv[4]);
                }
            }
            stencil = diffusion_stencil_2d(eps, theta);
        }
        A = par_stencil_grid(stencil, grid.data(), dim);
        delete[] stencil;
    }
#ifdef USING_MFEM
    else if (system == 2)
    {
        const char* mesh_file = argv[2];
        int mfem_system = 0;
        int order = 2;
        int seq_refines = 1;
        int par_refines = 1;
        int max_dofs = 1000000;
        if (argc > 3)
        {
            mfem_system = atoi(argv[3]);
            if (argc > 4)
            {
                order = atoi(argv[4]);
                if (argc > 5)
                {
                    seq_refines = atoi(argv[5]);
                    max_dofs = atoi(argv[5]);
                    if (argc > 6)
                    {
                        par_refines = atoi(argv[6]);
                    }
                }
            }
        }

        coarsen_type = HMIS;
        interp_type = Extended;
        switch (mfem_system)
        {
            case 0:
                A = mfem_laplacian(x, b, mesh_file, order, seq_refines, par_refines);
                break;
            case 1:
                strong_threshold = 0.0;
                A = mfem_grad_div(x, b, mesh_file, order, seq_refines, par_refines);
                break;
            case 2:
                strong_threshold = 0.5;
                A = mfem_linear_elasticity(x, b, &num_variables, mesh_file, order, 
                        seq_refines, par_refines);
                break;
            case 3:
                A = mfem_adaptive_laplacian(x, b, mesh_file, order);
                x.set_const_value(1.0);
                A->mult(x, b);
                x.set_const_value(0.0);
                break;
            case 4:
                A = mfem_dg_diffusion(x, b, mesh_file, order, seq_refines, par_refines);
                break;
            case 5:
                A = mfem_dg_elasticity(x, b, &num_variables, mesh_file, order, seq_refines, par_refines);
                break;
        }
    }
#endif
    else if (system == 3)
    {
        const char* file = "../../examples/LFAT5.pm";
        A = readParMatrix(file);
    }


    A->tap_comm = new TAPComm(A->partition, A->off_proc_column_map,
            A->on_proc_column_map);

    if (system != 2)
    {
        x = ParVector(A->global_num_cols, A->on_proc_num_cols, A->partition->first_local_col);
        b = ParVector(A->global_num_rows, A->local_num_rows, A->partition->first_local_row);
        x.set_rand_values();
        A->mult(x, b);
        x.set_const_value(0.0);
    }

    int n_tests = 100;
    int n_spmv_tests = 1000;

    // Ruge-Stuben AMG
    if (rank == 0) printf("Ruge Stuben Solver: \n");
    MPI_Barrier(MPI_COMM_WORLD);
    ml = new ParRugeStubenSolver(strong_threshold, coarsen_type, interp_type, Classical, SOR);
    ml->num_variables = num_variables;
    ml->setup(A);
    for (int i = 0; i < ml->num_levels - 1; i++)
    {
        ParCSRMatrix* Al = ml->levels[i]->A;
        ParCSRMatrix* Pl = ml->levels[i]->P;
        ParCSRMatrix* AP = Al->mult(Pl);
        ParCSCMatrix* P_csc = new ParCSCMatrix(Pl);
        ParCSRMatrix* C;

        if (rank == 0) printf("Level %d\n", i);

        if (rank == 0) printf("Comm Data for Al\n");
        if (!Al->comm) Al->comm = new ParComm(Al->partition, Al->off_proc_column_map, 
                Al->on_proc_column_map);
        print_comm_data(Al);
        if (rank == 0) printf("Comm Data for Pl\n");
        if (!P_csc->comm) P_csc->comm = new ParComm(P_csc->partition, P_csc->off_proc_column_map,
                P_csc->on_proc_column_map);
        print_comm_data(P_csc);



        // Test A*P
        tfinal = 0;
        for (int i = 0; i < n_tests; i++)
        {
           t0 = MPI_Wtime();
           C = Al->mult(Pl);
           tfinal += (MPI_Wtime() - t0);
           delete C;
        }
        tfinal /= n_tests;
        MPI_Reduce(&tfinal, &t0, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        if (rank == 0) printf("A mult P time: %e\n", t0);

        // Test A*P  tap_mult
        if (Al->tap_comm) delete Al->tap_comm;
        Al->tap_comm = new TAPComm(Al->partition, Al->off_proc_column_map, Al->on_proc_column_map);
        tfinal = 0;
        for (int i = 0; i < n_tests; i++)
        {
            t0 = MPI_Wtime();
            C = Al->tap_mult(Pl);
            tfinal += (MPI_Wtime() - t0);
            delete C;
        }
        tfinal /= n_tests;
        MPI_Reduce(&tfinal, &t0, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        if (rank == 0) printf("A tap_mult P time: %e\n", t0);

        // Test A*P simple tap_mult
        if (Al->tap_comm) delete Al->tap_comm;
        Al->tap_comm = new TAPComm(Al->partition, Al->off_proc_column_map, Al->on_proc_column_map, false);
        tfinal = 0;
        for (int i = 0; i < n_tests; i++)
        {
            t0 = MPI_Wtime();
            C = Al->tap_mult(Pl);
            tfinal += (MPI_Wtime() - t0);
            delete C;
        }
        tfinal /= n_tests;
        MPI_Reduce(&tfinal, &t0, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        if (rank == 0) printf("A simple tap_mult P time: %e\n", t0);


        // Test PT*AP
        tfinal = 0;
        for (int i = 0; i < n_tests; i++)
        {
           t0 = MPI_Wtime();
           C = AP->mult_T(P_csc);
           tfinal += (MPI_Wtime() - t0);
           delete C;
        }
        tfinal /= n_tests;
        MPI_Reduce(&tfinal, &t0, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        if (rank == 0) printf("PT mult AT time: %e\n", t0);

        // Test A*P  tap_mult
        if (P_csc->tap_comm) delete P_csc->tap_comm;
        P_csc->tap_comm = new TAPComm(P_csc->partition, P_csc->off_proc_column_map, P_csc->on_proc_column_map);
        tfinal = 0;
        for (int i = 0; i < n_tests; i++)
        {
            t0 = MPI_Wtime();
            C = AP->tap_mult_T(P_csc);
            tfinal += (MPI_Wtime() - t0);
            delete C;
        }
        tfinal /= n_tests;
        MPI_Reduce(&tfinal, &t0, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        if (rank == 0) printf("PT tap_mult AP time: %e\n", t0);

        // Test A*P simple tap_mult
        if (P_csc->tap_comm) delete P_csc->tap_comm;
        P_csc->tap_comm = new TAPComm(P_csc->partition, P_csc->off_proc_column_map, P_csc->on_proc_column_map, false);
        tfinal = 0;
        for (int i = 0; i < n_tests; i++)
        {
            t0 = MPI_Wtime();
            C = AP->tap_mult_T(P_csc);
            tfinal += (MPI_Wtime() - t0);
            delete C;
        }
        tfinal /= n_tests;
        MPI_Reduce(&tfinal, &t0, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        if (rank == 0) printf("PT simple tap_mult AP time: %e\n", t0);

        // Test PT*AP
        if (!Al->comm) Al->comm = new ParComm(Al->partition, Al->off_proc_column_map,
                Al->on_proc_column_map);
        ParVector x(Al->global_num_rows, Al->local_num_rows, Al->partition->first_local_row);
        ParVector b(Al->global_num_rows, Al->local_num_rows, Al->partition->first_local_row);
        t0 = MPI_Wtime();
        for (int i = 0; i < n_spmv_tests; i++)
        {
            x.set_const_value(0.0);
            Al->mult(x, b);
        }
        tfinal = (MPI_Wtime() - t0) / n_spmv_tests;
        MPI_Reduce(&tfinal, &t0, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        if (rank == 0) printf("Al mult x time: %e\n", t0);

        // Test A*P  tap_mult
        if (Al->tap_comm) delete Al->tap_comm;
        Al->tap_comm = new TAPComm(Al->partition, Al->off_proc_column_map, Al->on_proc_column_map);
        t0 = MPI_Wtime();
        for (int i = 0; i < n_spmv_tests; i++)
        {
            x.set_const_value(0.0);
            Al->tap_mult(x, b);
        }
        tfinal = (MPI_Wtime() - t0) / n_spmv_tests;
        MPI_Reduce(&tfinal, &t0, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        if (rank == 0) printf("Al tap_mult x time: %e\n", t0);

        // Test A*P simple tap_mult
        if (Al->tap_comm) delete Al->tap_comm;
        Al->tap_comm = new TAPComm(Al->partition, Al->off_proc_column_map, Al->on_proc_column_map, false);
        t0 = MPI_Wtime();
        for (int i = 0; i < n_spmv_tests; i++)
        {
            x.set_const_value(0.0);
            Al->tap_mult(x, b);
        }
        tfinal = (MPI_Wtime() - t0) / n_spmv_tests;
        MPI_Reduce(&tfinal, &t0, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        if (rank == 0) printf("Al simple tap_mult x time: %e\n", t0);


        delete AP;
        delete P_csc;
    }
    delete ml;


    delete A;

    MPI_Finalize();
    return 0;
}
