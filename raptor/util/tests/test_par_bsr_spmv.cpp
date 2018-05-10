// Copyright (c) 2015-2017, RAPtor Developer Team
// License: Simplified BSD, http://opensource.org/licenses/BSD-2-Clause
#include "gtest/gtest.h"
#include "core/types.hpp"
#include "core/par_matrix.hpp"
#include "gallery/par_matrix_IO.hpp"

using namespace raptor;

int main(int argc, char** argv)
{
    MPI_Init(&argc, &argv);

    int rank, num_procs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs);

    // Setup ParBSRMatrix 
    std::vector<std::vector<double>> on_blocks = {{1,0,2,1}, {6,7,8,2}, {1,4,5,1},
	    					{4,3,0,0}, {7,2,0,0}};
    std::vector<std::vector<double>> off_blocks = {{1,0,0,1}, {2,0,0,0}, {3,0,1,0}};
    std::vector<std::vector<int>> on_indx = {{0,0}, {0,1}, {1,1}, {2,1}, {2,2}};
    std::vector<std::vector<int>> off_indx = {{0,4}, {1,3}, {2,5}};

    ParBSRMatrix* A = new ParBSRMatrix(12, 12, 2, 2);

    for(int i=0; i<on_blocks.size(); i++){
        A->add_block(on_indx[i][0], on_indx[i][1], on_blocks[i]);
        A->add_block(on_indx[i][0]+3, on_indx[i][1]+3, on_blocks[i]);
    }

    for(int i=0; i<off_blocks.size(); i++){
        A->add_block(off_indx[i][0], off_indx[i][1], off_blocks[i]);
        A->add_block(off_indx[i][0]+3, off_indx[i][1]-3, off_blocks[i]);
    }

    // Finalize test matrix
    A->finalize(true, 2);
    // Expand off_proc_column_map for BSR matrix
    //A->expand_off_proc(2);

    // Check that matrix is correct
    for(int i=0; i<num_procs; i++){
        if(i == rank){
	    printf("on_proc: ");
	    for(int k=0; k<A->on_proc_column_map.size(); k++){
                printf("%d ", A->on_proc_column_map[k]);
	    }
	    printf("\n");
	    printf("off_proc: ");
	    for(int k=0; k<A->off_proc_column_map.size(); k++){
                printf("%d ", A->off_proc_column_map[k]);
	    }
	    printf("\n");
	}
	MPI_Barrier(MPI_COMM_WORLD);
    }


    // Vectors for Multiplication
    ParVector x(A->global_num_cols, A->on_proc_num_cols, A->partition->first_local_col);
    ParVector b(A->global_num_rows, A->local_num_rows, A->partition->first_local_row);
    
    // Test SpMV
    x.set_const_value(1.0);
    A->mult(x, b);
    
    for(int i=0; i<num_procs; i++){
        if(i == rank){
	    printf("Rank %d\n", rank);
	    b.local.print();
	}
	MPI_Barrier(MPI_COMM_WORLD);
    }

    // Delete A
    delete A;

    //::testing::InitGoogleTest(&argc, argv);
    //int temp = RUN_ALL_TESTS();
    MPI_Finalize();
    //return temp;
} // end of main() //

TEST(ParRandomSpMVTest, TestsInUtil)
{
    int rank, num_procs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_procs);

    FILE* f;
    double b_val;
    const char* rand_fn = "../../../../test_data/random.pm";
    const char* b_ones = "../../../../test_data/random_ones_b.txt";
    const char* b_T_ones = "../../../../test_data/random_ones_b_T.txt";
    const char* b_inc = "../../../../test_data/random_inc_b.txt";
    const char* b_T_inc = "../../../../test_data/random_inc_b_T.txt";
    ParCSRMatrix* A = readParMatrix(rand_fn);

    ParVector x(A->global_num_cols, A->on_proc_num_cols, A->partition->first_local_col);
    ParVector b(A->global_num_rows, A->local_num_rows, A->partition->first_local_row);

    x.set_const_value(1.0);
    A->mult(x, b);
    f = fopen(b_ones, "r");
    for (int i = 0; i < A->partition->first_local_row; i++)
    {
        fscanf(f, "%lg\n", &b_val);
    }
    for (int i = 0; i < A->local_num_rows; i++)
    {
        fscanf(f, "%lg\n", &b_val);
        ASSERT_NEAR(b[i], b_val, 1e-06);
    }
    fclose(f);

    b.set_const_value(1.0);
    A->mult_T(b, x);
    f = fopen(b_T_ones, "r");
    for (int i = 0; i < A->partition->first_local_col; i++)
    {
        fscanf(f, "%lg\n", &b_val);
    }
    for (int i = 0; i < A->on_proc_num_cols; i++)
    {
        fscanf(f, "%lg\n", &b_val);
        ASSERT_NEAR(x[i],b_val, 1e-06);
    }
    fclose(f);

    for (int i = 0; i < A->on_proc_num_cols; i++)
    {
        x[i] = A->partition->first_local_col + i;
    }
    A->mult(x, b);
    f = fopen(b_inc, "r");
    for (int i = 0; i < A->partition->first_local_row; i++)
    {
        fscanf(f, "%lg\n", &b_val);
    }
    for (int i = 0; i < A->local_num_rows; i++)
    {
        fscanf(f, "%lg\n", &b_val);
        ASSERT_NEAR(b[i], b_val, 1e-06);
    }
    fclose(f);

    for (int i = 0; i < A->local_num_rows; i++)
    {
        b[i] = A->partition->first_local_row + i;
    }
    A->mult_T(b, x);
    f = fopen(b_T_inc, "r");
    for (int i = 0; i < A->partition->first_local_col; i++)
    {
        fscanf(f, "%lg\n", &b_val);
    }
    for (int i = 0; i < A->on_proc_num_cols; i++)
    {
        fscanf(f, "%lg\n", &b_val);
        ASSERT_NEAR(x[i], b_val, 1e-06);
    }
    fclose(f);

    delete A;

} // end of TEST(ParRandomSpMVTest, TestsInUtil) //
